/* Virtual machine: backtracking interpreter over Program.code, with an
 * explicit thread stack (not the C call stack -- except for lookaround,
 * which recurses via a genuine nested call to vm_run) and a memoizing
 * fail-cache that's the main defense against classic
 * catastrophic-backtracking patterns. All execution scratch lives in a
 * caller-provided VMContext (one per exec call, reused across every start
 * position and lookaround depth -- see regexp.h for why: per-position
 * setup used to dominate unanchored searches by two orders of magnitude).
 * See docs/ARCHITECTURE.md's "VM" section for the full picture. The
 * history here: lookaround recursion used to be this engine's most
 * serious known crash risk (a fixed ~2.2MB C-stack frame per call,
 * regardless of the pattern's actual group count, crashed the process at
 * only 3 levels of nesting) until the backtrack stack, fail-cache, and
 * per-thread captures were moved to a right-sized heap arena instead.
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
 * upstream's layout.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucd.h"
#include "regexp.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

/* Decode one UTF-16 code point starting at *sp, advancing *sp past it (2
 * units for a valid surrogate pair, 1 otherwise -- an unpaired surrogate
 * decodes as itself). Extracted from jsvm2's include/js_string.h; this is
 * the one dependency regexp.c has on that header, and it doesn't need any
 * of the JSString/interning machinery that comes with it. `limit` bounds
 * the trail-surrogate peek and is a deliberate divergence from upstream:
 * jsvm2 only decodes NUL-terminated JSStrings, but here text need not be
 * NUL-terminated, so a buffer whose last unit is a lead surrogate would
 * otherwise be read one unit past its end (a confirmed OOB read). */
static inline uint32_t decode_utf16(const uint16_t** sp, const uint16_t* limit) {
    uint32_t cp = *(*sp)++;
    if (cp >= 0xD800 && cp <= 0xDBFF && *sp < limit) {
        if (**sp >= 0xDC00 && **sp <= 0xDFFF) {
            uint32_t trail = *(*sp)++;
            return ((cp - 0xD800) << 10) + (trail - 0xDC00) + 0x10000;
        }
    }
    return cp;
}

/* Mirror of decode_utf16 for right-to-left matching (lookbehind): decode
 * one code point ending just before *sp, moving *sp back past it (2 units
 * for a valid surrogate pair whose lead is still at or after `start`, 1
 * otherwise). Extracted from four independently-maintained inline copies
 * (OP_CHAR/OP_CLASS backward, and both sides of the ignore-case
 * backreference comparison) -- that duplication was an entire class of
 * "fixed on one side, not the mirror side" bug risk, proven when the
 * forward decoder's OOB read got probed and fixed while these stayed
 * separate. Callers must ensure
 * *sp > start before calling. */
static inline uint32_t decode_utf16_backward(const uint16_t** sp, const uint16_t* start) {
    uint32_t cp = *--(*sp);
    if (cp >= 0xDC00 && cp <= 0xDFFF && *sp > start) {
        uint32_t lead = *(*sp - 1);
        if (lead >= 0xD800 && lead <= 0xDBFF) {
            cp = ((lead - 0xD800) << 10) + (cp - 0xDC00) + 0x10000;
            (*sp)--;
        }
    }
    return cp;
}

