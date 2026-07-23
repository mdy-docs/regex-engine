# baru-re

A standalone C regex engine (ECMAScript-flavored: named groups, lookbehind,
Unicode property escapes, `/u`/`/v` mode, etc.), extracted from the jsvm2
JavaScript engine with a thin WASM shim on top. Zero runtime dependencies
beyond libc — no GC, no string-interning, no host-engine `Value`/`Object`
types.

**The name:** *bārû* is the Babylonian term for a diviner — a reader of
signs and patterns — plus *RE* for regular expression.

**Try it live:** [the WASM playground demo](https://mdy-docs.github.io/baru-re/)
(source in `web/`, published via `.github/workflows/pages.yml`) runs this
exact engine, compiled to WebAssembly, entirely in your browser.

**New to this repo?** Start with [`CLAUDE.md`](CLAUDE.md) — it's the
onboarding doc (for humans and AI agents alike) and points to
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) (how the engine works
internally, including the memory-safety and correctness history worth
knowing before relying on this in anything security-sensitive or exposed
to untrusted patterns).

## Provenance

Extracted from jsvm2's `src/regexp.c` / `include/regexp.h` (the compiler +
backtracking VM) and `include/ucd.h` (the Unicode character database:
case-folding, `\p{...}` property tables). Those three files never touched
jsvm2's GC-managed `JSString`/`Value`/`Object` types in the first place —
they operate purely on raw `const uint16_t*` UTF-16 buffers and a
self-contained `Program` struct, so the extraction is a straight copy. The
one dependency the extracted engine had on the rest of jsvm2 was an 8-line
inline UTF-16 surrogate-pair decoder (`decode_utf16`, originally in jsvm2's
`js_string.h`); it's copied inline here (`src/re_vm.c`) rather than pulling
in that whole header.

jsvm2's own `src/builtins_regexp.c` (not copied here) is the part that
stays behind — it's the glue that converts between jsvm2's `Value`/
`JSString`/`Object` and this engine's raw UTF-16 buffers. `src/regex_wasm.c`
in this directory is a from-scratch replacement for that glue, aimed at a
generic WASM host instead of jsvm2 specifically.

**Note on the engine's own file layout:** jsvm2's `src/regexp.c` was
originally extracted here as a single verbatim file of the same name. It
has since been split into `src/re_lexer.c` / `re_parser.c` / `re_compiler.c`
/ `re_vm.c` (plus a private `src/re_internal.h` for what's shared between
them) for maintainability — see `docs/ARCHITECTURE.md`'s intro for the
current layout and why. This is a
deliberate, acknowledged divergence from jsvm2's own single-file layout: if
you're porting a fix from (or back to) jsvm2's `regexp.c`, the code itself
is still line-for-line recognizable, just reorganized by pipeline stage —
`docs/ARCHITECTURE.md` maps each stage to its file.

## Layout

```
include/
  regexp.h       engine API: Program struct, compile_into, vm_execute_internal, vm_get_indices
  ucd.h          Unicode data tables (generated, header-only, ~40k lines)
  regex_wasm.h   declarations for the WASM shim's exported functions
src/
  re_lexer.c     scanner + CharClass/Unicode-property construction
  re_parser.c    recursive-descent AST builder
  re_compiler.c  AST -> bytecode compiler, plus compile_into (the top-level entry point)
  re_vm.c        backtracking VM (vm_execute_internal, vm_get_indices)
  re_internal.h  private, shared-between-the-above types/declarations (not public API)
  regex_wasm.c   WASM shim: opaque handles, UTF-16-buffer-in/int32-buffer-out API
scripts/
  generate_ucd.py generates include/ucd.h from unicode.org data (see below)
test/
  smoke.c        native sanity test against the shim API (no Emscripten needed)
  node_smoke.mjs end-to-end test against the actual compiled .wasm, via Node
web/
  index.html/style.css/app.js   the WASM playground demo (see "Web demo" below)
docs/
  ARCHITECTURE.md  how the lexer/parser/compiler/VM actually work
.github/workflows/
  test.yml         make test + make test-wasm on every push/PR
  pages.yml         builds the wasm demo and deploys web/ to GitHub Pages
```

## Building

```sh
make test       # native smoke test (cc, no Emscripten needed)
make wasm       # emcc build -> dist/baru-re.js + dist/baru-re.wasm
make test-wasm  # builds wasm, then runs node_smoke.mjs against the real artifact
make demo       # builds wasm, copies artifacts into web/ for local testing
```

To regenerate `include/ucd.h` (e.g. after bumping `UNICODE_VERSION` in the
script to pick up a new Unicode release):

```sh
python3 scripts/generate_ucd.py > include/ucd.h
```

Fetches from unicode.org on first run, caching each source file under
`.ucd_cache/` (gitignored) so subsequent regenerations at the same version
are offline/instant. No dependencies beyond the Python 3 standard library.

