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

    if (failures == 0) {
        printf("\nAll smoke tests passed.\n");
        return 0;
    }
    printf("\n%d smoke test(s) FAILED.\n", failures);
    return 1;
}
