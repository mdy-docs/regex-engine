/* Virtual machine: backtracking interpreter over Program.code, with an
 * explicit thread stack (not the C call stack -- except for lookaround,
 * which recurses via a genuine nested call to vm_execute_internal) and a
 * memoizing fail-cache that's the main defense against classic
 * catastrophic-backtracking patterns. See docs/ARCHITECTURE.md's "VM"
 * section for the full picture, and docs/IMPROVEMENTS.md #1.1 for the
 * history here: that lookaround recursion used to be this engine's most
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
 * upstream's layout, and docs/IMPROVEMENTS.md section 4 for the rationale.
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
 * otherwise be read one unit past its end (docs/IMPROVEMENTS.md #1.5,
 * confirmed OOB read). */
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

/* captures used to be a fixed MAX_GROUPS*2 (510-pointer) array embedded
 * directly in this struct -- and this struct was itself embedded 512 times
 * over in vm_execute_internal's backtrack stack, making that function's
 * own C stack frame ~2.2MB regardless of how many capture groups the
 * compiled pattern actually has. Confirmed via ASan (docs/IMPROVEMENTS.md
 * #1.1): this crashed the process after only ~3 levels of lookaround
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
    return h % CACHE_SIZE;
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

bool vm_execute_internal(Program* prog, int start_pc, int step, const uint16_t* original_text, const uint16_t* text_end, const uint16_t* search_start, const uint16_t** out_captures) {
    int cap_pairs = (prog->group_count + 1) * 2;

    /* One arena for every backtrack-stack thread's captures, plus one more
     * slice for `current` (the thread actively being stepped through
     * instructions, outside the stack array) -- see the Thread/
     * thread_copy_state comments above for why this, and the stack/
     * fail_cache arrays below, are heap-allocated here rather than being
     * plain C-stack locals like they used to be: this function recurses
     * into itself for every OP_LOOKAHEAD/OP_LOOKBEHIND, so whatever these
     * cost used to be paid again on the C stack at every nesting level. */
    const uint16_t** captures_arena = malloc(sizeof(uint16_t*) * (size_t)cap_pairs * (VM_STACK_CAPACITY + 1));
    Thread* stack = malloc(sizeof(Thread) * VM_STACK_CAPACITY);
    CacheEntry* fail_cache = malloc(sizeof(CacheEntry) * CACHE_SIZE);
    if (!captures_arena || !stack || !fail_cache) {
        fprintf(stderr, "Fatal Error: Out of memory\n");
        exit(EXIT_FAILURE);
    }
    int stack_ptr = 0;
    bool result = false;

    for (int i = 0; i < VM_STACK_CAPACITY; i++) stack[i].captures = captures_arena + (size_t)i * cap_pairs;
    for (int i = 0; i < CACHE_SIZE; i++) fail_cache[i].pc = -1;

    Thread current;
    current.captures = captures_arena + (size_t)VM_STACK_CAPACITY * cap_pairs;

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
        if (fail_cache[h].pc == current.pc && fail_cache[h].sp == current.sp) continue;

        int path_start_pc = current.pc; const uint16_t* path_start_sp = current.sp; bool path_failed = false;
        
        while (true) {
            Instruction inst = prog->code[current.pc];
            if (inst.op == OP_CHAR) {
                if (step > 0) {
                    if (current.sp < text_end) {
                        uint32_t cp;
                        const uint16_t* next_sp = current.sp;
                        if (prog->unicode) cp = decode_utf16(&next_sp, text_end);
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
                        if (prog->unicode) cp = decode_utf16(&next_sp, text_end);
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
                /* The sp < text_end guard mirrors OP_ASSERT_END's: text need
                 * not be NUL-terminated (text_units is authoritative, per
                 * README), so end-of-text must be detected by bound, not by
                 * reading a terminator (docs/IMPROVEMENTS.md #1.4, confirmed
                 * OOB read one past a tightly-sized buffer). */
                bool left_is_word = (current.sp > original_text) && is_word_char(*(current.sp - 1));
                bool right_is_word = (current.sp < text_end) && is_word_char(*current.sp);
                if (left_is_word != right_is_word) current.pc++; else { path_failed = true; break; }
            }
            else if (inst.op == OP_NON_WORD_BOUNDARY) {
                bool left_is_word = (current.sp > original_text) && is_word_char(*(current.sp - 1));
                bool right_is_word = (current.sp < text_end) && is_word_char(*current.sp);
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
                /* Sized to this pattern's actual cap_pairs (a small,
                 * on-C-stack VLA), not MAX_GROUPS*2 -- unlike the arenas
                 * above, this one's small enough (at most 510 pointers,
                 * typically far fewer) that there's no need to move it off
                 * the stack too; it's a single instance per instruction
                 * dispatch, not one per backtrack-stack slot. */
                const uint16_t* temp_captures[cap_pairs];
                memcpy(temp_captures, current.captures, sizeof(temp_captures));
                bool la_match = vm_execute_internal(prog, inst.arg1, 1, original_text, text_end, current.sp, temp_captures);
                if (la_match) {
                    memcpy((void*)current.captures, temp_captures, sizeof(temp_captures));
                    current.pc++;
                } else { path_failed = true; break; }
            }
            else if (inst.op == OP_NEG_LOOKAHEAD) {
                const uint16_t* temp_captures[cap_pairs];
                memcpy(temp_captures, current.captures, sizeof(temp_captures));
                bool la_match = vm_execute_internal(prog, inst.arg1, 1, original_text, text_end, current.sp, temp_captures);
                if (!la_match) {
                    current.pc++;
                } else { path_failed = true; break; }
            }
            else if (inst.op == OP_LOOKBEHIND || inst.op == OP_NEG_LOOKBEHIND) {
                const uint16_t* temp_captures[cap_pairs];
                memcpy(temp_captures, current.captures, sizeof(temp_captures));
                bool lb_match = vm_execute_internal(prog, inst.arg1, -1, original_text, text_end, current.sp, temp_captures);
                if ((inst.op == OP_LOOKBEHIND && lb_match) || (inst.op == OP_NEG_LOOKBEHIND && !lb_match)) {
                    if (lb_match) memcpy((void*)current.captures, temp_captures, sizeof(temp_captures));
                    current.pc++;
                } else {
                    path_failed = true; break;
                }
            }
            else if (inst.op == OP_CLEAR_CAPTURES) {
                /* Bound against this pattern's own group_count, not
                 * MAX_GROUPS -- captures is now sized to cap_pairs, not
                 * always the worst-case MAX_GROUPS*2 (see the Thread
                 * comment above), so the old `i < MAX_GROUPS` bound would
                 * write past the end of a smaller pattern's arena slice.
                 * (This opcode is defined and implemented but never
                 * actually emitted by the compiler today -- see
                 * docs/IMPROVEMENTS.md #1.8 -- so this bound is currently
                 * unreachable in practice; fixed anyway rather than left
                 * as a latent landmine for whenever #1.8 gets addressed.) */
                for (int i = inst.arg1; i <= inst.arg2 && i <= prog->group_count; i++) {
                    current.captures[i * 2] = NULL;
                    current.captures[i * 2 + 1] = NULL;
                }
                current.pc++;
            }
            else if (inst.op == OP_SPLIT) {
                thread_copy_state(&stack[stack_ptr], &current, cap_pairs);
                stack[stack_ptr].pc = inst.arg2;
                stack_ptr++;
                current.pc = inst.arg1;
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
            }
            else if (inst.op == OP_JMP) { current.pc = inst.arg1; }
            else if (inst.op == OP_MATCH) {
                if (out_captures) memcpy((void*)out_captures, current.captures, sizeof(const uint16_t*) * (size_t)cap_pairs);
                result = true;
                goto cleanup;
            }
        }
        if (path_failed) { fail_cache[h].pc = path_start_pc; fail_cache[h].sp = path_start_sp; }
    }
cleanup:
    free(captures_arena);
    free(stack);
    free(fail_cache);
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