`make wasm` needs `emcc` on `PATH` (Emscripten SDK). The build is
`MODULARIZE=1`, exporting a factory function `createBaruReModule`
importable from either a browser or Node (`ENVIRONMENT=web,node`).

## API (`src/regex_wasm.c` / `include/regex_wasm.h`)

Deliberately low-level: the caller owns all buffers (via the
Emscripten-exported `_malloc`/`_free`) and passes raw pointers + lengths.
No JS-friendly wrapper is included — that's a natural next layer to add in
whatever project consumes this, once you know what shape you actually want
match results in (a plain array of offsets? a `RegExp`-like class? etc.).

```c
int      regex_flag_bit(int flag_char);      // 'g'/'i'/'m'/'s'/'y'/'u'/'d'/'v' -> its REGEX_FLAG_* bit, or 0
uintptr_t regex_compile(const uint16_t* pattern, int pattern_units, int flags); // NUL-terminated pattern; returns 0 on failure
const char* regex_last_error(void);           // valid until the next regex_compile() call
int      regex_group_count(uintptr_t handle); // capturing groups, not counting group 0 (the whole match)
const char* regex_group_name(uintptr_t handle, int index); // "" if group `index` is unnamed
int      regex_exec(uintptr_t handle, const uint16_t* text, int text_units, int start_index);
void     regex_set_step_budget(uintptr_t handle, double max_steps); // 0 = unlimited (default); re-armed per exec
int      regex_budget_exhausted(uintptr_t handle); // 1 iff the most recent exec gave up on its budget
const int32_t* regex_captures_ptr(uintptr_t handle); // (group_count+1)*2 ints: [start0,end0, start1,end1, ...], -1 = unmatched
void     regex_free(uintptr_t handle);
```

Notes:
- **`regex_compile`'s `pattern` must be NUL-terminated** (the underlying
  `compile_into` has no length parameter — it scans for `\0`). `text`
  passed to `regex_exec` does *not* need NUL-termination; `text_units` is
  authoritative there.
- A compiled `Program` is a large, fixed-size struct (multi-megabyte —
  `MAX_OPCODES`/`MAX_CLASSES`/`MAX_GROUPS` in `regexp.h` are generous fixed
  bounds, not dynamically sized) and is always heap-allocated by
  `regex_compile`, never put on the stack. **Compile once per distinct
  pattern and reuse the handle** across many `regex_exec` calls — don't
  recompile per match attempt. If footprint matters for your target (e.g.
  compiling many distinct patterns at once), shrinking those `MAX_*`
  constants in `include/regexp.h` is the lever — see that file.
- If you call `compile_into` directly (bypassing `regex_compile`), the
  `Program` must be **zero-initialized before its first compile** (`calloc`
  or `memset`), and released via per-class `class_strings_free` — class
  string sets (`\q{…}`, Unicode properties of strings) are heap-owned,
  right-sized buffers, not part of the fixed-size struct; `regex_compile`/
  `regex_free` handle both automatically. See `CharClass` in
  `include/regexp.h` for the ownership rules.
- `regex_exec` honors the sticky (`/y`) and unicode (`/u`, `/v`) flags baked
  into the compiled pattern automatically (no need to pass them again):
  sticky anchors exactly at `start_index`; unicode mode advances the search
  cursor by code point (never splitting a surrogate pair) when scanning for
  a match start.
- All offsets are **UTF-16 code units**, matching JS `String` indexing
  semantics (a `String.prototype.slice(caps[0], caps[1])` on the original
  JS string reconstructs the match).
- **ReDoS / step budget**: the VM's fail cache defuses many classic
  catastrophic-backtracking patterns, but not all of them (it is
  direct-mapped, collision-evicted, and bypassed entirely for patterns with
  backreferences — `(a+)+$` against a few
  hundred characters backtracks exponentially). Hosts running untrusted
  patterns should always set a hard step budget. Through the shim:

  ```c
  regex_set_step_budget(handle, 1000000 + 2000.0 * text_units);
  if (!regex_exec(handle, text, text_units, 0) && regex_budget_exhausted(handle)) {
      /* gave up, not a genuine no-match -- surface a catchable error */
  }
  ```

  The budget is re-armed on every `regex_exec` call and spans that whole
  call (all start positions, lookaround recursion included); 0 means
  unlimited, which is the default. Native embedders driving the engine API
  directly get the same via `vm_context_set_step_budget` /
  `vm_context_budget_exhausted` on their `VMContext` — see the comment in
  `include/regexp.h` for sizing guidance.

### Using from JavaScript via npm

