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

## 1. [real] Unicode case folding of built-in class escapes under `/iu`

**Fails:** `regexp-modifiers/add-ignoreCase-affects-slash-{lower,upper}-{b,p,w}.js`
(6 tests), and the same behavior at top level (`/\w/iu`, `/\p{Lu}/iu`, …),
which test262 happens to exercise via the modifier directory.

**What's wrong:** under `/iu`, the built-in class escapes must match via
simple case folding — `U+017F` (LONG S) folds to `s`, so `/\w/iu` matches it
and `/\W/iu` must *not*; `/\p{Lu}/iu` matches `a` (folds to `A ∈ Lu`). This
engine already folds *explicit* `[...]` classes (`apply_case_folding` in
`re_lexer.c`), but the standalone `\w`/`\d`/`\s`/`\p{…}` token paths never
call it, and `\b`/`\B` use a fixed-ASCII `is_word_char`.

**Why it's not a one-liner (and must be all-or-nothing):** folding the
positive escapes (`\w`, `\p`) is easy — call `apply_case_folding` on their
class the way `[...]` already does. But `\W`/`\D`/`\S` are stored as
*pre-complemented positive sets* (see `fill_builtin_class`), so folding them
naively is wrong. Half-fixing (fold `\w` but not `\W`) is worse than not
folding: a character would match *both* `\w` and `\W`. The correct fix:
represent `\W`/`\D`/`\S` with the `negated` flag over the folded positive
set (let the VM's existing `if (cls->negated) matched = !matched` invert),
and make `is_word_char` fold-aware for `\b`/`\B`.

**Approach:** (a) give `\W`/`\D`/`\S` the negated-flag representation instead
of pre-complementing; (b) call `apply_case_folding` on every built-in class
and `\p{…}` result when `ignore_case && unicode`; (c) for `\b`/`\B` under
`/iu`, fold before the word-char test. Diff a wide codepoint sweep against
Node, not just the test's sample chars — folding has long tails (Kelvin
sign, final sigma, etc.).

## 2. [feature] `/v`-mode set operations

**Fails:** `unicodeSets/generated/*` intersection (`[a&&b]`), difference
(`[a--b]`), and nested-class tests (only under `make test262 --generated`;
~20 files).

**What's wrong:** `/v` (`unicodeSets`) adds `&&` / `--` operators and nested
character classes inside `[...]`. The lexer parses single classes and
`\q{…}` string alternatives but has no set-algebra layer, so `[[0-9]&&\w]`
etc. don't compile to the intersected set.

**Approach:** a real feature, not a patch — parse `[...]` under `/v` into a
class *expression tree* (union/intersection/difference of nested
classes/escapes/strings), then evaluate it to a single `CharClass` (+ string
set) at compile time. The `CharClass` range representation already supports
the needed set ops (they're just sorted-range merges/intersections). The
string-alternative side (`&&`/`--` involving `\q{}` or property-of-strings)
is the fiddly part. Sizable; scope it on its own.

## 3. [real/feature] Full multi-codepoint sequence coverage (RGI_Emoji cap)

**Fails:** `unicodeSets/generated/rgi-emoji-*.js`, and any real use of the
five large properties-of-strings (`RGI_Emoji` has 2604 sequences,
`RGI_Emoji_ZWJ_Sequence` 1468, …) expecting full coverage.

**What's wrong:** `CharClass.strings` is a fixed `[128]` array
(`include/regexp.h`), so these properties match only their first 128
sequences. Documented at length in `docs/IMPROVEMENTS.md` ("Residual
limitation surfaced by this fix").

**Approach:** the honest fix is to stop storing sequence sets inline in the
fixed-size `Program`. Options: a separate heap-allocated, right-sized
sequence pool referenced by the class; or (since these are compile-time
constants) point directly at the `ucd.h` `UCDStringSequence[]` arrays
instead of copying. The latter is cleanest but changes the `CharClass`
ownership model. Weigh against the ~2MB `Program` footprint goal.

## 4. [real] The `Unknown` / `Zzzz` script value

**Fails:** `property-escapes/special-property-value-Script_Extensions-Unknown.js`.

**What's wrong:** `\p{sc=Unknown}` / `\p{sc=Zzzz}` name the script of
codepoints with no assigned script; Node accepts them, this engine rejects
(the generator sees no `Zzzz` in `Scripts.txt`, since unassigned codepoints
aren't listed).

**Approach:** in `scripts/generate_ucd.py`, synthesize the `Unknown`/`Zzzz`
script as the complement of every assigned script's codepoints (same
technique already used for the `Assigned` binary property), and emit it
under both `SCRIPT` and `SCX` kinds. Small, self-contained generator change
+ regenerate `ucd.h`.

## 5. [real] `\p{…}` as a character-class range endpoint

**Fails:** `property-escapes/character-class-range-start.js`.

**What's wrong:** `[\p{Hex}--]` under `/u` must be an early `SyntaxError` (a
property escape can't be a range endpoint), but this engine accepts it. The
range-endpoint rejection in `parse_char_class` (`re_lexer.c`) covers
`\d`/`\w`/`\s` but not `\p{…}`, and the `!= '-'` guard on the check lets this
specific shape through.

**Approach:** extend the `is_special`-as-range-endpoint check to fire for
`\p{…}` too. Narrow, but re-diff the passing class-range tests carefully —
the guard conditions there are subtle and easy to over-broaden.

## 6. [scope] Match-result object descriptors

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
