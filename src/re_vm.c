/* Virtual machine: backtracking interpreter over Program.code, with an
 * explicit thread stack (not the C call stack -- except for lookaround,
 * which recurses via a genuine nested call to vm_execute_internal) and a
 * memoizing fail-cache that's the main defense against classic
 * catastrophic-backtracking patterns. See docs/ARCHITECTURE.md's "VM"
 * section for the full picture, and docs/IMPROVEMENTS.md #1.1 for why that
 * lookaround recursion is also this engine's most serious known crash risk.
 *
 * decode_utf16/is_word_char/annexb_canonicalize live here (not in
 * re_lexer.c, despite jsvm2's original file having them textually
 * positioned before its "LEXER" section) because this split follows actual
 * cross-file usage rather than historical textual position -- all three are
 * used only by the VM in this engine; the lexer has its own,
 * Lexer-state-aware decode_utf16_lexer instead.
 *
 * Split out of what was originally a single file (src/regexp.c, itself a
 * verbatim copy of jsvm2's src/regexp.c) for maintainability -- see
 * CLAUDE.md/README.md's "Provenance" section for why that diverges from
 * upstream's layout, and docs/IMPROVEMENTS.md section 4 for the rationale.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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

static inline bool is_word_char(uint32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
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
