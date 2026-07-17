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

## Priority summary

| # | Finding | Severity | Confirmed |
|---|---|---|---|
| 1.1 | 3 nested lookarounds crashes the process (stack overflow) | **P0 — crash/DoS** | ✅ ASan + plain build |
| 1.2 | `MAX_OPCODES`/`MAX_CLASSES`/`MAX_GROUPS`/`MAX_COUNTERS` never bounds-checked → heap/stack corruption | **P0 — memory corruption** | ✅ ASan |
| 1.3 | Flat (non-nested) long patterns also stack-overflow via linear AST recursion | **P0 — crash/DoS** | ✅ ASan |
| 1.4 | `\b`/`\B` read one code unit past `text_end` unconditionally | P1 — OOB read | ✅ ASan |
| 1.5 | `decode_utf16` reads past buffer end for a trailing lone lead surrogate | P1 — OOB read | ✅ ASan |
| 1.6 | Backreference bounds only validated under `/u` — `\999` OOB-reads in Annex B mode | P1 — OOB read | ✅ ASan |
| 1.7 | Non-unicode `.`/`\D`/`\W`/`\S` wrongly capped at codepoint 255 instead of 0xFFFF | P1 — wrong results | ✅ diffed vs Node |
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
  backward). Fixed by `annexb_canonicalize` (`src/regexp.c`, next to
  `apply_case_folding`), which implements ECMA-262's actual non-Unicode
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
  `fill_unicode_property`, and (b) a new `compile_class_with_strings`
  (`src/regexp.c`, by `compile_node`) that compiles a class containing
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

---

## 1. Correctness & memory safety

### 1.1 [P0] Nested lookaround crashes the process at depth 3

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
call to `vm_execute_internal` (regexp.c:1720–1747), and that function's
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

**Suggested source-level fix, in order of surgical-ness:**
1. Shrink `Thread.captures` from a fixed `MAX_GROUPS * 2` (510 pointers) to
   `(prog->group_count + 1) * 2`, either via a dynamically-sized
   (heap-allocated once per `vm_execute_internal` call tree, not per
   `Thread`) buffer, or by capping typical `MAX_GROUPS` much lower and
   raising it only for patterns that need it. Most patterns use a handful
   of groups; paying for 510 pointers per `Thread` (and there are 512
   `Thread`s in `stack[]`) is the dominant cost.
2. Move `stack[512]` and `fail_cache[CACHE_SIZE]` off the C stack (heap-
   allocate, or make them `static`/thread-local scratch buffers sized once
   and reused — note `static` alone isn't safe for the *recursive* call
   case without an explicit per-depth pool, since a lookaround's recursive
   call would clobber the outer call's in-progress buffer).
3. Enforce a maximum lookaround nesting depth at compile time (track depth
   in the parser, emit a clear `prog->error` past some sane limit like 50)
   as a backstop even after (1)/(2) — recursion depth should fail closed
   with a catchable error, not crash the process, regardless of how much
   the frame shrinks or how big a stack the host configures.

### 1.2 [P0] `MAX_*` bounds are never checked — heap/stack corruption

`include/regexp.h` defines `MAX_OPCODES=16384`, `MAX_CLASSES=64`,
`MAX_GROUPS=255`, `MAX_COUNTERS=16`, all as fixed array sizes inside
`Program` (heap-allocated) or `Thread` (stack-allocated, see 1.1). Every
increment of the corresponding counter is unchecked:

- `emit()` (regexp.c:1280): `prog->code[idx] = ...; idx = prog->code_count++;` — no check against `MAX_OPCODES`.
- Character-class allocation (regexp.c:898, 919, 991, 1045): `int cid = lexer->prog->class_count++;` then `memset(&lexer->prog->classes[cid], ...)` — no check against `MAX_CLASSES`.
- Group numbering (regexp.c:1153): `node->id = ++lexer->prog->group_count;` — no check against `MAX_GROUPS`.
- Counter allocation (regexp.c:1370): `int counter_id = prog->counter_count++;` — no check against `MAX_COUNTERS`.

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

`MAX_GROUPS` (256+ capture groups) and `MAX_OPCODES` (very long/complex
patterns) are structurally the same class of bug — unchecked writes into
fixed arrays sized by a compile-time constant, driven by pattern content —
confirmed by code inspection even where a specific probe hit a different
crash first (see 1.3, which triggers before `MAX_OPCODES` can be reached
via a *flat* pattern).

**Fix:** in each of the four increment sites, check against the `MAX_*`
bound and set `prog->error = "InternalError: pattern exceeds internal
limit"` (or a more specific message per resource) instead of writing OOB.
This is a small, mechanical, high-value fix — a handful of `if` checks
total. Consider whether the `MAX_*` constants themselves should be raised
now that they're actually enforced (right now they're aspirational, not
real limits).

