# Improvement analysis

A structural/quality/performance/testing/correctness pass over this repo as
of commit `862bf88`. Read `docs/ARCHITECTURE.md` first if you haven't —
this doc assumes it.

**Methodology:** every finding below marked with a repro was verified, not
inferred — either by compiling a targeted probe against
`src/regexp.c`/`src/regex_wasm.c` under `-fsanitize=address` (heap/stack
corruption findings) or by diffing this engine's output against Node's
built-in regex engine for the same pattern/flags/input (behavioral/spec-
compliance findings). None of this is theoretical; every "confirmed" item
below has a command that reproduces it.

**A note on file references below:** sections 1–3 were written against the
single-file `src/regexp.c` as it stood at commit `862bf88`, before the
lexer/parser/compiler/VM split described in section 4 (now resolved — see
`docs/ARCHITECTURE.md` for the current `re_lexer.c`/`re_parser.c`/
`re_compiler.c`/`re_vm.c`/`re_internal.h` layout). Prose references to named
functions have been updated to their current file and line; the verbatim
ASan/crash-dump transcripts quoted as evidence have deliberately **not**
been touched or re-run against the split files — they're the historical
record of exactly what was reproduced and when, and rewriting them to
show fabricated post-split line numbers would misrepresent them as
freshly-captured output. If you need to re-run one of those repros against
the current layout, the underlying bug (all still open as of this split)
is unaffected by the reorganization — only its file:line address moved,
predictably, along the mapping `docs/ARCHITECTURE.md`'s intro describes.

## Priority summary

| # | Finding | Severity | Confirmed |
|---|---|---|---|
| 1.1 | 3 nested lookarounds crashes the process (stack overflow) | **P0 — crash/DoS** | ✅ ASan + plain build — **FIXED** |
| 1.2 | `MAX_OPCODES`/`MAX_CLASSES`/`MAX_GROUPS`/`MAX_COUNTERS` never bounds-checked → heap/stack corruption | **P0 — memory corruption** | ✅ ASan — **FIXED** |
| 1.3 | Flat (non-nested) long patterns also stack-overflow via linear AST recursion | **P0 — crash/DoS** | ✅ ASan — **FIXED** |
| 1.4 | `\b`/`\B` read one code unit past `text_end` unconditionally | P1 — OOB read | ✅ ASan — **FIXED** |
| 1.5 | `decode_utf16` reads past buffer end for a trailing lone lead surrogate | P1 — OOB read | ✅ ASan — **FIXED** |
| 1.6 | Backreference bounds only validated under `/u` — `\999` OOB-reads in Annex B mode | P1 — OOB read | ✅ ASan — **FIXED** |
| 1.7 | Non-unicode `.`/`\D`/`\W`/`\S` wrongly capped at codepoint 255 instead of 0xFFFF | P1 — wrong results | ✅ diffed vs Node — **FIXED** |
| 1.8 | `OP_CLEAR_CAPTURES` defined + VM-implemented but never emitted — stale captures leak across alternation | P2 — wrong results | ✅ diffed vs Node |
| 1.9 | Assorted silent-truncation spots | P3 — minor | inspection |

Sections 2–5 below cover performance, testing, and code-quality findings
that aren't safety bugs but are worth knowing.

## Fixed since this analysis

Two additional correctness bugs (found in a follow-up pass, same
verify-before-claiming methodology) have been fixed, each with permanent
regression coverage added to `test/smoke.c`:

