# Architecture

How the engine actually works, for anyone about to modify it. This is a
single-pass-compile, backtracking-VM regex engine (think "hand-rolled PCRE
subset with a memoization cache bolted on for ReDoS mitigation," not an
NFA/DFA-simulation engine like RE2).

`include/regexp.h` holds the public types + entry points, and
`include/ucd.h` the generated Unicode data. The engine itself is split
across four `src/re_*.c` files — one per pipeline stage below — plus
`src/re_internal.h`, a private header (not part of the public API) that
shares the `Lexer`/`Token`/`ASTNode`/`NameSet` types and a handful of
cross-stage function declarations (`next_token`, `parse_alt`, `free_ast`,
`validate_group_names`) between them, since each stage now needs to call
into the previous one from a different translation unit. This was
originally one file (`src/regexp.c`, a verbatim copy of jsvm2's own
`src/regexp.c`) and was split for maintainability — see
`CLAUDE.md`/`README.md`'s "Provenance" section for what that means for
tracking upstream fixes.
Cross-stage code that's grouped by *actual usage* rather than *historical
textual position* moved with it — most notably, `decode_utf16`,
`is_word_char`, and `annexb_canonicalize` now live in `re_vm.c` (the only
place that calls them) even though jsvm2's original file had them
positioned before its own "LEXER" section.

## Pipeline

```
pattern (UTF-16, NUL-terminated)
   |
   v
Lexer (next_token)  --------- one token of lookahead, hand-written
   |
   v
Parser (parse_alt / parse_concat / parse_quantifier / parse_primary)
   |  recursive descent, builds an ASTNode tree
   v
Compiler (compile_node)  --- walks the AST once, emits Instruction[]
   |                          into Program.code (a flat array, not a
   |                          separate bytecode buffer)
   v
Program  (Instruction[] + CharClass[] + group metadata + flags)
   |
   v
VM (vm_execute + VMContext) - backtracking interpreter over Program.code,
                              invoked once per candidate start offset by
                              the caller (regex_exec in regex_wasm.c),
                              with one VMContext reused across the scan
```

`compile_into` (`re_compiler.c:317`) is the single entry point that drives
lexer → parser → validation → compiler and populates a caller-owned
`Program`. There is no separate "optimize" pass — bytecode is emitted
directly from the AST in one recursive walk (`compile_node`,
`re_compiler.c:129`).

## The `Program` struct — a small fixed header over right-sized heap buffers

```c
typedef struct {
    Instruction* code;                   // heap, grown by emit(); cap MAX_OPCODES
    int code_cap;
    CharClass classes[MAX_CLASSES];      // 256 small headers; buffers on the heap
    char group_names[MAX_GROUPS][32];    // 255
    int name_chain[MAX_GROUPS];          // same-name links for \k<...>
    int code_count, class_count, group_count, counter_count;
    bool ignore_case, multiline, dot_all, sticky, unicode, has_indices, unicode_sets;
    bool has_backrefs;                   // gates the VM's fail cache
    bool scan_filter, scan_non_ascii;    // first-unit scan filter
    uint8_t scan_ascii[16];              //   (see regexp.h)
    const char* error;
} Program;
```

The struct itself is a ~19KB fixed header (dominated by `group_names` and
the class slot table); everything that scales with the pattern — the
instruction array and each class's codepoint ranges and string set — is a
heap-allocated, right-sized buffer grown on demand during compilation,
with `MAX_OPCODES`/`MAX_CLASSES`/etc. surviving as hard caps rather than
allocation sizes. It was ~2MB of embedded fixed-size arrays (a 10-instruction
pattern paid for 32768; every class paid 16KB of `ranges[2048]`), which
priced a host out of holding many compiled patterns at once.
Consequences of heap ownership: a `Program` must be **zero-initialized
before its first `compile_into`** (`regex_compile` uses `calloc`),
re-compiling into the same `Program` frees the previous compile's class
buffers (the code buffer is kept and reused), and teardown must release
everything (`regex_free` does; a native host driving `compile_into`
directly calls the public `program_release`). See `CharClass` in
`include/regexp.h` for the per-class ownership rules — locally-built
classes are moved via free-then-struct-assign (`class_free`), and every
error path that abandons one frees it first.
(*Matching* allocates a `VMContext` — backtrack stacks, capture arenas,
fail caches — once per `regex_exec` call, reused across every start
position; see the VM section below.)