### 1.3 [P0] Long flat patterns stack-overflow via linear AST recursion

Separately from 1.1/1.2, `parse_concat` (regexp.c:1184) and `parse_alt`
(regexp.c:1206) build a **linear chain** of `AST_CONCAT`/`AST_ALT` nodes
for a flat sequence — not a balanced tree. A pattern with N sequential
atoms (e.g. `N` literal characters with no grouping at all) produces an
AST that's N nodes deep. Every one of `free_ast`, `validate_group_names`,
`validate_backrefs`, `validate_named_backrefs`, and `compile_node`
recurses through `node->left`/`node->right` with no iterative fallback and
no depth limit.

**Confirmed repro:** a pattern of 20,000 plain literal characters (e.g.
`"aaaa...a"` × 20000, no metacharacters, no nesting) segfaults with a stack
overflow inside `validate_group_names`, called from `compile_into` before
any bytecode is even emitted:
```
==ERROR: AddressSanitizer: stack-overflow
    #0 validate_group_names regexp.c:1228
    #1 validate_group_names regexp.c:1248
    #2 validate_group_names regexp.c:1248
    ... (repeats)
```
20,000 nested *groups* (`((((...a...))))`) hits the same crash for the
same structural reason (real nesting this time, not just a flat chain) —
both are "the AST is deep enough that per-node recursion exhausts the C
stack," just via different pattern shapes.

**Fix:** either (a) rewrite the four recursive walkers as iterative loops
with an explicit stack (doable — `AST_CONCAT` recursion is effectively
"walk a linked list," which is the easy case to de-recursify first), or
(b) enforce a maximum pattern length / AST depth at parse time and fail
with `prog->error` before recursion depth becomes a problem. (b) is
strictly necessary regardless of (a) as a backstop for genuinely
adversarial nested-group patterns, similar to 1.1's recommendation.

### 1.4 [P1] `\b`/`\B` read one past `text_end` unconditionally

```c
else if (inst.op == OP_WORD_BOUNDARY) {
    bool left_is_word = (current.sp > original_text) && is_word_char(*(current.sp - 1));
    bool right_is_word = is_word_char(*current.sp);   // <-- no current.sp < text_end check
```
(regexp.c:1617–1626, both `OP_WORD_BOUNDARY` and `OP_NON_WORD_BOUNDARY`.)

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

### 1.5 [P1] `decode_utf16` reads past the buffer for a trailing lone lead surrogate

```c
static inline uint32_t decode_utf16(const uint16_t** sp) {
    uint32_t cp = *(*sp)++;
    if (cp >= 0xD800 && cp <= 0xDBFF) {
        if (**sp >= 0xDC00 && **sp <= 0xDFFF) {   // <-- unconditional deref of **sp
```
(regexp.c:17–26.) Called from `OP_CHAR`/`OP_CLASS`'s forward-matching
branch (regexp.c:1522, 1578) whenever `prog->unicode` is set. If the last
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

### 1.6 [P1] Backreference bounds only validated under `/u`