- **Non-`/u` `/i` matching only folded ASCII.** Real JS folds the full BMP
  even without the `/u` flag (`/ä/i.test('Ä')`, `/стол/i.test('СТОЛ')` are
  both `true` in Node); this engine's non-unicode ignore-case path only
  handled `A-Z`/`a-z` at all five call sites that implement it (character-
  class expansion, `OP_CHAR` forward/backward, backreference forward/
  backward). Fixed by `annexb_canonicalize` (`src/re_vm.c:62` — it lives
  with the VM, not next to `apply_case_folding` in `src/re_lexer.c`, since
  it's only ever called from match-time code; see `docs/ARCHITECTURE.md`),
  which implements ECMA-262's actual non-Unicode
  `Canonicalize` operation via the generated `UCD_SIMPLE_UPPERCASE` table,
  including its "don't fold across the ASCII boundary" exception (verified
  against real JS: `KELVIN SIGN U+212A` doesn't fold to/from `k`/`K`;
  `LATIN SMALL LETTER LONG S U+017F` doesn't fold to/from `s`/`S`, even
  though its simple uppercase mapping is literally `S`). Verified correct
  against **~10,000 real-Node-checked (pattern-codepoint, text-codepoint)
  pairs** spanning Basic Latin, Latin-1 Supplement, Latin Extended-A/B,
  Greek, Cyrillic, and Latin Extended Additional — zero mismatches.

- **`/v` mode "properties of strings" (`\p{RGI_Emoji}`, `\p{Basic_Emoji}`,
  etc.) silently matched nothing.** Root cause turned out to be two
  layered bugs, not one: (a) `fill_unicode_property` never read
  `UCDProperty.sequences` at all, so multi-codepoint sequence data was
  simply discarded; and (b), more fundamentally, **the VM had no matching
  support for `CharClass.strings` whatsoever** — confirmed by grep, that
  field was written but never read anywhere in `vm_execute_internal`. This
  means the pre-existing `\q{...}` character-class string-alternative
  syntax (a `/v`-mode feature that predates this fix) was *also* silently
  broken in exactly the same way, not just the Unicode-property case.
  Fixed by (a) copying `sequences` into `CharClass.strings` in
  `fill_unicode_property` (`src/re_lexer.c:342`), and (b) a new
  `compile_class_with_strings` (`src/re_compiler.c:42`, by `compile_node`)
  that compiles a class containing
  strings into a real alternation (`OP_SPLIT`/`OP_CHAR`/`OP_JMP` chains,
  one branch per string, falling back to a plain `OP_CLASS` branch for any
  ordinary single-codepoint ranges in the same class) instead of inventing
  a new opcode — this reuses the VM's already-correct backtracking rather
  than adding a second, independent matching mode to get right. Also fixed:
  negating a property of strings (`\P{Basic_Emoji}`, `[\P{Basic_Emoji}]`)
  now correctly fails to compile, matching real engines, instead of
  silently doing nothing.

  **Residual limitation surfaced by this fix, not introduced by it:**
  `CharClass.strings` is a fixed `[128]` array (same size `\q{...}` parsing
  already enforced before this fix). Several real Unicode properties have
  far more sequences than that — `RGI_Emoji` has 2604, `RGI_Emoji_ZWJ_Sequence`
  1468, `RGI_Emoji_Modifier_Sequence` 655, `RGI_Emoji_Flag_Sequence` 259,
  `Basic_Emoji` 207 — so those specific properties now match their first
  128 sequences (in generation order) rather than silently matching
  nothing, but still don't match *every* valid sequence. Raising the cap to
  fit the largest property in full (2604) would add well over 10MB to
  `Program` (already the dominant cost at ~2MB, per `docs/ARCHITECTURE.md`)
  for a single class slot — the same fixed-size-vs-unbounded-input tension
  as finding 1.2 above, not a new problem, and out of scope for this fix to
  resolve. Worth knowing if a consuming project leans on one of the five
  large properties named above expecting full coverage.

- **Nested capture groups were numbered backwards.** The parser assigned
  `node->id = ++group_count` only *after* a group's body had been parsed,
  so ids followed closing-paren order — in `((a)b)`, the inner `(a)` was
  group 1 and the outer group 2, the reverse of ECMA-262's opening-paren
  numbering (Node: `/((a)b)/.exec('ab')` → group 1 `"ab"`, group 2 `"a"`).
  Wrong for *any* pattern with nested captures, in three observable ways:
  reported capture indices, numeric-backreference resolution (`\2` bound
  to the wrong paren), and the group-index→name table for nested named
  groups. Sibling-only patterns — which happened to be all the test suite
  exercised — were unaffected, which is how it survived this long; it was
  exposed by #1.8's verification, whose Node-diffed expectations came out
  systematically "swapped" for nested-group cases. Fixed by claiming the
  id at the opening paren, before `parse_alt` descends into the body
  (`re_parser.c`). Verified against Node: 2- and 4-deep nesting, `\3`
  resolving to the innermost group, nested named groups' index/name
  mapping, and sibling numbering unchanged. **This one is
  upstream-relevant**: jsvm2's parser has the same post-body assignment,
  so it mis-numbers nested groups the same way (see README "Provenance").
  Regression coverage in `test/smoke.c`.

---

## 1. Correctness & memory safety

### 1.1 [P0] Nested lookaround crashes the process at depth 3 — **FIXED**

```c
// (?=(?=(?=x))) -- three nested positive lookaheads, matched against "x"
```

```sh
$ ulimit -s        # 8192 (8MB, the common default on macOS/Linux)
$ ./probe_nest 3
Segmentation fault
```

**Root cause:** every lookaround (`OP_LOOKAHEAD`/`OP_NEG_LOOKAHEAD`/
`OP_LOOKBEHIND`/`OP_NEG_LOOKBEHIND`) is implemented as a genuine recursive
call to `vm_execute_internal` (`re_vm.c:309` onward), and that function's
stack frame is dominated by two large fixed-size locals declared on entry
regardless of how deep the search actually goes:

```c
Thread stack[512];              // sizeof(Thread) ≈ 4.3KB (mostly the
                                 // captures[MAX_GROUPS*2] = 510-pointer array)
                                 // => ~2.2MB just for this array
CacheEntry fail_cache[CACHE_SIZE]; // CACHE_SIZE=8192 * 16B ≈ 131KB
```

ASan's own frame dump for this function confirms the frame size directly:
`stack` occupies `[32, 2195488)` — **~2.1MB** — and the whole frame is
**~2.2MB**. On an 8MB thread stack, that's room for roughly 3–4 recursive
calls before overflow, confirmed empirically above (n=2 succeeds, n=3
segfaults). This is independent of pattern complexity — it's pure lookaround
*nesting depth*, and 3 is a completely ordinary nesting depth for a
hand-written pattern, not an adversarial one.

**Why this matters more than the other findings:** this isn't a fixed-size-
buffer-overflow-on-unusual-input bug, it's "the engine cannot safely
evaluate a common, syntactically simple pattern shape at all" on a default
stack. Any consumer that lets a user supply a pattern (the web demo this
task also asks for, or any embedding project) needs either a much smaller
per-call frame or a hard nesting-depth limit enforced *before* this can be
hit, or a bigger dedicated stack for whatever thread/worker calls into this
engine.

**It's worse under WASM than natively, until you configure it.** The parser
recurses too (see 1.3) — `parse_primary` → `parse_alt` → `parse_concat` →
`parse_quantifier` is a 4-deep call chain per nested group of *any* kind,
not just lookaround — so compiling `(?=(?=(?=...)))` alone (before any
matching happens) already recurses roughly 4×depth C frames deep.
Emscripten's **default WASM stack is 64KB**, versus a typical native
thread's 8MB, so the exact same pattern that needs ~3 levels of nesting to
crash a native 8MB-stack build crashed **during `regex_compile`, at just 8
levels of nesting**, under this project's default `make wasm` build —
confirmed directly. Worse: without `-s STACK_OVERFLOW_CHECK`, a WASM stack
overflow doesn't necessarily trap cleanly at all; it can silently walk the
stack pointer into other linear memory and corrupt state instead of
throwing, since WASM only traps on an actual out-of-bounds *memory access*,
not on the stack pointer itself moving past its logical region.

**Mitigation already applied to this repo's `make wasm` build** (see
`Makefile`): `-s STACK_OVERFLOW_CHECK=2` makes stack exhaustion a catchable
`WebAssembly.RuntimeError` instead of silent corruption, and
`-s STACK_SIZE=8388608` raises the WASM stack to 8MB to match a typical
native default, pushing the practical crash threshold back out to roughly
the same depth as a native build (confirmed: 5 levels of `(?=` nesting
now compiles and matches fine; ~20 still overflows — this raises the
threshold, it does not remove it). `web/app.js` additionally catches that
`RuntimeError` and transparently re-instantiates a fresh module (confirmed
safe: a brand-new `createRegexEngineModule()` instance works normally
immediately after a caught trap in a different instance) so the demo
degrades to an error message instead of a dead page. **This is a build-
level and host-level stopgap, not a fix** — the source-level fixes below
are still the real solution, and any *other* embedder of this engine
(native, or WASM without these flags) is still exposed.

**Fixed** by exactly the two source-level changes suggested above (1 and
2 — see `re_vm.c`):

1. **`Thread.captures` is now a pointer, not an embedded `MAX_GROUPS * 2`
   array**, sized per-call to `cap_pairs = (prog->group_count + 1) * 2` —
   the pattern's *actual* group count, not the worst case. It points into
   a shared arena `vm_execute_internal` allocates once per call (one slice
   per backtrack-stack slot, plus one more for `current`, the thread
   actively being stepped through instructions outside the stack array).
   Since a plain struct assignment now only copies a *pointer* (aliasing
   two threads' capture state instead of duplicating it), every push/pop
   that used to rely on `Thread` value semantics — `OP_SPLIT`,
   `OP_CHECK_COUNTER`'s two backtrack pushes, and popping the next thread
   off the stack — now goes through a small `thread_copy_state()` helper
   that copies the *data* `.captures` points to, into the destination
   thread's own already-assigned slice, explicitly.
2. **The backtrack stack (`Thread[512]`) and fail-cache
   (`CacheEntry[CACHE_SIZE]`) are heap-allocated**, not C-stack locals —
   `malloc`'d at the top of `vm_execute_internal` and `free`'d on every
   exit path (a single `goto cleanup` target, since there are now two
   return points: `OP_MATCH` succeeding, and the loop running out of
   threads). Lookaround's recursive call into this same function now costs
   heap, not C-stack, per nesting level.

(3) — a hard nesting-depth cap as a backstop — was **not** added on top:
with (1) and (2) in place, this function's own C-stack frame no longer
scales with `MAX_GROUPS` or the backtrack-stack/cache sizes at all (just a
handful of small locals), so nesting depth is no longer the scarce
resource a backstop would need to protect. The three `OP_LOOKAHEAD`/
`OP_NEG_LOOKAHEAD`/`OP_LOOKBEHIND`/`OP_NEG_LOOKBEHIND` blocks' own
`temp_captures` scratch buffer is sized to `cap_pairs` too (a small,
per-instruction-dispatch C-stack VLA — at most 510 pointers, nowhere near
large enough to need heap allocation the way the once-per-call arena did).
Separately, `docs/IMPROVEMENTS.md #1.3`'s `MAX_AST_DEPTH` already caps how
deeply a *pattern's source syntax* can nest at parse time (200, for a
different reason — bounding the parser's and the AST-walkers' own
recursion), which incidentally also caps lookaround nesting specifically,
but that's not what makes *this* fix safe; this fix stands on its own
regardless of that cap's value.