All of `MAX_OPCODES`/`MAX_CLASSES`/`MAX_GROUPS`/`MAX_COUNTERS` are
enforced at their allocation sites (they originally weren't, with
ASan-confirmed memory corruption when exceeded). If
you're changing any of these constants, or adding a new allocation site,
find the corresponding `_count` field's existing check-before-index
increment sites first (`alloc_class`, `++prog->group_count`,
`counter_count++`, `emit()`'s `code_count++`) and follow the same
pattern — a new unchecked site reintroduces exactly #1.2's bug.

## Lexer (`re_lexer.c`; `Lexer`/`Token` types in `re_internal.h`)

One token of lookahead (`lexer->current`), hand-written character-class
dispatch in `next_token` (`re_lexer.c:1289`). Notable pieces:

- `decode_utf16_lexer` (`re_lexer.c:26`) advances by one *code point* when
  `prog->unicode` is set (surrogate-pair aware), one *code unit* otherwise —
  this is the general pattern throughout the engine: unicode-mode
  operations are code-point-based, non-unicode-mode operations are
  UTF-16-code-unit-based (a historical `255` cap on non-unicode
  complement classes violated this in one place; since fixed and
  regression-tested).
- Escape sequences (`\d`, `\p{...}`, `\u{...}`, `\k<name>`, `\cX`, etc.) are
  all resolved inside `next_token`'s `case '\\':` block or inside the
  class parsers for class-internal escapes — there's no separate
  escape-resolution pass.
- Character classes (`[...]`) have **two parsers for two grammars**:
  `parse_char_class` (`re_lexer.c:759`) handles `/u` and legacy modes,
  while `/v` (`unicode_sets`) classes go to `parse_char_class_v`
  (`re_lexer.c:1196`) — a genuinely different ECMAScript 2024 grammar
  (`ClassSetExpression`) with set operators (`&&` intersection, `--`
  subtraction, both over single operands only and never mixed), nested
  classes, `\q{...}` string-alternative disjunctions (`parse_q_strings`,
  `re_lexer.c:988`; single-codepoint alternatives normalize into the
  codepoint ranges, so set ops see them there), reserved/doubled
  punctuators as syntax errors, and set algebra over the codepoint and
  string components independently (`class_union_v` /
  `class_intersect_v` / `class_subtract_v`). Everything is evaluated to
  one concrete `CharClass` at compile time — no expression tree survives
  parsing. Nested-class recursion shares the parser's `MAX_AST_DEPTH`
  guard via `Lexer.parse_depth`, and each frame's ~25KB `CharClass`
  operand scratch is heap-allocated so that guard's full depth can't
  overflow the C stack.
- Case-insensitive matching is a **compile-time set transformation, not a
  match-time comparison**, for everything except single `OP_CHAR`
  literals — and even those get their *constant* operand canonicalized at
  emit time (`emit_char_operand`, `re_compiler.c`), leaving only the input
  side to fold per dispatch: `apply_case_folding` closes a class's
  ranges under simple case folding (`/u`/`/v`) or Annex B canonicalization
  (legacy), and the built-in escapes (`\w` → `fill_builtin_class`),
  `\p{...}`/`\P{...}` (`parse_property_escape_class`, `re_lexer.c:704`),
  and `/v` operands all fold as they are built. Ordering is load-bearing:
  positive sets fold **before** `\W`/`\D`/`\S` complement (or a character
  could match both `\w` and `\W`), and `\P`'s fold/complement order
  differs between `/iu` and `/iv` — the comments at those functions carry
  the Node-verified details.
- `CharClass` ranges are kept **sorted and coalesced** by `add_range`
  (`re_lexer.c:316`) — every insertion merges into adjacent/overlapping
  ranges, so `cls->ranges` is always a minimal sorted disjoint-range
  representation. This is what lets `OP_CLASS` matching binary-search the
  range list (`class_contains`, `re_vm.c:132`) instead of scanning it —
  `\p{L}` alone is ~700 ranges, paid per text position.
- `CharClass` string sets are heap-owned (`class_strings_push` /
  `class_strings_free`, `re_lexer.c:291`) — right-sized buffers with
  deep-copy/ownership-transfer discipline, not fixed arrays; see the
  `Program` section above.
- Capture group names go through `parse_group_name` /
  `rx_idname_cp` / `rx_is_id_start` / `rx_is_id_continue`
  (`re_lexer.c:130–283`), which implement the actual ECMAScript
  `RegExpIdentifierName` grammar (including `\u` escapes *inside* a group
  name, e.g. `(?<a>x)`) against the UCD `ID_Start`/`ID_Continue`
  binary-search tables — not a simplified ASCII-only approximation.
- `fill_unicode_property` (`re_lexer.c:479`) has a small LRU-less cache
  (`prop_cache`, 64 entries) so repeated `\p{...}` escapes for the same
  property across one compile don't redo the UCD range union each time —
  cache is compile-scoped in practice since it's a `static` file-scope
  array that just grows across the process lifetime (NOT thread-safe: a
  multi-threaded native embedder must serialize compilation). The cache
  holds **ranges only** plus a pointer back to the generated
  `UCDProperty`: string sequences for "properties of strings"
  (`\p{RGI_Emoji}` and similar, `/v`-mode only) are re-copied from the
  immutable `ucd.h` table on every use, so the process-lifetime cache
  never owns heap string buffers. How those strings actually get
  *matched* — a single `OP_CLASS` instruction can't do it — lives in
  `re_compiler.c`'s `compile_class_with_strings`, described below.

## Parser (`re_parser.c`)

Standard precedence-climbing recursive descent, four levels:

```
parse_alt        ::= parse_concat ('|' parse_alt)?              -- alternation, right-recursive
parse_concat      ::= parse_quantifier+                          -- implicit sequencing
parse_quantifier   ::= parse_primary ('*'|'+'|'?'|'{m,n}') '?'?  -- '?' suffix = lazy
parse_primary      ::= literal | class | ^ | $ | \b | \B | backref
                      | ( ... ) | (?: ... ) | (?<name> ... )
                      | (?= ... ) | (?! ... ) | (?<= ... ) | (?<! ... )
                      | (?ims-ims: ... )
```

`parse_concat` and `parse_alt` build **left-leaning / right-leaning linear
chains** of `AST_CONCAT`/`AST_ALT` nodes for a flat sequence, not balanced
trees — a pattern with N flat concatenated atoms produces an AST N nodes
deep. `free_ast`, `validate_group_names`, `validate_backrefs`, and
`compile_node` all recurse through the tree with no iterative fallback,
which used to mean O(pattern length) recursion depth and a real
stack-overflow DoS on long patterns (confirmed via ASan). Two mechanisms
now bound it: `parse_concat` and `parse_alt` collect their atoms
iteratively and build **balanced** trees (`build_balanced`), so tree
height tracks real bracket nesting rather than pattern length (a
million-atom flat pattern is ~20 levels — flat length is no longer
limited); and each node's subtree height is tracked as it's built
(`ASTNode.depth`, via `finish_node()` in `re_parser.c`), with `parse_alt`
additionally checking a live recursion counter (`Lexer.parse_depth`)
before recursing into a group body, so a pattern that would produce a
tree taller than `MAX_AST_DEPTH` (`include/regexp.h`) is rejected with a
clean `prog->error` before any of the four walkers above — or the
parser's own recursion, for deeply nested groups — ever gets that deep.

Group numbering happens during parsing, not compilation:
`parse_primary`'s group case (`re_parser.c:106`) claims
`gid = ++lexer->prog->group_count` at the **opening paren**, before the
body is parsed, so group numbers follow source order of `(` — standard
ECMAScript numbering.

## Compiler (`compile_node`, `re_compiler.c:129`)

One switch over `ASTNode.type`, walked once, emitting directly into
`prog->code[]` via `emit()` (`re_compiler.c:35`). Match-time flag
decisions are **baked into instructions as they're emitted** rather than
read from `prog->*` at match time — `OP_CHAR`/backrefs carry effective
`ignore_case` in `arg2`, anchors carry effective `multiline` in `arg1`,
and `\b`/`\B` carry effective `ignore_case` in `arg1` (for `/iu`'s
fold-aware word-character set) — because `AST_MODIFIER_GROUP`
(`(?i:...)`/`(?-s:...)`) toggles those `prog` fields only for the
duration of the body's compile walk (the parser does the equivalent
toggle around the body's *lex*, for `.` and classes, which are built at
lex time). A few more things worth knowing before you touch this:

