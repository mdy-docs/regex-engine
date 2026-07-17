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

    if (failures == 0) {
        printf("\nAll smoke tests passed.\n");
        return 0;
    }
    printf("\n%d smoke test(s) FAILED.\n", failures);
    return 1;
}
