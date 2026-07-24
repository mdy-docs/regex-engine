/* Low-level WASM shim over the standalone regex engine (regexp.h/regexp.c).
 *
 * Exposes a handful of EMSCRIPTEN_KEEPALIVE functions. The caller (JS) does
 * its own memory management for input buffers via the Emscripten-exported
 * _malloc/_free: write a pattern/subject string into WASM linear memory as
 * UTF-16 code units, pass the pointer+length across, then read results back
 * out of a WASM-owned int32 buffer via a typed-array view -- no per-call
 * marshaling of match data through JS-visible function arguments.
 *
 * Typical JS-side flow (see README.md for the full walkthrough):
 *   1. write pattern units into WASM memory, call regex_compile()
 *   2. on success (non-zero handle), write subject units, call regex_exec()
 *   3. if matched, read regex_group_count()+1 pairs from regex_captures_ptr()
 *      as an Int32Array view -- [start0,end0, start1,end1, ...], -1 for an
 *      unmatched optional group; group 0 is the whole match
 *   4. regex_free() the handle when done with this compiled pattern
 *
 * A compiled Program is large (multi-MB fixed-size opcode/class tables --
 * see regexp.h) and always heap-allocated here, never on the stack. Compile
 * once per distinct pattern and reuse the handle across calls; don't
 * recompile per exec.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "regexp.h"
#include "regex_wasm.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

typedef struct {
    Program prog;
    int32_t* captures; /* (prog.group_count + 1) * 2 ints, owned by this handle */
    /* Created on first regex_exec and reused for the handle's lifetime:
     * context creation is cheap but its lazily-built scratch (backtrack
     * stack + arena + fail cache, ~200KB+ per lookaround depth) is not, and
     * a per-exec context re-paid all of it on every call -- the same
     * per-entry overhead VMContext exists to amortize, one level up. Safe
     * to cache here: WASM is single-threaded and a context is tied to the
     * Program it was created for, which is exactly this handle's. */
    VMContext* ctx;
    uint64_t step_budget;  /* 0 = unlimited; re-armed per exec call */
    int budget_exhausted;  /* result of the most recent exec on this handle */
} RegexHandle;

static char g_last_error[256] = {0};

static void set_last_error(const char* msg) {
    if (!msg) { g_last_error[0] = '\0'; return; }
    size_t n = strlen(msg);
    if (n >= sizeof(g_last_error)) n = sizeof(g_last_error) - 1;
    memcpy(g_last_error, msg, n);
    g_last_error[n] = '\0';
}

/* Maps a single flag character to its REGEX_FLAG_* bit (0 if not a flag
 * character), so the caller can build a mask from a normal "gimsuyd" string
 * without hardcoding the bit layout. regex_compile itself still takes the
 * raw integer mask -- this is a convenience, not a requirement. */
EMSCRIPTEN_KEEPALIVE
int regex_flag_bit(int flag_char) {
    switch (flag_char) {
        case 'g': return REGEX_FLAG_GLOBAL;
        case 'i': return REGEX_FLAG_IGNORECASE;
        case 'm': return REGEX_FLAG_MULTILINE;
        case 's': return REGEX_FLAG_DOTALL;
        case 'y': return REGEX_FLAG_STICKY;
        case 'u': return REGEX_FLAG_UNICODE;
        case 'd': return REGEX_FLAG_INDICES;
        case 'v': return REGEX_FLAG_UNICODE_SETS;
        default: return 0;
    }
}

/* Compiles `pattern[0..pattern_units)` (UTF-16 code units) with the given
 * REGEX_FLAG_* bitmask. Returns an opaque non-zero handle on success, or 0
 * on failure (call regex_last_error() for why). The returned handle must be
 * released with regex_free(). */
