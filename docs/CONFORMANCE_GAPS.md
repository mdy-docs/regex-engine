# Conformance gaps — the work-through list

Every known way baru-re diverges from a real ECMAScript engine, in rough
priority order. This is the actionable companion to two machine-readable
sources: `test/test262.expectations` (the exact failing test262 files, which
the CI conformance run enforces can only shrink) and the "Residual
limitation" notes scattered through `docs/IMPROVEMENTS.md`. If you fix a gap,
delete its entry here **and** the corresponding line(s) in
`test/test262.expectations` — `make test262` fails on an unexpected *pass*,
so a fixed gap that isn't de-listed turns CI red.

Status legend: **[real]** a genuine spec divergence worth closing ·
**[scope]** deliberately outside a standalone matcher's job ·
**[feature]** an unimplemented spec feature, larger than a bug fix.

Verify every fix the way the rest of this repo does: diff against Node
(`node -e "…"`) for the exact cases, run `make test`, `make test-asan`,
`make test262`, and a `make fuzz` burst.

---

## 4. [scope] Match-result object descriptors

**Fails:** `match-indices/indices-property.js`,
`match-indices/indices-groups-object.js`, `named-groups/groups-object.js`.

**What's wrong:** these assert the property descriptors
(writable/enumerable/configurable) and prototype of the `Array` that
`RegExp.prototype.exec` / `String.prototype.match` return. That object is
the *host's* RegExp result plumbing; this engine's contract ends at
producing capture spans (`regex_captures_ptr`). The test262 runner's shim
builds a plausible result array but doesn't replicate every descriptor.

**Not planned.** A consuming project that wants a spec-exact `RegExp` result
object builds it in its binding layer (as the runner's shim partially does);
it isn't the C engine's job. Listed for completeness so the count reconciles.

---

## Not conformance, but tracked

- **No Unicode normalization.** `ucd.h` carries decomposition / combining-
  class / composition-exclusion tables, but no engine step uses them — there
  is no NFC/NFD pass. Not required by RegExp semantics; noted because the
  data is present and might mislead.
- **`prop_cache` is process-global and not thread-safe** (`re_lexer.c`).
  Fine for WASM (single-threaded) and single-threaded native embedders;
  documented at its definition. A multi-threaded native embedder must
  serialize `regex_compile`. Deliberately left (see `docs/IMPROVEMENTS.md`
  #1.9).