- **Quantifiers compile to a counter loop, not unrolled repetition.**
  `AST_QUANTIFIER` (`re_compiler.c:233`) emits `OP_INIT_COUNTER` /
  `OP_CHECK_COUNTER` / `OP_CLEAR_CAPTURES` (resetting the captures of
  groups defined inside the body at the top of each iteration, per
  ECMA-262's RepeatMatcher — `group_id_range` computes the contiguous id
  span) / body / `OP_INC_COUNTER` / `OP_JMP` back to the check.
  `OP_CHECK_COUNTER` carries `min`/`max`/`exit_pc` and a `lazy` bit,
  and the VM's handling of it (`re_vm.c:585`) is where greedy-vs-lazy
  actually branches: greedy pushes the "give up and exit" thread onto the
  backtrack stack and continues into the body first; lazy does the
  opposite. Each quantifier gets its own counter slot
  (`prog->counter_count++`), bounds-checked against `MAX_COUNTERS` = 256
  (raised from 16 once the VM stopped embedding counter arrays in every
  `Thread` — real patterns clear 16 easily; test262's classic XML
  shallow-parsing regex needs 75).
- **Lookaround compiles to an out-of-line subroutine plus a jump around
  it.** `AST_LOOKAHEAD`/`AST_LOOKBEHIND` (`re_compiler.c:206`) emit an
  unconditional `OP_JMP` past the lookaround body, compile the body inline
  right after the jump (so it's reachable by PC but not by fallthrough),
  terminate it with `OP_MATCH`, then patch the jump target and emit
  `OP_LOOKAHEAD`/`OP_LOOKBEHIND` (whose `arg1` is the body's start PC) at
  the point where control actually flows. At runtime, `OP_LOOKAHEAD` etc.
  (`re_vm.c:520`) don't jump to that PC at all — they make a **fresh
  recursive call to `vm_run`** at `depth + 1`, starting at that PC, with a
  copy of the current captures, and only splice the captures back in on
  success. This is why lookaround is a real recursive call (C stack, not
  the VM's own thread stack) — though each depth's heavy scratch
  (backtrack stack, arena, fail cache) lives in the `VMContext`'s
  per-depth slots on the heap, so the C frames themselves are small
  (`re_vm.c`'s top comment retells the ~2.2MB-per-frame history).
- **Lookbehind reuses the same compiler in reverse (`rtl=true`).**
  `compile_node`'s `rtl` parameter (`re_compiler.c:129`) flips `AST_CONCAT`'s
  emission order and `AST_GROUP`'s save-instruction order, so the *same*
  AST for the lookbehind body compiles into bytecode that matches
  right-to-left. The VM's `step` parameter (+1 or -1) then walks `sp`
  backward instead of forward
  for `OP_CHAR`/`OP_CLASS`/backreference matching — every one of those
  opcode handlers in the VM has a `step > 0` / `else` branch pair
  (`re_vm.c:335` onward) implementing forward vs. backward matching
  logic side by side. If you fix a bug in one branch, check whether the
  mirror branch has the same bug — several historical findings were
  exactly this (a bounds check present on one side, missing
  on the other). The surrogate decoding itself is shared
  (`decode_utf16`/`decode_utf16_backward`), extracted from what used to be
  four independently-maintained inline copies.
- **Named backreferences resolve at *match* time, not compile time.**
  `AST_NAMED_BACKREF` emits `OP_NAMED_BACKREF`, and
  the VM's handler picks, among all same-named group ids, the one that
  actually *participated* in the match — required for ES2025
  duplicate group names across mutually-exclusive alternation branches,
  where which `(?<x>…)` a `\k<x>` refers to is only knowable at runtime.
  The *candidate set* is compile-time, though: `compile_into` builds
  `Program.name_chain` (each group id links to the next id sharing its
  name), so the handler walks a short id chain instead of the strcmp scan
  over every group name it used to do per execution.
  (Numeric backrefs still compile to `OP_BACKREF` with the id baked in.)
- **A first-unit scan filter is computed from the finished bytecode.**
  `compute_scan_filter` (`re_compiler.c`) walks the emitted code from
  pc 0 through zero-width opcodes (splits, jumps, saves, counters,
  assertions) to the first *consuming* opcodes, and collects which UTF-16
  code units can possibly begin a match into `Program.scan_ascii` (a
  128-bit bitmap) plus a `scan_non_ascii` flag. Unanchored scan loops —
  `regex_exec` in the shim does this — then skip inadmissible start
  positions without entering the VM, which is the dominant cost of a
  non-matching scan (~36x measured on a 1M-unit subject with a rare
  first character). The filter over-approximates and is simply disabled
  (`scan_filter == false`) for patterns where a cheap answer doesn't
  exist: anything that can match empty, or whose first consuming opcode
  is a backreference or lookaround.
- **Character classes containing multi-codepoint strings** (`\q{...}` or a
  Unicode "property of strings" like `\p{RGI_Emoji}`, `/v`
  mode only) don't compile to a plain `OP_CLASS` — `compile_class_with_strings`
  (`re_compiler.c:69`) expands them into a real `OP_SPLIT`/`OP_CHAR`
  alternation instead, since a single `OP_CLASS` instruction can only ever
  test one code point. Strings are emitted **longest-first** (a stable
  `qsort` over heap-sized scratch — the spec's v-mode matcher tries string
  alternatives in descending length order, and the `OP_SPLIT` chain's
  emission order is its preference order), then the ordinary
  single-codepoint ranges as a trailing `OP_CLASS` branch. An
  alternation (reusing existing VM backtracking) was chosen over a new
  opcode because a one-shot string matcher couldn't backtrack from
  `\q{ab|a}`'s longer alternative to its shorter one.

### Opcodes (`include/regexp.h:53`)

| Opcode | Meaning |
|---|---|
| `OP_CHAR`, `OP_CLASS` | match one code point (literal / class), advance `sp` |
| `OP_SPLIT` | push one branch onto the backtrack stack, continue into the other (alternation) |
| `OP_JMP` | unconditional jump |
| `OP_SAVE` | record `sp` into `captures[arg1]` (group open/close, and 0/1 for the whole match) |
| `OP_LOOKAHEAD`/`OP_NEG_LOOKAHEAD`/`OP_LOOKBEHIND`/`OP_NEG_LOOKBEHIND` | recursive sub-match, no `sp` consumption on success |
| `OP_MATCH` | success (top-level or lookaround-subroutine) |
| `OP_INIT_COUNTER`/`OP_INC_COUNTER`/`OP_CHECK_COUNTER` | bounded-repetition loop control |
| `OP_ASSERT_START`/`OP_ASSERT_END` | `^`/`$` (multiline baked into `arg1`) |
| `OP_BACKREF`/`OP_NAMED_BACKREF` | match previously captured text (numeric id baked in / re-resolved by name at match time for ES2025 duplicate names) |
| `OP_WORD_BOUNDARY`/`OP_NON_WORD_BOUNDARY` | `\b`/`\B` (`arg1` = effective ignoreCase; under `/iu` the word set folds, adding U+017F/U+212A) |
| `OP_CLEAR_CAPTURES` | reset captures `[arg1..arg2]` — emitted at the top of every quantifier iteration so a group that doesn't participate in the final iteration reads as unset (ECMA-262 RepeatMatcher) |

## VM (`vm_run` via `vm_execute`/`VMContext`, `re_vm.c`)

A backtracking interpreter with an explicit thread stack (not the C call
stack) plus a memoizing fail-cache. All execution scratch lives in a
`VMContext` (created by the caller once per exec *call*, passed to
`vm_execute` for every start position tried in that call): one set of
{backtrack stack, captures arena, fail cache} per lookaround recursion
depth, allocated lazily on first use at that depth and reused until the
context is freed. This replaced allocating and initializing those buffers
inside every VM entry — which an unanchored search performs once per text
position — worth roughly two orders of magnitude on scan-heavy
workloads. The fail cache is
logically cleared between entries by a generation counter on each entry
rather than an 8192-slot reset. `vm_execute_internal` survives as a
one-shot convenience wrapper (create context, run once, free):

```c
typedef struct {
    int pc; const uint16_t* sp;
    const uint16_t** captures;
    int* counters;
    const uint16_t** counter_sp;
} Thread;
```

- `captures` is a pointer, not an embedded array — it used to be a fixed
  `MAX_GROUPS * 2` (510-pointer) array, which made every `Thread` several
  KB and this function's own stack frame balloon to ~2.2MB regardless of
  how many groups the compiled pattern actually has, a real crash risk
  once lookaround's recursion (below) is in the picture (`re_vm.c`'s top
  comment has the history). `counters`/`counter_sp` follow the identical
  arena discipline for the identical reason — as embedded
  `[MAX_COUNTERS]` arrays they both bloated every `Thread` and pinned
  `MAX_COUNTERS` at 16, a limit ordinary patterns exceeded. Each `VMContext` depth slot
  holds one arena, sized to `cap_pairs =
  (prog->group_count + 1) * 2` — the pattern's *actual* group count — with
  one slice per backtrack-stack slot (the in-flight thread's captures live
  in a separate allocation so arena growth never moves them), and
  every `Thread.captures` points into its own slice. The backtrack stack
  starts at `VM_STACK_CAPACITY` (512) slots and **doubles on demand up to
  `VM_STACK_MAX`** (`vm_grow_stack` rebases each slot's arena pointer
  after the tandem realloc); a match needing more than `VM_STACK_MAX`
  entries is abandoned as no-match — the greedy-quantifier loop pushes one
  backtrack entry per iteration, so this bounds how long a single
  quantifier run can be, and the pre-growth fixed array with unchecked
  pushes was a confirmed text-driven heap overflow (see `test/smoke.c`'s
  100k-run regression tests). **One consequence
  worth knowing before editing this function:** since a plain struct
  assignment now only copies `captures`' *pointer* (aliasing two threads'
  capture state instead of duplicating it), every place that used to push
  or pop a `Thread` by relying on that assignment's value semantics —
  `OP_SPLIT`, `OP_CHECK_COUNTER`'s two backtrack pushes, and popping the
  next thread off the stack — now goes through a small `thread_copy_state()`
  helper that copies the *data* `.captures` points to explicitly, into the
  destination thread's own already-assigned slice.
- `fail_cache` (`CacheEntry`, one per context depth slot,
  `CACHE_SIZE` = 8192, `include/regexp.h:15`) is the ReDoS mitigation:
  before running a popped thread, `hash_state` (power-of-two mask, not a
  division) hashes
  `(pc, sp, counters[])` and checks whether that exact state was already
  tried and failed on *this* VM entry (entries are generation-stamped, so
  "this entry's" failures never leak into the next start position's); if
  so, the thread is dropped without re-running. The equality test covers
  the *full* key — pc, sp, and the pattern's live `counters`/`counter_sp`
  (stored in right-sized side arrays next to the cache; comparing pc/sp
  alone let hash-colliding states with different counters alias, a
  false-negative bug). Capture state is deliberately NOT part of the key;
  it can only influence the future through backreferences, so the VM
  **bypasses the cache entirely when `Program.has_backrefs` is set**
  rather than paying a capture-sized key on every pattern (without the
  bypass, `/(?:(x)|x)*\1y/` on `"xy"` returned the wrong match). This is
  what keeps classic catastrophic-backtracking patterns like `(a+)+$`
  fast in practice —
  verified, this genuinely works for the common cases. Each recursion depth's cache is allocated once per
  context but **generation-cleared on every VM entry at that depth**, so
  it does **not** protect across a lookaround boundary — a quantifier
  *containing* a lookaround that itself contains a quantifier gets a
  logically fresh, uncorrelated cache for the inner call each time the
  outer one is retried.
- The **step budget** (`vm_context_set_step_budget`, fields on
  `VMContext`) is the hard backstop the cache is not: the cache is
  direct-mapped (collisions evict) and hashes `(pc, sp, counters[])`, so
  counter-keyed quantifier states defeat it — `(a+)+$` against a few
  hundred characters with no terminator is confirmed exponential without a
  budget (see `test/smoke.c`'s budget block, which hangs the suite if
  enforcement regresses). One step = one instruction dispatch, counted in
  the innermost loop across all VM entries and lookaround recursion for
  the context's lifetime; backreference compares additionally charge their
  O(capture length) unit-compare work. Exhaustion abandons the in-flight
  match exactly like the `VM_STACK_MAX` abandon (plain `return false`; all
  scratch is context-owned, so the context stays coherent), sets a sticky
  flag readable via `vm_context_budget_exhausted`, and every subsequent
  `vm_run` entry on that context fails immediately. Default is 0 =
  unlimited — behavior is unchanged for consumers that don't opt in
  (including, currently, the `regex_wasm.c` shim's `regex_exec`).
- Opcode dispatch is a dense `switch` over the opcode enum
  (`re_vm.c:334`) inside the innermost `while (true)` loop — it compiles
  to a jump table where the original `if`/`else if` chain compiled to
  sequential compares;
  each case's `path_failed = true; break;` idiom breaks the switch, and
  the `if (path_failed) break;` after it completes the loop exit.
- `OP_CHAR`/`OP_CLASS`/backreference matching each have a `step > 0` branch
  (forward, used for normal matching and lookahead) and a `step <= 0`
  branch (backward, used for lookbehind) implementing the matching logic
  side by side — the surrogate decoding itself is shared
  (`decode_utf16`/`decode_utf16_backward`), but the case-folding and
  bounds logic around it is still per-branch; check the mirror branch
  when fixing either side.
- `is_word_char_fold` (`re_vm.c:92`) is the `\b`/`\B` word-character
  test: plain ASCII `[a-zA-Z0-9_]`, widened via `unicode_casefold` when
  the instruction's baked-in ignoreCase bit *and* `prog->unicode` are
  both set (ECMA-262 `GetWordCharacters` — exactly U+017F and U+212A in
  current Unicode, but derived from the fold table, not hardcoded).

`vm_get_indices` (`re_vm.c:643`) is the only other public entry point —
it converts the VM's raw `const uint16_t*` capture pointers back into
integer offsets relative to `original_text`, which is what
`regex_exec` (`src/regex_wasm.c:137`) calls after a successful match.

## Unicode data (`include/ucd.h`, `scripts/generate_ucd.py`)

`ucd.h` is fully generated, ~40k lines, and must never be hand-edited.
`scripts/generate_ucd.py` (regenerate with
`python3 scripts/generate_ucd.py > include/ucd.h`) pulls straight from
unicode.org (`UNICODE_VERSION` at the top of the script; results are cached
under `.ucd_cache/`, gitignored) and emits, per Unicode property, a sorted
`UCDRange[]` array plus a linear `UCD_PROPERTIES[]` table with a
linear-scan `lookup_unicode_property(name, kind)` (entry count is a few
hundred — every accepted alias spelling is its own entry sharing one
ranges array — so linear scan is fine; it's the *ranges within* a property
that are binary-searched, via `rx_cp_in_ucd` in `re_lexer.c` and the inline
lookup in `ucd.h` itself). The `kind` tag (`UCD_KIND_BINARY`/`GC`/
`SCRIPT`/`SCX`) mirrors ECMA-262's `\p{...}` grammar: bare `\p{Name}` may
only name a binary property or General_Category value, while Script and
Script_Extensions values require their `\p{Script=...}`/
`\p{Script_Extensions=...}` key — the same name (e.g. `Greek`) exists
under both `SCRIPT` and `SCX` kinds with *different* range sets, since
`scx` completes the file's listed exceptions with each script's own
unlisted members per UAX #24. Binary properties are whitelisted to the
spec's table-binary-unicode-properties (data-file extras like contributory
properties are deliberately not emitted — real engines reject them), and
the grouped General_Category values (`Letter`, `LC`, `punct`, ...) are
pre-computed unions, so `re_lexer.c` needs no alias/grouping special
cases. "Properties of strings" (`RGI_Emoji` and friends) additionally
carry a `UCDStringSequence[]` array of multi-codepoint sequences; those
come from `emoji-sequences.txt`/`emoji-zwj-sequences.txt`, which Unicode
stopped publishing under versioned `Public/emoji/<ver>/` directories
after 16.0 — the script fetches `Public/emoji/latest/` and **hard-verifies
each file's `# Version:` header against the pinned `EMOJI_SEQ_VERSION`**,
so a silent upstream move to a newer emoji version fails generation
instead of drifting the data. Also generated: case-folding pairs
(`UCD_CASE_FOLD[]`, binary-searched by `unicode_casefold`), simple upper/
lowercase mappings, combining-class/decomposition/composition-exclusion
tables (present in `ucd.h` but currently **unused** by the engine — no
normalization step exists), and special-casing
data.

## WASM shim (`src/regex_wasm.c`)

Thin opaque-handle wrapper: `RegexHandle` bundles a `Program` (embedded by
value — this is where that ~2MB comes from per handle; `calloc`'d, since
`compile_into` requires a zero-initialized `Program` before first use)
with a separately `malloc`'d `int32_t* captures` buffer sized to
`(group_count+1)*2`. `regex_free` — and every `regex_compile` failure
path — also releases each class's heap-owned string set via
`class_strings_free` (see the `Program` section above). All
state (the last compile error, the current handle's captures) is accessed
through the handle or a single `static g_last_error` buffer — there's
**no thread-local isolation**, matching the fact that a WASM module
instance is single-threaded by default; don't assume this is safe to call
from multiple Web Workers sharing one instantiated module. `regex_exec`
(`src/regex_wasm.c:137`) is also where the **global (non-sticky) search
loop** lives — it's the shim, not the engine core, that scans start offsets
looking for the first match, advancing by code point under `/u`/`/v` so a
surrogate pair is never split mid-scan.