**Verified** (ASan + UBSan, zero warnings under `-Wall -Wextra`, plus
macOS `leaks --atExit`: **0 leaks for 0 total leaked bytes** across every
scenario below — LeakSanitizer itself isn't supported on macOS, so `leaks`
stood in for it): the original repro (`(?=(?=(?=x)))`, 3 levels) now
compiles and matches correctly instead of crashing; nesting pushed to 199
levels (the edge of `MAX_AST_DEPTH`) compiles and matches correctly with
no crash; capture groups *inside* a lookahead round-trip correctly through
the recursive call's `temp_captures` (confirmed both in isolation and
combined with 50 levels of nesting) — right-sizing `Thread.captures`
didn't corrupt capture data; 2000 repeated compile/exec/free cycles with
nested lookahead completed cleanly. Also reconfirmed against the actual
compiled WASM artifact directly (not just native): the `web/`
demo's own "known crash" preset (40 levels of `(?=`, previously the
concrete trigger `web/app.js`'s crash-recovery UI was built to catch) now
simply compiles and matches, no `RuntimeError`, no reload. Permanent
regression coverage for all of the above in `test/smoke.c`.

**What this changes about the mitigations already in place.** The
WASM-build stopgaps described above (`-s STACK_OVERFLOW_CHECK=2`,
`-s STACK_SIZE=8388608` in the `Makefile`, and `web/app.js`'s catch-and-
reload) are no longer load-bearing *for this specific finding* — the
pattern that used to need them to fail safely under WASM now just works.
They're being left in place regardless: `STACK_OVERFLOW_CHECK` in
particular is generically valuable defense-in-depth against *any* future
stack issue (this fix doesn't make the engine immune to stack overflow in
general, just this specific, previously-worst offender), and removing
already-working safety nets isn't worth the marginal build-size/perf cost
of keeping them. `web/`'s "known crash" example pattern and footer copy
should be updated separately to stop describing something that no longer
reproduces — tracked as a follow-up, not done as part of this fix.

### 1.2 [P0] `MAX_*` bounds are never checked — heap/stack corruption — **FIXED**

`include/regexp.h` defines `MAX_OPCODES=16384`, `MAX_CLASSES=64`,
`MAX_GROUPS=255`, `MAX_COUNTERS=16`, all as fixed array sizes inside
`Program` (heap-allocated) or `Thread` (stack-allocated, see 1.1). Every
increment of the corresponding counter was unchecked:

- `emit()` (`re_compiler.c`): `prog->code[idx] = ...; idx = prog->code_count++;` — no check against `MAX_OPCODES`.
- Character-class allocation (`re_lexer.c`, 4 call sites): `int cid = lexer->prog->class_count++;` then `memset(&lexer->prog->classes[cid], ...)` — no check against `MAX_CLASSES`.
- Group numbering (`re_parser.c`): `node->id = ++lexer->prog->group_count;` — no check against `MAX_GROUPS`.
- Counter allocation (`re_compiler.c`): `int counter_id = prog->counter_count++;` — no check against `MAX_COUNTERS`.

**Confirmed repro (character classes, heap corruption via `memset` overrun):**
```c
// pattern: "[a]" repeated 70 times (MAX_CLASSES = 64)
```
```
==ERROR: AddressSanitizer: heap-buffer-overflow ... WRITE of size 25100
    #0 __asan_memset
    #1 next_token regexp.c:1046
```
25KB write past the end of a heap-allocated `Program` — this overruns
`classes[]` and corrupts whatever else the allocator placed after it on
the heap. This is memory corruption from pattern *text alone*, no special
input string needed.

**Confirmed repro (counters, stack corruption):**
```c
// pattern: 20 bounded quantifiers like "a{1,2}b{1,2}c{1,2}..." (MAX_COUNTERS = 16)
```
```
==ERROR: AddressSanitizer: stack-buffer-overflow ... WRITE of size 8
    #0 vm_execute_internal regexp.c:1758   (OP_INIT_COUNTER: current.counters[inst.arg1] = 0)
```

**Fixed** by bounds-checking all four allocation sites, each the same
shape: check the counter against its `MAX_*` bound *before* using it as an
array index; if the pattern has already hit the limit, set `prog->error`
(if not already set — the first failure wins) and return a clamped, always-
in-bounds index (the last valid slot) instead of writing OOB. This keeps
every subsequent `emit()`/`add_range()`/etc. call safe even after the limit
is hit, since a `Program` with `prog->error` set is discarded by
`regex_compile()` without ever being executed (`src/regex_wasm.c`) — the
resulting reused-slot bytecode is nonsensical but never read.

- `re_compiler.c`'s `emit()` and the `AST_QUANTIFIER` counter allocation:
  `code_count >= MAX_OPCODES` / `counter_count >= MAX_COUNTERS` before
  allocating — no reserved-slot subtlety, straightforward.
- `re_lexer.c`'s four `class_count++` sites consolidated into one new
  `alloc_class()` helper with the `class_count >= MAX_CLASSES` check,
  replacing the repeated allocate-then-`memset` pattern at each call site.
