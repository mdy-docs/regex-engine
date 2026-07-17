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
tracking upstream fixes, and `docs/IMPROVEMENTS.md` section 4 for why.
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
VM (vm_execute_internal) --- backtracking interpreter over Program.code,
                              invoked once per candidate start offset by
                              the caller (regex_exec in regex_wasm.c)
```

`compile_into` (`re_compiler.c:208`) is the single entry point that drives
lexer → parser → validation → compiler and populates a caller-owned
`Program`. There is no separate "optimize" pass — bytecode is emitted
directly from the AST in one recursive walk (`compile_node`,
`re_compiler.c:66`).

## The `Program` struct — why it's all fixed-size arrays

```c
typedef struct {
    Instruction code[MAX_OPCODES];       // 16384
    CharClass classes[MAX_CLASSES];      // 64
    char group_names[MAX_GROUPS][32];    // 255
    int code_count, class_count, group_count, counter_count;
    bool ignore_case, multiline, dot_all, sticky, unicode, has_indices, unicode_sets;
    const char* error;
} Program;
```

No `malloc` happens during *compilation* except the one
`malloc(sizeof(Program))` (or `sizeof(RegexHandle)` in the WASM shim) up
front — `Program` itself is ~2MB of fixed-size arrays, deliberately sized to
avoid any dynamic growth logic in a component meant to run inside a WASM
sandbox with no realloc-heavy allocator pressure. (*Matching* allocates a
`VMContext` — backtrack stacks, capture arenas, fail caches — once per
`regex_exec` call, reused across every start position; see the VM section
below.) `compile_into`'s top
comment (`re_compiler.c:208`) explains why it *doesn't* zero the whole struct on
each compile (`= {0}` would touch all ~2MB) — only the bookkeeping counters
and `group_names` (which is read by name lookups even for slots that were
never written) get reset.

All of `MAX_OPCODES`/`MAX_CLASSES`/`MAX_GROUPS`/`MAX_COUNTERS` are
enforced at their allocation sites (they originally weren't — see
`docs/IMPROVEMENTS.md` #1.2 for the confirmed corruption that caused). If
you're changing any of these constants, or adding a new allocation site,
find the corresponding `_count` field's existing check-before-index
increment sites first (`alloc_class`, `++prog->group_count`,
`counter_count++`, `emit()`'s `code_count++`) and follow the same
pattern — a new unchecked site reintroduces exactly #1.2's bug.

## Lexer (`re_lexer.c`; `Lexer`/`Token` types in `re_internal.h`)

One token of lookahead (`lexer->current`), hand-written character-class
dispatch in `next_token` (`re_lexer.c:823`). Notable pieces:

- `decode_utf16_lexer` (`re_lexer.c:24`) advances by one *code point* when
  `prog->unicode` is set (surrogate-pair aware), one *code unit* otherwise —
  this is the general pattern throughout the engine: unicode-mode
  operations are code-point-based, non-unicode-mode operations are
  UTF-16-code-unit-based (see `docs/IMPROVEMENTS.md`'s note on the `255`
  cap bug, which violates this pattern in one place).
- Escape sequences (`\d`, `\p{...}`, `\u{...}`, `\k<name>`, `\cX`, etc.) are
  all resolved inside `next_token`'s `case '\\':` block or inside
  `parse_char_class` for class-internal escapes — there's no separate
  escape-resolution pass.
- Character classes (`[...]`) are parsed by `parse_char_class`
  (`re_lexer.c:542`), which also implements `/v`-mode set operations (`&&`,
  `--`, nested `[...]`) and `\q{...}` string-literal alternatives in
  classes — these are `/v`-only (`unicode_sets`) ECMAScript 2024 features,
  gated on `lexer->prog->unicode_sets`.
- `CharClass` ranges are kept **sorted and coalesced** by `add_range`
  (`re_lexer.c:234`) — every insertion merges into adjacent/overlapping
  ranges, so `cls->ranges` is always a minimal sorted disjoint-range
  representation. This is what makes `OP_CLASS` matching a linear scan over
  a small range list rather than a per-character membership test.
- Capture group names go through `parse_group_name` /
  `rx_idname_cp` / `rx_is_id_start` / `rx_is_id_continue`
  (`re_lexer.c:90–233`), which implement the actual ECMAScript
  `RegExpIdentifierName` grammar (including `\u` escapes *inside* a group
  name, e.g. `(?<a>x)`) against the UCD `ID_Start`/`ID_Continue`
  binary-search tables — not a simplified ASCII-only approximation.
- `fill_unicode_property` (`re_lexer.c:342`) has a small LRU-less cache
  (`prop_cache`, 64 entries) so repeated `\p{...}` escapes for the same
  property across one compile don't redo the UCD range union each time —
  cache is compile-scoped in practice since it's a `static` file-scope
  array that just grows across the process lifetime (see
  `docs/IMPROVEMENTS.md` for the thread-safety implication). It also feeds
  `CharClass.strings` for "properties of strings" (`\p{RGI_Emoji}` and
  similar, `/v`-mode only) — see `docs/IMPROVEMENTS.md`'s "Fixed since this
  analysis" section for how those actually get matched, since a single
  `OP_CLASS` instruction can't do it (that part lives in `re_compiler.c`'s
  `compile_class_with_strings`, described below).

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
`compile_node` all recurse through that chain with no iterative fallback,
which used to mean O(pattern length) recursion depth and a real
stack-overflow DoS on long patterns (confirmed via ASan) — see
`docs/IMPROVEMENTS.md` #1.3, now fixed: `parse_concat`/`parse_alt`/
`parse_quantifier`/`parse_primary` track each node's subtree height as
it's built (`ASTNode.depth`, via `finish_node()` in `re_parser.c`) and
`parse_alt` additionally checks a live recursion counter
(`Lexer.parse_depth`) before recursing into itself or a group body, so a
pattern that would produce a tree taller than `MAX_AST_DEPTH`
(`include/regexp.h`) is rejected with a clean `prog->error` before any of
the four walkers above — or the parser's own recursion, for deeply nested
groups/alternation — ever gets that deep.

Group numbering happens during parsing, not compilation:
`parse_primary`'s group case (`re_parser.c:77`) does
`node->id = ++lexer->prog->group_count` immediately, so group numbers are
assigned in the order groups' **opening parens** appear in the source —
standard ECMAScript numbering.

## Compiler (`compile_node`, `re_compiler.c:66`)

One switch over `ASTNode.type`, walked once, emitting directly into
`prog->code[]` via `emit()` (`re_compiler.c:24`). A few things worth
knowing before you touch this:

- **Quantifiers compile to a counter loop, not unrolled repetition.**
  `AST_QUANTIFIER` (`re_compiler.c:152`) emits `OP_INIT_COUNTER` /
  `OP_CHECK_COUNTER` / body / `OP_INC_COUNTER` / `OP_JMP` back to the
  check. `OP_CHECK_COUNTER` carries `min`/`max`/`exit_pc` and a `lazy` bit,
  and the VM's handling of it (`re_vm.c:349`) is where greedy-vs-lazy
  actually branches: greedy pushes the "give up and exit" thread onto the
  backtrack stack and continues into the body first; lazy does the
  opposite. Each quantifier gets its own counter slot
  (`prog->counter_count++`) — this is the `MAX_COUNTERS` = 16 resource this
  repo's `docs/IMPROVEMENTS.md` flags as unbounded.
- **Lookaround compiles to an out-of-line subroutine plus a jump around
  it.** `AST_LOOKAHEAD`/`AST_LOOKBEHIND` (`re_compiler.c:125`) emit an
  unconditional `OP_JMP` past the lookaround body, compile the body inline
  right after the jump (so it's reachable by PC but not by fallthrough),
  terminate it with `OP_MATCH`, then patch the jump target and emit
  `OP_LOOKAHEAD`/`OP_LOOKBEHIND` (whose `arg1` is the body's start PC) at
  the point where control actually flows. At runtime, `OP_LOOKAHEAD` etc.
  (`re_vm.c:309`) don't jump to that PC at all — they make a **fresh
  recursive call to `vm_execute_internal`** starting at that PC, with a
  copy of the current captures, and only splice the captures back in on
  success. This is why lookaround is a real recursive call (C stack, not
  the VM's own thread stack) — deeply nested lookarounds cost C stack
  frames.
- **Lookbehind reuses the same compiler in reverse (`rtl=true`).**
  `compile_node`'s `rtl` parameter (`re_compiler.c:66`) flips `AST_CONCAT`'s
  emission order and `AST_GROUP`'s save-instruction order, so the *same*
  AST for the lookbehind body compiles into bytecode that matches
  right-to-left. The VM's `step` parameter (`vm_execute_internal`'s 2nd-to-
  last-but-one arg, +1 or -1) then walks `sp` backward instead of forward
  for `OP_CHAR`/`OP_CLASS`/backreference matching — every one of those
  opcode handlers in the VM has a `step > 0` / `else` branch pair
  (`re_vm.c:106` onward) implementing forward vs. backward matching
  logic side by side. If you fix a bug in one branch, check whether the
  mirror branch has the same bug — several of `docs/IMPROVEMENTS.md`'s
  findings are exactly this (a bounds check present on one side, missing
  on the other).
- **Backreferences resolve group *numbers* at compile time even for named
  backrefs.** `AST_NAMED_BACKREF` (`re_compiler.c:74`) looks up the name
  against `prog->group_names[]` at compile time and emits a plain
  `OP_BACKREF` with the resolved numeric id — `OP_NAMED_BACKREF` exists as
  an opcode and has VM support (`re_vm.c:216`, the
  `inst.op == OP_NAMED_BACKREF` branch re-resolves by name at *runtime* to
  handle the "duplicate group name across mutually-exclusive alternation
  branches" ES2025 case) but is never actually emitted by the compiler —
  `AST_NAMED_BACKREF` always emits `OP_BACKREF`. That runtime-resolution VM
  code path is currently dead; worth knowing before assuming
  `OP_NAMED_BACKREF` is reachable.
- **Character classes containing multi-codepoint strings** (`\q{...}` or a
  Unicode "property of strings" like `\p{RGI_Emoji_Flag_Sequence}`, `/v`
  mode only) don't compile to a plain `OP_CLASS` — `compile_class_with_strings`
  (`re_compiler.c:42`) expands them into a real `OP_SPLIT`/`OP_CHAR`
  alternation instead, since a single `OP_CLASS` instruction can only ever
  test one code point. See `docs/IMPROVEMENTS.md`'s "Fixed since this
  analysis" section for the bug this fixed and why an alternation (reusing
  existing VM machinery) was chosen over a new opcode.

### Opcodes (`include/regexp.h:22`)

| Opcode | Meaning |
|---|---|
| `OP_CHAR`, `OP_CLASS` | match one code point (literal / class), advance `sp` |
| `OP_SPLIT` | push one branch onto the backtrack stack, continue into the other (alternation) |
| `OP_JMP` | unconditional jump |
| `OP_SAVE` | record `sp` into `captures[arg1]` (group open/close, and 0/1 for the whole match) |
| `OP_LOOKAHEAD`/`OP_NEG_LOOKAHEAD`/`OP_LOOKBEHIND`/`OP_NEG_LOOKBEHIND` | recursive sub-match, no `sp` consumption on success |
| `OP_MATCH` | success (top-level or lookaround-subroutine) |
| `OP_INIT_COUNTER`/`OP_INC_COUNTER`/`OP_CHECK_COUNTER` | bounded-repetition loop control |
| `OP_ASSERT_START`/`OP_ASSERT_END` | `^`/`$` (multiline-aware) |
| `OP_BACKREF`/`OP_NAMED_BACKREF` | match previously captured text |
| `OP_WORD_BOUNDARY`/`OP_NON_WORD_BOUNDARY` | `\b`/`\B` |
| `OP_CLEAR_CAPTURES` | **defined, VM-implemented, never emitted by the compiler** — see `docs/IMPROVEMENTS.md`; this is the missing piece behind the "stale captures from earlier alternation branch survive a later iteration" bug |

## VM (`vm_run` via `vm_execute`/`VMContext`, `re_vm.c`)

A backtracking interpreter with an explicit thread stack (not the C call
stack) plus a memoizing fail-cache. All execution scratch lives in a
`VMContext` (created by the caller once per exec *call*, passed to
`vm_execute` for every start position tried in that call): one set of
{backtrack stack, captures arena, fail cache} per lookaround recursion
depth, allocated lazily on first use at that depth and reused until the
context is freed. This replaced allocating and initializing those buffers
inside every VM entry — which an unanchored search performs once per text
position — worth roughly two orders of magnitude on scan-heavy workloads
(see `docs/IMPROVEMENTS.md` §2 for measurements). The fail cache is
logically cleared between entries by a generation counter on each entry
rather than an 8192-slot reset. `vm_execute_internal` survives as a
one-shot convenience wrapper (create context, run once, free):

```c
typedef struct {
    int pc; const uint16_t* sp;
    const uint16_t** captures;
    int counters[MAX_COUNTERS];
    const uint16_t* counter_sp[MAX_COUNTERS];
} Thread;
```

- `captures` is a pointer, not an embedded array — it used to be a fixed
  `MAX_GROUPS * 2` (510-pointer) array, which made every `Thread` several
  KB and this function's own stack frame balloon to ~2.2MB regardless of
  how many groups the compiled pattern actually has, a real crash risk
  once lookaround's recursion (below) is in the picture — see
  `docs/IMPROVEMENTS.md` #1.1 for the history. Each `VMContext` depth slot
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
  `CACHE_SIZE` = 8192, `include/regexp.h:11`) is the ReDoS mitigation:
  before running a popped thread, `hash_state` (power-of-two mask, not a
  division) hashes
  `(pc, sp, counters[])` and checks whether that exact state was already
  tried and failed on *this* VM entry (entries are generation-stamped, so
  "this entry's" failures never leak into the next start position's); if
  so, the thread is dropped without re-running. This is what keeps classic
  catastrophic-backtracking patterns like `(a+)+$` fast in practice (see
  `docs/IMPROVEMENTS.md`'s "positive finding" — this genuinely works for
  the common cases). It's allocated fresh per call, including per
  recursive lookaround call, so it does **not** protect across a
  lookaround boundary — a quantifier *containing* a lookaround that itself
  contains a quantifier gets a fresh, uncorrelated cache for the inner
  call each time the outer one is retried.
- Each opcode handler is a big `if`/`else if` chain (not a jump table /
  `switch`) inside the innermost `while (true)` loop (`re_vm.c:172`
  onward) — this is the hot loop; see `docs/IMPROVEMENTS.md`'s performance
  section for the `switch` vs `if`-chain tradeoff here.
- `OP_CHAR`/`OP_CLASS`/backreference matching each have a `step > 0` branch
  (forward, used for normal matching and lookahead) and a `step <= 0`
  branch (backward, used for lookbehind) that independently reimplement
  surrogate-pair decoding, case-folding, and bounds checks. They are *not*
  shared helper functions — this duplication is the root cause of several
  "bug fixed on one side, not the mirror side" findings.

`vm_get_indices` (`re_vm.c:379`) is the only other public entry point —
it converts the VM's raw `const uint16_t*` capture pointers back into
integer offsets relative to `original_text`, which is what
`regex_exec` (`src/regex_wasm.c:132`) calls after a successful match.

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
cases. Also generated: case-folding pairs
(`UCD_CASE_FOLD[]`, binary-searched by `unicode_casefold`), simple upper/
lowercase mappings, combining-class/decomposition/composition-exclusion
tables (present in `ucd.h` but currently **unused** by the engine — no
normalization step exists; see `docs/IMPROVEMENTS.md`), and special-casing
data.

## WASM shim (`src/regex_wasm.c`)

Thin opaque-handle wrapper: `RegexHandle` bundles a `Program` (embedded by
value — this is where that ~2MB comes from per handle) with a separately
`malloc`'d `int32_t* captures` buffer sized to `(group_count+1)*2`. All
state (the last compile error, the current handle's captures) is accessed
through the handle or a single `static g_last_error` buffer — there's
**no thread-local isolation**, matching the fact that a WASM module
instance is single-threaded by default; don't assume this is safe to call
from multiple Web Workers sharing one instantiated module. `regex_exec`
(`src/regex_wasm.c:132`) is also where the **global (non-sticky) search
loop** lives — it's the shim, not the engine core, that scans start offsets
looking for the first match, advancing by code point under `/u`/`/v` so a
surrogate pair is never split mid-scan.
