#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>

#include "ucd.h"
#include "regexp.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

/* Decode one UTF-16 code point starting at *sp, advancing *sp past it (2
 * units for a valid surrogate pair, 1 otherwise -- an unpaired surrogate
 * decodes as itself). Extracted verbatim from jsvm2's include/js_string.h;
 * this is the one dependency regexp.c has on that header, and it doesn't
 * need any of the JSString/interning machinery that comes with it. */
static inline uint32_t decode_utf16(const uint16_t** sp) {
    uint32_t cp = *(*sp)++;
    if (cp >= 0xD800 && cp <= 0xDBFF) {
        if (**sp >= 0xDC00 && **sp <= 0xDFFF) {
            uint32_t trail = *(*sp)++;
            return ((cp - 0xD800) << 10) + (trail - 0xDC00) + 0x10000;
        }
    }
    return cp;
}

/* ==========================================================================
 * 2. LEXER & HELPERS
 * ========================================================================== */
typedef enum {
    TOK_LITERAL, TOK_CLASS, TOK_STAR, TOK_PLUS, TOK_QUESTION, 
    TOK_BOUNDS, TOK_OR, TOK_LPAREN, TOK_RPAREN, TOK_LOOKAHEAD, TOK_NEG_LOOKAHEAD, TOK_EOF,
    TOK_CARET, TOK_DOLLAR, TOK_BACKREF, TOK_NONCAP_GROUP, TOK_WORD_BOUNDARY, TOK_NON_WORD_BOUNDARY,
    TOK_NAMED_GROUP, TOK_NAMED_BACKREF, TOK_LOOKBEHIND, TOK_NEG_LOOKBEHIND, TOK_MODIFIER_GROUP
} TokenType;

typedef struct {
    TokenType type;
    uint32_t ch;
    int class_id;
    int min;
    int max;
    char name[32];
    int flags_on;
    int flags_off;
} Token;

typedef struct {
    const uint16_t* src;
    int pos;
    Token current;
    Program* prog;
    bool has_named_groups;
} Lexer;

static inline uint32_t decode_utf16_lexer(Lexer* lexer) {
    uint32_t cp = lexer->src[lexer->pos++];
    if (lexer->prog->unicode && cp >= 0xD800 && cp <= 0xDBFF) {
        if (lexer->src[lexer->pos] >= 0xDC00 && lexer->src[lexer->pos] <= 0xDFFF) {
            cp = ((cp - 0xD800) << 10) + (lexer->src[lexer->pos++] - 0xDC00) + 0x10000;
        }
    }
    return cp;
}

static inline bool is_word_char(uint32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static inline bool is_digit_char(uint32_t c) {
    return c >= '0' && c <= '9';
}

void invert_class(CharClass* cls);

static bool parse_hex(Lexer* lexer, int digits, uint32_t* out) {
    uint32_t val = 0;
    for (int i = 0; i < digits; i++) {
        uint32_t c = lexer->src[lexer->pos + i];
        if (c == '\0') return false;
        int hex_val = -1;
        val <<= 4;
        if (c >= '0' && c <= '9') hex_val = c - '0';
        else if (c >= 'a' && c <= 'f') hex_val = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') hex_val = c - 'A' + 10;
        else return false;
        val |= hex_val;
    }
    lexer->pos += digits;
    *out = val;
    return true;
}

static bool parse_braced_hex(Lexer* lexer, uint32_t* out) {
    if (lexer->src[lexer->pos] != '{') {
        lexer->prog->error = "SyntaxError: Invalid Unicode escape sequence";
        return false;
    }
    lexer->pos++;
    uint32_t cp = 0;
    int digits = 0;
    while (lexer->src[lexer->pos] != '}' && lexer->src[lexer->pos] != '\0') {
        uint32_t c = lexer->src[lexer->pos];
        int hex_val = -1;
        if (c >= '0' && c <= '9') hex_val = c - '0';
        else if (c >= 'a' && c <= 'f') hex_val = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') hex_val = c - 'A' + 10;
        else { lexer->prog->error = "SyntaxError: Invalid Unicode escape sequence"; return false; }
        uint64_t new_cp = ((uint64_t)cp << 4) | hex_val;
        if (new_cp > 0x10FFFF) { lexer->prog->error = "SyntaxError: Unicode escape sequence out of range"; return false; }
        cp = (uint32_t)new_cp;
        lexer->pos++; digits++;
    }
    if (digits == 0) { lexer->prog->error = "SyntaxError: Empty Unicode escape sequence"; return false; }
    if (lexer->src[lexer->pos] != '}') { lexer->prog->error = "SyntaxError: Unterminated Unicode escape sequence"; return false; }
    lexer->pos++;
    *out = cp;
    return true;
}

/* ---- RegExpIdentifierName (capture group name) validation --------------- */

static bool rx_cp_in_ucd(const UCDProperty* prop, uint32_t cp) {
    if (!prop) return false;
    int lo = 0, hi = prop->count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (cp < prop->ranges[mid].start) hi = mid - 1;
        else if (cp > prop->ranges[mid].end) lo = mid + 1;
        else return true;
    }
    return false;
}

/* RegExpIdentifierStart: UnicodeIDStart | $ | _ | \u escape thereof. */
static bool rx_is_id_start(uint32_t cp) {
    if (cp == '$' || cp == '_') return true;
    if (cp < 0x80) return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
    static const UCDProperty* prop = NULL;
    if (!prop) prop = lookup_unicode_property("ID_Start");
    return rx_cp_in_ucd(prop, cp);
}

/* RegExpIdentifierPart: UnicodeIDContinue | $ | _ | ZWNJ | ZWJ | \u escape. */
static bool rx_is_id_continue(uint32_t cp) {
    if (cp == '$' || cp == '_' || cp == 0x200C || cp == 0x200D) return true;
    if (cp < 0x80) return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9');
    static const UCDProperty* prop = NULL;
    if (!prop) prop = lookup_unicode_property("ID_Continue");
    return rx_cp_in_ucd(prop, cp);
}

static int rx_hexval(uint16_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode one code point of a RegExpIdentifierName from src at *pos: a \u /
 * \u{...} escape, or a (possibly surrogate-paired) source code unit. Returns the
 * code point, or -1 on a malformed escape or lone surrogate. Advances *pos on
 * success. */
static int64_t rx_idname_cp(const uint16_t* src, int* pos) {
    uint16_t c = src[*pos];
    if (c == '\\') {
        if (src[*pos + 1] != 'u') return -1;
        *pos += 2;
        if (src[*pos] == '{') {
            (*pos)++;
            int64_t cp = 0; int digits = 0;
            int h;
            while ((h = rx_hexval(src[*pos])) >= 0) {
                cp = cp * 16 + h; (*pos)++; digits++;
                if (cp > 0x10FFFF) return -1;
            }
            if (digits == 0 || src[*pos] != '}') return -1;
            (*pos)++;
            return cp;
        }
        int cp = 0;
        for (int i = 0; i < 4; i++) {
            int h = rx_hexval(src[*pos]);
            if (h < 0) return -1;
            cp = cp * 16 + h; (*pos)++;
        }
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            /* High surrogate: pair with a following \uXXXX low surrogate. */
            if (src[*pos] == '\\' && src[*pos + 1] == 'u') {
                int save = *pos; *pos += 2;
                int lo = 0; bool ok = true;
                for (int i = 0; i < 4; i++) {
                    int h = rx_hexval(src[*pos]);
                    if (h < 0) { ok = false; break; }
                    lo = lo * 16 + h; (*pos)++;
                }
                if (ok && lo >= 0xDC00 && lo <= 0xDFFF)
                    return 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                *pos = save;
            }
            return -1; /* lone surrogate */
        }
        if (cp >= 0xDC00 && cp <= 0xDFFF) return -1;
        return cp;
    }
    if (c >= 0xD800 && c <= 0xDBFF) {
        uint16_t lo = src[*pos + 1];
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
            *pos += 2;
            return 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        }
        return -1; /* lone surrogate */
    }
    if (c >= 0xDC00 && c <= 0xDFFF) return -1;
    (*pos)++;
    return c;
}