- `re_parser.c`'s group numbering needed one extra subtlety: capture group
  id 0 is reserved for "whole match" everywhere else in the engine
  (`group_names[0]` is never a real named group;
  `captures[0]`/`captures[1]` are the whole-match span, not any capture
  group's), so it already occupies one of `MAX_GROUPS`'s 255 array slots
  even though real capture groups are only ever assigned ids 1 and up. The
  correct ceiling is therefore `group_count < MAX_GROUPS - 1` (254 usable
  capture groups), not `< MAX_GROUPS` — the naive version would have let
  `node->id` reach 255, one past the end of both `group_names[MAX_GROUPS]`
  and (doubled) `captures[MAX_GROUPS*2]`.

**Verified** (all under ASan+UBSan, zero warnings under `-Wall -Wextra`):
exact-boundary precision for all four resources — 64 classes/16 counters
compile successfully, 65/17 fail cleanly with a message naming the
resource; 255 capture groups (one past the real 254 ceiling) fails cleanly;
a single `\p{RGI_Emoji_ZWJ_Sequence}` character class repeated 20 times
(each class expanding to hundreds of opcodes via `compile_class_with_strings`,
clearing `MAX_OPCODES` without needing anywhere near the AST depth 1.3
would need) fails cleanly. None of these crash; all set a specific,
resource-naming `prog->error` instead. Permanent regression coverage for
all four in `test/smoke.c`.

**An interaction worth knowing about, discovered while verifying this
fix — not introduced by it:** the *legitimate*, non-erroring maximum of
exactly 254 capture groups (`(a)(a)(a)...` × 254, no bounds violation)
independently crashes via 1.3's still-open stack-overflow bug in
`validate_group_names` — confirmed via ASan (~247 recursion frames before
overflow, right at that group count). This fix's own error path is
unaffected *only* because `next_token` short-circuits to `TOK_EOF` the
moment `prog->error` is set (this fix triggers *during* parsing, before
`validate_group_names` ever runs — see `compile_into`'s `if (!prog->error)`
guard ahead of that call), so 255+ groups reliably fail cleanly without
ever reaching the dangerous code path. A pattern that stays *within* the
now-enforced limit has no such shortcut. In other words: this fix
correctly closes the OOB-write hole this finding was about, but raising
`MAX_GROUPS` itself, or otherwise trying to make full use of the newly-
enforced 254-group ceiling, still runs straight into 1.3 — that's a
separate fix, tracked there, not part of this one.

### 1.3 [P0] Long flat patterns stack-overflow via linear AST recursion — **FIXED**

`parse_concat` and `parse_alt` build a **linear chain** of `AST_CONCAT`/
`AST_ALT` nodes for a flat sequence — not a balanced tree. A pattern with N
sequential atoms (e.g. `N` literal characters with no grouping at all)
produces an AST that's N nodes deep. Every one of `free_ast`,
`validate_group_names`, `validate_backrefs`, `validate_named_backrefs`, and
`compile_node` recurses through `node->left`/`node->right` with no
iterative fallback and no depth limit.

**Confirmed repro:** a pattern of 20,000 plain literal characters (e.g.
`"aaaa...a"` × 20000, no metacharacters, no nesting) segfaulted with a
stack overflow inside `validate_group_names`, called from `compile_into`
before any bytecode was even emitted:
```
==ERROR: AddressSanitizer: stack-overflow
    #0 validate_group_names regexp.c:1228
    #1 validate_group_names regexp.c:1248
    #2 validate_group_names regexp.c:1248
    ... (repeats)
```
20,000 nested *groups* (`((((...a...))))`) hit the same crash for the same
structural reason (real nesting this time, not just a flat chain), and a
20,000-way alternation (`a|a|a|...|a`) hit it a third way — `parse_alt`
recurses on its own right-hand side for every `|`, so that one crashed
*during parsing itself*, before the resulting tree was ever handed to any
of the walkers above. All three are "recursion depth proportional to
pattern shape, with no cap," just reached via different grammar
constructs.

**Fixed** by capping AST height at `MAX_AST_DEPTH` (`include/regexp.h`),
checked at parse time via two complementary mechanisms in `re_parser.c`,
matching the two different ways excessive depth could arise:

- **`ASTNode.depth`**, maintained incrementally as `finish_node()` combines
  a node's already-known children depths (`1 + max(left, right)`) at every
  point the parser attaches children — the four sites identified above
  (`parse_quantifier`, `parse_concat`'s loop, `parse_alt`'s combine, and
  `parse_primary`'s group/lookaround wrap). This is what bounds a flat
  literal chain or nested-group tree: neither causes deep *parser*
  recursion on its own (`parse_concat`'s loop is iterative; group nesting's
  recursion is comparatively shallow C-stack-wise), but both build a tree
  deep enough to endanger the *later* walkers once parsing finishes.
- **`Lexer.parse_depth`**, a live counter checked at the top of `parse_alt`
  itself, *before* recursing further — necessary because `ASTNode.depth`
  is only known *after* a subtree is fully built, which is too late to stop
  the recursion that built it. Both nested groups (`parse_primary` calls
  `parse_alt` for the group body) and chained alternation (`parse_alt`
  calls itself for each `|`) recurse through `parse_alt`, so checking there
  catches both, including the alternation-chain case where the *parser's
  own* stack was previously what overflowed.

Once either check trips, `prog->error` is set (first failure wins, same
"clamp and let the caller's existing short-circuit unwind things" pattern
used for the four `MAX_*` bounds in 1.2) and parsing winds down quickly:
`next_token()` starts returning `TOK_EOF` immediately once `prog->error` is
set, so the still-in-flight recursive `parse_concat`/`parse_alt` calls
return with minimal further work rather than continuing to build. This is
also why `validate_group_names` and friends are safe even though a
"doomed" AST may still contain a node or two past the limit: `compile_into`
gates every one of those four walkers behind `if (!prog->error)`, so once
this check fires, none of them run at all. (`free_ast` is the one
exception — it isn't gated, since the AST needs freeing regardless of
error state — but it has by far the smallest per-frame stack cost of the
five, so a tree just past `MAX_AST_DEPTH` is nowhere near dangerous for it
specifically.)

**Verified** (ASan + UBSan, zero warnings under `-Wall -Wextra`): all three
of the original repro shapes (20,000 flat literals, 20,000 nested groups,
20,000-way alternation) now fail cleanly with a resource-limit error
instead of crashing. Critically, the case this fix actually targets —
**a *legitimate*, within-`MAX_GROUPS` pattern (254 capture groups, no
bounds violation, would have compiled successfully before this fix) that
still crashed via `validate_group_names`' own recursion once it ran on the
resulting AST** — is confirmed to no longer crash either: the depth guard
now rejects it before `validate_group_names` ever runs, converting a
segfault into a clean, catchable `InternalError`. A moderately nested and
quantified pattern well clear of the limit still compiles and matches
correctly, confirming the guard doesn't disturb ordinary usage. Permanent
regression coverage for all of the above in `test/smoke.c`.

**What this fix does *not* cover — 1.1 remains separate and open.**
Finding 1.1 (3 nested lookarounds crashing the process) is a *different*
recursion path: `vm_execute_internal`'s own recursive call for
`OP_LOOKAHEAD`/`OP_LOOKBEHIND` at **match time**, not anything in the
parser. `MAX_AST_DEPTH=200` bounds how deeply a *pattern's syntax* can
nest at parse time, but does nothing to stop a pattern that compiles fine
(e.g. 10 nested lookaheads, comfortably under 200) from crashing the VM
later when matched, since that recursion's ~2.2MB-per-call stack frame
only tolerates roughly 3–4 levels regardless of what the parser allowed
through. Fixing that would need either shrinking `Thread`/the VM's own
stack frame or a separate, much tighter (single-digit) lookaround-nesting
cap at parse time — deliberately not bundled into this fix, which was
scoped to the `validate_group_names`-and-siblings bug specifically.

### 1.4 [P1] `\b`/`\B` read one past `text_end` unconditionally — **FIXED**

```c
else if (inst.op == OP_WORD_BOUNDARY) {
    bool left_is_word = (current.sp > original_text) && is_word_char(*(current.sp - 1));
    bool right_is_word = is_word_char(*current.sp);   // <-- no current.sp < text_end check
```
(`re_vm.c:206–216`, both `OP_WORD_BOUNDARY` and `OP_NON_WORD_BOUNDARY`.)

Contrast with `OP_ASSERT_END` immediately above it, which correctly
short-circuits: `current.sp >= text_end || (prog->multiline && *current.sp
== '\n')`. The word-boundary handlers are missing that same guard — `\b`
or `\B` matching at the very end of the text always dereferences one past
it.

**Confirmed repro:** `/abc\b/` against a 3-unit heap buffer holding exactly
`"abc"` with **no NUL terminator and no slack** (which is explicitly
allowed — `regex_exec`'s contract per `README.md` is that `text` need not
be NUL-terminated and `text_units` is authoritative):
```
==ERROR: AddressSanitizer: heap-buffer-overflow ... READ of size 2
    #0 vm_execute_internal regexp.c:1619
```
This will read whatever byte(s) happen to follow the buffer; in a WASM
sandbox this reads adjacent linear memory rather than crashing (usually),
so this bug is likelier to manifest as *silently wrong `\b` results near a
memory-page boundary* than a crash — arguably worse than an immediate
fault.

**Fix:** `bool right_is_word = (current.sp < text_end) && is_word_char(*current.sp);`

**Fixed** exactly as suggested above, applied to both `OP_WORD_BOUNDARY` and
`OP_NON_WORD_BOUNDARY`. **Verified** (ASan+UBSan, zero warnings under
`-Wall -Wextra`): the original repro (`/abc\b/` against an exactly-3-unit
heap buffer, no NUL, no slack) now matches cleanly with no sanitizer report,
and the same buffer under `/abc\B/` correctly fails to match (end-of-text is
a word boundary). `\b`/`\B` against a zero-length text also checked against
Node (`/\b/.test('')` is `false`, `/\B/.test('')` is `true` — this engine
agrees). The pre-fix code was re-confirmed to trip ASan
(`heap-buffer-overflow` in the `OP_WORD_BOUNDARY` handler) on the new
regression test before the fix was applied — i.e. the test genuinely guards
this, it doesn't just pass vacuously. Permanent regression coverage in
`test/smoke.c`, which is now also runnable under sanitizers via a new
`make test-asan` target (see §3 — a plain build can pass these
buffer-edge tests by silently reading adjacent memory; only the sanitizer
build actually proves them).

### 1.5 [P1] `decode_utf16` reads past the buffer for a trailing lone lead surrogate — **FIXED**

```c
static inline uint32_t decode_utf16(const uint16_t** sp) {
    uint32_t cp = *(*sp)++;
    if (cp >= 0xD800 && cp <= 0xDBFF) {
        if (**sp >= 0xDC00 && **sp <= 0xDFFF) {   // <-- unconditional deref of **sp
```
(`re_vm.c:35–44`.) Called from `OP_CHAR`/`OP_CLASS`'s forward-matching
branch (`re_vm.c:111, 167`) whenever `prog->unicode` is set. If the last
code unit in the buffer is a lead surrogate with nothing after it (a
legitimately malformed-but-possible input — untrusted UTF-16 buffers do
end mid-surrogate-pair sometimes, and nothing upstream guarantees
otherwise), this reads one unit past `text_end`.

**Confirmed repro:** `/./u` against a 1-unit buffer containing only
`0xD83D` (a lead surrogate, nothing after, no NUL, no slack):
```
==ERROR: AddressSanitizer: heap-buffer-overflow ... READ of size 2
    #0 vm_execute_internal regexp.c:1578   (OP_CLASS, decode_utf16 inlined)
```

**Fix:** `decode_utf16` needs a `text_end` (or equivalent) parameter to
bound the lookahead read, threaded through from its callers — currently
it only receives the moving pointer, not a limit. This is a slightly more
invasive fix than 1.4 since it changes a shared helper's signature, but
every call site already has `text_end` in scope.

**Fixed** as suggested: `decode_utf16` now takes a `limit` parameter
bounding the trail-surrogate peek (a lone lead surrogate at the limit
decodes as itself, per spec), threaded through all four call sites. Three
pass `text_end`; the fourth — the ignore-case backreference comparison's
*capture-side* decode (`re_vm.c`, `OP_BACKREF`'s `/iu` loop) — deliberately
passes the capture's own `end` instead, since a capture ending in a lone
lead surrogate must decode it as itself rather than pairing it with
whatever text unit happens to follow the capture (that decode could
otherwise never read out of bounds of the *text*, but could read past the
*capture*, which is a semantic bug of the same shape). The helper's
"extracted verbatim from jsvm2" status is now "extracted, plus a bounds
parameter" — jsvm2 only ever decodes NUL-terminated JSStrings, so upstream
doesn't have this bug in practice, but the divergence is called out in the
comment on the function. **Verified** (ASan+UBSan, zero warnings): the
original repro (`/./u` against a 1-unit buffer holding only `0xD83D`, no
NUL, no slack) matches the lone surrogate as a single unit with no
sanitizer report, and `/(.)\1/iu` against two lone lead surrogates
(exercising the backreference decode exactly at the buffer edge) matches
the full 2-unit span — both outcomes confirmed identical in Node.
Permanent regression coverage for both in `test/smoke.c` (meaningful under
`make test-asan`, same caveat as 1.4).

### 1.6 [P1] Backreference bounds only validated under `/u` — **FIXED**

```c
/* In unicode mode, backreferences to non-existent groups are early errors */
if (!prog->error && prog->unicode) {
    validate_backrefs(ast, prog->group_count, &prog->error);
}
```
(`re_compiler.c:251–254`.) This matches spec (ECMAScript makes an
out-of-range numeric backreference an early `SyntaxError` only under `/u`;
in Annex B / non-unicode mode, `\9` when there's no group 9 is legal
syntax that's supposed to fall back to an identity/octal-ish escape for
small numbers) — but this engine doesn't implement that Annex B fallback
for backreference-shaped escapes at all; it always tokenizes `\` followed
by digits `1`–`9` as `TOK_BACKREF` (`re_lexer.c:931–940`) regardless of mode,
with no upper bound on how many digits, then emits `OP_BACKREF` with
whatever numeric id it parsed — completely unchecked in non-unicode mode.

**Confirmed repro:** `/(a)\999/` (non-unicode mode) against `"ab"`:
```
==ERROR: AddressSanitizer: stack-use-after-scope ... READ of size 8
    #0 vm_execute_internal regexp.c:1631   (current.captures[inst.arg1 * 2], inst.arg1 = 999)
```
`current.captures` is a `const uint16_t* [MAX_GROUPS * 2]` (510 entries);
index 1998 reads far outside it into adjacent stack memory — reads
uninitialized/unrelated stack data and (via the `if (start && end)` check
right after) may treat garbage as valid capture pointers, feeding them
into subsequent pointer arithmetic.

**Fix, in order of correctness vs. effort:** minimally, validate
backrefs unconditionally (drop the `prog->unicode` gate) — this changes
non-unicode-mode error behavior for `\9`-with-no-group-9 patterns from
"undefined behavior" to "SyntaxError," which is a strictly safer outcome
even if it's not a byte-for-byte match of Annex B's actual fallback
semantics (treating small out-of-range backrefs as octal/identity escapes
instead of hard errors). Doing the full Annex B fallback is a bigger,
separate spec-compliance project; closing the memory-safety hole doesn't
require it.

**Fixed** with the minimal option: the `&& prog->unicode` gate on
`validate_backrefs` in `compile_into` (`re_compiler.c`) is gone, so
out-of-range numeric backreferences are a compile-time `SyntaxError` in
every mode. This is a **known, deliberate deviation from Annex B** (Node
accepts `/(a)\999/` un-flagged and treats `\9`-ish escapes as
octal/identity fallbacks; this engine now rejects it) — documented in the
comment at the validation site. The full Annex B fallback remains a
separate spec-compliance project, per the paragraph above. **Verified**
(ASan+UBSan, zero warnings): `/(a)\999/` un-flagged now fails compilation
with `SyntaxError: Invalid backreference` instead of OOB-reading
`captures[]` at match time; in-range non-`/u` backrefs (`/(a)\1/` against
`"aa"`) still compile and match. Permanent regression coverage in
`test/smoke.c`.

### 1.7 [P1] Non-unicode-mode builtin classes wrongly capped at codepoint 255 — **FIXED**

```c
} else if (type == 'D') {
    add_range(cls, 0, '0' - 1);
    add_range(cls, '9' + 1, unicode ? 0x10FFFF : 255);   // <-- should be 0xFFFF
```
Same pattern at `re_lexer.c:305` (`\W`'s upper range), :313/:321/:322
(`\s`/`\S`'s per-entry `> 255` cutoff), and :907 (`.`'s `max_cp`).

**This breaks the engine's own stated invariant** (see
`docs/ARCHITECTURE.md`'s lexer section): non-unicode mode is supposed to
operate over the full UTF-16 *code-unit* space (0–0xFFFF), only
*surrogate-pair-decoding* is gated on `/u`. Capping at 255 instead
silently narrows `.`, `\D`, `\W`, and `\S` to Latin-1, which is not what
JavaScript does — `/./.test("€")` and `/\W/.test("Ā")` are both
`true` in real JS with no flags at all, since neither `.` nor `\W`'s
implicit complement has ever been Latin-1-scoped in ECMAScript, `/u` or
not.

**Confirmed repro (diffed against Node, no flags on either side):**

| Pattern | Input | Node | This engine |
|---|---|---|---|
| `/x.y/` | `"x€y"` | matches | **no match** |
| `/\s/` | `""` | matches | **no match** |
| `/\W/` | `"Ā"` | matches | **no match** |

**Fix:** replace the `255` literals in `re_lexer.c` at lines 294 (`\D`,
already correct — check this one specifically, see note below), 305, 313,
321, 322, and 907 with `0xFFFF` where they represent "non-unicode-mode
upper bound." Double check `\d`/`\D` too even though `\D`'s repro wasn't
separately diffed above — `fill_builtin_class`'s `'D'` branch has the
exact same `: 255` pattern at line 328 and is presumably equally wrong for
the same reason, just not independently confirmed against Node in the
table above.

**Fixed**, with one significant addition beyond the mechanical `255` →
`0xFFFF` swap. The changed sites, all in `re_lexer.c`: `\D`'s and `\W`'s
complement upper bound in `fill_builtin_class`; `\s`'s `> 255` entry
cutoff (removed entirely rather than raised — the spec's WhiteSpace/
LineTerminator list is mode-independent, so there is nothing to cut);
`\S`'s post-inversion clamp (now `0xFFFF` without `/u`, `0x10FFFF` with);
and `.`'s `max_cp`. The addition: **lifting the cap on `.` exposed a
second, previously-masked bug in the same branch** — non-unicode `.` never
excluded the LineTerminators U+2028/U+2029 (only the `/u` branch did),
which was unobservable while both were above the 255 cap but wrong the
moment the cap rose (Node: `/./.test(' ')` is `false`, no flags).
The two branches are now collapsed into one that always carves out
2028–2029, differing only in `max_cp`. Deliberately *not* changed, per
this finding's own "don't pattern-match blindly" warning: the
`0x10FFFF` literals in escape-range validation (genuine max-codepoint
checks) and in `invert_class` (whose above-0xFFFF overshoot is
unreachable without `/u`, since undecoded code units can't exceed
0xFFFF). **Verified** against Node (no flags on either side) across 25
probe cases under ASan+UBSan, zero mismatches: all three of the original
repro rows above, `\D`/`\S` (flagged above as not independently
confirmed — both were in fact equally wrong, now equally fixed), the
2027/2028/2029/202A boundary around `.`'s LineTerminator exclusion,
dotAll, the in-class `[\s]`/`[\D]`/`[^\S]` path (a different
`fill_builtin_class` call site than the standalone escapes), and `/u`
behavior unchanged. Permanent regression coverage in `test/smoke.c`.

### 1.8 [P2] `OP_CLEAR_CAPTURES` is dead code — stale captures leak across alternation

`OP_CLEAR_CAPTURES` is fully defined (`include/regexp.h:25`, `:47`) and
has a working VM implementation (`re_vm.c:337–343`) but
**`compile_node` never emits it** — confirmed by grep, its only two
non-VM references are the enum/macro definitions. The result: when a
repeated group contains alternation and a later iteration takes a
different branch than an earlier one, the capture from the branch that
*didn't* run on the final iteration is left stale from an earlier
iteration instead of becoming unset, per spec.

**Confirmed repro (diffed against Node):**
```js
/(?:(a)|(b))+/.exec("ab")
// Node:        ["ab", undefined, "b"]   -- group 1 unset (didn't participate in the last iteration)
// This engine:  match, group 1 = [0,1) "a", group 2 = [1,2) "b"   -- group 1 incorrectly retains its stale value
```

**Fix:** this is more invasive than the others — it needs `compile_node`'s
`AST_QUANTIFIER` case to know which group-id range is "inside this
quantified body" and emit `OP_CLEAR_CAPTURES` for that range at the top of
each loop iteration (before the body runs), so a branch that doesn't
execute this time clears rather than keeps its old value. Lower priority
than 1.1–1.7 since it's a wrong-answer bug, not a memory-safety one — but
worth fixing given the VM-side plumbing already exists and is just
unreachable.

### 1.9 [P3] Minor / silent-truncation spots (inspection only, not independently repro'd)

- `rx_name_append_utf8` (`re_lexer.c:186`) silently stops appending past 31
  bytes rather than erroring — an extremely long capture-group name
  quietly truncates instead of failing compilation. Low practical impact
  (name buffer is generous relative to any real identifier) but worth a
  `prog->error` instead of silent truncation for consistency with how
  every other limit in this file is handled (fail loud).
- `prop_cache` (`re_lexer.c:328–332`, `MAX_PROP_CACHE = 64`) silently stops
  caching once full (`if (prop_cache_count < MAX_PROP_CACHE)`) rather than
  evicting — correct (never wrong results, just loses the caching
  speedup for the 65th+ distinct `\p{...}` property used across the
  process lifetime), but note it's a `static` file-scope cache: it
  persists **across unrelated `compile_into` calls for different
  patterns** and is **not thread-safe** if this engine is ever called
  from multiple threads natively (WASM is single-threaded by default so
  this is currently moot there, but would bite a native multi-threaded
  embedder).

---

## 2. Performance

- **1.1's root cause is also the biggest performance lever.** Every
  `vm_execute_internal` call — including the top-level one *and* every
  lookaround's recursive call — pays for a ~2.2MB stack frame and a
  531,072-byte-ish set of `Thread` copies made of `memcpy`-sized
  `MAX_GROUPS*2`-pointer arrays, regardless of how many groups the pattern
  actually has. A pattern with 2 groups pays for a 510-pointer capture
  array on every `OP_SPLIT` push. Sizing `Thread.captures` to
  `prog->group_count` (see 1.1's fix #1) fixes the crash *and* shrinks
  every `Thread` push/pop and every `memcpy(temp_captures, ...)` in the
  lookaround opcodes (`re_vm.c:310, 319, 327`) by a large constant
  factor for typical patterns.
- `fail_cache[CACHE_SIZE]` (8192 entries) is fully re-initialized
  (`for (int i = 0; i < CACHE_SIZE; i++) fail_cache[i].pc = -1;`,
  `re_vm.c:90`) on **every single call**, including every recursive
  lookaround call, even when the lookaround body is trivial. For a pattern
  with many small lookaheads evaluated in a hot loop, this is a fixed
  ~8192-iteration cost paid repeatedly for bodies that might only execute
  a handful of instructions. Consider a generation counter instead of a
  full array re-init (bump a `generation` int per call, store it alongside
  each cache entry, treat a stale generation as "empty" — avoids the
  O(CACHE_SIZE) reset entirely).
- Opcode dispatch is an `if`/`else if` chain (`re_vm.c:105` onward), not a
  `switch`. Modern compilers often turn a dense `switch` over a small enum
  into a jump table; an `if`/`else if` chain over the same enum is not
  guaranteed the same treatment and in practice tends to compile to a
  sequence of compares in source order. Worth benchmarking a `switch`
  rewrite — `OP_CHAR`/`OP_CLASS`/`OP_SPLIT` are almost certainly the
  hottest opcodes and are already near the top of the chain, which limits
  the downside today, but this is a cheap, low-risk change to try.
- `OP_CLASS` matching (`re_vm.c:171, 195`) does a **linear scan** over
  `cls->ranges[]` to find whether a code point is covered. `add_range`
  (`re_lexer.c:234`) already guarantees ranges are sorted and coalesced —
  binary search is a straightforward drop-in given that invariant already
  holds, and matters most for large Unicode-property classes (`\p{L}` etc.
  can have hundreds of ranges) matched against long inputs.
- `hash_state` (`re_vm.c:79`) uses `% CACHE_SIZE` (a true modulo,
  division) on every thread pop; `CACHE_SIZE` is `8192` — a power of two —
  so `& (CACHE_SIZE - 1)` is equivalent and avoids the division.

## 3. Testing

Current coverage is exactly two files: `test/smoke.c` (15 assertions,
native) and `test/node_smoke.mjs` (13 assertions, against the real
compiled `.wasm`). Both are useful as fast, zero-dependency sanity checks
and both currently pass — but they only exercise happy-path ASCII
patterns plus one surrogate-pair case. None of the P0/P1 findings above
would have been caught by either file, and there's no regression test
guarding against any of them once fixed.

- ~~**No CI.**~~ — **resolved.** `.github/workflows/test.yml` runs three
  jobs on every push and PR to `main`: `make test` (native), `make
  test-asan` (ASan+UBSan — see next bullet), and `make test-wasm` (the
  real compiled artifact under Node, via `setup-emsdk`). The GitHub Pages
  workflow (`pages.yml`) independently gates its deploy on `make test`,
  so a broken engine can't ship to the demo site even if someone pushes
  without waiting for the test workflow.
- **No memory-safety testing.** Every finding in §1 above was found with
  a throwaway ASan probe in ten minutes. ~~Adding a `make test-asan` target
  (same `test/smoke.c`, built with `-fsanitize=address,undefined`)~~ —
  **the target now exists** (added alongside the 1.4/1.5 fixes, whose
  buffer-edge regression tests are only meaningful under a sanitizer: a
  plain build can pass them by silently reading adjacent memory) **and
  runs in CI** as `test.yml`'s `asan` job. One wrinkle worth knowing:
  UBSan findings are only fatal because the target compiles with
  `-fno-sanitize-recover=undefined` — by default UBSan *prints* its
  report and exits 0, which would sail through CI unnoticed (ASan's
  reports abort by default; UBSan's don't).
- **No fuzzing.** `regex_compile` + `regex_exec` is an unusually clean
  fuzz target — two pure functions, deterministic, small state, already
  isolated behind a stable C API. A libFuzzer harness that feeds random
  bytes as (pattern, flags, text) and asserts "never crashes, never
  ASan-trips" would likely surface more of the class of bug found in §1
  beyond what manual probing found. This is a natural next step given how
  productive manual probing already was.
- **No spec-conformance coverage.** `README.md` already points at jsvm2's
  `test262` RegExp subset as the "real" test suite for this engine's
  matching semantics, but explicitly as an *external, disconnected*
  resource — you have to go clone/find jsvm2 and run its test runner by
  hand, and there's no guarantee anyone does that before merging a change
  here. Given this repo is the one meant to be embedded standalone,
  vendoring a curated subset of test262's RegExp built-ins directly into
  `test/` (even a modest few hundred cases covering lookaround, Unicode
  properties, named groups, and quantifier edge cases) would make
  spec-compliance regressions visible in *this* repo's own CI rather than
  requiring a manual cross-check against a sibling project.
- **No negative/error-path tests.** All 28 combined assertions today
  check for a match or a specific capture span; only one checks a compile
  error path (`test/smoke.c`'s unbalanced-paren case). None check the
  `MAX_*` limits, backreference bounds, or malformed Unicode escapes — the
  exact surface area where §1's bugs live.

## 4. Structure & code quality

- ~~`src/regexp.c` is a single 1800-line file spanning lexer, parser,
  compiler, and VM...~~ **RESOLVED.** Split into `src/re_lexer.c`,
  `src/re_parser.c`, `src/re_compiler.c`, `src/re_vm.c`, and a private
  `src/re_internal.h` for the cross-file `Lexer`/`Token`/`ASTNode`/
  `NameSet` types and the handful of function declarations
  (`next_token`, `parse_alt`, `free_ast`, `validate_group_names`) each
  stage needs to call into the previous one across translation units now.
  This *does* diverge from jsvm2's own single-file layout, as originally
  flagged here — done anyway, by request, on the judgment that the
  maintainability win outweighs the upstream-diffing cost for this repo.
  Verified behavior-preserving: full test suite (native, ASan+UBSan, and
  the real compiled WASM artifact) passes identically before and after,
  and every one of the original file's ~39 top-level functions was
  confirmed to appear in exactly one of the four new files (no accidental
  duplication or drop). See `docs/ARCHITECTURE.md`'s intro for the current
  layout and which few functions moved by actual cross-file usage rather
  than jsvm2's original textual position (`decode_utf16`, `is_word_char`,
  and `annexb_canonicalize` all ended up in `re_vm.c`, the only place that
  calls them, despite being positioned before jsvm2's "LEXER" section).
- **Forward/backward branch duplication in the VM** (documented in
  `docs/ARCHITECTURE.md`) is the direct cause of at least one of the bugs
  above having a fix needed in two places conceptually (1.4's `\b`/`\B` is
  actually not direction-dependent, but 1.5's `decode_utf16` forward path
  and the VM's separate inlined backward-surrogate-decode logic — e.g.
  `re_vm.c:134` onward — are two independently-maintained
  implementations of "decode one code point in this direction," and only
  one of them was probed here). Extracting shared
  `decode_forward(sp, text_end)` / `decode_backward(sp, text_start)`
  helpers used by both `OP_CHAR` and `OP_CLASS` (and the backreference
  matching code, which duplicates this a *third* time at `re_vm.c:242` and
  `:261`) would both shrink `re_vm.c` substantially and remove an entire
  class of "fixed on one side, not the mirror side" bug risk — this is
  independent of and not addressed by the file split above, which
  reorganized without deduplicating.
- `include/regexp.h:28–47` defines `OP_CHAR`/`OP_CLASS`/etc. as macros
  aliasing `REGEX_OP_CHAR`/`REGEX_OP_CLASS`/etc. (the actual enum members).
  Every use site across the engine's four `re_*.c` files uses the short
  `OP_*` form; the `REGEX_OP_*` names are never referenced anywhere except
  their own enum/macro definitions. This double-naming appears to be a
  leftover from some prior refactor or namespacing concern — worth
  collapsing to one name (presumably the enum becomes `OP_*` directly)
  unless there's an external reason (e.g. a header consumer elsewhere in
  jsvm2) to keep the `REGEX_OP_*` prefix as the "public" name.
- `fill_unicode_property` (`re_lexer.c:342–468`) is a large `if`/`else if`
  chain of `strcmp` calls mapping short Unicode general-category aliases
  (`Lu`, `Ll`, `Nd`, ...) to their long names, plus another chain for the
  grouped categories (`L`, `M`, `N`, ...). A small static lookup table
  (array of `{short, long}` pairs, linear- or binary-searched) would be
  more maintainable and marginally faster than ~30 sequential `strcmp`s
  per lookup — low priority since this only runs at compile time, not
  per-match, but it's the least readable function in the file today.
- Magic numbers without named constants in a few spots: the `32`-byte
  group-name buffer size appears as a literal in multiple structs
  (`Token.name`, `ASTNode.name`, `Program.group_names[MAX_GROUPS][32]`)
  rather than a shared `#define`; the `64`-byte property-name buffer in
  the `\p{...}` parsing code (`re_lexer.c:654, 987`) is similarly a bare
  literal repeated at each call site. Not a bug, just a maintainability
  nit — a future change to one and not the others would silently
  reintroduce a truncation bug.

## 5. Suggested order of work

If tackling this list, roughly in order of (safety impact) / (effort):

1. ~~**1.2** (bounds-check the four `MAX_*` counters)~~ — **done.**
2. ~~**1.3** (depth-limit the parser/AST walkers)~~ — **done**, via
   `MAX_AST_DEPTH` + `ASTNode.depth`/`Lexer.parse_depth` in `re_parser.c`.
   This also happened to close the specific 254-capture-group crash noted
   under 1.2 (a legitimate, within-`MAX_GROUPS` pattern that used to reach
   `validate_group_names` and crash there) — that was always a 1.3 issue
   wearing a 1.2-shaped trigger, and is now fixed alongside it.
3. ~~**1.1** (shrink `Thread`/the VM stack frame)~~ — **done**: captures is
   now sized to `(prog->group_count + 1) * 2` instead of `MAX_GROUPS * 2`,
   and the backtrack stack + fail-cache are heap-allocated instead of
   C-stack locals — see 1.1's write-up for what that took (`Thread`
   captures becoming a pointer meant every push/pop needed an explicit
   copy instead of relying on struct-assignment). All four original P0
   crash/memory-corruption findings (1.1, 1.2, 1.3, and the 1.2-shaped
   1.3 interaction above) are now fixed.
4. ~~**1.4** and **1.5** (the two unconditional OOB reads)~~ — **done**:
   `\b`/`\B` now bound their right-side peek by `text_end`, and
   `decode_utf16` takes a `limit` parameter (with the backreference
   comparison's capture-side decode bounded by the capture's own `end`,
   not `text_end` — see 1.5's write-up). Both came with a new
   `make test-asan` target, since a plain build can't observe these.
5. ~~**1.6** (unconditional backref bounds validation)~~ — **done**: the
   `&& prog->unicode` gate is dropped; out-of-range numeric backrefs are
   now a `SyntaxError` in every mode (a documented Annex B deviation —
   see 1.6's write-up). All three P1 OOB reads are now closed.
6. ~~**1.7** (the `255` → `0xFFFF` cap fix)~~ — **done**, and the
   verify-each-occurrence caution paid off twice: lifting `.`'s cap
   exposed a masked missing-LineTerminator-exclusion bug in the same
   branch (fixed together — see 1.7's write-up), and two `0x10FFFF`
   families (escape-range validation, `invert_class`) were confirmed
   correct and left alone. All four P1 findings (1.4–1.7) are now closed;
   everything remaining in §1 is P2/P3.
7. ~~Wire up CI (§3)~~ — **done**: `test.yml` runs native, ASan+UBSan
   (with `-fno-sanitize-recover=undefined` so UBSan findings actually
   fail the job), and WASM smoke jobs on every push/PR; the Pages deploy
   was already gated on `make test`. Every regression test added by the
   fixes above now runs automatically, including the sanitizer-only
   buffer-edge ones.
8. **1.8** (emit `OP_CLEAR_CAPTURES`) — lower urgency (wrong answer, not
   unsafe), tackle once the memory-safety items are clear.