EMSCRIPTEN_KEEPALIVE
uintptr_t regex_compile(const uint16_t* pattern, int pattern_units, int flags) {
    (void)pattern_units; /* compile_into expects a NUL-terminated unit buffer */
    /* calloc, not malloc: compile_into requires a zero-initialized Program
     * before its first use, so the heap-owned class string buffers (see
     * CharClass in regexp.h) are never freed from garbage pointers. */
    RegexHandle* h = (RegexHandle*)calloc(1, sizeof(RegexHandle));
    if (!h) {
        set_last_error("OutOfMemory: could not allocate Program");
        return 0;
    }
    h->captures = NULL;
    compile_into(&h->prog, pattern, flags);
    if (h->prog.error) {
        set_last_error(h->prog.error);
        for (int i = 0; i < h->prog.class_count; i++) class_strings_free(&h->prog.classes[i]);
        free(h);
        return 0;
    }
    int pair_count = (h->prog.group_count + 1) * 2;
    h->captures = (int32_t*)malloc(sizeof(int32_t) * (size_t)pair_count);
    if (!h->captures) {
        set_last_error("OutOfMemory: could not allocate capture buffer");
        for (int i = 0; i < h->prog.class_count; i++) class_strings_free(&h->prog.classes[i]);
        free(h);
        return 0;
    }
    set_last_error(NULL);
    return (uintptr_t)(uintptr_t)h;
}

/* Human-readable reason the most recent regex_compile() call on this thread
 * failed. Valid until the next regex_compile() call; the pointer is into a
 * static buffer, not something to free. */
EMSCRIPTEN_KEEPALIVE
const char* regex_last_error(void) {
    return g_last_error;
}

/* Number of capturing groups (not counting group 0, the whole match). */
EMSCRIPTEN_KEEPALIVE
int regex_group_count(uintptr_t handle) {
    RegexHandle* h = (RegexHandle*)(uintptr_t)handle;
    if (!h) return 0;
    return h->prog.group_count;
}

/* NUL-terminated UTF-8/ASCII name of capture group `index` (1-based; index 0
 * is the whole match and has no name), or "" if that group is unnamed. */
EMSCRIPTEN_KEEPALIVE
const char* regex_group_name(uintptr_t handle, int index) {
    RegexHandle* h = (RegexHandle*)(uintptr_t)handle;
    if (!h || index < 0 || index > h->prog.group_count) return "";
    return h->prog.group_names[index];
}

/* Searches `text[0..text_units)` for a match starting at or after
 * start_index (in UTF-16 code units). Honors the sticky/unicode flags baked
 * into the compiled pattern (anchors exactly at start_index when sticky;
 * otherwise scans forward, advancing by code point under the u/v flags so a
 * surrogate pair is never split). Returns 1 if matched (read results via
 * regex_captures_ptr()) or 0 if not. */