The compiled engine is published as
[`@mdy-docs/baru-re`](https://www.npmjs.com/package/@mdy-docs/baru-re) —
the package ships exactly two artifacts, `dist/baru-re.js` (the Emscripten
glue, UMD-style: CommonJS `require`, ESM `import`, or a browser `<script>`
global all work) and `dist/baru-re.wasm`, which the glue locates next to
itself automatically in both Node and bundler/browser setups.

```sh
npm install @mdy-docs/baru-re
```

```js
import createBaruReModule from "@mdy-docs/baru-re";
const Module = await createBaruReModule();
// Module.cwrap / Module._malloc etc. as below
```

If a bundler or server setup needs the `.wasm` asset's path explicitly
(e.g. to serve it statically), it's exported as a subpath:
`import.meta.resolve("@mdy-docs/baru-re/baru-re.wasm")` /
`require.resolve("@mdy-docs/baru-re/baru-re.wasm")`.

The package intentionally exposes the same low-level API documented above —
see the note at the top of this section about the missing (deliberately)
JS-friendly wrapper layer.

### Minimal JS usage sketch (see `test/node_smoke.mjs` for a full working example)

```js
import createBaruReModule from "@mdy-docs/baru-re"; // or "./dist/baru-re.js" from a checkout
const Module = await createBaruReModule();
const regex_compile = Module.cwrap("regex_compile", "number", ["number", "number", "number"]);
// ...cwrap the rest per regex_wasm.h...

function writeUtf16(str) {
  const ptr = Module._malloc((str.length + 1) * 2);
  const units = new Uint16Array(Module.HEAPU16.buffer, ptr, str.length + 1);
  for (let i = 0; i < str.length; i++) units[i] = str.charCodeAt(i);
  units[str.length] = 0;
  return ptr;
}

const patPtr = writeUtf16("(\\d+)-([a-z]+)");
const handle = regex_compile(patPtr, 0, /* flags */ 0);
// regex_exec(handle, textPtr, textLen, 0), then read regex_captures_ptr(handle)
```

## Web demo

`web/` is a small dependency-free regex playground (pattern + flags +
subject text, live highlighted matches, a capture-group table, a handful of
preset patterns) built directly on `regex_wasm.c`'s API — it's both a demo
and a worked example of the "host-appropriate binding layer" step below
(see `web/app.js`'s `findAllMatches` for a from-scratch, matchAll-style
global-search loop built on the low-level `regex_exec` API).

To run it locally:
```sh
make demo                       # builds wasm, copies artifacts into web/
python3 -m http.server -d web   # then open http://localhost:8000/
```
(`baru-re.js`/`.wasm` fetched by the page over `file://` won't work in
most browsers — serve it over `http://`.)

It's deployed automatically to GitHub Pages by
`.github/workflows/pages.yml` on every push to `main` that touches
`src/`, `include/`, or `web/` — the workflow does the equivalent of
`make demo` in CI and publishes the result. One-time setup this workflow
can't do for you: in the repo's **Settings → Pages**, set **Source** to
**GitHub Actions**.

The demo's Makefile target enables Emscripten's `STACK_OVERFLOW_CHECK` and
a larger `STACK_SIZE` because deeply nested patterns once caused a
confirmed C-stack-overflow crash here (since fixed, but parsing still
recurses per nested group and Emscripten's default stack is only 64KB) —
without those flags an overflow can silently corrupt WASM linear memory
instead of throwing a catchable error. `web/app.js` catches that error and
transparently reloads the WASM instance so the page keeps working; that's a
build/host-level mitigation, not a fix for the underlying engine bug.

## Integrating into another WASM package

This directory is meant to be copied (or `git subtree`/submodule'd) into
the target project. From there:
1. Drop `include/` and `src/` in, add `src/re_lexer.c` + `src/re_parser.c` +
   `src/re_compiler.c` + `src/re_vm.c` + `src/regex_wasm.c` (or your own
   replacement shim in place of the last one) to that project's Emscripten
   build.
2. If the target project has its own opinions about `EXPORTED_FUNCTIONS`/
   `EXPORTED_RUNTIME_METHODS`/`ENVIRONMENT`, merge the flags this
   `Makefile`'s `wasm` target uses into that build rather than running this
   one standalone.
3. Write a host-appropriate binding layer on top of `regex_wasm.c`'s raw
   API — a friendlier match-result shape, pattern caching, whatever the
   target project's ergonomics call for. `src/builtins_regexp.c` in jsvm2
   is one worked example of such a layer (jsvm2-specific, not reusable
   as-is, but illustrative of what each API call is for: `build_groups_object`
   for named-capture objects, the sticky/global search loop in
   `run_regexp_exec`, etc.).

## Testing against jsvm2's own regex test coverage

jsvm2's `test/test262/test/built-ins/RegExp/` subset and `test/*regex*.js`
exercise this same engine (pre-extraction) far more thoroughly than the
smoke tests here. If you change the engine itself (not just the shim), it's
worth cross-checking against jsvm2's test262 RegExp subset before and after,
since this is a verbatim copy of that same code and any regression there
would apply here too:

```sh
# from the jsvm2 repo root, not this directory
node test/run_test262.js test/test262/test/built-ins/RegExp
```

## License

MIT — see [`LICENSE`](LICENSE).