static inline bool is_word_char(uint32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* ECMA-262 GetWordCharacters: under /iu the word set additionally contains
 * every character whose simple case folding lands in [a-zA-Z0-9_] (exactly
 * U+017F and U+212A in current Unicode, but derived via the fold table
 * rather than hardcoded). `fold` is the emitted instruction's effective
 * ignoreCase (arg1) AND prog->unicode -- plain /i never widens \b. */
static inline bool is_word_char_fold(uint32_t c, bool fold) {
    if (fold) c = unicode_casefold(c);
    return is_word_char(c);
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

/* `gen` implements O(1) logical clearing: an entry counts as present only
 * if its gen matches the owning VMDepth's current generation, so "clearing"
 * the 8192-entry cache between VM entries is one integer increment instead
 * of an 8192-iteration reset -- that reset used to run once per *start
 * position* of an unanchored search. */
typedef struct { int pc; const uint16_t* sp; unsigned int gen; } CacheEntry;

/* Binary search over CharClass.ranges -- add_range (re_lexer.c) maintains
 * them sorted and coalesced, so this is a drop-in for the old linear
 * scan; \p{L} alone is ~700 ranges, paid per text position. */
static inline bool class_contains(const CharClass* cls, uint32_t cp) {
    int lo = 0, hi = cls->range_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (cp < cls->ranges[mid].start) hi = mid - 1;
        else if (cp > cls->ranges[mid].end) lo = mid + 1;
        else return true;
    }
    return false;
}

/* captures used to be a fixed MAX_GROUPS*2 (510-pointer) array embedded
 * directly in this struct -- and this struct was itself embedded 512 times
 * over in vm_execute_internal's backtrack stack, making that function's
 * own C stack frame ~2.2MB regardless of how many capture groups the
 * compiled pattern actually has. Confirmed via ASan: this crashed the
 * process after only ~3 levels of lookaround
 * recursion, since every level of nesting is a fresh, non-tail call to
 * this same function, each paying that same ~2.2MB again. Fixed by making
 * captures a pointer into a shared arena that vm_execute_internal
 * allocates once per call (see there), sized to the *pattern's actual*
 * group_count instead of the worst-case MAX_GROUPS -- both eliminating the
 * stack cost (the arena lives on the heap) and shrinking the typical-case
 * footprint by roughly two orders of magnitude (a handful of groups vs.
 * 254). The tradeoff: every place that used to rely on plain struct
 * assignment to duplicate a Thread's captures (pushing a backtrack point,
 * popping one back off) now needs an explicit copy instead -- see
 * thread_copy_state(), and its call sites below -- since assignment now
 * only copies the *pointer*, aliasing two threads' capture state instead
 * of giving each its own snapshot. */
typedef struct {
    int pc; const uint16_t* sp; const uint16_t** captures; int counters[MAX_COUNTERS]; const uint16_t* counter_sp[MAX_COUNTERS];
} Thread;

static inline unsigned int hash_state(const Thread* t, int counter_count) {
    unsigned int h = (unsigned int)((t->pc * 31) + (size_t)t->sp);
    for (int i = 0; i < counter_count; i++) h ^= (t->counters[i] * 73);
    /* CACHE_SIZE is a power of two, so masking replaces the division a
     * true % would need -- this runs once per backtrack-stack pop. */
    return h & (CACHE_SIZE - 1);
}

/* Copies src's pc/sp/counters/counter_sp, and the *data* src->captures
 * points to, into dst -- but never dst->captures itself (that pointer is
 * assigned once, into dst's own permanent slice of the shared arena, and
 * must stay put). This is what plain struct assignment (`*dst = *src;`)
 * used to do for free back when captures was an embedded array; now that
 * it's a pointer, assignment would just copy the pointer value and leave
 * dst aliasing src's capture state instead of owning an independent copy. */
static inline void thread_copy_state(Thread* dst, const Thread* src, int cap_pairs) {
    dst->pc = src->pc;
    dst->sp = src->sp;
    memcpy((void*)dst->captures, src->captures, sizeof(const uint16_t*) * (size_t)cap_pairs);
    memcpy(dst->counters, src->counters, sizeof(dst->counters));
    memcpy(dst->counter_sp, src->counter_sp, sizeof(dst->counter_sp));
}

#define VM_STACK_CAPACITY 512
/* Hard ceiling for backtrack-stack growth. The greedy-quantifier loop
 * (OP_CHECK_COUNTER) pushes one backtrack entry per iteration, so a single
 * quantifier matching a run of N units needs N live entries -- the stack
 * was a fixed Thread[512] with UNCHECKED pushes, meaning e.g. [a-z]+
 * against 600 consecutive letters wrote past its end (confirmed via ASan:
 * heap-buffer-overflow in thread_copy_state -- a text-driven memory-safety
 * bug, unlike the pattern-driven MAX_* overflow class). The
 * stack now doubles on demand up to this many entries; a match needing
 * more is abandoned as no-match (a documented engine limit, like
 * MAX_GROUPS -- there's no error channel on the exec path) rather than
 * corrupting memory. 2^19 entries * ~250B/entry caps the worst case
 * around 130MB, tolerable transiently even under WASM's default heap. */
#define VM_STACK_MAX (1 << 19)

/* Per-recursion-depth execution scratch. Lookaround runs the VM
 * recursively, and a nested run can't share the outer run's backtrack
 * stack or fail cache -- so each depth owns a set, allocated on first use
 * and kept for the context's lifetime. */
typedef struct {
    Thread* stack;
    const uint16_t** arena;            /* stack slots' capture slices */
    const uint16_t** current_captures; /* the in-flight thread's, outside the arena */
    CacheEntry* cache;
    int capacity;
    unsigned int gen; /* current fail-cache generation, see CacheEntry */
} VMDepth;

/* Reusable scratch for every VM entry within one exec call -- see
 * regexp.h's comment for why this exists (per-start-position allocation +
 * fail-cache initialization used to dominate unanchored searches by
 * orders of magnitude). depth[] is indexed by lookaround recursion depth;
 * MAX_AST_DEPTH bounds it because every lookaround level costs at least
 * one AST level, which the parser caps (MAX_AST_DEPTH, regexp.h). */
struct VMContext {
    int cap_pairs;
    /* Step budget (see regexp.h): counts every instruction executed through
     * this context, across VM entries and lookaround recursion alike, for
     * the context's lifetime. step_budget 0 = unlimited (the default via
     * calloc); budget_exhausted is sticky so an exhausted context keeps
     * refusing work instead of burning the tail of its budget once per
     * start position of a scan loop. */
    uint64_t step_budget;
    uint64_t steps_used;
    bool budget_exhausted;
    VMDepth depth[MAX_AST_DEPTH];
};

VMContext* vm_context_new(const Program* prog) {
    VMContext* ctx = calloc(1, sizeof(VMContext));
    if (!ctx) {
        fprintf(stderr, "Fatal Error: Out of memory\n");
        exit(EXIT_FAILURE);
    }
    ctx->cap_pairs = (prog->group_count + 1) * 2;
    return ctx;
}

void vm_context_set_step_budget(VMContext* ctx, uint64_t max_steps) {
    ctx->step_budget = max_steps;
    ctx->steps_used = 0;
    ctx->budget_exhausted = false;
}

bool vm_context_budget_exhausted(const VMContext* ctx) {
    return ctx->budget_exhausted;
}

void vm_context_free(VMContext* ctx) {
    if (!ctx) return;
    for (int i = 0; i < MAX_AST_DEPTH; i++) {
        free(ctx->depth[i].stack);
        free((void*)ctx->depth[i].arena);
        free((void*)ctx->depth[i].current_captures);
        free(ctx->depth[i].cache);
    }
    free(ctx);
}

/* Doubles the backtrack stack and its captures arena in tandem. Every
 * stack slot's .captures points into the arena at a fixed stride, so a
 * realloc that moves the arena invalidates all of them -- they're rebased
 * below. (The capture *data* -- pointers into the subject text -- moves
 * with the realloc and stays valid; it's only the slot->arena pointers
 * that need recomputing. The in-flight thread's captures live in their
 * own allocation, not the arena, precisely so growth never moves them.)
 * Returns false only at VM_STACK_MAX; allocation failure stays fatal,
 * matching the OOM policy everywhere else in this file. */
static bool vm_grow_stack(VMDepth* d, int cap_pairs) {
    if (d->capacity >= VM_STACK_MAX) return false;
    int new_capacity = d->capacity * 2;
    Thread* new_stack = realloc(d->stack, sizeof(Thread) * (size_t)new_capacity);
    const uint16_t** new_arena = realloc((void*)d->arena, sizeof(uint16_t*) * (size_t)cap_pairs * (size_t)new_capacity);
    if (!new_stack || !new_arena) {
        fprintf(stderr, "Fatal Error: Out of memory\n");
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < new_capacity; i++) new_stack[i].captures = new_arena + (size_t)i * cap_pairs;
    d->stack = new_stack;
    d->arena = new_arena;
    d->capacity = new_capacity;
    return true;
}

static bool vm_run(Program* prog, VMContext* ctx, int depth, int start_pc, int step, const uint16_t* original_text, const uint16_t* text_end, const uint16_t* search_start, const uint16_t** out_captures) {
    int cap_pairs = ctx->cap_pairs;
    if (depth >= MAX_AST_DEPTH) return false; /* unreachable: the parser caps nesting */
    /* Sticky budget check: once exhausted, every further entry (the scan
     * loop's next start position, an outer thread retrying a lookaround)
     * fails immediately instead of re-arming the counter. */
    if (ctx->budget_exhausted) return false;
    VMDepth* d = &ctx->depth[depth];
    if (!d->stack) {
        d->capacity = VM_STACK_CAPACITY;
        d->arena = malloc(sizeof(uint16_t*) * (size_t)cap_pairs * (size_t)d->capacity);
        d->stack = malloc(sizeof(Thread) * (size_t)d->capacity);
        d->cache = malloc(sizeof(CacheEntry) * CACHE_SIZE);
        d->current_captures = malloc(sizeof(uint16_t*) * (size_t)cap_pairs);
        if (!d->arena || !d->stack || !d->cache || !d->current_captures) {
            fprintf(stderr, "Fatal Error: Out of memory\n");
            exit(EXIT_FAILURE);
        }
        for (int i = 0; i < d->capacity; i++) d->stack[i].captures = d->arena + (size_t)i * cap_pairs;
        /* gen 0 marks every entry stale forever; real generations start at 1. */
        for (int i = 0; i < CACHE_SIZE; i++) d->cache[i].gen = 0;
        d->gen = 0;
    }
    if (++d->gen == 0) {
        /* Generation wrapped (2^32 entries at one depth in one context) --
         * hard-reset so gen-0 entries can't masquerade as current. */
        for (int i = 0; i < CACHE_SIZE; i++) d->cache[i].gen = 0;
        d->gen = 1;
    }
    const unsigned int gen = d->gen;
    Thread* stack = d->stack;
    CacheEntry* fail_cache = d->cache;
    int stack_capacity = d->capacity;
    int stack_ptr = 0;

    Thread current;
    current.captures = d->current_captures;

    stack[0].pc = start_pc;
    stack[0].sp = search_start;
    memset((void*)stack[0].captures, 0, sizeof(const uint16_t*) * (size_t)cap_pairs);
    if (out_captures) memcpy((void*)stack[0].captures, out_captures, sizeof(const uint16_t*) * (size_t)cap_pairs);
    memset(stack[0].counters, 0, sizeof(stack[0].counters));
    memset(stack[0].counter_sp, 0, sizeof(stack[0].counter_sp));
    stack_ptr = 1;

    while (stack_ptr > 0) {
        stack_ptr--;
        thread_copy_state(&current, &stack[stack_ptr], cap_pairs);

        unsigned int h = hash_state(&current, prog->counter_count);
        if (fail_cache[h].gen == gen && fail_cache[h].pc == current.pc && fail_cache[h].sp == current.sp) continue;

        int path_start_pc = current.pc; const uint16_t* path_start_sp = current.sp; bool path_failed = false;

        while (true) {
            /* One step = one instruction dispatch. Abandoning mid-path is
             * safe for the same reason the VM_STACK_MAX abandon is: all
             * scratch is context-owned and (re)initialized per entry, so a
             * plain return leaves the context reusable. */
            if (ctx->step_budget && ++ctx->steps_used > ctx->step_budget) {
                ctx->budget_exhausted = true;
                return false;
            }
            Instruction inst = prog->code[current.pc];
            /* A dense switch over the opcode enum compiles to a jump table
             * where the old if/else-if chain compiled to sequential
             * compares. The opcode bodies
             * are unchanged; their `path_failed = true; break;` idiom now
             * breaks the switch instead of this while, so the
             * `if (path_failed) break;` after the switch completes the
             * exit -- same control flow, one extra branch on failure. */
            switch (inst.op) {
            case OP_CHAR: {
                if (step > 0) {
                    if (current.sp < text_end) {
                        uint32_t cp;
                        const uint16_t* next_sp = current.sp;
                        if (prog->unicode) cp = decode_utf16(&next_sp, text_end);
                        else cp = *next_sp++;
                        
                        if (inst.arg2) {
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
                        if (prog->unicode) cp = decode_utf16_backward(&next_sp, original_text);
                        else cp = *(--next_sp);
                        if (inst.arg2) {
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
            } break;
            case OP_CLASS: {
                if (step > 0) {
                    if (current.sp < text_end) {
                        uint32_t cp;
                        const uint16_t* next_sp = current.sp;
                        if (prog->unicode) cp = decode_utf16(&next_sp, text_end);
                        else cp = *next_sp++;
                        CharClass* cls = &prog->classes[inst.arg1];
                        bool matched = class_contains(cls, cp);
                        if (cls->negated) matched = !matched;
                        if (matched) { current.pc++; current.sp = next_sp; } else { path_failed = true; break; }
                    } else { path_failed = true; break; }
                } else {
                    if (current.sp > original_text) {
                        const uint16_t* next_sp = current.sp;
                        uint32_t cp;
                        if (prog->unicode) cp = decode_utf16_backward(&next_sp, original_text);
                        else cp = *(--next_sp);
                        CharClass* cls = &prog->classes[inst.arg1];
                        bool matched = class_contains(cls, cp);
                        if (cls->negated) matched = !matched;
                        if (matched) { current.pc++; current.sp = next_sp; } else { path_failed = true; break; }
                    } else { path_failed = true; break; }
                }
            } break;
            case OP_SAVE: { current.captures[inst.arg1] = current.sp; current.pc++; } break;
            case OP_ASSERT_START: { if (current.sp == original_text || (inst.arg1 && current.sp > original_text && *(current.sp - 1) == '\n')) current.pc++; else { path_failed = true; break; } } break;
            case OP_ASSERT_END:   { if (current.sp >= text_end || (inst.arg1 && *current.sp == '\n')) current.pc++; else { path_failed = true; break; } } break;
            case OP_WORD_BOUNDARY: {
                /* The sp < text_end guard mirrors OP_ASSERT_END's: text need
                 * not be NUL-terminated (text_units is authoritative, per
                 * README), so end-of-text must be detected by bound, not by
                 * reading a terminator (a confirmed OOB read one past a
                 * tightly-sized buffer). */
                bool fold = inst.arg1 && prog->unicode;
                bool left_is_word = (current.sp > original_text) && is_word_char_fold(*(current.sp - 1), fold);
                bool right_is_word = (current.sp < text_end) && is_word_char_fold(*current.sp, fold);
                if (left_is_word != right_is_word) current.pc++; else { path_failed = true; break; }
            } break;
            case OP_NON_WORD_BOUNDARY: {
                bool fold = inst.arg1 && prog->unicode;
                bool left_is_word = (current.sp > original_text) && is_word_char_fold(*(current.sp - 1), fold);
                bool right_is_word = (current.sp < text_end) && is_word_char_fold(*current.sp, fold);
                if (left_is_word == right_is_word) current.pc++; else { path_failed = true; break; }
            } break;
            case OP_BACKREF:
            case OP_NAMED_BACKREF: {
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
                    /* A backref compare is O(capture length) inside one
                     * instruction dispatch; charge it against the budget so
                     * long captures can't multiply per-step work past the
                     * bound the budget is meant to enforce. */
                    if (ctx->step_budget) {
                        ctx->steps_used += (uint64_t)(end - start);
                        if (ctx->steps_used > ctx->step_budget) {
                            ctx->budget_exhausted = true;
                            return false;
                        }
                    }
                    bool match = true;
                    if (inst.arg2) {
                        const uint16_t* temp_sp = current.sp;
                        if (step > 0) {
                            const uint16_t* temp_start = start;
                            while (temp_start < end) {
                                if (temp_sp >= text_end) { match = false; break; }
                                uint32_t cp1, cp2;
                                if (prog->unicode) {
                                    /* The capture-side decode is bounded by the
                                     * capture's own end, not text_end: a capture
                                     * ending in a lone lead surrogate must decode
                                     * it as itself, not pair it with whatever
                                     * text unit happens to follow the capture. */
                                    cp1 = decode_utf16(&temp_sp, text_end);
                                    cp2 = decode_utf16(&temp_start, end);
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
                                    /* Like the forward direction above, the
                                     * capture-side decode is bounded by the
                                     * capture's own start, not the text's. */
                                    cp1 = decode_utf16_backward(&temp_sp, original_text);
                                    cp2 = decode_utf16_backward(&temp_end, start);
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
            } break;
            case OP_LOOKAHEAD: {
                /* Sized to this pattern's actual cap_pairs (a small,
                 * on-C-stack VLA), not MAX_GROUPS*2 -- unlike the arenas
                 * above, this one's small enough (at most 510 pointers,
                 * typically far fewer) that there's no need to move it off
                 * the stack too; it's a single instance per instruction
                 * dispatch, not one per backtrack-stack slot. */
                const uint16_t* temp_captures[cap_pairs];
                memcpy(temp_captures, current.captures, sizeof(temp_captures));
                bool la_match = vm_run(prog, ctx, depth + 1, inst.arg1, 1, original_text, text_end, current.sp, temp_captures);
                if (la_match) {
                    memcpy((void*)current.captures, temp_captures, sizeof(temp_captures));
                    current.pc++;
                } else { path_failed = true; break; }
            } break;
            case OP_NEG_LOOKAHEAD: {
                const uint16_t* temp_captures[cap_pairs];
                memcpy(temp_captures, current.captures, sizeof(temp_captures));
                bool la_match = vm_run(prog, ctx, depth + 1, inst.arg1, 1, original_text, text_end, current.sp, temp_captures);
                if (!la_match) {
                    current.pc++;
                } else { path_failed = true; break; }
            } break;
            case OP_LOOKBEHIND:
            case OP_NEG_LOOKBEHIND: {
                const uint16_t* temp_captures[cap_pairs];
                memcpy(temp_captures, current.captures, sizeof(temp_captures));
                bool lb_match = vm_run(prog, ctx, depth + 1, inst.arg1, -1, original_text, text_end, current.sp, temp_captures);
                if ((inst.op == OP_LOOKBEHIND && lb_match) || (inst.op == OP_NEG_LOOKBEHIND && !lb_match)) {
                    if (lb_match) memcpy((void*)current.captures, temp_captures, sizeof(temp_captures));
                    current.pc++;
                } else {
                    path_failed = true; break;
                }
            } break;
            case OP_CLEAR_CAPTURES: {
                /* Bound against this pattern's own group_count, not
                 * MAX_GROUPS -- captures is now sized to cap_pairs, not
                 * always the worst-case MAX_GROUPS*2 (see the Thread
                 * comment above), so the old `i < MAX_GROUPS` bound would
                 * write past the end of a smaller pattern's arena slice.
                 * Emitted by compile_node's AST_QUANTIFIER case at the top
                 * of every loop iteration (ECMA-262 RepeatMatcher). */
                for (int i = inst.arg1; i <= inst.arg2 && i <= prog->group_count; i++) {
                    current.captures[i * 2] = NULL;
                    current.captures[i * 2 + 1] = NULL;
                }
                current.pc++;
            } break;
            case OP_SPLIT: {
                /* Every push site checks capacity first; hitting VM_STACK_MAX
                 * abandons the whole match (see the define above). Growth
                 * reallocates through the VMDepth, so refresh the locals. */
                if (stack_ptr == stack_capacity) {
                    if (!vm_grow_stack(d, cap_pairs)) return false;
                    stack = d->stack;
                    stack_capacity = d->capacity;
                }
                thread_copy_state(&stack[stack_ptr], &current, cap_pairs);
                stack[stack_ptr].pc = inst.arg2;
                stack_ptr++;
                current.pc = inst.arg1;
            } break;
            case OP_INIT_COUNTER: { current.counters[inst.arg1] = 0; current.counter_sp[inst.arg1] = NULL; current.pc++; } break;
            case OP_INC_COUNTER:  { current.counters[inst.arg1]++; current.pc++; } break;
            case OP_CHECK_COUNTER: {
                int c = current.counters[inst.arg1], min = inst.arg2, max = inst.arg3, exit_pc = inst.arg4;
                if (c > 0 && current.counter_sp[inst.arg1] == current.sp && c >= min) {
                    current.pc = exit_pc;
                } else {
                    current.counter_sp[inst.arg1] = current.sp;
                    if (c < min) current.pc++;
                    else if (max != -1 && c == max) current.pc = exit_pc;
                    else {
                        if (stack_ptr == stack_capacity) {
                            if (!vm_grow_stack(d, cap_pairs)) return false;
                            stack = d->stack;
                            stack_capacity = d->capacity;
                        }
                        if (inst.lazy) {
                            thread_copy_state(&stack[stack_ptr], &current, cap_pairs);
                            stack[stack_ptr].pc = current.pc + 1;
                            stack_ptr++;
                            current.pc = exit_pc;
                        } else {
                            thread_copy_state(&stack[stack_ptr], &current, cap_pairs);
                            stack[stack_ptr].pc = exit_pc;
                            stack_ptr++;
                            current.pc++;
                        }
                    }
                }
            } break;
            case OP_JMP: { current.pc = inst.arg1; } break;
            case OP_MATCH: {
                if (out_captures) memcpy((void*)out_captures, current.captures, sizeof(const uint16_t*) * (size_t)cap_pairs);
                return true;
            }
            default:
                /* Unreachable with well-formed bytecode; failing the path
                 * beats the if/else chain's old behavior (silent infinite
                 * loop on an unknown opcode). */
                path_failed = true;
                break;
            }
            if (path_failed) break;
        }
        if (path_failed) { fail_cache[h].pc = path_start_pc; fail_cache[h].sp = path_start_sp; fail_cache[h].gen = gen; }
    }
    return false;
}

bool vm_execute(Program* prog, VMContext* ctx, int start_pc, int step, const uint16_t* original_text, const uint16_t* text_end, const uint16_t* search_start, const uint16_t** out_captures) {
    return vm_run(prog, ctx, 0, start_pc, step, original_text, text_end, search_start, out_captures);
}

bool vm_execute_internal(Program* prog, int start_pc, int step, const uint16_t* original_text, const uint16_t* text_end, const uint16_t* search_start, const uint16_t** out_captures) {
    VMContext* ctx = vm_context_new(prog);
    bool result = vm_run(prog, ctx, 0, start_pc, step, original_text, text_end, search_start, out_captures);
    vm_context_free(ctx);
    return result;
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