static void rx_name_append_utf8(char* buf, int* len, uint32_t cp) {
    /* buf is 32 bytes; a well-formed name that fits in 31 UTF-16 units of source
     * cannot overflow this in practice, but bound it defensively. */
    char tmp[4]; int n;
    if (cp < 0x80) { tmp[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { tmp[0] = (char)(0xC0 | (cp >> 6)); tmp[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { tmp[0] = (char)(0xE0 | (cp >> 12)); tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else { tmp[0] = (char)(0xF0 | (cp >> 18)); tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    if (*len + n >= 32) return;
    for (int i = 0; i < n; i++) buf[(*len)++] = tmp[i];
}

/* Parse a RegExpIdentifierName from lexer->src, starting just past the opening
 * '<'. Validates each code point against RegExpIdentifierStart/Part, decoding
 * \u escapes, and writes the canonical UTF-8 form (so a and 'a' match) to
 * out[<=31]. On success consumes the closing '>' and returns true; otherwise
 * sets prog->error and returns false. */
static bool parse_group_name(Lexer* lexer, char* out) {
    const uint16_t* src = lexer->src;
    int len = 0;
    bool first = true;
    while (src[lexer->pos] != '>') {
        if (src[lexer->pos] == '\0') {
            lexer->prog->error = "SyntaxError: Invalid capture group name";
            return false;
        }
        int64_t cp = rx_idname_cp(src, &lexer->pos);
        if (cp < 0) {
            lexer->prog->error = "SyntaxError: Invalid capture group name";
            return false;
        }
        bool ok = first ? rx_is_id_start((uint32_t)cp) : rx_is_id_continue((uint32_t)cp);
        if (!ok) {
            lexer->prog->error = "SyntaxError: Invalid capture group name";
            return false;
        }
        rx_name_append_utf8(out, &len, (uint32_t)cp);
        first = false;
    }
    if (first) { /* empty name */
        lexer->prog->error = "SyntaxError: Invalid capture group name";
        return false;
    }
    lexer->pos++; /* consume '>' */
    out[len] = '\0';
    return true;
}

static bool add_range(CharClass* cls, uint32_t start, uint32_t end) {
    if (start > end) return true;
    
    int i;
    for (i = 0; i < cls->range_count; i++) {
        if (cls->ranges[i].start > start) break;
    }
    
    if (i > 0 && cls->ranges[i-1].end + 1 >= start) {
        if (end > cls->ranges[i-1].end) {
            cls->ranges[i-1].end = end;
        }
        int out = i;
        for (int j = i; j < cls->range_count; j++) {
            if (cls->ranges[out-1].end + 1 >= cls->ranges[j].start) {
                if (cls->ranges[j].end > cls->ranges[out-1].end) {
                    cls->ranges[out-1].end = cls->ranges[j].end;
                }
            } else {
                cls->ranges[out++] = cls->ranges[j];
            }
        }
        cls->range_count = out;
        return true;
    }
    
    if (i < cls->range_count && end + 1 >= cls->ranges[i].start) {
        cls->ranges[i].start = start;
        if (end > cls->ranges[i].end) {
            cls->ranges[i].end = end;
        }
        int out = i + 1;
        for (int j = i + 1; j < cls->range_count; j++) {
            if (cls->ranges[out-1].end + 1 >= cls->ranges[j].start) {
                if (cls->ranges[j].end > cls->ranges[out-1].end) {
                    cls->ranges[out-1].end = cls->ranges[j].end;
                }
            } else {
                cls->ranges[out++] = cls->ranges[j];
            }
        }
        cls->range_count = out;
        return true;
    }
    
    if (cls->range_count >= 2048) return false;
    
    for (int j = cls->range_count; j > i; j--) {
        cls->ranges[j] = cls->ranges[j-1];
    }
    cls->ranges[i] = (CodePointRange){start, end};
    cls->range_count++;
    return true;
}

static void fill_builtin_class(CharClass* cls, char type, bool unicode) {
    if (type == 'd') {
        add_range(cls, '0', '9');
    } else if (type == 'D') {
        add_range(cls, 0, '0' - 1);
        add_range(cls, '9' + 1, unicode ? 0x10FFFF : 255);
    } else if (type == 'w') {
        add_range(cls, '0', '9');
        add_range(cls, 'A', 'Z');
        add_range(cls, '_', '_');
        add_range(cls, 'a', 'z');
    } else if (type == 'W') {
        add_range(cls, 0, '0' - 1);
        add_range(cls, '9' + 1, 'A' - 1);
        add_range(cls, 'Z' + 1, '_' - 1);
        add_range(cls, '_' + 1, 'a' - 1);
        add_range(cls, 'z' + 1, unicode ? 0x10FFFF : 255);
    } else if (type == 's') {
        uint32_t spaces[] = {
            0x0009, 0x000D, 0x0020, 0x0020, 0x00A0, 0x00A0, 0x1680, 0x1680,
            0x2000, 0x200A, 0x2028, 0x2029, 0x202F, 0x202F, 0x205F, 0x205F,
            0x3000, 0x3000, 0xFEFF, 0xFEFF
        };
        for (size_t i = 0; i < sizeof(spaces)/sizeof(spaces[0]); i += 2) {
            if (!unicode && spaces[i] > 255) break;
            add_range(cls, spaces[i], spaces[i+1]);
        }
    } else if (type == 'S') {
        CharClass temp = {0};
        fill_builtin_class(&temp, 's', unicode);
        invert_class(&temp);
        for (int i = 0; i < temp.range_count; i++) {
            if (!unicode && temp.ranges[i].start > 255) break;
            uint32_t end = (!unicode && temp.ranges[i].end > 255) ? 255 : temp.ranges[i].end;
            add_range(cls, temp.ranges[i].start, end);
        }
    }
}

#define MAX_PROP_CACHE 64
static struct {
    char name[64];
    CharClass cls;
} prop_cache[MAX_PROP_CACHE];
static int prop_cache_count = 0;

/* Returns false iff `negate` was requested on a "property of strings" (e.g.
 * \P{RGI_Emoji_Flag_Sequence}) -- negating a set that contains multi-
 * codepoint strings has no well-defined single-codepoint complement, and
 * real engines reject it as a SyntaxError (confirmed: `\P{Basic_Emoji}` and
 * `[\P{Basic_Emoji}]` both fail to compile in a spec-compliant engine, not
 * just the already-handled `[^\p{Basic_Emoji}]` negated-class case). Callers
 * must check this and set prog->error accordingly. */
static bool fill_unicode_property(CharClass* cls, const char* prop, bool negate) {
    CharClass temp = {0};
    
    bool cached = false;
    for (int i = 0; i < prop_cache_count; i++) {
        if (strcmp(prop_cache[i].name, prop) == 0) {
            temp = prop_cache[i].cls;
            cached = true;
            break;
        }
    }
    
    if (!cached) {
        if (strcmp(prop, "L") == 0 || strcmp(prop, "Letter") == 0) {
            fill_unicode_property(&temp, "Lowercase_Letter", false);
            fill_unicode_property(&temp, "Uppercase_Letter", false);
            fill_unicode_property(&temp, "Titlecase_Letter", false);
            fill_unicode_property(&temp, "Modifier_Letter", false);
            fill_unicode_property(&temp, "Other_Letter", false);
    } else if (strcmp(prop, "M") == 0 || strcmp(prop, "Mark") == 0 || strcmp(prop, "Combining_Mark") == 0) {
        fill_unicode_property(&temp, "Nonspacing_Mark", false);
        fill_unicode_property(&temp, "Spacing_Mark", false);
        fill_unicode_property(&temp, "Enclosing_Mark", false);
    } else if (strcmp(prop, "N") == 0 || strcmp(prop, "Number") == 0) {
        fill_unicode_property(&temp, "Decimal_Number", false);
        fill_unicode_property(&temp, "Letter_Number", false);
        fill_unicode_property(&temp, "Other_Number", false);
    } else if (strcmp(prop, "P") == 0 || strcmp(prop, "Punctuation") == 0 || strcmp(prop, "punct") == 0) {
        fill_unicode_property(&temp, "Connector_Punctuation", false);
        fill_unicode_property(&temp, "Dash_Punctuation", false);
        fill_unicode_property(&temp, "Open_Punctuation", false);
        fill_unicode_property(&temp, "Close_Punctuation", false);
        fill_unicode_property(&temp, "Initial_Punctuation", false);
        fill_unicode_property(&temp, "Final_Punctuation", false);
        fill_unicode_property(&temp, "Other_Punctuation", false);
    } else if (strcmp(prop, "S") == 0 || strcmp(prop, "Symbol") == 0) {
        fill_unicode_property(&temp, "Math_Symbol", false);
        fill_unicode_property(&temp, "Currency_Symbol", false);
        fill_unicode_property(&temp, "Modifier_Symbol", false);
        fill_unicode_property(&temp, "Other_Symbol", false);
    } else if (strcmp(prop, "Z") == 0 || strcmp(prop, "Separator") == 0) {
        fill_unicode_property(&temp, "Space_Separator", false);
        fill_unicode_property(&temp, "Line_Separator", false);
        fill_unicode_property(&temp, "Paragraph_Separator", false);
    } else if (strcmp(prop, "C") == 0 || strcmp(prop, "Other") == 0) {
        fill_unicode_property(&temp, "Control", false);
        fill_unicode_property(&temp, "Format", false);
        fill_unicode_property(&temp, "Surrogate", false);
        fill_unicode_property(&temp, "Private_Use", false);
        fill_unicode_property(&temp, "Unassigned", false);
    } else {
        const char* long_prop = prop;
        if (strcmp(prop, "Lu") == 0) long_prop = "Uppercase_Letter";
        else if (strcmp(prop, "Ll") == 0) long_prop = "Lowercase_Letter";
        else if (strcmp(prop, "Lt") == 0) long_prop = "Titlecase_Letter";
        else if (strcmp(prop, "Lm") == 0) long_prop = "Modifier_Letter";
        else if (strcmp(prop, "Lo") == 0) long_prop = "Other_Letter";
        else if (strcmp(prop, "Mn") == 0) long_prop = "Nonspacing_Mark";
        else if (strcmp(prop, "Mc") == 0) long_prop = "Spacing_Mark";
        else if (strcmp(prop, "Me") == 0) long_prop = "Enclosing_Mark";
        else if (strcmp(prop, "Nd") == 0) long_prop = "Decimal_Number";
        else if (strcmp(prop, "Nl") == 0) long_prop = "Letter_Number";
        else if (strcmp(prop, "No") == 0) long_prop = "Other_Number";
        else if (strcmp(prop, "Pc") == 0) long_prop = "Connector_Punctuation";
        else if (strcmp(prop, "Pd") == 0) long_prop = "Dash_Punctuation";
        else if (strcmp(prop, "Ps") == 0) long_prop = "Open_Punctuation";
        else if (strcmp(prop, "Pe") == 0) long_prop = "Close_Punctuation";
        else if (strcmp(prop, "Pi") == 0) long_prop = "Initial_Punctuation";
        else if (strcmp(prop, "Pf") == 0) long_prop = "Final_Punctuation";
        else if (strcmp(prop, "Po") == 0) long_prop = "Other_Punctuation";
        else if (strcmp(prop, "Sm") == 0) long_prop = "Math_Symbol";
        else if (strcmp(prop, "Sc") == 0) long_prop = "Currency_Symbol";
        else if (strcmp(prop, "Sk") == 0) long_prop = "Modifier_Symbol";
        else if (strcmp(prop, "So") == 0) long_prop = "Other_Symbol";
        else if (strcmp(prop, "Zs") == 0) long_prop = "Space_Separator";
        else if (strcmp(prop, "Zl") == 0) long_prop = "Line_Separator";
        else if (strcmp(prop, "Zp") == 0) long_prop = "Paragraph_Separator";
        else if (strcmp(prop, "Cc") == 0) long_prop = "Control";
        else if (strcmp(prop, "Cf") == 0) long_prop = "Format";
        else if (strcmp(prop, "Cs") == 0) long_prop = "Surrogate";
        else if (strcmp(prop, "Co") == 0) long_prop = "Private_Use";
        else if (strcmp(prop, "Cn") == 0) long_prop = "Unassigned";

        const UCDProperty* ucd_prop = lookup_unicode_property(long_prop);
        if (ucd_prop) {
            for (int i = 0; i < ucd_prop->count; i++) {
                add_range(&temp, ucd_prop->ranges[i].start, ucd_prop->ranges[i].end);
            }
            /* "Properties of strings" (RGI_Emoji, RGI_Emoji_Flag_Sequence,
             * Basic_Emoji, etc. -- /v mode only) carry multi-codepoint
             * sequences alongside or instead of single-codepoint ranges. */
            for (int i = 0; i < ucd_prop->sequence_count && temp.string_count < 128; i++) {
                const UCDStringSequence* seq = &ucd_prop->sequences[i];
                StringSequence* out = &temp.strings[temp.string_count++];
                int len = seq->length < 16 ? seq->length : 16;
                for (int k = 0; k < len; k++) out->cps[k] = seq->cps[k];
                out->length = len;
            }
        }
    }
        
        if (prop_cache_count < MAX_PROP_CACHE) {
            strncpy(prop_cache[prop_cache_count].name, prop, 63);
            prop_cache[prop_cache_count].cls = temp;
            prop_cache_count++;
        }
    }
    
    if (negate) {
        if (temp.string_count > 0) return false;
        uint32_t current = 0;
        for (int i = 0; i < temp.range_count; i++) {
            if (temp.ranges[i].start > current) {
                add_range(cls, current, temp.ranges[i].start - 1);
            }
            current = temp.ranges[i].end + 1;
        }
        if (current <= 0x10FFFF) add_range(cls, current, 0x10FFFF);
    } else {
        for (int i = 0; i < temp.range_count; i++) add_range(cls, temp.ranges[i].start, temp.ranges[i].end);
        for (int i = 0; i < temp.string_count && cls->string_count < 128; i++) {
            cls->strings[cls->string_count++] = temp.strings[i];
        }
    }
    return true;
}

/* Non-unicode-mode (Annex B) per-character canonicalization: fold via the
 * character's simple uppercase mapping, except when that mapping would cross
 * from a non-ASCII source into an ASCII target -- real engines skip that
 * fold specifically to avoid non-ASCII/ASCII cross-contamination (confirmed
 * against real JS: LATIN SMALL LETTER LONG S U+017F, whose simple uppercase
 * mapping is 'S', does NOT case-insensitively match 's'/'S'; KELVIN SIGN
 * U+212A, which has no simple uppercase mapping at all, does not match 'k'/
 * 'K' either). This is ECMA-262's Canonicalize abstract operation for the
 * non-Unicode case, expressed with the "simple" (single-code-point) mapping
 * table -- which is exactly what a *simple* uppercase mapping already is, so
 * the "if toUppercase(ch) has length != 1, don't fold" spec clause falls out
 * for free rather than needing separate handling. */
static uint32_t annexb_canonicalize(uint32_t ch) {
    int lo = 0, hi = (int)(sizeof(UCD_SIMPLE_UPPERCASE) / sizeof(SimpleCaseMapping)) - 1;
    uint32_t cu = ch;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (UCD_SIMPLE_UPPERCASE[mid].cp == ch) { cu = UCD_SIMPLE_UPPERCASE[mid].mapping; break; }
        if (UCD_SIMPLE_UPPERCASE[mid].cp < ch) lo = mid + 1; else hi = mid - 1;
    }
    if (ch >= 128 && cu < 128) return ch;
    return cu;
}

static void apply_case_folding(CharClass* cls, bool unicode) {
    if (unicode) {
        bool changed;
        do {
            changed = false;
            for (size_t i = 0; i < sizeof(UCD_CASE_FOLD)/sizeof(CaseFoldMapping); i++) {
                uint32_t from = UCD_CASE_FOLD[i].from;
                uint32_t to = UCD_CASE_FOLD[i].to;
                bool from_in = false, to_in = false;
                for (int j = 0; j < cls->range_count; j++) {
                    if (from >= cls->ranges[j].start && from <= cls->ranges[j].end) from_in = true;
                    if (to >= cls->ranges[j].start && to <= cls->ranges[j].end) to_in = true;
                    if (from_in && to_in) break;
                }
                if (from_in && !to_in) { if (add_range(cls, to, to)) changed = true; }
                if (to_in && !from_in) { if (add_range(cls, from, from)) changed = true; }
            }
        } while (changed);
    } else {
        /* Table-driven, like the unicode branch above -- iterating every
         * candidate codepoint in a wide range (e.g. non-unicode mode's
         * 0-0xFFFF space) would be far too slow for a compile-time step.
         * Skip pairs the ASCII-crossing rule above would reject, so e.g.
         * 's'/'S' never pull in long-s U+017F and vice versa. */
        bool changed;
        do {
            changed = false;
            for (size_t i = 0; i < sizeof(UCD_SIMPLE_UPPERCASE)/sizeof(SimpleCaseMapping); i++) {
                uint32_t from = UCD_SIMPLE_UPPERCASE[i].cp;
                uint32_t to = UCD_SIMPLE_UPPERCASE[i].mapping;
                if (from >= 128 && to < 128) continue;
                bool from_in = false, to_in = false;
                for (int j = 0; j < cls->range_count; j++) {
                    if (from >= cls->ranges[j].start && from <= cls->ranges[j].end) from_in = true;
                    if (to >= cls->ranges[j].start && to <= cls->ranges[j].end) to_in = true;
                    if (from_in && to_in) break;
                }
                if (from_in && !to_in) { if (add_range(cls, to, to)) changed = true; }
                if (to_in && !from_in) { if (add_range(cls, from, from)) changed = true; }
            }
        } while (changed);
    }
}

void invert_class(CharClass* cls) {
    CharClass res = {0};
    uint32_t current = 0;
    for (int i = 0; i < cls->range_count; i++) {
        if (cls->ranges[i].start > current) {
            add_range(&res, current, cls->ranges[i].start - 1);
        }
        current = cls->ranges[i].end + 1;
    }
    if (current <= 0x10FFFF) add_range(&res, current, 0x10FFFF);
    res.negated = false;
    *cls = res;
}

static void intersect_classes(CharClass* a, CharClass* b) {
    if (a->negated) invert_class(a);
    if (b->negated) invert_class(b);
    CharClass res = {0};
    for (int i = 0; i < a->range_count; i++) {
        for (int j = 0; j < b->range_count; j++) {
            uint32_t start = a->ranges[i].start > b->ranges[j].start ? a->ranges[i].start : b->ranges[j].start;
            uint32_t end = a->ranges[i].end < b->ranges[j].end ? a->ranges[i].end : b->ranges[j].end;
            if (start <= end) add_range(&res, start, end);
        }
    }
    res.negated = false;
    *a = res;
}

static void parse_char_class(Lexer* lexer, CharClass* cls) {
    bool negate = false;
    if (lexer->src[lexer->pos] == '^') { negate = true; lexer->pos++; }
    
    CharClass current_union = {0};
    int set_op = 0; // 0=none, 1=&&, 2=--
    
    while (lexer->src[lexer->pos] != ']' && lexer->src[lexer->pos] != '\0') {
        if (lexer->prog->unicode_sets && lexer->src[lexer->pos] == '&' && lexer->src[lexer->pos+1] == '&') {
            if (lexer->prog->ignore_case) apply_case_folding(&current_union, lexer->prog->unicode);
            if (set_op == 1) intersect_classes(cls, &current_union);
            else if (set_op == 2) { invert_class(&current_union); intersect_classes(cls, &current_union); }
            else *cls = current_union;
            
            memset(&current_union, 0, sizeof(CharClass));
            set_op = 1;
            lexer->pos += 2;
            continue;
        }
        if (lexer->prog->unicode_sets && lexer->src[lexer->pos] == '-' && lexer->src[lexer->pos+1] == '-') {
            if (lexer->prog->ignore_case) apply_case_folding(&current_union, lexer->prog->unicode);
            if (set_op == 1) intersect_classes(cls, &current_union);
            else if (set_op == 2) { invert_class(&current_union); intersect_classes(cls, &current_union); }
            else *cls = current_union;
            
            memset(&current_union, 0, sizeof(CharClass));
            set_op = 2;
            lexer->pos += 2;
            continue;
        }
        
        if (lexer->prog->unicode_sets && lexer->src[lexer->pos] == '[') {
            lexer->pos++;
            CharClass nested = {0};
            parse_char_class(lexer, &nested);
            if (nested.negated) invert_class(&nested);
            for (int i = 0; i < nested.range_count; i++) add_range(&current_union, nested.ranges[i].start, nested.ranges[i].end);
        } else {
            uint32_t start_char = decode_utf16_lexer(lexer);
            bool is_special = false;
            
            if (start_char == '\\') {
                uint32_t esc = decode_utf16_lexer(lexer);
                if (esc < 128 && strchr("dDwWsS", (char)esc)) { fill_builtin_class(&current_union, (char)esc, lexer->prog->unicode); is_special = true; }
                else if (esc == 'x') {
                    if (!parse_hex(lexer, 2, &start_char)) {
                        lexer->prog->error = "SyntaxError: Invalid hexadecimal escape sequence";
                    }
                } else if (lexer->prog->unicode_sets && esc == 'q') {
                    if (negate) {
                        lexer->prog->error = "SyntaxError: Character class with strings cannot be negated";
                        return;
                    }
                    if (lexer->src[lexer->pos] != '{') {
                        lexer->prog->error = "SyntaxError: \\q escape must be followed by {";
                        return;
                    }
                    lexer->pos++; // consume '{'
                    is_special = true;

                    while (lexer->src[lexer->pos] != '}' && lexer->src[lexer->pos] != '\0') {
                        if (lexer->prog->error) return;
                        StringSequence seq = {0};
                        while (lexer->src[lexer->pos] != '|' && lexer->src[lexer->pos] != '}' && lexer->src[lexer->pos] != '\0') {
                            uint32_t cp = decode_utf16_lexer(lexer);
                            if (cp == '\\') {
                                uint32_t esc_cp = decode_utf16_lexer(lexer);
                                switch (esc_cp) {
                                    case 'n': cp = '\n'; break; case 't': cp = '\t'; break;
                                    case 'r': cp = '\r'; break; case 'b': cp = '\b'; break;
                                    case 'f': cp = '\f'; break; case 'v': cp = '\v'; break;
                                    case 'c': {
                                        uint32_t ctrl = lexer->src[lexer->pos];
                                        if ((ctrl >= 'a' && ctrl <= 'z') || (ctrl >= 'A' && ctrl <= 'Z')) {
                                            cp = ctrl % 32; lexer->pos++;
                                        } else { lexer->prog->error = "SyntaxError: Invalid control escape in \\q{}"; return; }
                                        break;
                                    }
                                    case 'x':
                                        if (!parse_hex(lexer, 2, &cp)) { lexer->prog->error = "SyntaxError: Invalid hexadecimal escape sequence in \\q{}"; return; }
                                        break;
                                    case 'u':
                                        if (lexer->prog->unicode && lexer->src[lexer->pos] == '{') {
                                            if (!parse_braced_hex(lexer, &cp)) return;
                                        } else {
                                            if (!parse_hex(lexer, 4, &cp)) {
                                                if (lexer->prog->unicode) { lexer->prog->error = "SyntaxError: Invalid Unicode escape sequence in \\q{}"; return; }
                                                else cp = 'u';
                                            }
                                        }
                                        break;
                                    default: cp = esc_cp; break; // IdentityEscape
                                }
                            }
                            if (seq.length >= 16) { lexer->prog->error = "InternalError: String in character class too long"; return; }
                            seq.cps[seq.length++] = cp;
                        }
                        if (current_union.string_count >= 128) { lexer->prog->error = "InternalError: Too many strings in character class"; return; }
                        current_union.strings[current_union.string_count++] = seq;
                        if (lexer->src[lexer->pos] == '|') lexer->pos++;
                    }

                    if (lexer->src[lexer->pos] != '}') {
                        lexer->prog->error = "SyntaxError: Unterminated \\q{} escape";
                        return;
                    }
                    lexer->pos++; // consume '}'
                }
                else if (esc == 'p' || esc == 'P') {
                    if (lexer->prog->unicode) {
                        if (lexer->src[lexer->pos] == '{') {
                            lexer->pos++;
                            char prop[64] = {0};
                            int prop_len = 0;
                            char* val = prop;
                            while (lexer->src[lexer->pos] != '}' && lexer->src[lexer->pos] != '\0' && prop_len < 63) {
                                char c = (char)lexer->src[lexer->pos++];
                                prop[prop_len++] = c;
                                if (c == '=') val = prop + prop_len;
                            }
                            if (lexer->src[lexer->pos] == '}') lexer->pos++;
                            else lexer->prog->error = "SyntaxError: Unterminated Unicode property escape";
                            
                            if (!fill_unicode_property(&current_union, val, esc == 'P')) {
                                lexer->prog->error = "SyntaxError: Invalid property name";
                                return;
                            }
                            is_special = true;
                        } else {
                            lexer->prog->error = "SyntaxError: Invalid property escape";
                        }
                    } else {
                        start_char = esc;
                    }
                }
                else if (esc == 'u') {
                    if (lexer->prog->unicode && lexer->src[lexer->pos] == '{') {
                        if (!parse_braced_hex(lexer, &start_char)) return;
                    } else {
                        if (!parse_hex(lexer, 4, &start_char)) {
                            if (lexer->prog->unicode) {
                                lexer->prog->error = "SyntaxError: Invalid Unicode escape sequence";
                            } else {
                                start_char = 'u';
                            }
                        } else if (lexer->prog->unicode && start_char >= 0xD800 && start_char <= 0xDBFF &&
                                   lexer->src[lexer->pos] == '\\' && lexer->src[lexer->pos+1] == 'u') {
                            int saved_pos = lexer->pos;
                            lexer->pos += 2;
                            uint32_t trail;
                            if (parse_hex(lexer, 4, &trail) && trail >= 0xDC00 && trail <= 0xDFFF) {
                                start_char = ((start_char - 0xD800) << 10) + (trail - 0xDC00) + 0x10000;
                            } else {
                                lexer->pos = saved_pos;
                            }
                        }
                    }
                }
                else if (esc == 'c') {
                    uint32_t ctrl = lexer->src[lexer->pos];
                    if ((ctrl >= 'a' && ctrl <= 'z') || (ctrl >= 'A' && ctrl <= 'Z')) {
                        start_char = ctrl % 32;
                        lexer->pos++;
                    } else {
                        lexer->prog->error = "SyntaxError: Invalid control escape";
                    }
                }
                else if (esc == 'b') start_char = '\b';
                else if (esc == 'n') start_char = '\n';
                else if (esc == 't') start_char = '\t';
                else if (esc == 'r') start_char = '\r';
                else if (esc == '0') start_char = '\0';
                else {
                    if (lexer->prog->unicode && (esc >= 128 || !strchr("^$\\.*+?()[]{}|/-", (char)esc))) {
                        lexer->prog->error = "SyntaxError: Invalid identity escape";
                    }
                    start_char = esc;
                }
            }
            
            /* In unicode mode, a class escape (\d, \w, etc.) cannot be a range endpoint */
            if (is_special && lexer->prog->unicode &&
                lexer->src[lexer->pos] == '-' && lexer->src[lexer->pos+1] != ']' &&
                lexer->src[lexer->pos+1] != '&' && lexer->src[lexer->pos+1] != '-') {
                lexer->prog->error = "SyntaxError: Invalid character class range";
                return;
            }

            if (!is_special) {
                if (lexer->src[lexer->pos] == '-' && lexer->src[lexer->pos+1] != ']' && lexer->src[lexer->pos+1] != '&' && lexer->src[lexer->pos+1] != '-') {
                    lexer->pos++;
                    uint32_t end_char = decode_utf16_lexer(lexer);
                    if (end_char == '\\') {
                        uint32_t esc = decode_utf16_lexer(lexer);
                        /* Class escapes (\d \w \s etc.) cannot be range endpoints in unicode mode */
                        if (lexer->prog->unicode && esc < 128 && strchr("dDwWsS", (char)esc)) {
                            lexer->prog->error = "SyntaxError: Invalid character class range";
                            return;
                        }
                        if (esc == 'x') {
                            if (!parse_hex(lexer, 2, &end_char)) {
                                lexer->prog->error = "SyntaxError: Invalid hexadecimal escape sequence";
                            }
                        }
                        else if (esc == 'u') {
                            if (lexer->prog->unicode && lexer->src[lexer->pos] == '{') {
                                if (!parse_braced_hex(lexer, &end_char)) return;
                            } else {
                                if (!parse_hex(lexer, 4, &end_char)) {
                                    if (lexer->prog->unicode) {
                                        lexer->prog->error = "SyntaxError: Invalid Unicode escape sequence";
                                    } else {
                                        end_char = 'u';
                                    }
                                } else if (lexer->prog->unicode && end_char >= 0xD800 && end_char <= 0xDBFF &&
                                           lexer->src[lexer->pos] == '\\' && lexer->src[lexer->pos+1] == 'u') {
                                    int saved_pos = lexer->pos;
                                    lexer->pos += 2;
                                    uint32_t trail;
                                    if (parse_hex(lexer, 4, &trail) && trail >= 0xDC00 && trail <= 0xDFFF) {
                                        end_char = ((end_char - 0xD800) << 10) + (trail - 0xDC00) + 0x10000;
                                    } else {
                                        lexer->pos = saved_pos;
                                    }
                                }
                            }
                        }
                        else if (esc == 'c') {
                            uint32_t ctrl = lexer->src[lexer->pos];
                            if ((ctrl >= 'a' && ctrl <= 'z') || (ctrl >= 'A' && ctrl <= 'Z')) {
                                end_char = ctrl % 32;
                                lexer->pos++;
                            } else {
                                lexer->prog->error = "SyntaxError: Invalid control escape";
                            }
                        }
                        else if (esc == 'b') end_char = '\b';
                        else if (esc == 'n') end_char = '\n';
                        else if (esc == 't') end_char = '\t';
                        else if (esc == 'r') end_char = '\r';
                        else if (esc == '0') end_char = '\0';
                        else {
                            if (lexer->prog->unicode && (esc >= 128 || !strchr("^$\\.*+?()[]{}|/-", (char)esc))) {
                                lexer->prog->error = "SyntaxError: Invalid identity escape";
                            }
                            end_char = esc;
                        }
                    }
                    if (start_char > end_char) {
                        lexer->prog->error = "SyntaxError: Range out of order in character class";
                        break;
                    }
                    add_range(&current_union, start_char, end_char);
                } else {
                    add_range(&current_union, start_char, start_char);
                }
            }
        }
    }
    
    if (lexer->prog->ignore_case) apply_case_folding(&current_union, lexer->prog->unicode);
    
    if (set_op == 1) intersect_classes(cls, &current_union);
    else if (set_op == 2) { invert_class(&current_union); intersect_classes(cls, &current_union); }
    else *cls = current_union;
    
    if (negate && cls->string_count > 0) {
        lexer->prog->error = "SyntaxError: Character class with strings cannot be negated";
        return;
    }

    cls->negated = negate;
    if (lexer->src[lexer->pos] == ']') {
        lexer->pos++;
    } else {
        lexer->prog->error = "SyntaxError: Unterminated character class";
    }
}

static void next_token(Lexer* lexer) {
    if (lexer->prog->error) {
        lexer->current = (Token){TOK_EOF, 0, 0, 0, 0, ""};
        return;
    }
    uint32_t c = decode_utf16_lexer(lexer);
    if (c == '\0') { lexer->current = (Token){TOK_EOF, 0, 0, 0, 0, ""}; lexer->pos--; return; }
    
    switch (c) {
        case '*': lexer->current = (Token){TOK_STAR, 0, 0, 0, 0, ""}; break;
        case '+': lexer->current = (Token){TOK_PLUS, 0, 0, 0, 0, ""}; break;
        case '?': lexer->current = (Token){TOK_QUESTION, 0, 0, 0, 0, ""}; break;
        case '|': lexer->current = (Token){TOK_OR, 0, 0, 0, 0, ""}; break;
        case ')': lexer->current = (Token){TOK_RPAREN, 0, 0, 0, 0, ""}; break;
        case '^': lexer->current = (Token){TOK_CARET, 0, 0, 0, 0, ""}; break;
        case '$': lexer->current = (Token){TOK_DOLLAR, 0, 0, 0, 0, ""}; break;
        case '(': 
            if (lexer->src[lexer->pos] == '?' && lexer->src[lexer->pos+1] == '=') {
                lexer->pos += 2;
                lexer->current = (Token){TOK_LOOKAHEAD, 0, 0, 0, 0, ""};
            } else if (lexer->src[lexer->pos] == '?' && lexer->src[lexer->pos+1] == '!') {
                lexer->pos += 2;
                lexer->current = (Token){TOK_NEG_LOOKAHEAD, 0, 0, 0, 0, ""};
            } else if (lexer->src[lexer->pos] == '?' && lexer->src[lexer->pos+1] == ':') {
                lexer->pos += 2;
                lexer->current = (Token){TOK_NONCAP_GROUP, 0, 0, 0, 0, ""};
            } else if (lexer->src[lexer->pos] == '?' && lexer->src[lexer->pos+1] == '<') {
                if (lexer->src[lexer->pos+2] == '=') {
                    lexer->pos += 3;
                    lexer->current = (Token){TOK_LOOKBEHIND, 0, 0, 0, 0, ""};
                } else if (lexer->src[lexer->pos+2] == '!') {
                    lexer->pos += 3;
                    lexer->current = (Token){TOK_NEG_LOOKBEHIND, 0, 0, 0, 0, ""};
                } else {
                lexer->pos += 2; /* consume '?<' */
                char name[32] = {0};
                if (!parse_group_name(lexer, name)) return;
                Token t = {TOK_NAMED_GROUP, 0, 0, 0, 0, ""};
                strcpy(t.name, name);
                lexer->current = t;
                }
            } else if (lexer->src[lexer->pos] == '?') {
                int p = lexer->pos + 1;
                int flags_on = 0, flags_off = 0;
                bool negative = false, has_flags = false;
                while (true) {
                    char group_ch = lexer->src[p];
                    if (group_ch == '-') {
                        if (negative) { lexer->prog->error = "SyntaxError: Invalid group"; return; }
                        negative = true; p++; continue;
                    }
                    int flag = 0;
                    if (group_ch == 'i') flag = REGEX_FLAG_IGNORECASE;
                    else if (group_ch == 'm') flag = REGEX_FLAG_MULTILINE;
                    else if (group_ch == 's') flag = REGEX_FLAG_DOTALL;
                    else break;
                    
                    has_flags = true;
                    if (negative) {
                        if (flags_on & flag) { lexer->prog->error = "SyntaxError: Invalid group"; return; }
                        flags_off |= flag;
                    } else {
                        if (flags_off & flag) { lexer->prog->error = "SyntaxError: Invalid group"; return; }
                        flags_on |= flag;
                    }
                    p++;
                }
                
                if (has_flags && lexer->src[p] == ':') {
                    lexer->pos = p + 1;
                    Token t = {TOK_MODIFIER_GROUP};
                    t.flags_on = flags_on;
                    t.flags_off = flags_off;
                    lexer->current = t;
                } else {
                    lexer->prog->error = "SyntaxError: Invalid group";
                }
            } else {
                lexer->current = (Token){TOK_LPAREN, 0, 0, 0, 0, ""};
            }
            break;
        case '.': {
            int cid = lexer->prog->class_count++;
            memset(&lexer->prog->classes[cid], 0, sizeof(CharClass)); /* slots are built in place */
            uint32_t max_cp = lexer->prog->unicode ? 0x10FFFF : 255;
            if (!lexer->prog->dot_all) {
                add_range(&lexer->prog->classes[cid], 0, '\n' - 1);
                add_range(&lexer->prog->classes[cid], '\n' + 1, '\r' - 1);
                if (lexer->prog->unicode) {
                    add_range(&lexer->prog->classes[cid], '\r' + 1, 0x2027);
                    add_range(&lexer->prog->classes[cid], 0x202A, max_cp);
                } else {
                    add_range(&lexer->prog->classes[cid], '\r' + 1, max_cp);
                }
            } else {
                add_range(&lexer->prog->classes[cid], 0, max_cp);
            }
            lexer->current = (Token){TOK_CLASS, 0, cid};
            break;
        }
        case '\\': {
            uint32_t esc = decode_utf16_lexer(lexer);
            if (esc < 128 && strchr("dDwWsS", (char)esc)) {
                int cid = lexer->prog->class_count++;
            memset(&lexer->prog->classes[cid], 0, sizeof(CharClass)); /* slots are built in place */
                fill_builtin_class(&lexer->prog->classes[cid], (char)esc, lexer->prog->unicode);
                lexer->current = (Token){TOK_CLASS, 0, cid};
            } 
            else if (esc >= '1' && esc <= '9') {
                if (lexer->prog->unicode && (esc == '8' || esc == '9')) {
                    lexer->prog->error = "SyntaxError: Invalid escape sequence in unicode mode";
                } else {
                    int backref_id = esc - '0';
                    while (is_digit_char(lexer->src[lexer->pos])) {
                        backref_id = backref_id * 10 + (lexer->src[lexer->pos] - '0');
                        lexer->pos++;
                    }
                    lexer->current = (Token){TOK_BACKREF, 0, backref_id};
                }
            }
            else if (esc == 'k') {
                /* \k is a named backreference iff the pattern contains a named
                 * group, or the u/v flag is set. Otherwise (Annex B) it is the
                 * literal character 'k'. */
                if (lexer->has_named_groups || lexer->prog->unicode) {
                    if (lexer->src[lexer->pos] != '<') {
                        lexer->prog->error = "SyntaxError: Invalid named capture referenced";
                        return;
                    }
                    lexer->pos++; /* consume '<' */
                    char name[32] = {0};
                    if (!parse_group_name(lexer, name)) return;
                    Token t = {TOK_NAMED_BACKREF};
                    strcpy(t.name, name);
                    lexer->current = t;
                } else {
                    lexer->current = (Token){TOK_LITERAL, 'k'};
                }
            }
            else if (esc == 'b') {
                lexer->current = (Token){TOK_WORD_BOUNDARY};
            }
            else if (esc == 'B') {
                lexer->current = (Token){TOK_NON_WORD_BOUNDARY};
            }
            else if (esc == 'x') {
                uint32_t val;
                if (!parse_hex(lexer, 2, &val)) {
                    lexer->prog->error = "SyntaxError: Invalid hexadecimal escape sequence";
                } else lexer->current = (Token){TOK_LITERAL, val};
            }
            else if (esc == 'c') {
                uint32_t ctrl = lexer->src[lexer->pos];
                if ((ctrl >= 'a' && ctrl <= 'z') || (ctrl >= 'A' && ctrl <= 'Z')) {
                    lexer->current = (Token){TOK_LITERAL, ctrl % 32};
                    lexer->pos++;
                } else {
                    lexer->prog->error = "SyntaxError: Invalid control escape";
                }
            }
            else if (esc == 'p' || esc == 'P') {
                if (lexer->prog->unicode) {
                    if (lexer->src[lexer->pos] == '{') {
                        lexer->pos++;
                        char prop[64] = {0};
                        int prop_len = 0;
                        char* val = prop;
                        while (lexer->src[lexer->pos] != '}' && lexer->src[lexer->pos] != '\0' && prop_len < 63) {
                            char prop_ch = (char)lexer->src[lexer->pos++];
                            prop[prop_len++] = prop_ch;
                            if (prop_ch == '=') val = prop + prop_len;
                        }
                        if (lexer->src[lexer->pos] == '}') lexer->pos++;
                        else lexer->prog->error = "SyntaxError: Unterminated Unicode property escape";
                        
                        int cid = lexer->prog->class_count++;
            memset(&lexer->prog->classes[cid], 0, sizeof(CharClass)); /* slots are built in place */
                        if (!fill_unicode_property(&lexer->prog->classes[cid], val, esc == 'P')) {
                            lexer->prog->error = "SyntaxError: Invalid property name";
                            return;
                        }
                        lexer->current = (Token){TOK_CLASS, 0, cid};
                    } else {
                        lexer->prog->error = "SyntaxError: Invalid property escape";
                    }
                } else {
                    lexer->current = (Token){TOK_LITERAL, esc, 0, 0, 0, ""};
                }
            }
            else if (esc == 'u') {
                if (lexer->prog->unicode && lexer->src[lexer->pos] == '{') {
                    uint32_t cp;
                    if (!parse_braced_hex(lexer, &cp)) return;
                    lexer->current = (Token){TOK_LITERAL, cp};
                } else {
                    uint32_t val;
                    if (parse_hex(lexer, 4, &val)) {
                        /* In unicode mode, \uLead\uTrail forms a surrogate pair */
                        if (lexer->prog->unicode && val >= 0xD800 && val <= 0xDBFF &&
                            lexer->src[lexer->pos] == '\\' && lexer->src[lexer->pos+1] == 'u') {
                            int saved_pos = lexer->pos;
                            lexer->pos += 2;
                            uint32_t trail;
                            if (parse_hex(lexer, 4, &trail) && trail >= 0xDC00 && trail <= 0xDFFF) {
                                val = ((val - 0xD800) << 10) + (trail - 0xDC00) + 0x10000;
                            } else {
                                lexer->pos = saved_pos;
                            }
                        }
                        lexer->current = (Token){TOK_LITERAL, val};
                    } else {
                        if (lexer->prog->unicode) {
                            lexer->prog->error = "SyntaxError: Invalid Unicode escape sequence";
                        } else {
                            lexer->current = (Token){TOK_LITERAL, 'u'};
                        }
                    }
                }
            }
            else if (esc == 'n') lexer->current = (Token){TOK_LITERAL, '\n'};
            else if (esc == 't') lexer->current = (Token){TOK_LITERAL, '\t'};
            else if (esc == 'r') lexer->current = (Token){TOK_LITERAL, '\r'};
            else if (esc == '0') lexer->current = (Token){TOK_LITERAL, '\0'};
            else {
                if (lexer->prog->unicode && (esc >= 128 || !strchr("^$\\.*+?()[]{}|/", (char)esc))) {
                    lexer->prog->error = "SyntaxError: Invalid identity escape";
                }
                lexer->current = (Token){TOK_LITERAL, esc};
            }
            break;
        }
        case '[': {
            int cid = lexer->prog->class_count++;
            memset(&lexer->prog->classes[cid], 0, sizeof(CharClass)); /* slots are built in place */
            parse_char_class(lexer, &lexer->prog->classes[cid]);
            lexer->current = (Token){TOK_CLASS, 0, cid};
            break;
        }
        case '{': { 
            int start_pos = lexer->pos;
            int min = 0, max = -1;
            bool has_min = false, has_comma = false, has_max = false;
            
            while (lexer->src[lexer->pos] >= '0' && lexer->src[lexer->pos] <= '9') {
                min = min * 10 + (lexer->src[lexer->pos] - '0');
                has_min = true; lexer->pos++;
            }
            if (lexer->src[lexer->pos] == ',') {
                has_comma = true; lexer->pos++;
                while (lexer->src[lexer->pos] >= '0' && lexer->src[lexer->pos] <= '9') {
                    if (max == -1) max = 0;
                    max = max * 10 + (lexer->src[lexer->pos] - '0');
                    has_max = true; lexer->pos++;
                }
            } else if (has_min) max = min;
            
            if (has_min && lexer->src[lexer->pos] == '}') {
                lexer->pos++;
                lexer->current = (Token){TOK_BOUNDS, 0, 0, min, (has_comma && !has_max) ? -1 : max};
                break;
            }
            lexer->pos = start_pos;
            if (lexer->prog->unicode) {
                lexer->prog->error = "SyntaxError: Incomplete quantifier or unescaped brace in unicode mode";
            } else {
                lexer->current = (Token){TOK_LITERAL, '{'};
            }
            break;
        }
        default: lexer->current = (Token){TOK_LITERAL, c}; break;
    }
}

/* ==========================================================================
 * 3. PARSER (AST Builder)
 * ========================================================================== */
typedef enum { AST_LITERAL, AST_CLASS, AST_CONCAT, AST_ALT, AST_GROUP, AST_LOOKAHEAD, AST_NEG_LOOKAHEAD, AST_QUANTIFIER, AST_ASSERT_START, AST_ASSERT_END, AST_BACKREF, AST_NAMED_BACKREF, AST_WORD_BOUNDARY, AST_NON_WORD_BOUNDARY, AST_LOOKBEHIND, AST_NEG_LOOKBEHIND, AST_MODIFIER_GROUP } ASTType;

typedef struct ASTNode {
    ASTType type; uint32_t ch; int id; int min; int max; bool lazy; 
    char name[32];
    int flags_on; int flags_off;
    struct ASTNode* left; struct ASTNode* right;
} ASTNode;

static ASTNode* create_node(ASTType type) {
    ASTNode* node = calloc(1, sizeof(ASTNode));
    if (!node) {
        fprintf(stderr, "Fatal Error: Out of memory\n");
        exit(EXIT_FAILURE);
    }
    node->type = type;
    return node;
}

ASTNode* parse_alt(Lexer* lexer);

static ASTNode* parse_primary(Lexer* lexer) {
    ASTNode* node = NULL;
    if (lexer->current.type == TOK_LITERAL) { node = create_node(AST_LITERAL); node->ch = lexer->current.ch; next_token(lexer); } 
    else if (lexer->current.type == TOK_CLASS) { node = create_node(AST_CLASS); node->id = lexer->current.class_id; next_token(lexer); } 
    else if (lexer->current.type == TOK_CARET) { node = create_node(AST_ASSERT_START); next_token(lexer); } 
    else if (lexer->current.type == TOK_DOLLAR) { node = create_node(AST_ASSERT_END); next_token(lexer); } 
    else if (lexer->current.type == TOK_BACKREF) { node = create_node(AST_BACKREF); node->id = lexer->current.class_id; next_token(lexer); } 
    else if (lexer->current.type == TOK_NAMED_BACKREF) { node = create_node(AST_NAMED_BACKREF); strcpy(node->name, lexer->current.name); next_token(lexer); } 
    else if (lexer->current.type == TOK_WORD_BOUNDARY) { node = create_node(AST_WORD_BOUNDARY); next_token(lexer); } 
    else if (lexer->current.type == TOK_NON_WORD_BOUNDARY) { node = create_node(AST_NON_WORD_BOUNDARY); next_token(lexer); } 
    else if (lexer->current.type == TOK_LPAREN || lexer->current.type == TOK_LOOKAHEAD || lexer->current.type == TOK_NEG_LOOKAHEAD || lexer->current.type == TOK_LOOKBEHIND || lexer->current.type == TOK_NEG_LOOKBEHIND || lexer->current.type == TOK_NONCAP_GROUP || lexer->current.type == TOK_NAMED_GROUP || lexer->current.type == TOK_MODIFIER_GROUP) {
        bool is_la = (lexer->current.type == TOK_LOOKAHEAD);
        bool is_neg_la = (lexer->current.type == TOK_NEG_LOOKAHEAD);
        bool is_lb = (lexer->current.type == TOK_LOOKBEHIND);
        bool is_neg_lb = (lexer->current.type == TOK_NEG_LOOKBEHIND);
        bool is_noncap = (lexer->current.type == TOK_NONCAP_GROUP);
        bool is_named = (lexer->current.type == TOK_NAMED_GROUP);
        bool is_modifier = (lexer->current.type == TOK_MODIFIER_GROUP);
        char name[32] = {0};
        int flags_on = 0, flags_off = 0;
        if (is_named) strcpy(name, lexer->current.name);
        if (is_modifier) {
            flags_on = lexer->current.flags_on;
            flags_off = lexer->current.flags_off;
        }
        
        next_token(lexer);
        ASTNode* inner = parse_alt(lexer);
        if (lexer->current.type == TOK_RPAREN) {
            next_token(lexer);
        } else {
            lexer->prog->error = "SyntaxError: Unterminated group";
        }
        
        if (is_modifier) {
            node = create_node(AST_MODIFIER_GROUP);
            node->flags_on = flags_on;
            node->flags_off = flags_off;
        } else {
            node = create_node(is_la ? AST_LOOKAHEAD : (is_neg_la ? AST_NEG_LOOKAHEAD : (is_lb ? AST_LOOKBEHIND : (is_neg_lb ? AST_NEG_LOOKBEHIND : AST_GROUP))));
        }
        node->left = inner;
        if (!is_la && !is_neg_la && !is_lb && !is_neg_lb && !is_noncap && !is_modifier) {
            node->id = ++lexer->prog->group_count; 
            if (is_named) strcpy(lexer->prog->group_names[node->id], name);
            if (is_named) strcpy(node->name, name);
        }
    }
    return node;
}

static ASTNode* parse_quantifier(Lexer* lexer) {
    ASTNode* node = parse_primary(lexer);
    if (!node) return NULL;
    TokenType t = lexer->current.type;
    if (t == TOK_STAR || t == TOK_PLUS || t == TOK_QUESTION || t == TOK_BOUNDS) {
        /* Lookahead/lookbehind assertions cannot be quantified */
        if (node->type == AST_LOOKAHEAD || node->type == AST_NEG_LOOKAHEAD ||
            node->type == AST_LOOKBEHIND || node->type == AST_NEG_LOOKBEHIND) {
            lexer->prog->error = "SyntaxError: Invalid quantifier applied to assertion";
        }
        ASTNode* q = create_node(AST_QUANTIFIER);
        q->left = node;
        if (t == TOK_STAR)      { q->min = 0; q->max = -1; }
        else if (t == TOK_PLUS) { q->min = 1; q->max = -1; }
        else if (t == TOK_QUESTION){ q->min = 0; q->max = 1; }
        else if (t == TOK_BOUNDS)  { q->min = lexer->current.min; q->max = lexer->current.max; }
        next_token(lexer);
        if (lexer->current.type == TOK_QUESTION) { q->lazy = true; next_token(lexer); }
        node = q;
    }
    return node;
}

static ASTNode* parse_concat(Lexer* lexer) {
    ASTNode* node = parse_quantifier(lexer);
    while (lexer->current.type == TOK_LITERAL || lexer->current.type == TOK_CLASS || 
           lexer->current.type == TOK_LPAREN || lexer->current.type == TOK_LOOKAHEAD ||
           lexer->current.type == TOK_NEG_LOOKAHEAD ||
           lexer->current.type == TOK_LOOKBEHIND ||
           lexer->current.type == TOK_NEG_LOOKBEHIND ||
           lexer->current.type == TOK_CARET || lexer->current.type == TOK_DOLLAR || lexer->current.type == TOK_MODIFIER_GROUP ||
           lexer->current.type == TOK_BACKREF || 
           lexer->current.type == TOK_NAMED_BACKREF ||
           lexer->current.type == TOK_NONCAP_GROUP ||
           lexer->current.type == TOK_NAMED_GROUP ||
           lexer->current.type == TOK_WORD_BOUNDARY ||
           lexer->current.type == TOK_NON_WORD_BOUNDARY) {
        ASTNode* right = parse_quantifier(lexer);
        ASTNode* concat = create_node(AST_CONCAT);
        concat->left = node; concat->right = right;
        node = concat;
    }
    return node;
}

ASTNode* parse_alt(Lexer* lexer) {
    ASTNode* node = parse_concat(lexer);
    if (lexer->current.type == TOK_OR) {
        next_token(lexer);
        ASTNode* right = parse_alt(lexer);
        ASTNode* alt = create_node(AST_ALT);
        alt->left = node; alt->right = right;
        node = alt;
    }
    return node;
}

static void free_ast(ASTNode* node) {
    if (!node) return;
    free_ast(node->left); free_ast(node->right); free(node);
}

typedef struct {
    char names[MAX_GROUPS][32];
    int count;
} NameSet;

static bool validate_group_names(ASTNode* node, NameSet* out_set, const char** error) {
    if (!node) return true;
    if (node->type == AST_ALT) {
        NameSet left_set = {0}, right_set = {0};
        if (!validate_group_names(node->left, &left_set, error)) return false;
        if (!validate_group_names(node->right, &right_set, error)) return false;
        
        for (int i = 0; i < left_set.count; i++) {
            strcpy(out_set->names[out_set->count++], left_set.names[i]);
        }
        for (int i = 0; i < right_set.count; i++) {
            bool found = false;
            for (int j = 0; j < left_set.count; j++) {
                if (strcmp(right_set.names[i], left_set.names[j]) == 0) { found = true; break; }
            }
            if (!found && out_set->count < MAX_GROUPS) strcpy(out_set->names[out_set->count++], right_set.names[i]);
        }
        return true;
    } else {
        NameSet left_set = {0}, right_set = {0};
        if (!validate_group_names(node->left, &left_set, error)) return false;
        if (!validate_group_names(node->right, &right_set, error)) return false;
        
        for (int i = 0; i < left_set.count; i++) {
            strcpy(out_set->names[out_set->count++], left_set.names[i]);
        }
        for (int i = 0; i < right_set.count; i++) {
            for (int j = 0; j < left_set.count; j++) {
                if (strcmp(right_set.names[i], left_set.names[j]) == 0) {
                    *error = "SyntaxError: Duplicate capture group name";
                    return false;
                }
            }
            if (out_set->count < MAX_GROUPS) strcpy(out_set->names[out_set->count++], right_set.names[i]);
        }
        
        if (node->type == AST_GROUP && node->name[0] != '\0') {
            for (int i = 0; i < out_set->count; i++) {
                if (strcmp(out_set->names[i], node->name) == 0) {
                    *error = "SyntaxError: Duplicate capture group name";
                    return false;
                }
            }
            if (out_set->count < MAX_GROUPS) strcpy(out_set->names[out_set->count++], node->name);
        }
        return true;
    }
}

/* ==========================================================================
 * 4. COMPILER (Bytecode Generator)
 * ========================================================================== */
static int emit(Program* prog, RegexOpCode op, int arg1, int arg2, int arg3, int arg4, bool lazy) {
    int idx = prog->code_count++;
    prog->code[idx] = (Instruction){op, arg1, arg2, arg3, arg4, lazy};
    return idx;
}

/* A character class containing multi-codepoint string alternatives (from
 * \q{...} or a Unicode "property of strings" like \p{RGI_Emoji_Flag_Sequence})
 * can't be matched by a single OP_CLASS instruction, which only ever tests
 * one code point against cls->ranges. Compile it instead as an alternation
 * -- each string in declaration order, then (if the class also has ordinary
 * single-codepoint ranges) a final OP_CLASS fallback -- reusing the VM's
 * existing, already-correct OP_SPLIT/backtracking machinery rather than
 * teaching OP_CLASS a second matching mode. Preferring strings before the
 * single-codepoint fallback mirrors how the Unicode property data itself is
 * structured (ranges and sequences are disjoint code point sets in every
 * property this engine generates); rtl mirrors AST_CONCAT's lookbehind
 * handling by emitting each string's code points in reverse. */
static void compile_class_with_strings(Program* prog, int class_id, bool rtl) {
    CharClass* cls = &prog->classes[class_id];
    int jmp_pcs[128];
    int jmp_count = 0;
    for (int i = 0; i < cls->string_count; i++) {
        bool has_more = (i < cls->string_count - 1) || (cls->range_count > 0);
        int split = has_more ? emit(prog, OP_SPLIT, 0, 0, 0, 0, false) : -1;
        if (split != -1) prog->code[split].arg1 = prog->code_count;
        StringSequence* seq = &cls->strings[i];
        if (rtl) {
            for (int k = seq->length - 1; k >= 0; k--) emit(prog, OP_CHAR, seq->cps[k], 0, 0, 0, false);
        } else {
            for (int k = 0; k < seq->length; k++) emit(prog, OP_CHAR, seq->cps[k], 0, 0, 0, false);
        }
        if (has_more) {
            jmp_pcs[jmp_count++] = emit(prog, OP_JMP, 0, 0, 0, 0, false);
            prog->code[split].arg2 = prog->code_count;
        }
    }
    if (cls->range_count > 0) emit(prog, OP_CLASS, class_id, 0, 0, 0, false);
    int end_pc = prog->code_count;
    for (int i = 0; i < jmp_count; i++) prog->code[jmp_pcs[i]].arg1 = end_pc;
}

static void compile_node(ASTNode* node, Program* prog, bool rtl) {
    if (!node) return;
    switch (node->type) {
        case AST_ASSERT_START: emit(prog, OP_ASSERT_START, 0, 0, 0, 0, false); break;
        case AST_ASSERT_END:   emit(prog, OP_ASSERT_END, 0, 0, 0, 0, false); break;
        case AST_WORD_BOUNDARY:emit(prog, OP_WORD_BOUNDARY, 0, 0, 0, 0, false); break;
        case AST_NON_WORD_BOUNDARY:emit(prog, OP_NON_WORD_BOUNDARY, 0, 0, 0, 0, false); break;
        case AST_BACKREF:      emit(prog, OP_BACKREF, node->id, 0, 0, 0, false); break;
        case AST_NAMED_BACKREF: {
            int gid = -1;
            for (int i = 1; i <= prog->group_count; i++) {
                if (strcmp(prog->group_names[i], node->name) == 0) {
                    gid = i;
                    break;
                }
            }
            if (gid != -1) emit(prog, OP_BACKREF, gid, 0, 0, 0, false);
            break;
        }
        case AST_MODIFIER_GROUP: {
            bool old_ignore_case = prog->ignore_case;
            bool old_multiline = prog->multiline;
            bool old_dot_all = prog->dot_all;

            if (node->flags_on & REGEX_FLAG_IGNORECASE) prog->ignore_case = true;
            if (node->flags_on & REGEX_FLAG_MULTILINE) prog->multiline = true;
            if (node->flags_on & REGEX_FLAG_DOTALL) prog->dot_all = true;
            if (node->flags_off & REGEX_FLAG_IGNORECASE) prog->ignore_case = false;
            if (node->flags_off & REGEX_FLAG_MULTILINE) prog->multiline = false;
            if (node->flags_off & REGEX_FLAG_DOTALL) prog->dot_all = false;

            compile_node(node->left, prog, rtl);

            prog->ignore_case = old_ignore_case; prog->multiline = old_multiline; prog->dot_all = old_dot_all;
            break;
        }
        case AST_LITERAL: emit(prog, OP_CHAR, node->ch, 0, 0, 0, false); break;
        case AST_CLASS:
            if (prog->classes[node->id].string_count > 0) compile_class_with_strings(prog, node->id, rtl);
            else emit(prog, OP_CLASS, node->id, 0, 0, 0, false);
            break;
        case AST_CONCAT:
            if (rtl) {
                compile_node(node->right, prog, rtl); compile_node(node->left, prog, rtl);
            } else {
                compile_node(node->left, prog, rtl); compile_node(node->right, prog, rtl);
            }
            break;
        case AST_GROUP:
            if (rtl) {
                if (node->id > 0) emit(prog, OP_SAVE, node->id * 2 + 1, 0, 0, 0, false);
                compile_node(node->left, prog, rtl);
                if (node->id > 0) emit(prog, OP_SAVE, node->id * 2, 0, 0, 0, false);
            } else {
                if (node->id > 0) emit(prog, OP_SAVE, node->id * 2, 0, 0, 0, false);
                compile_node(node->left, prog, rtl);
                if (node->id > 0) emit(prog, OP_SAVE, node->id * 2 + 1, 0, 0, 0, false);
            }
            break;
        case AST_LOOKAHEAD:
        case AST_NEG_LOOKAHEAD: {
            int jmp = emit(prog, OP_JMP, 0, 0, 0, 0, false);
            int la_start = prog->code_count;
            compile_node(node->left, prog, false);
            emit(prog, OP_MATCH, 0, 0, 0, 0, false); 
            prog->code[jmp].arg1 = prog->code_count; 
            emit(prog, (node->type == AST_LOOKAHEAD) ? OP_LOOKAHEAD : OP_NEG_LOOKAHEAD, la_start, 0, 0, 0, false); break;
        }
        case AST_LOOKBEHIND:
        case AST_NEG_LOOKBEHIND: {
            int jmp = emit(prog, OP_JMP, 0, 0, 0, 0, false);
            int lb_start = prog->code_count;
            compile_node(node->left, prog, true);
            emit(prog, OP_MATCH, 0, 0, 0, 0, false); 
            prog->code[jmp].arg1 = prog->code_count; 
            emit(prog, (node->type == AST_LOOKBEHIND) ? OP_LOOKBEHIND : OP_NEG_LOOKBEHIND, lb_start, 0, 0, 0, false); break;
        }
        case AST_ALT: {
            int split = emit(prog, OP_SPLIT, 0, 0, 0, 0, false);
            prog->code[split].arg1 = prog->code_count;
            compile_node(node->left, prog, rtl);
            int jmp = emit(prog, OP_JMP, 0, 0, 0, 0, false);
            prog->code[split].arg2 = prog->code_count;
            compile_node(node->right, prog, rtl);
            prog->code[jmp].arg1 = prog->code_count; break;
        }
        case AST_QUANTIFIER: {
            int counter_id = prog->counter_count++;
            emit(prog, OP_INIT_COUNTER, counter_id, 0, 0, 0, false);
            int split_pc = prog->code_count;
            int check_idx = emit(prog, OP_CHECK_COUNTER, counter_id, node->min, node->max, 0, node->lazy);
            compile_node(node->left, prog, rtl);
            emit(prog, OP_INC_COUNTER, counter_id, 0, 0, 0, false);
            emit(prog, OP_JMP, split_pc, 0, 0, 0, false);
            prog->code[check_idx].arg4 = prog->code_count; 
            break;
        }
    }
}

static void validate_backrefs(ASTNode* node, int group_count, const char** error) {
    if (!node || *error) return;
    if (node->type == AST_BACKREF && node->id > group_count) {
        *error = "SyntaxError: Invalid backreference";
        return;
    }
    validate_backrefs(node->left, group_count, error);
    validate_backrefs(node->right, group_count, error);
}

/* Every named backreference must resolve to a group with that name. */
static void validate_named_backrefs(ASTNode* node, Program* prog, const char** error) {
    if (!node || *error) return;
    if (node->type == AST_NAMED_BACKREF) {
        bool found = false;
        for (int i = 1; i <= prog->group_count; i++) {
            if (strcmp(prog->group_names[i], node->name) == 0) { found = true; break; }
        }
        if (!found) { *error = "SyntaxError: Invalid named capture referenced"; return; }
    }
    validate_named_backrefs(node->left, prog, error);
    validate_named_backrefs(node->right, prog, error);
}

/* Whether the pattern text contains a named capture group '(?<name>' (not a
 * lookbehind '(?<=' / '(?<!'). Governs whether '\k' is a named backreference.
 * Character-class contents and escaped chars are skipped. */
static bool scan_has_named_group(const uint16_t* src) {
    bool in_class = false;
    for (int i = 0; src[i] != '\0'; i++) {
        uint16_t c = src[i];
        if (c == '\\') { if (src[i + 1] != '\0') i++; continue; }
        if (in_class) { if (c == ']') in_class = false; continue; }
        if (c == '[') { in_class = true; continue; }
        if (c == '(' && src[i + 1] == '?' && src[i + 2] == '<' &&
            src[i + 3] != '=' && src[i + 3] != '!') {
            return true;
        }
    }
    return false;
}

void compile_into(Program* prog, const uint16_t* regex, int flags) {
    /* Program is ~2MB; a full `= {0}` zeroing (and return-by-value copying)
     * dominated RegExp construction cost. Only the bookkeeping needs
     * initialisation: code[]/classes[] entries are fully written before use
     * and never read past their counts; group_names IS read by name lookups
     * for unnamed groups, so it stays zeroed. */
    memset(prog->group_names, 0, sizeof(prog->group_names));
    prog->code_count = 0;
    prog->class_count = 0;
    prog->group_count = 0;
    prog->counter_count = 0;
    prog->error = NULL;
    prog->ignore_case = (flags & REGEX_FLAG_IGNORECASE) != 0;
    prog->multiline = (flags & REGEX_FLAG_MULTILINE) != 0;
    prog->dot_all = (flags & REGEX_FLAG_DOTALL) != 0;
    prog->sticky = (flags & REGEX_FLAG_STICKY) != 0;
    prog->unicode = (flags & REGEX_FLAG_UNICODE) != 0 || (flags & REGEX_FLAG_UNICODE_SETS) != 0;
    prog->unicode_sets = (flags & REGEX_FLAG_UNICODE_SETS) != 0;
    prog->has_indices = (flags & REGEX_FLAG_INDICES) != 0;
    Lexer lexer = {regex, 0, {0}, prog, false};
    lexer.has_named_groups = scan_has_named_group(regex);
    next_token(&lexer);
    ASTNode* ast = parse_alt(&lexer);

    if (!prog->error && lexer.current.type != TOK_EOF) {
        if (lexer.current.type == TOK_STAR || lexer.current.type == TOK_PLUS || lexer.current.type == TOK_QUESTION || lexer.current.type == TOK_BOUNDS) {
            prog->error = "SyntaxError: Nothing to repeat";
        } else if (lexer.current.type == TOK_RPAREN) {
            prog->error = "SyntaxError: Unmatched ')'";
        } else {
            prog->error = "SyntaxError: Trailing unparsed tokens";
        }
    }

    if (!prog->error) {
        NameSet set = {0};
        validate_group_names(ast, &set, &prog->error);
    }

    if (!prog->error) {
        validate_named_backrefs(ast, prog, &prog->error);
    }

    /* In unicode mode, backreferences to non-existent groups are early errors */
    if (!prog->error && prog->unicode) {
        validate_backrefs(ast, prog->group_count, &prog->error);
    }

    if (!prog->error) {
        emit(prog, OP_SAVE, 0, 0, 0, 0, false);
        compile_node(ast, prog, false);
        emit(prog, OP_SAVE, 1, 0, 0, 0, false);
        emit(prog, OP_MATCH, 0, 0, 0, 0, false);
    }
    free_ast(ast);
}

/* ==========================================================================
 * 5. VIRTUAL MACHINE (With ReDoS Cache & Loop Registers)
 * ========================================================================== */
typedef struct { int pc; const uint16_t* sp; } CacheEntry;
typedef struct {
    int pc; const uint16_t* sp; const uint16_t* captures[MAX_GROUPS * 2]; int counters[MAX_COUNTERS]; const uint16_t* counter_sp[MAX_COUNTERS];
} Thread;

static inline unsigned int hash_state(const Thread* t, int counter_count) {
    unsigned int h = (unsigned int)((t->pc * 31) + (size_t)t->sp);
    for (int i = 0; i < counter_count; i++) h ^= (t->counters[i] * 73); 
    return h % CACHE_SIZE;
}

bool vm_execute_internal(Program* prog, int start_pc, int step, const uint16_t* original_text, const uint16_t* text_end, const uint16_t* search_start, const uint16_t** out_captures) {
    Thread stack[512];
    int stack_ptr = 0;
    
    CacheEntry fail_cache[CACHE_SIZE];
    for (int i = 0; i < CACHE_SIZE; i++) fail_cache[i].pc = -1;
    
    Thread init_thread = {start_pc, search_start, {NULL}, {0}, {NULL}};
    if (out_captures) memcpy(init_thread.captures, out_captures, sizeof(const uint16_t*) * MAX_GROUPS * 2);
    stack[stack_ptr++] = init_thread;
    
    while (stack_ptr > 0) {
        Thread current = stack[--stack_ptr];
        
        unsigned int h = hash_state(&current, prog->counter_count);
        if (fail_cache[h].pc == current.pc && fail_cache[h].sp == current.sp) continue; 
        
        int path_start_pc = current.pc; const uint16_t* path_start_sp = current.sp; bool path_failed = false;
        
        while (true) {
            Instruction inst = prog->code[current.pc];
            if (inst.op == OP_CHAR) {
                if (step > 0) {
                    if (current.sp < text_end) {
                        uint32_t cp;
                        const uint16_t* next_sp = current.sp;
                        if (prog->unicode) cp = decode_utf16(&next_sp);
                        else cp = *next_sp++;
                        
                        if (prog->ignore_case) {
                            uint32_t inst_cp = inst.arg1;
                            uint32_t match_cp = cp;
                            if (prog->unicode) {
                                inst_cp = unicode_casefold(inst_cp);
                                match_cp = unicode_casefold(match_cp);
                            } else {
                                inst_cp = annexb_canonicalize(inst_cp);
                                match_cp = annexb_canonicalize(match_cp);
                            }
                            if (inst_cp == match_cp) { current.pc++; current.sp = next_sp; } else { path_failed = true; break; }
                        } else {
                            if (cp == (uint32_t)inst.arg1) { current.pc++; current.sp = next_sp; } else { path_failed = true; break; }
                        }
                    } else { path_failed = true; break; }
                } else {
                    if (current.sp > original_text) {
                        const uint16_t* next_sp = current.sp;
                        uint32_t cp;
                        if (prog->unicode) {
                            next_sp--; cp = *next_sp;
                            if (cp >= 0xDC00 && cp <= 0xDFFF && next_sp > original_text) {
                                uint32_t lead = *(next_sp - 1);
                                if (lead >= 0xD800 && lead <= 0xDBFF) {
                                    cp = ((lead - 0xD800) << 10) + (cp - 0xDC00) + 0x10000;
                                    next_sp--;
                                }
                            }
                        } else {
                            cp = *(--next_sp);
                        }
                        if (prog->ignore_case) {
                            uint32_t inst_cp = inst.arg1;
                            uint32_t match_cp = cp;
                            if (prog->unicode) {
                                inst_cp = unicode_casefold(inst_cp);
                                match_cp = unicode_casefold(match_cp);
                            } else {
                                inst_cp = annexb_canonicalize(inst_cp);
                                match_cp = annexb_canonicalize(match_cp);
                            }
                            if (inst_cp == match_cp) { current.pc++; current.sp = next_sp; } else { path_failed = true; break; }
                        } else {
                            if (cp == (uint32_t)inst.arg1) { current.pc++; current.sp = next_sp; } else { path_failed = true; break; }
                        }
                    } else { path_failed = true; break; }
                }
            } 
            else if (inst.op == OP_CLASS) {
                if (step > 0) {
                    if (current.sp < text_end) {
                        uint32_t cp;
                        const uint16_t* next_sp = current.sp;
                        if (prog->unicode) cp = decode_utf16(&next_sp);
                        else cp = *next_sp++;
                        bool matched = false;
                        CharClass* cls = &prog->classes[inst.arg1];
                        for (int i = 0; i < cls->range_count; i++) {
                            if (cp >= cls->ranges[i].start && cp <= cls->ranges[i].end) { matched = true; break; }
                        }
                        if (cls->negated) matched = !matched;
                        if (matched) { current.pc++; current.sp = next_sp; } else { path_failed = true; break; }
                    } else { path_failed = true; break; }
                } else {
                    if (current.sp > original_text) {
                        const uint16_t* next_sp = current.sp;
                        uint32_t cp;
                        if (prog->unicode) {
                            next_sp--; cp = *next_sp;
                            if (cp >= 0xDC00 && cp <= 0xDFFF && next_sp > original_text) {
                                uint32_t lead = *(next_sp - 1);
                                if (lead >= 0xD800 && lead <= 0xDBFF) {
                                    cp = ((lead - 0xD800) << 10) + (cp - 0xDC00) + 0x10000;
                                    next_sp--;
                                }
                            }
                        } else {
                            cp = *(--next_sp);
                        }
                        bool matched = false;
                        CharClass* cls = &prog->classes[inst.arg1];
                        for (int i = 0; i < cls->range_count; i++) {
                            if (cp >= cls->ranges[i].start && cp <= cls->ranges[i].end) { matched = true; break; }
                        }
                        if (cls->negated) matched = !matched;
                        if (matched) { current.pc++; current.sp = next_sp; } else { path_failed = true; break; }
                    } else { path_failed = true; break; }
                }
            }
            else if (inst.op == OP_SAVE) { current.captures[inst.arg1] = current.sp; current.pc++; }
            else if (inst.op == OP_ASSERT_START) { if (current.sp == original_text || (prog->multiline && current.sp > original_text && *(current.sp - 1) == '\n')) current.pc++; else { path_failed = true; break; } }
            else if (inst.op == OP_ASSERT_END)   { if (current.sp >= text_end || (prog->multiline && *current.sp == '\n')) current.pc++; else { path_failed = true; break; } }
            else if (inst.op == OP_WORD_BOUNDARY) {
                bool left_is_word = (current.sp > original_text) && is_word_char(*(current.sp - 1));
                bool right_is_word = is_word_char(*current.sp);
                if (left_is_word != right_is_word) current.pc++; else { path_failed = true; break; }
            }
            else if (inst.op == OP_NON_WORD_BOUNDARY) {
                bool left_is_word = (current.sp > original_text) && is_word_char(*(current.sp - 1));
                bool right_is_word = is_word_char(*current.sp);
                if (left_is_word == right_is_word) current.pc++; else { path_failed = true; break; }
            }
            else if (inst.op == OP_BACKREF || inst.op == OP_NAMED_BACKREF) {
                const uint16_t* start = NULL;
                const uint16_t* end = NULL;
                if (inst.op == OP_BACKREF) {
                    start = current.captures[inst.arg1 * 2];
                    end = current.captures[inst.arg1 * 2 + 1];
                } else {
                    for (int i = 1; i <= prog->group_count; i++) {
                        if (prog->group_names[i][0] && strcmp(prog->group_names[i], prog->group_names[inst.arg1]) == 0) {
                            if (current.captures[i * 2] && current.captures[i * 2 + 1]) {
                                start = current.captures[i * 2];
                                end = current.captures[i * 2 + 1];
                                break;
                            }
                        }
                    }
                }
                if (start && end) {
                    bool match = true;
                    if (prog->ignore_case) {
                        const uint16_t* temp_sp = current.sp;
                        if (step > 0) {
                            const uint16_t* temp_start = start;
                            while (temp_start < end) {
                                if (temp_sp >= text_end) { match = false; break; }
                                uint32_t cp1, cp2;
                                if (prog->unicode) {
                                    cp1 = decode_utf16(&temp_sp);
                                    cp2 = decode_utf16(&temp_start);
                                    cp1 = unicode_casefold(cp1);
                                    cp2 = unicode_casefold(cp2);
                                } else {
                                    cp1 = *temp_sp++;
                                    cp2 = *temp_start++;
                                    cp1 = annexb_canonicalize(cp1);
                                    cp2 = annexb_canonicalize(cp2);
                                }
                                if (cp1 != cp2) { match = false; break; }
                            }
                            if (match) current.sp = temp_sp;
                        } else {
                            const uint16_t* temp_end = end;
                            while (temp_end > start) {
                                if (temp_sp <= original_text) { match = false; break; }
                                uint32_t cp1, cp2;
                                if (prog->unicode) {
                                    temp_sp--; cp1 = *temp_sp;
                                    if (cp1 >= 0xDC00 && cp1 <= 0xDFFF && temp_sp > original_text) {
                                        uint32_t lead = *(temp_sp - 1);
                                        if (lead >= 0xD800 && lead <= 0xDBFF) { cp1 = ((lead - 0xD800) << 10) + (cp1 - 0xDC00) + 0x10000; temp_sp--; }
                                    }
                                    temp_end--; cp2 = *temp_end;
                                    if (cp2 >= 0xDC00 && cp2 <= 0xDFFF && temp_end > start) {
                                        uint32_t lead = *(temp_end - 1);
                                        if (lead >= 0xD800 && lead <= 0xDBFF) { cp2 = ((lead - 0xD800) << 10) + (cp2 - 0xDC00) + 0x10000; temp_end--; }
                                    }
                                    cp1 = unicode_casefold(cp1);
                                    cp2 = unicode_casefold(cp2);
                                } else {
                                    cp1 = *(--temp_sp);
                                    cp2 = *(--temp_end);
                                    cp1 = annexb_canonicalize(cp1);
                                    cp2 = annexb_canonicalize(cp2);
                                }
                                if (cp1 != cp2) { match = false; break; }
                            }
                            if (match) current.sp = temp_sp;
                        }
                    } else {
                        int len = end - start;
                        if (step > 0) {
                            const uint16_t* temp_sp = current.sp;
                            for (int i = 0; i < len; i++) {
                                if (temp_sp >= text_end || *temp_sp++ != start[i]) { match = false; break; }
                            }
                            if (match) current.sp = temp_sp;
                        } else {
                            const uint16_t* temp_sp = current.sp;
                            for (int i = len - 1; i >= 0; i--) {
                                if (temp_sp <= original_text || *(--temp_sp) != start[i]) { match = false; break; }
                            }
                            if (match) current.sp = temp_sp;
                        }
                    }
                    if (match) {
                        current.pc++;
                    } else {
                        path_failed = true; break;
                    }
                } else {
                    current.pc++;
                }
            }
            else if (inst.op == OP_LOOKAHEAD) {
                const uint16_t* temp_captures[MAX_GROUPS * 2];
                memcpy(temp_captures, current.captures, sizeof(temp_captures));
                bool la_match = vm_execute_internal(prog, inst.arg1, 1, original_text, text_end, current.sp, temp_captures);
                if (la_match) {
                    memcpy(current.captures, temp_captures, sizeof(temp_captures));
                    current.pc++;
                } else { path_failed = true; break; }
            }
            else if (inst.op == OP_NEG_LOOKAHEAD) {
                const uint16_t* temp_captures[MAX_GROUPS * 2];
                memcpy(temp_captures, current.captures, sizeof(temp_captures));
                bool la_match = vm_execute_internal(prog, inst.arg1, 1, original_text, text_end, current.sp, temp_captures);
                if (!la_match) {
                    current.pc++;
                } else { path_failed = true; break; }
            }
            else if (inst.op == OP_LOOKBEHIND || inst.op == OP_NEG_LOOKBEHIND) {
                const uint16_t* temp_captures[MAX_GROUPS * 2];
                memcpy(temp_captures, current.captures, sizeof(temp_captures));
                bool lb_match = vm_execute_internal(prog, inst.arg1, -1, original_text, text_end, current.sp, temp_captures);
                if ((inst.op == OP_LOOKBEHIND && lb_match) || (inst.op == OP_NEG_LOOKBEHIND && !lb_match)) {
                    if (lb_match) memcpy(current.captures, temp_captures, sizeof(temp_captures));
                    current.pc++;
                } else {
                    path_failed = true; break;
                }
            }
            else if (inst.op == OP_CLEAR_CAPTURES) {
                for (int i = inst.arg1; i <= inst.arg2 && i < MAX_GROUPS; i++) {
                    current.captures[i * 2] = NULL;
                    current.captures[i * 2 + 1] = NULL;
                }
                current.pc++;
            }
            else if (inst.op == OP_SPLIT) {
                stack[stack_ptr] = current; stack[stack_ptr++].pc = inst.arg2; current.pc = inst.arg1;            
            }
            else if (inst.op == OP_INIT_COUNTER) { current.counters[inst.arg1] = 0; current.counter_sp[inst.arg1] = NULL; current.pc++; }
            else if (inst.op == OP_INC_COUNTER)  { current.counters[inst.arg1]++; current.pc++; }
            else if (inst.op == OP_CHECK_COUNTER) {
                int c = current.counters[inst.arg1], min = inst.arg2, max = inst.arg3, exit_pc = inst.arg4;
                if (c > 0 && current.counter_sp[inst.arg1] == current.sp && c >= min) {
                    current.pc = exit_pc; 
                } else {
                    current.counter_sp[inst.arg1] = current.sp;
                    if (c < min) current.pc++; 
                    else if (max != -1 && c == max) current.pc = exit_pc; 
                    else {
                        if (inst.lazy) {
                            stack[stack_ptr] = current; stack[stack_ptr++].pc = current.pc + 1; 
                            current.pc = exit_pc; 
                        } else {
                            stack[stack_ptr] = current; stack[stack_ptr++].pc = exit_pc; 
                            current.pc++; 
                        }
                    }
                }
            }
            else if (inst.op == OP_JMP) { current.pc = inst.arg1; } 
            else if (inst.op == OP_MATCH) {
                if (out_captures) memcpy((void*)out_captures, current.captures, sizeof(const uint16_t*) * MAX_GROUPS * 2);
                return true; 
            }
        }
        if (path_failed) { fail_cache[h].pc = path_start_pc; fail_cache[h].sp = path_start_sp; }
    }
    return false;
}

void vm_get_indices(const uint16_t* original_text, const uint16_t** captures, CaptureIndex* out_indices, int group_count) {
    for (int i = 0; i <= group_count; i++) {
        if (captures[i * 2] && captures[i * 2 + 1]) {
            out_indices[i].start = (int)(captures[i * 2] - original_text);
            out_indices[i].end = (int)(captures[i * 2 + 1] - original_text);
        } else {
            out_indices[i].start = -1;
            out_indices[i].end = -1;
        }
    }
}