EMSCRIPTEN_KEEPALIVE
int regex_exec(uintptr_t handle, const uint16_t* text, int text_units, int start_index) {
    RegexHandle* h = (RegexHandle*)(uintptr_t)handle;
    if (!h || !text || start_index < 0 || start_index > text_units) return 0;

    const uint16_t* text_end = text + text_units;
    const uint16_t* captures[MAX_GROUPS * 2] = {0};
    int matched = 0;

    /* One context for the handle's whole lifetime, not one per call (see
     * RegexHandle above). Re-arming the budget every call also resets the
     * context's sticky exhaustion state, so one runaway subject doesn't
     * poison later execs on the same handle. */
    if (!h->ctx) h->ctx = vm_context_new(&h->prog);
    VMContext* ctx = h->ctx;
    vm_context_set_step_budget(ctx, h->step_budget);

    if (h->prog.sticky) {
        matched = vm_execute(&h->prog, ctx, 0, 1, text, text_end, text + start_index, captures);
    } else {
        int is_unicode = h->prog.unicode || h->prog.unicode_sets;
        const int use_filter = h->prog.scan_filter;
        for (int index = start_index; index <= text_units; ) {
            /* First-unit filter (see Program.scan_filter): positions whose
             * first code unit can't start any match are skipped without
             * entering the VM -- the dominant cost of a non-matching scan.
             * index == text_units (the end position) is always tried: a
             * filtered pattern can't match empty, but the bound keeps this
             * loop honoring "text need not be NUL-terminated". */
            if (use_filter && index < text_units) {
                uint16_t u = text[index];
                int admissible = (u < 128)
                    ? (h->prog.scan_ascii[u >> 3] >> (u & 7)) & 1
                    : h->prog.scan_non_ascii;
                if (!admissible) { index++; continue; }
            }
            matched = vm_execute(&h->prog, ctx, 0, 1, text, text_end, text + index, captures);
            if (matched) break;
            if (is_unicode && index < text_units &&
                text[index] >= 0xD800 && text[index] <= 0xDBFF &&
                index + 1 < text_units && text[index + 1] >= 0xDC00 && text[index + 1] <= 0xDFFF) {
                index += 2;
            } else {
                index++;
            }
        }
    }
    h->budget_exhausted = vm_context_budget_exhausted(ctx) ? 1 : 0;

    int pair_count = (h->prog.group_count + 1) * 2;
    if (!matched) {
        for (int i = 0; i < pair_count; i++) h->captures[i] = -1;
        return 0;
    }
    for (int i = 0; i <= h->prog.group_count; i++) {
        const uint16_t* start = captures[i * 2];
        const uint16_t* end = captures[i * 2 + 1];
        if (!start || !end) {
            h->captures[i * 2] = -1;
            h->captures[i * 2 + 1] = -1;
        } else {
            h->captures[i * 2] = (int32_t)(start - text);
            h->captures[i * 2 + 1] = (int32_t)(end - text);
        }
    }
    return 1;
}

/* Sets the VM step budget applied to every subsequent regex_exec() on this
 * handle (re-armed per call): 0 = unlimited (the default). This is the
 * engine's defense against catastrophic backtracking -- see
 * vm_context_set_step_budget in regexp.h for how to size it (linear in the
 * subject length, e.g. 1e6 + 2000/unit). Takes a double because JS numbers
 * cross the WASM boundary as f64; budgets are well below 2^53 so the
 * conversion is exact. Hosts running untrusted patterns should always set
 * one. */
EMSCRIPTEN_KEEPALIVE
void regex_set_step_budget(uintptr_t handle, double max_steps) {
    RegexHandle* h = (RegexHandle*)(uintptr_t)handle;
    if (!h || max_steps < 0) return;
    h->step_budget = (uint64_t)max_steps;
}

/* Whether the most recent regex_exec() on this handle stopped because it
 * exhausted the step budget (1) rather than genuinely finding no match (0).
 * Lets a host surface a catchable "pattern too expensive" error instead of
 * conflating it with no-match. */
EMSCRIPTEN_KEEPALIVE
int regex_budget_exhausted(uintptr_t handle) {
    RegexHandle* h = (RegexHandle*)(uintptr_t)handle;
    return h ? h->budget_exhausted : 0;
}

/* Pointer to (regex_group_count(handle)+1)*2 int32 values, [start0,end0,
 * start1,end1, ...] in UTF-16-code-unit offsets into the text passed to the
 * most recent regex_exec() on this handle; -1 for an unmatched group. View
 * from JS as `new Int32Array(memory.buffer, ptr, (groupCount+1)*2)`. */
EMSCRIPTEN_KEEPALIVE
const int32_t* regex_captures_ptr(uintptr_t handle) {
    RegexHandle* h = (RegexHandle*)(uintptr_t)handle;
    return h ? h->captures : NULL;
}

/* Releases everything associated with a compiled-pattern handle,
 * including the classes' heap-owned string sets. */
EMSCRIPTEN_KEEPALIVE
void regex_free(uintptr_t handle) {
    RegexHandle* h = (RegexHandle*)(uintptr_t)handle;
    if (!h) return;
    for (int i = 0; i < h->prog.class_count; i++) class_strings_free(&h->prog.classes[i]);
    vm_context_free(h->ctx);
    free(h->captures);
    free(h);
}