```c
/* In unicode mode, backreferences to non-existent groups are early errors */
if (!prog->error && prog->unicode) {
    validate_backrefs(ast, prog->group_count, &prog->error);
}
```
(regexp.c:1468–1471.) This matches spec (ECMAScript makes an
out-of-range numeric backreference an early `SyntaxError` only under `/u`;
in Annex B / non-unicode mode, `\9` when there's no group 9 is legal
syntax that's supposed to fall back to an identity/octal-ish escape for
small numbers) — but this engine doesn't implement that Annex B fallback
for backreference-shaped escapes at all; it always tokenizes `\` followed
by digits `1`–`9` as `TOK_BACKREF` (regexp.c:924–935) regardless of mode,
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

### 1.7 [P1] Non-unicode-mode builtin classes wrongly capped at codepoint 255

```c
} else if (type == 'D') {
    add_range(cls, 0, '0' - 1);
    add_range(cls, '9' + 1, unicode ? 0x10FFFF : 255);   // <-- should be 0xFFFF
```
Same pattern at regexp.c:339 (`\W`'s upper range), :347/:355/:356
(`\s`/`\S`'s per-entry `> 255` cutoff), and :900 (`.`'s `max_cp`).

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

**Fix:** replace the four `255` literals at regexp.c:328(`\D`, already
correct — check this one specifically, see note below), 339, 347, 355,
356, 900 with `0xFFFF` where they represent "non-unicode-mode upper
bound." Double check `\d`/`\D` too even though `\D`'s repro wasn't
separately diffed above — `fill_builtin_class`'s `'D'` branch has the
exact same `: 255` pattern at line 328 and is presumably equally wrong for
the same reason, just not independently confirmed against Node in the
table above.

### 1.8 [P2] `OP_CLEAR_CAPTURES` is dead code — stale captures leak across alternation

`OP_CLEAR_CAPTURES` is fully defined (`include/regexp.h:25`, `:47`) and
has a working VM implementation (regexp.c:1748–1754) but
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

- `rx_name_append_utf8` (regexp.c:220) silently stops appending past 31
  bytes rather than erroring — an extremely long capture-group name
  quietly truncates instead of failing compilation. Low practical impact
  (name buffer is generous relative to any real identifier) but worth a
  `prog->error` instead of silent truncation for consistency with how
  every other limit in this file is handled (fail loud).
- `prop_cache` (regexp.c:362–367, `MAX_PROP_CACHE = 64`) silently stops
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
  lookaround opcodes (regexp.c:1721, 1730, 1738) by a large constant
  factor for typical patterns.
- `fail_cache[CACHE_SIZE]` (8192 entries) is fully re-initialized
  (`for (int i = 0; i < CACHE_SIZE; i++) fail_cache[i].pc = -1;`,
  regexp.c:1501) on **every single call**, including every recursive
  lookaround call, even when the lookaround body is trivial. For a pattern
  with many small lookaheads evaluated in a hot loop, this is a fixed
  ~8192-iteration cost paid repeatedly for bodies that might only execute
  a handful of instructions. Consider a generation counter instead of a
  full array re-init (bump a `generation` int per call, store it alongside
  each cache entry, treat a stale generation as "empty" — avoids the
  O(CACHE_SIZE) reset entirely).
- Opcode dispatch is an `if`/`else if` chain (regexp.c:1516 onward), not a
  `switch`. Modern compilers often turn a dense `switch` over a small enum
  into a jump table; an `if`/`else if` chain over the same enum is not
  guaranteed the same treatment and in practice tends to compile to a
  sequence of compares in source order. Worth benchmarking a `switch`
  rewrite — `OP_CHAR`/`OP_CLASS`/`OP_SPLIT` are almost certainly the
  hottest opcodes and are already near the top of the chain, which limits
  the downside today, but this is a cheap, low-risk change to try.
- `OP_CLASS` matching (regexp.c:1582, 1606) does a **linear scan** over
  `cls->ranges[]` to find whether a code point is covered. `add_range`
  (regexp.c:268) already guarantees ranges are sorted and coalesced —
  binary search is a straightforward drop-in given that invariant already
  holds, and matters most for large Unicode-property classes (`\p{L}` etc.
  can have hundreds of ranges) matched against long inputs.
- `hash_state` (regexp.c:1490) uses `% CACHE_SIZE` (a true modulo,
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

- **No CI.** There's no `.github/workflows/` at all — nothing runs
  `make test` (or anything else) on push or PR. This is the single
  cheapest structural improvement available: even a minimal workflow
  running `make test` on every push catches regressions for free. (The
  GitHub Pages deploy workflow this task also sets up, see
  `docs/ARCHITECTURE.md`/README, could double as this if it's split into
  a `test` job that gates the `deploy` job.)
- **No memory-safety testing.** Every finding in §1 above was found with
  a throwaway ASan probe in ten minutes. Adding a `make test-asan` target
  (same `test/smoke.c`, built with `-fsanitize=address,undefined`) and
  running it in CI would have caught 1.1–1.6 automatically, and cheaply
  guards against regressions once fixed.
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

- `src/regexp.c` is a single 1800-line file spanning lexer, parser,
  compiler, and VM. That's a lot for one file, but given this is a
  **verbatim upstream copy from jsvm2** (see README "Provenance" and
  `CLAUDE.md`), splitting it into `lexer.c`/`parser.c`/`compiler.c`/`vm.c`
  would diverge from upstream's file layout and complicate diffing future
  upstream fixes back in — worth raising with whoever owns the jsvm2 side
  before restructuring, rather than doing it unilaterally here.
- **Forward/backward branch duplication in the VM** (documented in
  `docs/ARCHITECTURE.md`) is the direct cause of at least one of the bugs
  above having a fix needed in two places conceptually (1.4's `\b`/`\B` is
  actually not direction-dependent, but 1.5's `decode_utf16` forward path
  and the VM's separate inlined backward-surrogate-decode logic — e.g.
  regexp.c:1544–1552 — are two independently-maintained
  implementations of "decode one code point in this direction," and only
  one of them was probed here). Extracting shared
  `decode_forward(sp, text_end)` / `decode_backward(sp, text_start)`
  helpers used by both `OP_CHAR` and `OP_CLASS` (and the backreference
  matching code, which duplicates this a *third* time at regexp.c:1653 and
  :1672) would both shrink the file substantially and remove an entire
  class of "fixed on one side, not the mirror side" bug risk.
- `include/regexp.h:28–47` defines `OP_CHAR`/`OP_CLASS`/etc. as macros
  aliasing `REGEX_OP_CHAR`/`REGEX_OP_CLASS`/etc. (the actual enum members).
  Every use site in `src/regexp.c` uses the short `OP_*` form; the
  `REGEX_OP_*` names are never referenced anywhere except their own
  enum/macro definitions. This double-naming appears to be a leftover
  from some prior refactor or namespacing concern — worth collapsing to
  one name (presumably the enum becomes `OP_*` directly) unless there's
  an external reason (e.g. a header consumer elsewhere in jsvm2) to keep
  the `REGEX_OP_*` prefix as the "public" name.
- `fill_unicode_property` (regexp.c:369–479) is a large `if`/`else if`
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
  the `\p{...}` parsing code (regexp.c:652, 980) is similarly a bare
  literal repeated at each call site. Not a bug, just a maintainability
  nit — a future change to one and not the others would silently
  reintroduce a truncation bug.

## 5. Suggested order of work

If tackling this list, roughly in order of (safety impact) / (effort):

1. **1.2** (bounds-check the four `MAX_*` counters) — small, mechanical,
   closes real heap/stack corruption.
2. **1.4** and **1.5** (the two unconditional OOB reads) — one-line-ish
   guards each.
3. **1.6** (unconditional backref bounds validation) — drop one `&&
   prog->unicode` condition.
4. **1.7** (the `255` → `0xFFFF` cap fix) — mechanical, but grep carefully
   for every occurrence and verify each one against real JS behavior
   rather than pattern-matching blindly, since not every `255`/`0x10FFFF`
   pair in the file is necessarily wrong.
5. **1.1** (shrink `Thread`/the VM stack frame, add a depth limit) — the
   highest-impact fix and also the most invasive; budget real time for it,
   and add the depth-limit backstop even if the frame-shrinking part is
   deferred, since the backstop alone converts a crash into a catchable
   error immediately.
6. **1.3** (de-recursify or depth-limit the AST walkers) — same shape as
   (5)'s backstop; a length/depth cap at parse time is the fast partial
   fix, de-recursifying the walkers is the complete one.
7. Wire up CI (§3) around whatever subset of `make test` / an ASan build
   exists at each step above, so each fix in this list gets a permanent
   regression guard as it lands rather than all testing infrastructure
   being a separate final step.
8. **1.8** (emit `OP_CLEAR_CAPTURES`) — lower urgency (wrong answer, not
   unsafe), tackle once the memory-safety items are clear.
