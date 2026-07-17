/* Native sanity check for the extracted engine + WASM shim, no Emscripten
 * required. Exercises the exact regex_wasm.c API a JS host would call
 * (EMSCRIPTEN_KEEPALIVE is a no-op outside __EMSCRIPTEN__, so these are
 * plain C functions here) against a few patterns covering flags, capture
 * groups (named and positional), and a no-match case. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "regex_wasm.h"

static int failures = 0;

/* Test strings here are all ASCII, so widening byte-for-byte is lossless. */
static uint16_t* to_utf16(const char* s) {
    size_t n = strlen(s);
    uint16_t* out = (uint16_t*)malloc(sizeof(uint16_t) * (n + 1));
    for (size_t i = 0; i < n; i++) out[i] = (uint16_t)(unsigned char)s[i];
    out[n] = 0;
    return out;
}

static void check(int cond, const char* what) {
    if (cond) {
        printf("[PASS] %s\n", what);
    } else {
        printf("[FAIL] %s\n", what);
        failures++;
    }
}

int main(void) {
    /* Basic match + capture groups, case-insensitive flag. */
    {
        uint16_t* pattern = to_utf16("(\\d+)-([a-z]+)");
        int flags = regex_flag_bit('i');
        uintptr_t h = regex_compile(pattern, 0, flags);
        check(h != 0, "compile (\\d+)-([a-z]+) succeeds");
        check(regex_group_count(h) == 2, "group_count is 2");

        uint16_t* text = to_utf16("order 42-ABC done");
        int matched = regex_exec(h, text, (int)strlen("order 42-ABC done"), 0);
        check(matched, "matches '42-ABC' under /i");

        const int32_t* caps = regex_captures_ptr(h);
        check(caps[0] == 6 && caps[1] == 12, "whole-match span is [6,12)");
        check(caps[2] == 6 && caps[3] == 8, "group 1 span is [6,8) ('42')");
        check(caps[4] == 9 && caps[5] == 12, "group 2 span is [9,12) ('ABC' under /i)");

        free(pattern);
        free(text);
        regex_free(h);
    }

    /* Named groups. */
    {
        uint16_t* pattern = to_utf16("(?<year>\\d{4})-(?<month>\\d{2})");
        uintptr_t h = regex_compile(pattern, 0, 0);
        check(h != 0, "compile named-group pattern succeeds");
        check(strcmp(regex_group_name(h, 1), "year") == 0, "group 1 named 'year'");
        check(strcmp(regex_group_name(h, 2), "month") == 0, "group 2 named 'month'");

        uint16_t* text = to_utf16("2026-07");
        int matched = regex_exec(h, text, (int)strlen("2026-07"), 0);
        check(matched, "matches '2026-07'");

        free(pattern);
        free(text);
        regex_free(h);
    }

    /* No match. */
    {
        uint16_t* pattern = to_utf16("xyz");
        uintptr_t h = regex_compile(pattern, 0, 0);
        uint16_t* text = to_utf16("abc");
        int matched = regex_exec(h, text, (int)strlen("abc"), 0);
        check(!matched, "'xyz' does not match 'abc'");
        const int32_t* caps = regex_captures_ptr(h);
        check(caps[0] == -1 && caps[1] == -1, "unmatched whole-match span is [-1,-1)");

        free(pattern);
        free(text);
        regex_free(h);
    }

    /* Compile error surfaces via regex_last_error(). */
    {
        uint16_t* pattern = to_utf16("(unclosed");
        uintptr_t h = regex_compile(pattern, 0, 0);
        check(h == 0, "unbalanced paren fails to compile");
        check(strlen(regex_last_error()) > 0, "regex_last_error() is non-empty on failure");
        free(pattern);
    }

    /* Sticky flag anchors exactly at start_index. */
    {
        uint16_t* pattern = to_utf16("\\d+");
        uintptr_t h = regex_compile(pattern, 0, regex_flag_bit('y'));
        uint16_t* text = to_utf16("ab123");
        int matched_at_0 = regex_exec(h, text, (int)strlen("ab123"), 0);
        check(!matched_at_0, "sticky /y does not match at index 0 ('ab123')");
        int matched_at_2 = regex_exec(h, text, (int)strlen("ab123"), 2);
        check(matched_at_2, "sticky /y matches at index 2 ('123')");
        free(pattern);
        free(text);
        regex_free(h);
    }

    /* Non-ASCII case-insensitive matching without /u (Annex B Canonicalize).
     * Regression test for a confirmed bug: this previously only folded
     * ASCII A-Z/a-z, silently missing every non-ASCII case pair even though
     * real JS engines fold the full BMP in this mode (verified against
     * Node across ~10k codepoint pairs spanning Latin/Greek/Cyrillic). */
    {
        uint16_t pattern[] = { 0x00E4, 0 }; /* ä "ä" */
        uintptr_t h = regex_compile(pattern, 0, regex_flag_bit('i'));
        uint16_t text[] = { 0x00C4 }; /* "Ä" */
        check(regex_exec(h, text, 1, 0), "non-/u /i: 'ä' matches 'Ä' (non-ASCII Latin fold)");
        regex_free(h);
    }
    {
        uint16_t pattern[] = { 0x0441, 0x0442, 0x043E, 0x043B, 0 }; /* "стол" */
        uintptr_t h = regex_compile(pattern, 0, regex_flag_bit('i'));
        uint16_t text[] = { 0x0421, 0x0422, 0x041E, 0x041B }; /* "СТОЛ" */
        check(regex_exec(h, text, 4, 0), "non-/u /i: cyrillic lowercase matches uppercase");
        regex_free(h);
    }
    {
        /* Skip-rule: a fold that would cross from non-ASCII into ASCII is
         * rejected (KELVIN SIGN U+212A has no simple uppercase mapping at
         * all; LONG S U+017F maps to 'S' but the cross-script fold is
         * specifically excluded) -- confirmed against real JS. */
        uint16_t* pattern_k = to_utf16("k");
        uintptr_t hk = regex_compile(pattern_k, 0, regex_flag_bit('i'));
        uint16_t kelvin[] = { 0x212A };
        check(!regex_exec(hk, kelvin, 1, 0), "non-/u /i: 'k' does not match KELVIN SIGN U+212A");
        free(pattern_k);
        regex_free(hk);

        uint16_t* pattern_s = to_utf16("s");
        uintptr_t hs = regex_compile(pattern_s, 0, regex_flag_bit('i'));
        uint16_t longs[] = { 0x017F };
        check(!regex_exec(hs, longs, 1, 0), "non-/u /i: 's' does not match LONG S U+017F");
        free(pattern_s);
        regex_free(hs);
    }
    {
        /* Class expansion (compile-time apply_case_folding) needs the same
         * fix as match-time folding -- and ASCII ranges must stay unaffected. */
        uint16_t* pattern = to_utf16("[a-z]");
        uintptr_t h = regex_compile(pattern, 0, regex_flag_bit('i'));
        uint16_t not_pulled_in[] = { 0x00C4 };
        check(!regex_exec(h, not_pulled_in, 1, 0), "non-/u /i class [a-z] does not spuriously pull in 'Ä'");
        uint16_t still_ascii[] = { 'A' };
        check(regex_exec(h, still_ascii, 1, 0), "non-/u /i class [a-z] still matches 'A'");
        free(pattern);
        regex_free(h);
    }

    /* Unicode "properties of strings" under /v mode (\p{...} escapes whose
     * value is a set of multi-codepoint sequences, e.g. emoji). Regression
     * test for a confirmed bug: the VM had no matching support for
     * CharClass.strings at all (also affected plain \q{...} character-class
     * string alternatives, exercised below too), so these always compiled
     * successfully but could never match anything. */
    {
        /* Emoji_Keycap_Sequence: 12 short sequences (well under the
         * existing 128-strings-per-class cap that \q{...} already had --
         * larger properties like RGI_Emoji (2604 sequences) or
         * RGI_Emoji_Flag_Sequence (259) still silently truncate to their
         * first 128 after this fix; see docs/IMPROVEMENTS.md). */
        uint16_t* pattern = to_utf16("\\p{Emoji_Keycap_Sequence}");
        uintptr_t h = regex_compile(pattern, 0, regex_flag_bit('v'));
        check(h != 0, "\\p{Emoji_Keycap_Sequence} compiles under /v");
        /* keycap digit "1": '1' U+0031, VARIATION SELECTOR-16 U+FE0F, COMBINING ENCLOSING KEYCAP U+20E3 */
        uint16_t text[] = { 0x0031, 0xFE0F, 0x20E3 };
        int matched = regex_exec(h, text, 3, 0);
        check(matched, "\\p{Emoji_Keycap_Sequence} matches keycap digit 1 (was: never matched anything)");
        if (matched) {
            const int32_t* caps = regex_captures_ptr(h);
            check(caps[0] == 0 && caps[1] == 3, "keycap match spans the full 3-codepoint sequence");
        }
        free(pattern);
        regex_free(h);
    }
    {
        /* Negating a property of strings has no well-defined complement and
         * must be a compile error, in both syntactic forms -- confirmed
         * against real JS engines. */
        uint16_t* pattern1 = to_utf16("\\P{Emoji_Keycap_Sequence}");
        uintptr_t h1 = regex_compile(pattern1, 0, regex_flag_bit('v'));
        check(h1 == 0, "\\P{Emoji_Keycap_Sequence} (bare negated) is a compile error");
        if (h1) regex_free(h1);
        free(pattern1);

        uint16_t* pattern2 = to_utf16("[\\P{Emoji_Keycap_Sequence}]");
        uintptr_t h2 = regex_compile(pattern2, 0, regex_flag_bit('v'));
        check(h2 == 0, "[\\P{Emoji_Keycap_Sequence}] (negated in class) is a compile error");
        if (h2) regex_free(h2);
        free(pattern2);
    }
    {
        /* \q{...} literal string alternatives inside a class -- the same
         * previously-unimplemented VM matching path, exercised directly,
         * mixed with plain single-character class members. */
        uint16_t* pattern = to_utf16("[\\q{ab|cd}xyz]+");
        uintptr_t h = regex_compile(pattern, 0, regex_flag_bit('v'));
        uint16_t text[] = { 'a', 'b', 'x', 'c', 'd' }; /* "abxcd" */
        int matched = regex_exec(h, text, 5, 0);
        check(matched, "\\q{ab|cd} string alternatives now match (were unmatchable before this fix)");
        if (matched) {
            const int32_t* caps = regex_captures_ptr(h);
            check(caps[0] == 0 && caps[1] == 5, "[\\q{ab|cd}xyz]+ spans all of 'abxcd'");
        }
        free(pattern);
        regex_free(h);
    }
    {
        /* Ordinary (non-string) properties must be entirely unaffected. */
        uint16_t* pattern = to_utf16("\\p{L}+");
        uintptr_t h = regex_compile(pattern, 0, regex_flag_bit('u'));
        uint16_t text[] = { 'h', 'i' };
        check(regex_exec(h, text, 2, 0), "\\p{L}+ (ordinary property, no strings) still works under /u");
        free(pattern);
        regex_free(h);
    }

    /* MAX_CLASSES/MAX_GROUPS/MAX_COUNTERS/MAX_OPCODES bounds checking.
     * Regression tests for a confirmed bug: none of these were enforced
     * (docs/IMPROVEMENTS.md #1.2) -- a pattern exceeding any of them wrote
     * past the corresponding fixed-size array (heap corruption for classes/
     * opcodes, stack corruption for groups/counters), confirmed via ASan.
     * Each "exceeds the limit" case below is chosen to reject *without*
     * crashing; the exact boundary itself is deliberately not exercised
     * here for capture groups -- see the comment on that block below. */
    {
        /* MAX_CLASSES = 64: exactly at the limit succeeds, one more fails
         * cleanly instead of overflowing prog->classes[]. */
        char pat64[64 * 3 + 1] = {0};
        for (int i = 0; i < 64; i++) strcat(pat64, "[a]");
        uint16_t* p64 = to_utf16(pat64);
        uintptr_t h64 = regex_compile(p64, 0, 0);
        check(h64 != 0, "MAX_CLASSES: exactly 64 character classes compiles");
        if (h64) regex_free(h64);
        free(p64);

        char pat65[65 * 3 + 1] = {0};
        for (int i = 0; i < 65; i++) strcat(pat65, "[a]");
        uint16_t* p65 = to_utf16(pat65);
        uintptr_t h65 = regex_compile(p65, 0, 0);
        check(h65 == 0, "MAX_CLASSES: 65 character classes is a clean compile error, not a crash");
        check(h65 == 0 && strstr(regex_last_error(), "maximum character class count") != NULL,
              "MAX_CLASSES: error message identifies the resource limit");
        if (h65) regex_free(h65);
        free(p65);
    }
    {
        /* MAX_COUNTERS = 16: exactly at the limit succeeds, one more fails
         * cleanly instead of overflowing the VM's per-thread counters[]. */
        char pat16[16 * 8 + 1] = {0};
        for (int i = 0; i < 16; i++) { char b[8]; snprintf(b, sizeof(b), "%c{1,2}", 'a' + (i % 26)); strcat(pat16, b); }
        uint16_t* p16 = to_utf16(pat16);
        uintptr_t h16 = regex_compile(p16, 0, 0);
        check(h16 != 0, "MAX_COUNTERS: exactly 16 bounded quantifiers compiles");
        if (h16) regex_free(h16);
        free(p16);

        char pat17[17 * 8 + 1] = {0};
        for (int i = 0; i < 17; i++) { char b[8]; snprintf(b, sizeof(b), "%c{1,2}", 'a' + (i % 26)); strcat(pat17, b); }
        uint16_t* p17 = to_utf16(pat17);
        uintptr_t h17 = regex_compile(p17, 0, 0);
        check(h17 == 0, "MAX_COUNTERS: 17 bounded quantifiers is a clean compile error, not a crash");
        check(h17 == 0 && strstr(regex_last_error(), "maximum quantifier count") != NULL,
              "MAX_COUNTERS: error message identifies the resource limit");
        if (h17) regex_free(h17);
        free(p17);
    }
    {
        /* MAX_GROUPS: 255 groups (one over the real ceiling of 254 -- see
         * re_parser.c's comment on the off-by-one from group id 0 being
         * reserved for "whole match") is rejected cleanly, not a crash.
         * Not asserting *which* safety net catches it: MAX_AST_DEPTH (see
         * below) is now the tighter, earlier-firing bound for any pattern
         * shaped like a chain of "(a)"s, since every way of accumulating
         * that many groups also grows AST depth past MAX_AST_DEPTH first
         * (255 groups implies at least 255 levels of concat/group nesting,
         * comfortably past MAX_AST_DEPTH=200) -- the group-count check
         * beneath it is still correct and necessary in its own right, just
         * not the one a simple test like this can observe firing first.
         * Both are exercised directly below. */
        char* pat = malloc(255 * 4 + 1); pat[0] = 0;
        for (int i = 0; i < 255; i++) strcat(pat, "(a)");
        uint16_t* p = to_utf16(pat);
        uintptr_t h = regex_compile(p, 0, 0);
        check(h == 0, "MAX_GROUPS: 255 capture groups is a clean compile error, not a crash");
        if (h) regex_free(h);
        free(pat); free(p);
    }

    /* MAX_AST_DEPTH: regression tests for the actual stack-overflow bug
     * (docs/IMPROVEMENTS.md #1.3) -- free_ast, validate_group_names,
     * validate_backrefs, validate_named_backrefs, and compile_node all
     * recurse through the parsed AST with no depth limit, and
     * parse_concat/parse_alt build linear (unbalanced) chains for flat
     * sequences, so a sufficiently long or deeply nested pattern produced
     * an AST deep enough to exhaust the C stack -- confirmed via ASan
     * (validate_group_names crashes around recursion depth ~247, the most
     * fragile of the five thanks to two ~8KB NameSet locals per frame).
     * Every case below previously crashed (verified against this exact
     * commit's pre-fix behavior); all now fail cleanly instead. */
    {
        /* The original, simplest repro: a long flat run of literals with no
         * groups/alternation at all -- pure parse_concat chain depth. */
        char* pat = malloc(20001); memset(pat, 'a', 20000); pat[20000] = 0;
        uint16_t* p = to_utf16(pat);
        uintptr_t h = regex_compile(p, 0, 0);
        check(h == 0, "MAX_AST_DEPTH: 20000 flat literal characters is a clean compile error, not a crash");
        check(h == 0 && strstr(regex_last_error(), "deeply nested or too long") != NULL,
              "MAX_AST_DEPTH: error message identifies the resource limit");
        if (h) regex_free(h);
        free(pat); free(p);
    }
    {
        /* Deeply nested groups: "((((...a...))))" -- tests both the
         * parser's own recursion (parse_primary -> parse_alt per nesting
         * level, guarded by Lexer.parse_depth) and the resulting AST's
         * depth (guarded by ASTNode.depth via finish_node). */
        int n = 20000;
        char* pat = malloc(2 * n + 2);
        for (int i = 0; i < n; i++) pat[i] = '(';
        pat[n] = 'a';
        for (int i = 0; i < n; i++) pat[n + 1 + i] = ')';
        pat[2 * n + 1] = 0;
        uint16_t* p = to_utf16(pat);
        uintptr_t h = regex_compile(p, 0, 0);
        check(h == 0, "MAX_AST_DEPTH: 20000 nested groups is a clean compile error, not a crash");
        if (h) regex_free(h);
        free(pat); free(p);
    }
    {
        /* Long alternation chain: "a|a|a|...|a" -- parse_alt recurses on
         * its own right-hand side for every '|', so this exercises the
         * parser's own C-stack recursion via Lexer.parse_depth specifically
         * (parse_concat, used for plain literal runs above, is iterative
         * and has no such recursion of its own -- this is a genuinely
         * different code path). */
        int n = 20000;
        char* pat = malloc(2 * n + 1);
        for (int i = 0; i < n; i++) { pat[2*i] = 'a'; pat[2*i+1] = (i < n - 1) ? '|' : 0; }
        uint16_t* p = to_utf16(pat);
        uintptr_t h = regex_compile(p, 0, 0);
        check(h == 0, "MAX_AST_DEPTH: 20000-way alternation is a clean compile error, not a crash");
        if (h) regex_free(h);
        free(pat); free(p);
    }
    {
        /* The specific case this fix was written for: a *legitimate*,
         * within-MAX_GROUPS pattern (254 capture groups -- no bounds
         * violation, would compile successfully before this fix) that
         * still crashed via validate_group_names' own recursion once it
         * ran on the resulting 254-deep AST. Confirmed via ASan, pre-fix,
         * to crash with no other error condition involved. Now rejected
         * cleanly by the depth guard before validate_group_names ever
         * runs (compile_into only calls it inside an `if (!prog->error)`
         * guard) instead of reaching it at all. */
        char* pat = malloc(254 * 4 + 1); pat[0] = 0;
        for (int i = 0; i < 254; i++) strcat(pat, "(a)");
        uint16_t* p = to_utf16(pat);
        uintptr_t h = regex_compile(p, 0, 0);
        check(h == 0, "MAX_AST_DEPTH: 254 groups (legitimate under MAX_GROUPS) no longer crashes validate_group_names");
        if (h) regex_free(h);
        free(pat); free(p);
    }
    {
        /* Moderate nesting/length, comfortably clear of MAX_AST_DEPTH --
         * confirms the guard doesn't disturb ordinary complex-but-sane
         * patterns. */
        uint16_t* pattern = to_utf16("(?:(?:(?:(?:(?:a+)+)+)?)*){1,3}(b|c|d){2,5}");
        uintptr_t h = regex_compile(pattern, 0, 0);
        check(h != 0, "moderately nested/quantified pattern still compiles normally");
        uint16_t text[] = { 'a','a','a','c','c' };
        check(regex_exec(h, text, 5, 0), "moderately nested pattern still matches");
        free(pattern);
        regex_free(h);
    }
    {
        /* Moderate group count, well clear of both the MAX_GROUPS ceiling
         * and the separate recursion-depth danger zone -- confirms the
         * bounds check above doesn't disturb ordinary multi-group patterns. */
        uint16_t* pattern = to_utf16("(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)");
        uintptr_t h = regex_compile(pattern, 0, 0);
        check(h != 0 && regex_group_count(h) == 10, "10 capture groups still compiles normally");
        uint16_t text[] = { 'a','b','c','d','e','f','g','h','i','j' };
        check(regex_exec(h, text, 10, 0), "10-group pattern still matches");
        free(pattern);
        regex_free(h);
    }
    {
        /* MAX_OPCODES = 16384: a single character class containing many
         * multi-codepoint string alternatives (\p{RGI_Emoji_ZWJ_Sequence},
         * capped at 128 sequences per class -- see the property-of-strings
         * fix above) compiles to far more than one instruction per AST
         * node, so a handful of repetitions clears the limit without the
         * deep AST recursion a long flat/nested pattern would need (and
         * without tripping the separate bug that would risk). */
        char pat[20 * 32 + 1] = {0};
        for (int i = 0; i < 20; i++) strcat(pat, "\\p{RGI_Emoji_ZWJ_Sequence}");
        uint16_t* p = to_utf16(pat);
        uintptr_t h = regex_compile(p, 0, regex_flag_bit('v'));
        check(h == 0, "MAX_OPCODES: 20x \\p{RGI_Emoji_ZWJ_Sequence} is a clean compile error, not a crash");
        check(h == 0 && strstr(regex_last_error(), "maximum compiled instruction count") != NULL,
              "MAX_OPCODES: error message identifies the resource limit");
        if (h) regex_free(h);
        free(p);
    }

    /* Nested lookaround no longer exhausts the C stack (docs/IMPROVEMENTS.md
     * #1.1). vm_execute_internal used to stack-allocate a ~2.2MB backtrack
     * stack + fail-cache on *every* call, including every recursive call
     * OP_LOOKAHEAD/OP_LOOKBEHIND makes into itself -- confirmed via ASan to
     * crash the process after only 3 levels of lookaround nesting on an
     * 8MB stack. Fixed by heap-allocating those buffers (so recursion costs
     * heap, not C-stack, per level) and sizing Thread.captures to the
     * pattern's actual group_count instead of the worst-case MAX_GROUPS
     * (which also fixed the ~2.2MB-per-call cost itself, not just its
     * location -- a typical few-group pattern's buffers are now a couple
     * hundred KB, not 2MB+). */
    {
        /* The original P0 repro (3 levels crashed natively pre-fix). */
        uint16_t* pattern = to_utf16("(?=(?=(?=x)))");
        uintptr_t h = regex_compile(pattern, 0, 0);
        check(h != 0, "3 nested lookaheads compiles");
        uint16_t text[] = { 'x' };
        check(regex_exec(h, text, 1, 0), "3 nested lookaheads matches (was: crashed the process)");
        free(pattern);
        regex_free(h);
    }
    {
        /* Deep nesting well past the old ~3-level crash point, up to the
         * edge of MAX_AST_DEPTH (199 -- one level short of the parser's own
         * separate 200 cap, see MAX_AST_DEPTH's regression tests above). */
        int n = 199;
        char* pat = malloc((size_t)n * 4 + 2); /* n*"(?=" + 'x' + n*")" + '\0' */
        char* q = pat;
        for (int i = 0; i < n; i++) { *q++ = '('; *q++ = '?'; *q++ = '='; }
        *q++ = 'x';
        for (int i = 0; i < n; i++) *q++ = ')';
        *q = 0;
        uint16_t* p = to_utf16(pat);
        uintptr_t h = regex_compile(p, 0, 0);
        check(h != 0, "199 nested lookaheads compiles (was: rejected far earlier by crash-prevention alone)");
        uint16_t text[] = { 'x' };
        int m = regex_exec(h, text, 1, 0);
        check(m, "199 nested lookaheads matches without crashing");
        if (m) {
            const int32_t* caps = regex_captures_ptr(h);
            check(caps[0] == 0 && caps[1] == 0, "match is zero-width, as a pure lookahead chain should be");
        }
        free(pat); free(p);
        regex_free(h);
    }
    {
        /* Capture groups *inside* a nested lookahead, and more afterward --
         * exercises that right-sizing Thread.captures to group_count
         * doesn't corrupt capture data as it round-trips through the
         * recursive call's temp_captures. */
        uint16_t* pattern = to_utf16("(?=(\\d+)-(\\w+))(\\d+)-(\\w+)");
        uintptr_t h = regex_compile(pattern, 0, 0);
        check(h != 0 && regex_group_count(h) == 4, "lookahead containing capture groups compiles, group_count is 4");
        uint16_t text[] = { '4','2','-','a','b','c' };
        check(regex_exec(h, text, 6, 0), "matches '42-abc'");
        const int32_t* caps = regex_captures_ptr(h);
        check(caps[2] == 0 && caps[3] == 2, "group 1 (inside lookahead) = '42'");
        check(caps[4] == 3 && caps[5] == 6, "group 2 (inside lookahead) = 'abc'");
        check(caps[6] == 0 && caps[7] == 2, "group 3 (outside) = '42'");
        check(caps[8] == 3 && caps[9] == 6, "group 4 (outside) = 'abc'");
        free(pattern);
        regex_free(h);
    }

    /* The three P1 OOB reads (docs/IMPROVEMENTS.md #1.4/#1.5/#1.6). The
     * buffer-edge cases below use exactly-sized heap buffers with no NUL
     * terminator and no slack -- explicitly allowed by regex_exec's contract
     * (text_units is authoritative; see README) -- so that the pre-fix
     * one-past-the-end reads are hard ASan violations under `make test-asan`
     * rather than silently reading whatever byte happens to follow. */
    {
        /* #1.4: \b/\B at the very end of the text used to dereference one
         * unit past text_end unconditionally (OP_ASSERT_END has the sp <
         * text_end guard; the word-boundary ops were missing it). Behavior
         * checked against Node: /abc\b/ matches 'abc', /abc\B/ does not. */
        uint16_t* pattern_b = to_utf16("abc\\b");
        uintptr_t hb = regex_compile(pattern_b, 0, 0);
        uint16_t* text = (uint16_t*)malloc(sizeof(uint16_t) * 3);
        text[0] = 'a'; text[1] = 'b'; text[2] = 'c';
        check(regex_exec(hb, text, 3, 0), "\\b matches at end of a tightly-sized buffer (was: OOB read)");
        free(pattern_b);
        regex_free(hb);

        uint16_t* pattern_nb = to_utf16("abc\\B");
        uintptr_t hnb = regex_compile(pattern_nb, 0, 0);
        check(!regex_exec(hnb, text, 3, 0), "\\B does not match at end of text (end-of-text is a boundary)");
        free(pattern_nb);
        regex_free(hnb);
        free(text);
    }
    {
        /* #1.5: decode_utf16's trail-surrogate peek used to be unbounded, so
         * a buffer ending in a lone lead surrogate was read one unit past its
         * end. A lone surrogate must decode as itself (Node: /./u.exec of a
         * bare U+D83D matches it). */
        uint16_t* pattern = to_utf16(".");
        uintptr_t h = regex_compile(pattern, 0, regex_flag_bit('u'));
        uint16_t* text = (uint16_t*)malloc(sizeof(uint16_t));
        text[0] = 0xD83D;
        int matched = regex_exec(h, text, 1, 0);
        check(matched, "/./u matches a trailing lone lead surrogate (was: OOB read)");
        if (matched) {
            const int32_t* caps = regex_captures_ptr(h);
            check(caps[0] == 0 && caps[1] == 1, "lone lead surrogate decodes as one unit, not a phantom pair");
        }
        free(pattern);
        free(text);
        regex_free(h);
    }
    {
        /* #1.5, backreference path: the ignore-case backref comparison has
         * its own decode_utf16 calls; text of two lone lead surrogates puts
         * the second decode exactly at the buffer edge. Node:
         * /(.)\1/iu.exec('\uD83D\uD83D') matches the whole 2-unit string. */
        uint16_t* pattern = to_utf16("(.)\\1");
        uintptr_t h = regex_compile(pattern, 0, regex_flag_bit('i') | regex_flag_bit('u'));
        uint16_t* text = (uint16_t*)malloc(sizeof(uint16_t) * 2);
        text[0] = 0xD83D; text[1] = 0xD83D;
        int matched = regex_exec(h, text, 2, 0);
        check(matched, "/(.)\\1/iu matches two lone lead surrogates (backref decode at buffer edge)");
        if (matched) {
            const int32_t* caps = regex_captures_ptr(h);
            check(caps[0] == 0 && caps[1] == 2, "backref match spans both units");
        }
        free(pattern);
        free(text);
        regex_free(h);
    }
    {
        /* #1.6: out-of-range numeric backreferences were only validated
         * under /u; in non-unicode mode /(a)\999/ emitted OP_BACKREF 999,
         * indexing captures[] far out of bounds at match time. Now a
         * SyntaxError in every mode -- a documented deviation from Annex B
         * (which re-reads small out-of-range \N as octal/identity escapes,
         * a fallback this engine has never implemented) in exchange for
         * closing the OOB read. */
        uint16_t* bad = to_utf16("(a)\\999");
        uintptr_t hbad = regex_compile(bad, 0, 0);
        check(hbad == 0, "non-/u (a)\\999 is a clean compile error, not an OOB read at match time");
        check(hbad == 0 && strstr(regex_last_error(), "backreference") != NULL,
              "out-of-range backref error message names the problem");
        if (hbad) regex_free(hbad);
        free(bad);

        uint16_t* good = to_utf16("(a)\\1");
        uintptr_t hgood = regex_compile(good, 0, 0);
        check(hgood != 0, "non-/u (a)\\1 (in-range backref) still compiles");
        uint16_t text[] = { 'a', 'a' };
        check(regex_exec(hgood, text, 2, 0), "non-/u (a)\\1 still matches 'aa'");
        free(good);
        regex_free(hgood);
    }

    /* Non-unicode-mode builtin classes span the full UTF-16 code-unit space
     * (docs/IMPROVEMENTS.md #1.7). `.`/`\D`/`\W`/`\S` were wrongly capped at
     * codepoint 255 (and \s dropped its >255 entries) -- ECMAScript has
     * never Latin-1-scoped these; /u only gates surrogate-pair decoding.
     * Every expectation below was diffed against Node with no flags. */
    {
        uint16_t* pattern = to_utf16("x.y");
        uintptr_t h = regex_compile(pattern, 0, 0);
        uint16_t text[] = { 'x', 0x20AC, 'y' }; /* "x€y" */
        check(regex_exec(h, text, 3, 0), "non-/u /x.y/ matches 'x\\u20ACy' (dot was capped at 255)");
        free(pattern);
        regex_free(h);
    }
    {
        /* The 255 cap was also masking a second bug in the same branch:
         * non-/u `.` never excluded the LineTerminators U+2028/U+2029
         * (both unreachable under the cap). Node: /./.test('\u2028') is
         * false, /./s.test('\u2028') is true. */
        uint16_t* pattern = to_utf16(".");
        uintptr_t h = regex_compile(pattern, 0, 0);
        uint16_t ls[] = { 0x2028 };
        check(!regex_exec(h, ls, 1, 0), "non-/u /./ still excludes LINE SEPARATOR U+2028 above the old cap");
        uint16_t ok[] = { 0x202A };
        check(regex_exec(h, ok, 1, 0), "non-/u /./ matches U+202A (exclusion is exactly 2028-2029)");
        free(pattern);
        regex_free(h);

        uint16_t* pattern_s = to_utf16(".");
        uintptr_t hs = regex_compile(pattern_s, 0, regex_flag_bit('s'));
        check(regex_exec(hs, ls, 1, 0), "/./s (dotAll) matches U+2028");
        free(pattern_s);
        regex_free(hs);
    }
    {
        uint16_t* pd = to_utf16("\\D");
        uintptr_t hd = regex_compile(pd, 0, 0);
        uint16_t amacron[] = { 0x0100 };
        check(regex_exec(hd, amacron, 1, 0), "non-/u /\\D/ matches U+0100");
        free(pd); regex_free(hd);

        uint16_t* pw = to_utf16("\\W");
        uintptr_t hw = regex_compile(pw, 0, 0);
        check(regex_exec(hw, amacron, 1, 0), "non-/u /\\W/ matches U+0100");
        free(pw); regex_free(hw);
    }
    {
        uint16_t* ps = to_utf16("\\s");
        uintptr_t hs = regex_compile(ps, 0, 0);
        uint16_t bom[] = { 0xFEFF };
        check(regex_exec(hs, bom, 1, 0), "non-/u /\\s/ matches BOM U+FEFF (\\s list is mode-independent)");
        free(ps); regex_free(hs);

        uint16_t* pS = to_utf16("\\S");
        uintptr_t hS = regex_compile(pS, 0, 0);
        uint16_t euro[] = { 0x20AC };
        check(regex_exec(hS, euro, 1, 0), "non-/u /\\S/ matches EURO U+20AC");
        check(!regex_exec(hS, bom, 1, 0), "non-/u /\\S/ does not match BOM (complement of the full \\s list)");
        free(pS); regex_free(hS);
    }
    {
        /* Inside a character class the same fill_builtin_class path is
         * reached via a different call site -- guard it separately. */
        uint16_t* pattern = to_utf16("[\\s]");
        uintptr_t h = regex_compile(pattern, 0, 0);
        uint16_t ls[] = { 0x2028 };
        check(regex_exec(h, ls, 1, 0), "non-/u /[\\s]/ matches U+2028 (in-class builtin path)");
        free(pattern);
        regex_free(h);
    }

    /* Nested capture groups are numbered by OPENING paren position, per
     * ECMA-262. Regression tests for a confirmed bug (present in jsvm2
     * upstream too): the parser assigned ids after parsing a group's body,
     * numbering nested groups by *closing* paren instead -- so in ((a)b)
     * the inner group was 1 and the outer 2, backwards from every real JS
     * engine, corrupting both reported capture indices and numeric-backref
     * resolution for any pattern with nested captures. Every expectation
     * below diffed against Node. */
    {
        uint16_t* pattern = to_utf16("((a)b)");
        uintptr_t h = regex_compile(pattern, 0, 0);
        uint16_t text[] = { 'a', 'b' };
        int m = regex_exec(h, text, 2, 0);
        const int32_t* caps = regex_captures_ptr(h);
        check(m && caps[2] == 0 && caps[3] == 2, "((a)b): group 1 is the OUTER paren [0,2)");
        check(m && caps[4] == 0 && caps[5] == 1, "((a)b): group 2 is the inner paren [0,1)");
        free(pattern);
        regex_free(h);
    }
    {
        uint16_t* pattern = to_utf16("(a(b(c)))\\3");
        uintptr_t h = regex_compile(pattern, 0, 0);
        uint16_t text[] = { 'a', 'b', 'c', 'c' };
        int m = regex_exec(h, text, 4, 0);
        const int32_t* caps = regex_captures_ptr(h);
        check(m && caps[0] == 0 && caps[1] == 4, "(a(b(c)))\\3: \\3 resolves to the innermost group");
        check(m && caps[6] == 2 && caps[7] == 3, "(a(b(c)))\\3: group 3 is (c) at [2,3)");
        free(pattern);
        regex_free(h);
    }
    {
        /* Named nested groups: the index->name table must follow the same
         * opening-paren order as the ids themselves. */
        uint16_t* pattern = to_utf16("(?<out>(?<in>a)b)");
        uintptr_t h = regex_compile(pattern, 0, 0);
        check(h != 0 && strcmp(regex_group_name(h, 1), "out") == 0, "nested named groups: group 1 is 'out'");
        check(h != 0 && strcmp(regex_group_name(h, 2), "in") == 0, "nested named groups: group 2 is 'in'");
        free(pattern);
        regex_free(h);
    }

    /* Captures inside a quantified atom reset at the start of every
     * iteration (ECMA-262 RepeatMatcher; docs/IMPROVEMENTS.md #1.8).
     * Regression tests for a confirmed bug: OP_CLEAR_CAPTURES existed in
     * the VM but was never emitted, so a branch that didn't participate in
     * the final iteration kept its stale value from an earlier one. Every
     * expectation below diffed against Node. */
    {
        /* The doc's original repro: Node gives ["ab", undefined, "b"] --
         * group 1 must be unset, not retain "a" from iteration 1. */
        uint16_t* pattern = to_utf16("(?:(a)|(b))+");
        uintptr_t h = regex_compile(pattern, 0, 0);
        uint16_t text[] = { 'a', 'b' };
        int m = regex_exec(h, text, 2, 0);
        const int32_t* caps = regex_captures_ptr(h);
        check(m && caps[0] == 0 && caps[1] == 2, "(?:(a)|(b))+ matches 'ab'");
        check(m && caps[2] == -1 && caps[3] == -1, "group 1 is unset (was: stale 'a' from iteration 1)");
        check(m && caps[4] == 1 && caps[5] == 2, "group 2 is 'b' from the final iteration");
        free(pattern);
        regex_free(h);
    }
    {
        /* The clear must NOT leak past loop exit: the exit thread carries
         * the captures of the last successful iteration. Node:
         * /(?:(a)|(x))+/.exec('a') -> ["a", "a", undefined]. */
        uint16_t* pattern = to_utf16("(?:(a)|(x))+");
        uintptr_t h = regex_compile(pattern, 0, 0);
        uint16_t text[] = { 'a' };
        int m = regex_exec(h, text, 1, 0);
        const int32_t* caps = regex_captures_ptr(h);
        check(m && caps[2] == 0 && caps[3] == 1, "last successful iteration's capture survives loop exit");
        free(pattern);
        regex_free(h);
    }
    {
        /* Quantified capturing group with an optional inner group: outer
         * re-set each iteration, inner cleared when it doesn't participate.
         * Node: /((a)?b)+/.exec('abb') -> ["abb", "b", undefined]. Also
         * exercises nested-group numbering inside a quantifier. */
        uint16_t* pattern = to_utf16("((a)?b)+");
        uintptr_t h = regex_compile(pattern, 0, 0);
        uint16_t text[] = { 'a', 'b', 'b' };
        int m = regex_exec(h, text, 3, 0);
        const int32_t* caps = regex_captures_ptr(h);
        check(m && caps[2] == 2 && caps[3] == 3, "((a)?b)+: outer group is final iteration's 'b'");
        check(m && caps[4] == -1 && caps[5] == -1, "((a)?b)+: inner (a) cleared on final iteration");
        free(pattern);
        regex_free(h);
    }
    {
        /* Groups OUTSIDE the quantified atom must be untouched by its
         * per-iteration clears. Node: /(a)(?:b|(c))+/.exec('abb') ->
         * ["abb", "a", undefined]. */
        uint16_t* pattern = to_utf16("(a)(?:b|(c))+");
        uintptr_t h = regex_compile(pattern, 0, 0);
        uint16_t text[] = { 'a', 'b', 'b' };
        int m = regex_exec(h, text, 3, 0);
        const int32_t* caps = regex_captures_ptr(h);
        check(m && caps[2] == 0 && caps[3] == 1, "group before the loop is not cleared by it");
        free(pattern);
        regex_free(h);
    }
    {
        /* Bounded quantifiers take the same loop shape and clear too.
         * Node: /(?:(a)|(b)){2}/.exec('ab') -> ["ab", undefined, "b"]. */
        uint16_t* pattern = to_utf16("(?:(a)|(b)){2}");
        uintptr_t h = regex_compile(pattern, 0, 0);
        uint16_t text[] = { 'a', 'b' };
        int m = regex_exec(h, text, 2, 0);
        const int32_t* caps = regex_captures_ptr(h);
        check(m && caps[2] == -1 && caps[3] == -1 && caps[4] == 1 && caps[5] == 2,
              "{2} bounded quantifier clears per iteration");
        free(pattern);
        regex_free(h);
    }
    {
        /* Group inside a lookahead inside the atom clears/re-sets with the
         * iterations too (RepeatMatcher's range covers the whole atom).
         * Node: /(?:(?=(a))a|b)+/ on 'ba' -> g1 'a'; on 'ab' -> g1 unset. */
        uint16_t* pattern = to_utf16("(?:(?=(a))a|b)+");
        uintptr_t h = regex_compile(pattern, 0, 0);
        uint16_t ba[] = { 'b', 'a' };
        int m1 = regex_exec(h, ba, 2, 0);
        const int32_t* caps1 = regex_captures_ptr(h);
        check(m1 && caps1[2] == 1 && caps1[3] == 2, "lookahead capture in atom: set by final iteration");
        uint16_t ab[] = { 'a', 'b' };
        int m2 = regex_exec(h, ab, 2, 0);
        const int32_t* caps2 = regex_captures_ptr(h);
        check(m2 && caps2[2] == -1 && caps2[3] == -1, "lookahead capture in atom: cleared by final iteration");
        free(pattern);
        regex_free(h);
    }

    /* Silent-failure spots (docs/IMPROVEMENTS.md #1.9): unknown \p{...}
     * names silently compiled to an empty class (so \p{Bogus} never matched
     * and \P{Bogus} matched EVERYTHING, where Node raises SyntaxError for
     * both), and capture-group names silently truncated to 31 UTF-8 bytes
     * (turning two long names distinct only past byte 31 into a spurious
     * "Duplicate capture group name" error). Both now fail loudly. */
    {
        uint16_t* p1 = to_utf16("\\p{Bogus}");
        uintptr_t h1 = regex_compile(p1, 0, regex_flag_bit('u'));
        check(h1 == 0 && strstr(regex_last_error(), "Invalid property name") != NULL,
              "\\p{Bogus}/u is a SyntaxError (was: silently-empty class)");
        if (h1) regex_free(h1);
        free(p1);

        uint16_t* p2 = to_utf16("\\P{Bogus}");
        uintptr_t h2 = regex_compile(p2, 0, regex_flag_bit('u'));
        check(h2 == 0, "\\P{Bogus}/u is a SyntaxError (was: matched everything)");
        if (h2) regex_free(h2);
        free(p2);

        /* A known property must still resolve afterward -- rejected names
         * must not have been cached as (empty) results. */
        uint16_t* p3 = to_utf16("\\p{L}");
        uintptr_t h3 = regex_compile(p3, 0, regex_flag_bit('u'));
        uint16_t text[] = { 'a' };
        check(h3 != 0 && regex_exec(h3, text, 1, 0), "\\p{L} still resolves after a rejected name (cache not poisoned)");
        if (h3) regex_free(h3);
        free(p3);
    }
    {
        /* 40-char name: engine limit (31 UTF-8 bytes + NUL), reported
         * loudly. A deliberate deviation from the spec, which has no name
         * length limit -- same nature as the MAX_GROUPS/MAX_CLASSES caps. */
        uint16_t* plong = to_utf16("(?<abcdefghijklmnopqrstuvwxyz0123456789abcd>x)");
        uintptr_t hlong = regex_compile(plong, 0, 0);
        check(hlong == 0 && strstr(regex_last_error(), "name exceeds maximum length") != NULL,
              "40-char group name is a clean resource-limit error (was: silent truncation)");
        if (hlong) regex_free(hlong);
        free(plong);

        /* Exactly at the limit: 31 chars, including \k<...> resolution. */
        uint16_t* pfit = to_utf16("(?<abcdefghijklmnopqrstuvwxyz01234>x)\\k<abcdefghijklmnopqrstuvwxyz01234>");
        uintptr_t hfit = regex_compile(pfit, 0, 0);
        uint16_t xx[] = { 'x', 'x' };
        check(hfit != 0 && regex_exec(hfit, xx, 2, 0), "31-char group name (at the limit) still works, incl. \\k backref");
        if (hfit) regex_free(hfit);
        free(pfit);
    }

    /* \p{...} key/namespace handling per ECMA-262 (the spec-compliance item
     * under docs/IMPROVEMENTS.md #1.9): bare names may only be binary
     * properties or General_Category values; Script/Script_Extensions
     * require their keyed form; keys are case-sensitive; contributory
     * properties are rejected; properties of strings require /v. All of
     * this previously collapsed into "ignore everything before '=' and
     * look the value up in one flat table" -- bare \p{Greek} was accepted,
     * \p{Foo=Greek} was accepted, sc= and scx= were conflated. Every
     * expectation here (and 235 cases total, in the fix's verification run)
     * diffed against Node. */
    {
        uint16_t alpha[] = { 0x03B1 };   /* α: Script=Greek */
        uint16_t perisp[] = { 0x0342 };  /* combining perispomeni: sc=Inherited, scx includes Greek */
        int u = regex_flag_bit('u');

        uint16_t* p1 = to_utf16("\\p{Greek}");
        uintptr_t h1 = regex_compile(p1, 0, u);
        check(h1 == 0, "bare \\p{Greek} is a SyntaxError (scripts require Script=)");
        if (h1) regex_free(h1);
        free(p1);

        uint16_t* p2 = to_utf16("\\p{Script=Greek}");
        uintptr_t h2 = regex_compile(p2, 0, u);
        check(h2 != 0 && regex_exec(h2, alpha, 1, 0), "\\p{Script=Greek} matches alpha");
        check(h2 != 0 && !regex_exec(h2, perisp, 1, 0), "\\p{Script=Greek} does NOT match U+0342 (sc=Inherited)");
        if (h2) regex_free(h2);
        free(p2);

        uint16_t* p3 = to_utf16("\\p{Script_Extensions=Greek}");
        uintptr_t h3 = regex_compile(p3, 0, u);
        check(h3 != 0 && regex_exec(h3, perisp, 1, 0), "\\p{Script_Extensions=Greek} DOES match U+0342 (scx != sc)");
        check(h3 != 0 && regex_exec(h3, alpha, 1, 0), "\\p{Script_Extensions=Greek} still matches alpha (scx completed per UAX #24)");
        if (h3) regex_free(h3);
        free(p3);

        uint16_t* p4 = to_utf16("\\p{sc=Grek}");
        uintptr_t h4 = regex_compile(p4, 0, u);
        check(h4 != 0 && regex_exec(h4, alpha, 1, 0), "\\p{sc=Grek} (short key + script alias) works");
        if (h4) regex_free(h4);
        free(p4);
    }
    {
        /* Invalid keys and namespace mixups are SyntaxErrors, matching Node:
         * keys are case-sensitive, unknown keys rejected, a binary property
         * is not a gc value, contributory properties aren't in the spec's
         * binary table. */
        const char* bad[] = { "\\p{script=Greek}", "\\p{Foo=Bar}", "\\p{gc=Alphabetic}",
                              "\\p{gc=Greek}", "\\p{Other_Alphabetic}", "\\p{lu}" };
        for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
            uint16_t* p = to_utf16(bad[i]);
            uintptr_t h = regex_compile(p, 0, regex_flag_bit('u'));
            char what[96];
            snprintf(what, sizeof(what), "%s is a SyntaxError", bad[i]);
            check(h == 0, what);
            if (h) regex_free(h);
            free(p);
        }
    }
    {
        /* Aliases and grouped GC values are direct table entries now. */
        uint16_t bang[] = { '!' };
        uint16_t cap_a[] = { 'A' };
        uint16_t* pp = to_utf16("\\p{punct}");
        uintptr_t hp = regex_compile(pp, 0, regex_flag_bit('u'));
        check(hp != 0 && regex_exec(hp, bang, 1, 0), "\\p{punct} (gc alias) matches '!'");
        if (hp) regex_free(hp);
        free(pp);

        uint16_t* pl = to_utf16("\\p{LC}");
        uintptr_t hl = regex_compile(pl, 0, regex_flag_bit('u'));
        check(hl != 0 && regex_exec(hl, cap_a, 1, 0), "\\p{LC} (Cased_Letter group) matches 'A'");
        if (hl) regex_free(hl);
        free(pl);

        uint16_t* pa = to_utf16("\\p{AHex}");
        uintptr_t ha = regex_compile(pa, 0, regex_flag_bit('u'));
        check(ha != 0 && regex_exec(ha, cap_a, 1, 0), "\\p{AHex} (binary alias) matches 'A'");
        if (ha) regex_free(ha);
        free(pa);
    }
    {
        /* Properties of strings exist only in /v mode -- \p{Basic_Emoji}/u
         * is a SyntaxError in real engines (previously accepted here). */
        uint16_t* pattern = to_utf16("\\p{Basic_Emoji}");
        uintptr_t hu = regex_compile(pattern, 0, regex_flag_bit('u'));
        check(hu == 0, "\\p{Basic_Emoji} under /u is a SyntaxError (strings need /v)");
        if (hu) regex_free(hu);
        uintptr_t hv = regex_compile(pattern, 0, regex_flag_bit('v'));
        check(hv != 0, "\\p{Basic_Emoji} under /v still compiles");
        if (hv) regex_free(hv);
        free(pattern);
    }

    if (failures == 0) {
        printf("\nAll smoke tests passed.\n");
        return 0;
    }
    printf("\n%d smoke test(s) FAILED.\n", failures);
    return 1;
}
