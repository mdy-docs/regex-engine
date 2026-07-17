# CLAUDE.md

Onboarding for AI agents (and humans) working in this repo. Read this first;
it links out to deeper docs rather than repeating them.

## What this is

A standalone C implementation of an ECMAScript-flavored regex engine
(backtracking VM, not NFA/DFA-simulation) — named groups, lookaround,
`\p{...}` Unicode property escapes, `/u`/`/v` mode, `/s`/`/m`/`/y`, etc. Two
consumption modes:

1. **Native C**, compiled straight into a host C/C++ project.
2. **WASM**, via `src/regex_wasm.c`'s low-level shim, consumed from JS in
   both browser and Node.

It was extracted from a JS engine called jsvm2 (see README.md
"Provenance") and started as a **verbatim copy** of that engine's regex
compiler + VM in one file. That file (`src/regexp.c`) has since been split
into `src/re_lexer.c` / `re_parser.c` / `re_compiler.c` / `re_vm.c` (plus
`src/re_internal.h` for what's shared between them) for maintainability —
see `docs/ARCHITECTURE.md`'s intro and `docs/IMPROVEMENTS.md` section 4.
The *code* is still upstream-derived (treat it as "upstream-derived," not
repo-native code you'd casually restyle) — the *file layout* is not; it's
an intentional, acknowledged divergence from jsvm2's own single-file
version. Bugs found here likely exist in jsvm2 too (see README's "Testing
against jsvm2's own regex test coverage").

## Read these next, in order

1. `README.md` — build instructions, public API, integration guide. Ground
   truth for "how do I use this."
2. `docs/ARCHITECTURE.md` — how the engine actually works internally: lexer
   → AST → bytecode → backtracking VM with a fail-cache, `Program` struct
   layout, the UCD table generator. Read this before touching
   `src/re_lexer.c`/`re_parser.c`/`re_compiler.c`/`re_vm.c`.
3. `docs/IMPROVEMENTS.md` — a structural/quality/perf/testing/correctness
   analysis with concrete, verified findings (several are confirmed memory-
   safety bugs, not style nits). **Read this before assuming the engine is
   fully spec-correct or safe on adversarial input** — it currently is
   neither, in specific, enumerated ways.

## Layout (see README.md for the authoritative version)

```
include/regexp.h     engine API + Program/Instruction/CharClass structs, MAX_* bounds
include/ucd.h         generated Unicode tables (~40k lines, do not hand-edit)
include/regex_wasm.h  WASM shim's public C API
src/re_lexer.c         scanner + CharClass/Unicode-property construction
src/re_parser.c        recursive-descent AST builder
src/re_compiler.c      AST -> bytecode compiler, + compile_into (top-level entry point)
src/re_vm.c             backtracking VM (vm_execute_internal, vm_get_indices)
src/re_internal.h      private cross-file types/decls (Lexer/Token/ASTNode/NameSet) -- not public API
src/regex_wasm.c      thin WASM shim: opaque handles, UTF-16-in/int32-out
scripts/generate_ucd.py  regenerates include/ucd.h from unicode.org data
test/smoke.c           native smoke test (no Emscripten needed)
test/node_smoke.mjs    end-to-end test against the real compiled .wasm
docs/                  ARCHITECTURE.md, IMPROVEMENTS.md (this repo's own docs)
web/                   browser regex-playground demo, deployed to GitHub Pages
```

## Build / test loop

```sh
make test       # native smoke test, cc only, fast — run this after any src/re_*.c change
make test-asan  # same suite under ASan+UBSan — run this too if you touched the VM or any buffer handling
make wasm       # emcc build -> dist/baru-re.js + .wasm (needs emcc on PATH)
make test-wasm  # builds wasm, runs test/node_smoke.mjs against the real artifact
make test262    # builds wasm, runs tc39/test262 RegExp conformance vs test/test262.expectations
make fuzz       # builds the fuzz harness (test/fuzz.c); run ./test/fuzz [iters] [seed]
make demo       # builds wasm, copies artifacts into web/, so the demo runs locally
```

`emcc` comes from the Emscripten SDK (`emsdk`); if it's not on `PATH`,
`make wasm`/`make test-wasm`/`make test262`/`make demo` will fail with a
clear "command not found" — `make test` alone needs nothing but a C compiler.

**Always run `make test` after editing any `src/re_*.c`/`.h` or `include/regexp.h`.**
It's a broad regression suite now (every `docs/IMPROVEMENTS.md` §1 finding
plus the conformance/fuzz-found bugs), but still not exhaustive — if you're
touching matching semantics (not just the shim), also run `make test262`,
and sanity-check new cases against a real JS engine
(`node -e "console.log(/pattern/flags.exec('text'))"`). `make test262`
is the real spec-conformance gate; its known gaps are enumerated with
reasons in `test/test262.expectations`.

## Load-bearing constraints — do not violate these silently

- **`Program` is a large fixed-size struct** (`MAX_OPCODES`, `MAX_CLASSES`,
  `MAX_GROUPS`, `MAX_COUNTERS` in `include/regexp.h`), always heap-allocated,
  never on the stack, and **compiled once and reused** across many
  `regex_exec` calls — don't recompile per match attempt.
- **All of those `MAX_*` bounds are now enforced at compile time** (they
  weren't originally — see `docs/IMPROVEMENTS.md` §Correctness, P0 items,
  all fixed): exceeding one sets `prog->error` and fails compilation
  cleanly instead of overflowing a fixed-size array. If you add a new
  allocation site against any of these arrays, follow the same
  check-before-index pattern (`docs/IMPROVEMENTS.md` #1.2) — the bound
  being enforced elsewhere won't protect your new site.
- `regex_compile`'s `pattern` **must be NUL-terminated**; `regex_exec`'s
  `text` does **not** need to be, and `text_units` is authoritative — a
  tightly-sized, non-NUL-terminated buffer is fully supported (the
  `\b`/`\B` and lone-surrogate overreads that used to violate this are
  fixed; `make test-asan` guards them). New match-time code that peeks
  ahead must bound the peek by `text_end`, never by looking for a
  terminator.
- `include/ucd.h` is **generated** — never hand-edit it; change
  `scripts/generate_ucd.py` and regenerate (`python3 scripts/generate_ucd.py
  > include/ucd.h`).
- The engine logic (not the file layout — see "What this is" above) is a
  **verbatim upstream copy** from jsvm2 for the compiler/VM. Prefer minimal,
  surgical diffs over stylistic rewrites when fixing something, and
  consider whether a fix should also go back upstream (see README
  "Provenance").

## Conventions already in place (follow these, don't relitigate)

- No comments explaining *what* code does; comments here explain *why*
  (see the `decode_utf16` and `compile_into`'s `memset` comments for the
  house style).
- Errors are reported via `prog->error` (a `const char*` set once, checked
  with `if (!prog->error)` guards throughout `compile_into`) rather than
  early-return-on-error — that's the established pattern across the
  lexer/parser/compiler, not something to refactor into exceptions/longjmp/
  Result types in place.
- `regex_wasm.c` is deliberately low-level (caller-owned buffers, raw
  pointers, no JS ergonomics) — see README "API" section for why. Don't add
  a friendlier wrapper there; that belongs in a consuming project.
