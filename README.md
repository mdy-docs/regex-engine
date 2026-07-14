# regex-engine

A standalone C regex engine (ECMAScript-flavored: named groups, lookbehind,
Unicode property escapes, `/u`/`/v` mode, etc.), extracted from the jsvm2
JavaScript engine with a thin WASM shim on top. Zero runtime dependencies
beyond libc — no GC, no string-interning, no host-engine `Value`/`Object`
types.

## Provenance

Extracted from jsvm2's `src/regexp.c` / `include/regexp.h` (the compiler +
backtracking VM) and `include/ucd.h` (the Unicode character database:
case-folding, `\p{...}` property tables). Those three files never touched
jsvm2's GC-managed `JSString`/`Value`/`Object` types in the first place —
they operate purely on raw `const uint16_t*` UTF-16 buffers and a
self-contained `Program` struct, so the extraction is a straight copy. The
one dependency `regexp.c` had on the rest of jsvm2 was an 8-line inline
UTF-16 surrogate-pair decoder (`decode_utf16`, originally in jsvm2's
`js_string.h`); it's copied inline at the top of `src/regexp.c` here rather
than pulling in that whole header.

jsvm2's own `src/builtins_regexp.c` (not copied here) is the part that
stays behind — it's the glue that converts between jsvm2's `Value`/
`JSString`/`Object` and this engine's raw UTF-16 buffers. `src/regex_wasm.c`
in this directory is a from-scratch replacement for that glue, aimed at a
generic WASM host instead of jsvm2 specifically.

## Layout

```
include/
  regexp.h       engine API: Program struct, compile_into, vm_execute_internal, vm_get_indices
  ucd.h          Unicode data tables (generated, header-only, ~40k lines)
  regex_wasm.h   declarations for the WASM shim's exported functions
src/
  regexp.c       the engine itself (lexer/parser/compiler + backtracking VM)
  regex_wasm.c   WASM shim: opaque handles, UTF-16-buffer-in/int32-buffer-out API
test/
  smoke.c        native sanity test against the shim API (no Emscripten needed)
  node_smoke.mjs end-to-end test against the actual compiled .wasm, via Node
```

## Building

```sh
make test       # native smoke test (cc, no Emscripten needed)
make wasm       # emcc build -> dist/regex-engine.js + dist/regex-engine.wasm
make test-wasm  # builds wasm, then runs node_smoke.mjs against the real artifact
```

`make wasm` needs `emcc` on `PATH` (Emscripten SDK). The build is
`MODULARIZE=1`, exporting a factory function `createRegexEngineModule`
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
- `regex_exec` honors the sticky (`/y`) and unicode (`/u`, `/v`) flags baked
  into the compiled pattern automatically (no need to pass them again):
  sticky anchors exactly at `start_index`; unicode mode advances the search
  cursor by code point (never splitting a surrogate pair) when scanning for
  a match start.
- All offsets are **UTF-16 code units**, matching JS `String` indexing
  semantics (a `String.prototype.slice(caps[0], caps[1])` on the original
  JS string reconstructs the match).

### Minimal JS usage sketch (see `test/node_smoke.mjs` for a full working example)

```js
import createRegexEngineModule from "./dist/regex-engine.js";
const Module = await createRegexEngineModule();
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

## Integrating into another WASM package

This directory is meant to be copied (or `git subtree`/submodule'd) into
the target project. From there:
1. Drop `include/` and `src/` in, add `src/regexp.c` + `src/regex_wasm.c`
   (or your own replacement shim) to that project's Emscripten build.
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
