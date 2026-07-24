// Runs tc39/test262's RegExp conformance tests against THIS engine (the
// compiled dist/baru-re.wasm, via its real JS glue), not the host JS
// engine's. Fetch the pinned test262 checkout first: scripts/get_test262.sh
// (or `make test262`, which does both).
//
// How: each test file runs in a fresh `vm` context whose `RegExp` is a
// class defined *inside* the sandbox (so instanceof / assert.throws
// work naturally) backed by injected bridge functions that call the WASM
// engine. Test262 sources use regex *literals*, which would otherwise
// compile to the host engine's regexes -- a small tokenizer rewrites every
// literal to `new RegExp("...", "...")` before evaluation, and
// String.prototype.match/replace/search/split are patched inside the
// sandbox to route through the shim.
//
// Pass/fail is judged against test/test262.expectations: a file listing
// tests KNOWN to fail (real conformance gaps, engine limits, or
// RegExp-object semantics outside a regex engine's scope -- each entry
// says which). The run exits nonzero on any unexpected failure OR any
// unexpected pass (so fixed gaps must be removed from the list -- it can
// only shrink silently, never grow).
//
// Usage: node test/test262_runner.mjs [--verbose] [pathFilter]

import { readFileSync, readdirSync, existsSync } from "node:fs";
import { join, dirname, relative } from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";
import createBaruReModule from "../dist/baru-re.js";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const T262 = process.env.TEST262_DIR || join(ROOT, "test262");
const EXPECTATIONS_PATH = join(ROOT, "test", "test262.expectations");

// Directories whose tests exercise matching behavior / pattern syntax --
// the parts a standalone regex engine is responsible for. RegExp-object
// plumbing trees (Symbol.*, prototype getters, @@species, escape) are out
// of scope entirely.
const TEST_DIRS = [
  "test/built-ins/RegExp/CharacterClassEscapes",
  "test/built-ins/RegExp/dotall",
  "test/built-ins/RegExp/lookBehind",
  "test/built-ins/RegExp/match-indices",
  "test/built-ins/RegExp/named-groups",
  "test/built-ins/RegExp/property-escapes",
  "test/built-ins/RegExp/regexp-modifiers",
  "test/built-ins/RegExp/unicodeSets",
  "test/built-ins/RegExp/prototype/exec",
  "test/built-ins/RegExp/prototype/test",
  // NOT test/language/literals/regexp: those test the JS *tokenizer's*
  // literal grammar (line terminators inside literals, \u-escaped flags,
  // etc.), which the literal-to-constructor transform cannot faithfully
  // represent -- they'd measure the transform, not the engine.
];

// The classic pattern-semantics suites (S15.10.1 grammar, S15.10.2
// escapes/anchors/quantifiers/classes) live as FLAT files directly in the
// RegExp root. Walked non-recursively so the RegExp-OBJECT plumbing
// subtrees (flags/source/toString/Symbol.* getters, @@species, etc.)
// stay out of scope -- those test the host RegExp object, not matching.
// This root was originally omitted entirely, which let engine bugs the
// modern suites never touch (\f/\v escapes, multiline \r anchors) coexist
// with a green run.
const FLAT_TEST_DIRS = [
  "test/built-ins/RegExp",
];

// Features that need host capabilities the sandbox deliberately lacks.
const SKIP_FEATURES = new Set([
  "cross-realm", "Symbol.match", "Symbol.replace", "Symbol.search",
  "Symbol.split", "Symbol.matchAll", "RegExp.escape",
]);

/* ---- engine bridge ------------------------------------------------------ */

const Module = await createBaruReModule();
const c_compile = Module.cwrap("regex_compile", "number", ["number", "number", "number"]);
const c_exec = Module.cwrap("regex_exec", "number", ["number", "number", "number", "number"]);
const c_free = Module.cwrap("regex_free", null, ["number"]);
const c_lastError = Module.cwrap("regex_last_error", "string", []);
const c_groupCount = Module.cwrap("regex_group_count", "number", ["number"]);
const c_groupName = Module.cwrap("regex_group_name", "string", ["number", "number"]);
const c_capturesPtr = Module.cwrap("regex_captures_ptr", "number", ["number"]);
const c_flagBit = Module.cwrap("regex_flag_bit", "number", ["number"]);

function writeUtf16(str) {
  const ptr = Module._malloc((str.length + 1) * 2);
  const units = new Uint16Array(Module.HEAPU16.buffer, ptr, str.length + 1);
  for (let i = 0; i < str.length; i++) units[i] = str.charCodeAt(i);
  units[str.length] = 0;
  return ptr;
}

let liveHandles = [];

const bridge = {
  compile(pattern, flagBits) {
    const p = writeUtf16(pattern);
    const h = c_compile(p, 0, flagBits);
    Module._free(p);
    if (!h) return { error: c_lastError() };
    liveHandles.push(h);
    const names = [];
    const n = c_groupCount(h);
    for (let i = 1; i <= n; i++) names.push(c_groupName(h, i));
    return { handle: h, groupCount: n, names };
  },
  flagBit(ch) {
    return c_flagBit(ch.charCodeAt(0));
  },
  exec(handle, str, start) {
    const t = writeUtf16(str);
    const m = c_exec(handle, t, str.length, start);
    Module._free(t);
    if (!m) return null;
    const n = c_groupCount(handle);
    const caps = new Int32Array(Module.HEAP32.buffer, c_capturesPtr(handle), (n + 1) * 2);
    return Array.from(caps);
  },
};

function freeHandles() {
  for (const h of liveHandles) c_free(h);
  liveHandles = [];
}

/* ---- regex-literal -> constructor-call transform ------------------------ */

// Tokenizer-level rewrite. `/` starts a regex literal iff the previous
// significant token can't end an expression -- the classic heuristic, which
// holds for test262's simple, formulaic sources. Files the transform
// can't handle surface as their own result category, never silent misses.
const BEFORE_REGEX_KEYWORDS = new Set([
  "return", "typeof", "instanceof", "in", "of", "new", "delete", "void",
  "throw", "case", "do", "else", "yield", "await",
]);

function transformSource(src) {
  let out = "";
  let i = 0;
  let prev = ""; // last significant token text ("" = start)

  const regexAllowed = () => {
    if (prev === "") return true;
    if (BEFORE_REGEX_KEYWORDS.has(prev)) return true;
    return /[-+*/%=<>!&|^~?:;,({[}]$/.test(prev);
  };

  const scanTemplate = () => { // at opening backtick
    out += src[i++];
    while (i < src.length) {
      const ch = src[i];
      if (ch === "\\") { out += src[i] + (src[i + 1] ?? ""); i += 2; continue; }
      if (ch === "`") { out += src[i++]; return; }
      if (ch === "$" && src[i + 1] === "{") {
        out += "${"; i += 2;
        let depth = 1;
        // interpolations in these files are trivial; copy through verbatim
        while (i < src.length && depth > 0) {
          if (src[i] === "{") depth++;
          else if (src[i] === "}") depth--;
          out += src[i++];
        }
        continue;
      }
      out += src[i++];
    }
  };

  while (i < src.length) {
    const ch = src[i];
    if (ch === "/" && src[i + 1] === "/") {
      const nl = src.indexOf("\n", i);
      out += nl === -1 ? src.slice(i) : src.slice(i, nl);
      i = nl === -1 ? src.length : nl;
      continue;
    }
    if (ch === "/" && src[i + 1] === "*") {
      const end = src.indexOf("*/", i + 2);
      if (end === -1) { out += src.slice(i); i = src.length; continue; }
      out += src.slice(i, end + 2);
      i = end + 2;
      continue;
    }
    if (ch === '"' || ch === "'") {
      const q = ch;
      let j = i + 1;
      while (j < src.length && src[j] !== q) {
        if (src[j] === "\\") j++;
        j++;
      }
      out += src.slice(i, j + 1);
      prev = '""';
      i = j + 1;
      continue;
    }
    if (ch === "`") { scanTemplate(); prev = '""'; continue; }
    if (ch === "/" && regexAllowed()) {
      // scan the literal body
      let j = i + 1;
      let inClass = false;
      let ok = false;
      while (j < src.length) {
        const c = src[j];
        if (c === "\\") { j += 2; continue; }
        if (c === "\n") break; // unterminated -- leave source as-is
        if (c === "[") inClass = true;
        else if (c === "]") inClass = false;
        else if (c === "/" && !inClass) { ok = true; break; }
        j++;
      }
      if (!ok) { out += ch; i++; prev = "/"; continue; }
      const body = src.slice(i + 1, j);
      let k = j + 1;
      while (k < src.length && /[A-Za-z]/.test(src[k])) k++;
      const flags = src.slice(j + 1, k);
      out += `new RegExp(${JSON.stringify(body)}, ${JSON.stringify(flags)})`;
      prev = ")";
      i = k;
      continue;
    }
    if (/[A-Za-z0-9_$]/.test(ch)) {
      let j = i;
      while (j < src.length && /[A-Za-z0-9_$]/.test(src[j])) j++;
      prev = src.slice(i, j);
      out += prev;
      i = j;
      continue;
    }
    if (!/\s/.test(ch)) prev = ch; // last char of an operator is enough for the heuristic
    out += ch;
    i++;
  }
  return out;
}

/* ---- sandbox bootstrap (runs INSIDE each vm context) -------------------- */

const BOOTSTRAP = String.raw`
"use strict";
(function(bridge) {
  const FLAG_PROPS = { d: "hasIndices", g: "global", i: "ignoreCase",
                       m: "multiline", s: "dotAll", u: "unicode",
                       v: "unicodeSets", y: "sticky" };

  class RegExp2 {
    constructor(pattern, flags) {
      if (pattern instanceof RegExp2 && flags === undefined) {
        flags = pattern.flags;
        pattern = pattern.source === "(?:)" ? "" : pattern.source;
      } else if (pattern instanceof RegExp2) {
        pattern = pattern.source === "(?:)" ? "" : pattern.source;
      }
      pattern = pattern === undefined ? "" : String(pattern);
      flags = flags === undefined ? "" : String(flags);
      let bits = 0;
      const seen = new Set();
      for (const ch of flags) {
        if (!(ch in FLAG_PROPS) || seen.has(ch)) throw new SyntaxError("Invalid flags: " + flags);
        seen.add(ch);
        if (ch !== "g") bits |= bridge.flagBit(ch);
      }
      if (seen.has("u") && seen.has("v")) throw new SyntaxError("Invalid flags: cannot combine u and v");
      const r = bridge.compile(pattern, bits);
      if (r.error) throw new SyntaxError(r.error + " in /" + pattern + "/" + flags);
      this._handle = r.handle;
      this._groupCount = r.groupCount;
      this._names = r.names;
      this.source = pattern === "" ? "(?:)" : pattern;
      this.flags = flags;
      for (const ch in FLAG_PROPS) this[FLAG_PROPS[ch]] = seen.has(ch);
      // Spec descriptor: writable, non-enumerable, non-configurable
      // (test262 lastIndex.js asserts exactly this shape).
      Object.defineProperty(this, "lastIndex", {
        value: 0, writable: true, enumerable: false, configurable: false,
      });
    }

    exec(str) {
      if (!(this instanceof RegExp2)) throw new TypeError("exec called on non-RegExp");
      str = String(str);
      const useLast = this.global || this.sticky;
      // RegExpBuiltinExec reads lastIndex (Get + ToLength, one valueOf)
      // unconditionally, even when global/sticky are unset and the value
      // is then discarded -- test262 counts the reads.
      let start = Math.trunc(+this.lastIndex) || 0;
      if (!useLast) start = 0;
      if (start < 0) start = 0;
      let caps = null;
      if (start <= str.length) caps = bridge.exec(this._handle, str, start);
      if (!caps) {
        if (useLast) this.lastIndex = 0;
        return null;
      }
      if (useLast) this.lastIndex = caps[1];
      const m = [];
      for (let i = 0; i <= this._groupCount; i++) {
        const s = caps[i * 2], e = caps[i * 2 + 1];
        m.push(s === -1 ? undefined : str.slice(s, e));
      }
      m.index = caps[0];
      m.input = str;
      const hasNames = this._names.some((n) => n !== "");
      if (hasNames) {
        const g = Object.create(null);
        for (let i = 1; i <= this._groupCount; i++) {
          const name = this._names[i - 1];
          if (!name) continue;
          if (!(name in g) || g[name] === undefined) g[name] = m[i];
        }
        m.groups = g;
      } else {
        m.groups = undefined;
      }
      if (this.hasIndices) {
        const ind = [];
        for (let i = 0; i <= this._groupCount; i++) {
          const s = caps[i * 2], e = caps[i * 2 + 1];
          ind.push(s === -1 ? undefined : [s, e]);
        }
        if (hasNames) {
          const g = Object.create(null);
          for (let i = 1; i <= this._groupCount; i++) {
            const name = this._names[i - 1];
            if (!name) continue;
            if (!(name in g) || g[name] === undefined) g[name] = ind[i];
          }
          ind.groups = g;
        } else {
          ind.groups = undefined;
        }
        m.indices = ind;
      }
      return m;
    }

    test(str) {
      if (!(this instanceof RegExp2)) throw new TypeError("test called on non-RegExp");
      return this.exec(str) !== null;
    }
    toString() { return "/" + this.source + "/" + this.flags; }
  }

  // Object.prototype.toString must report "[object RegExp]".
  Object.defineProperty(RegExp2.prototype, Symbol.toStringTag, {
    value: "RegExp", writable: false, enumerable: false, configurable: true,
  });

  const advance = (str, i, unicode) => {
    if (unicode && i < str.length - 1) {
      const c = str.charCodeAt(i);
      if (c >= 0xd800 && c <= 0xdbff) {
        const d = str.charCodeAt(i + 1);
        if (d >= 0xdc00 && d <= 0xdfff) return i + 2;
      }
    }
    return i + 1;
  };

  const toRe = (v) => (v instanceof RegExp2 ? v : new RegExp2(v === undefined ? "" : v));

  String.prototype.match = function (re) {
    re = toRe(re);
    if (!re.global) return re.exec(this);
    re.lastIndex = 0;
    const out = [];
    let m;
    while ((m = re.exec(this))) {
      out.push(m[0]);
      if (m[0] === "") re.lastIndex = advance(String(this), re.lastIndex, re.unicode || re.unicodeSets);
    }
    return out.length ? out : null;
  };

  String.prototype.search = function (re) {
    re = toRe(re);
    const save = re.lastIndex;
    re.lastIndex = 0;
    const g = re.global, y = re.sticky;
    re.global = false; re.sticky = false;
    const m = re.exec(this);
    re.global = g; re.sticky = y; re.lastIndex = save;
    return m ? m.index : -1;
  };

  function getSubstitution(matched, str, position, captures, groups, replacement) {
    let result = "";
    for (let i = 0; i < replacement.length; i++) {
      const ch = replacement[i];
      if (ch !== "$" || i === replacement.length - 1) { result += ch; continue; }
      const next = replacement[i + 1];
      if (next === "$") { result += "$"; i++; }
      else if (next === "&") { result += matched; i++; }
      else if (next === "\u0060") { result += str.slice(0, position); i++; } // backtick, written as an escape so it can't terminate the host's String.raw template
      else if (next === "'") { result += str.slice(position + matched.length); i++; }
      else if (next === "<") {
        const close = replacement.indexOf(">", i + 2);
        if (close === -1 || groups === undefined) { result += ch; continue; }
        const name = replacement.slice(i + 2, close);
        const v = groups[name];
        result += v === undefined ? "" : v;
        i = close;
      } else if (/[0-9]/.test(next)) {
        let num = next, len = 1;
        if (/[0-9]/.test(replacement[i + 2]) && parseInt(next + replacement[i + 2], 10) <= captures.length) {
          num = next + replacement[i + 2]; len = 2;
        }
        const idx = parseInt(num, 10);
        if (idx >= 1 && idx <= captures.length) {
          const v = captures[idx - 1];
          result += v === undefined ? "" : v;
          i += len;
        } else {
          result += ch;
        }
      } else {
        result += ch;
      }
    }
    return result;
  }

  String.prototype.replace = function (re, repl) {
    if (!(re instanceof RegExp2)) {
      // string search: first occurrence only, per the String algorithm
      const s = String(this), pat = String(re);
      const pos = s.indexOf(pat);
      if (pos === -1) return s;
      const r = typeof repl === "function" ? String(repl(pat, pos, s))
              : getSubstitution(pat, s, pos, [], undefined, String(repl));
      return s.slice(0, pos) + r + s.slice(pos + pat.length);
    }
    const s = String(this);
    let result = "";
    let lastEnd = 0;
    if (re.global) re.lastIndex = 0;
    for (;;) {
      const m = re.exec(s);
      if (!m) break;
      const captures = m.slice(1);
      const r = typeof repl === "function"
        ? String(repl(m[0], ...captures, m.index, s, ...(m.groups !== undefined ? [m.groups] : [])))
        : getSubstitution(m[0], s, m.index, captures, m.groups, String(repl));
      result += s.slice(lastEnd, m.index) + r;
      lastEnd = m.index + m[0].length;
      if (!re.global) break;
      if (m[0] === "") re.lastIndex = advance(s, re.lastIndex, re.unicode || re.unicodeSets);
    }
    return result + s.slice(lastEnd);
  };

  String.prototype.matchAll = function (re) {
    if (!(re instanceof RegExp2)) re = new RegExp2(re === undefined ? "" : re, "g");
    if (!re.global) throw new TypeError("matchAll requires the g flag");
    const s = String(this);
    const clone = new RegExp2(re.source === "(?:)" ? "" : re.source, re.flags);
    clone.lastIndex = re.lastIndex;
    return (function* () {
      for (;;) {
        const m = clone.exec(s);
        if (!m) return;
        yield m;
        if (m[0] === "") clone.lastIndex = advance(s, clone.lastIndex, clone.unicode || clone.unicodeSets);
      }
    })();
  };

  String.prototype.replaceAll = function (re, repl) {
    if (re instanceof RegExp2) {
      if (!re.global) throw new TypeError("replaceAll requires the g flag");
      return this.replace(re, repl);
    }
    const s = String(this), pat = String(re);
    if (pat === "") return s;
    let result = "", idx = 0;
    for (;;) {
      const pos = s.indexOf(pat, idx);
      if (pos === -1) break;
      const r = typeof repl === "function" ? String(repl(pat, pos, s))
              : getSubstitution(pat, s, pos, [], undefined, String(repl));
      result += s.slice(idx, pos) + r;
      idx = pos + pat.length;
    }
    return result + s.slice(idx);
  };

  const nativeSplit = String.prototype.split;
  String.prototype.split = function (re, limit) {
    if (!(re instanceof RegExp2)) {
      return nativeSplit.call(String(this), re, limit);
    }
    const s = String(this);
    const lim = limit === undefined ? 4294967295 : limit >>> 0;
    if (lim === 0) return [];
    const splitter = new RegExp2(re.source === "(?:)" ? "" : re.source,
      re.flags.includes("y") ? re.flags : re.flags + "y");
    if (s.length === 0) {
      return splitter.exec("") === null ? [s] : [];
    }
    const out = [];
    let p = 0, q = 0;
    while (q < s.length) {
      splitter.lastIndex = q;
      const m = splitter.exec(s);
      if (m === null) { q = advance(s, q, re.unicode || re.unicodeSets); continue; }
      const e = Math.min(splitter.lastIndex, s.length);
      if (e === p) { q = advance(s, q, re.unicode || re.unicodeSets); continue; }
      out.push(s.slice(p, q));
      if (out.length === lim) return out;
      for (let i = 1; i < m.length; i++) {
        out.push(m[i]);
        if (out.length === lim) return out;
      }
      p = e;
      q = p;
    }
    out.push(s.slice(p));
    return out;
  };

  // RegExp is callable without new; a Proxy forwards construct and
  // plain-call to the same class. Per 22.2.4.1, RegExp(re) with flags
  // undefined returns the SAME object when its constructor is RegExp --
  // several classic S15.10.3.1 tests assert identity.
  const RegExpCallable = new Proxy(RegExp2, {
    apply(target, thisArg, args) {
      // 22.2.4.1: only when the value's own .constructor is this very
      // RegExp does the call return it unchanged -- a tampered
      // constructor forces a fresh object (asserted by
      // call_with_regexp_not_same_constructor.js).
      if (args[0] instanceof RegExp2 && args[1] === undefined &&
          args[0].constructor === RegExpCallable) return args[0];
      return new target(...args);
    },
  });
  // instance.constructor must be the global RegExp value.
  Object.defineProperty(RegExp2.prototype, "constructor", {
    value: RegExpCallable, writable: true, enumerable: false, configurable: true,
  });
  // Spec descriptor for the global binding: writable, non-enumerable,
  // configurable (test262 prop-desc.js asserts this shape).
  Object.defineProperty(globalThis, "RegExp", {
    value: RegExpCallable, writable: true, enumerable: false, configurable: true,
  });
  globalThis.$DONOTEVALUATE = function () {};
})(globalThis.__bridge);
`;

/* ---- frontmatter -------------------------------------------------------- */

function parseFrontmatter(src) {
  const m = src.match(/\/\*---([\s\S]*?)---\*\//);
  const meta = { includes: [], features: [], flags: [], negative: null };
  if (!m) return meta;
  const yaml = m[1];
  const list = (key) => {
    const inline = yaml.match(new RegExp(`^\\s*${key}:\\s*\\[(.*?)\\]`, "m"));
    if (inline) return inline[1].split(",").map((s) => s.trim()).filter(Boolean);
    const block = yaml.match(new RegExp(`^\\s*${key}:\\s*\\n((?:\\s+-\\s+.*\\n?)+)`, "m"));
    if (block) return block[1].split("\n").map((s) => s.replace(/^\s*-\s*/, "").trim()).filter(Boolean);
    return [];
  };
  meta.includes = list("includes");
  meta.features = list("features");
  meta.flags = list("flags");
  const neg = yaml.match(/^\s*negative:\s*\n\s+phase:\s*(\S+)\s*\n\s+type:\s*(\S+)/m);
  if (neg) meta.negative = { phase: neg[1], type: neg[2] };
  return meta;
}

/* ---- runner ------------------------------------------------------------- */

const harnessCache = new Map();
function harness(name) {
  if (!harnessCache.has(name)) {
    harnessCache.set(name, readFileSync(join(T262, "harness", name), "utf8"));
  }
  return harnessCache.get(name);
}

function runFile(path, meta, verbose) {
  const raw = readFileSync(path, "utf8");
  if (meta.flags.includes("raw") || meta.flags.includes("module") || meta.flags.includes("async")) {
    return { status: "SKIP", reason: "flags: " + meta.flags.join(",") };
  }
  for (const f of meta.features) {
    if (SKIP_FEATURES.has(f)) return { status: "SKIP", reason: "feature: " + f };
  }

  const context = vm.createContext(Object.create(null));
  context.__bridge = bridge;
  context.print = () => {};
  try {
    vm.runInContext(BOOTSTRAP, context, { filename: "bootstrap.js" });
    vm.runInContext(harness("assert.js"), context, { filename: "assert.js" });
    vm.runInContext(harness("sta.js"), context, { filename: "sta.js" });
    vm.runInContext(harness("compareArray.js"), context, { filename: "compareArray.js" });
    for (const inc of meta.includes) {
      vm.runInContext(harness(inc), context, { filename: inc });
    }
    // sta.js defines $DONOTEVALUATE to throw a string, which is how real
    // test262 aborts phase:parse tests before execution. But our
    // literal->constructor transform turns a would-be *parse*-phase regex
    // SyntaxError into a *runtime* one from `new RegExp`, so we must let
    // execution proceed past the marker to reach (and catch) that throw.
    vm.runInContext("$DONOTEVALUATE = function () {};", context, { filename: "restub.js" });
  } catch (e) {
    return { status: "FAIL", reason: "harness setup: " + e.message };
  }

  const transformed = transformSource(raw);
  const source = meta.flags.includes("onlyStrict") ? '"use strict";\n' + transformed : transformed;

  let thrown = null;
  try {
    vm.runInContext(source, context, { filename: path, timeout: 120000 });
  } catch (e) {
    thrown = e;
  } finally {
    freeHandles();
  }

  if (meta.negative) {
    if (!thrown) return { status: "FAIL", reason: `expected ${meta.negative.type}, no error thrown` };
    const name = thrown && thrown.constructor ? thrown.constructor.name : String(thrown);
    if (name === meta.negative.type) return { status: "PASS" };
    return { status: "FAIL", reason: `expected ${meta.negative.type}, got ${name}: ${thrown.message}` };
  }
  if (thrown) {
    const msg = String(thrown && thrown.message || thrown).split("\n")[0].slice(0, 200);
    if (verbose) console.error(`  ${path}\n    ${msg}`);
    return { status: "FAIL", reason: msg };
  }
  return { status: "PASS" };
}

function* walk(dir, includeGenerated) {
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, entry.name);
    if (entry.isDirectory()) {
      // The `generated/` trees are machine-generated exhaustive codepoint
      // sweeps -- tens of thousands of match calls per file, minutes to
      // run. Skipped by default (the hand-written tests plus this repo's
      // own Node-diffed property probe cover the same logic); --generated
      // includes them for a thorough local pass.
      if (entry.name === "generated" && !includeGenerated) continue;
      yield* walk(p, includeGenerated);
    } else if (entry.name.endsWith(".js") && !entry.name.endsWith("_FIXTURE.js")) yield p;
  }
}

const verbose = process.argv.includes("--verbose");
const includeGenerated = process.argv.includes("--generated");
const filter = process.argv.slice(2).find((a) => !a.startsWith("--"));

if (!existsSync(join(T262, "harness", "assert.js"))) {
  console.error(`test262 checkout not found at ${T262} -- run scripts/get_test262.sh first`);
  process.exit(2);
}

const expected = new Set();
if (existsSync(EXPECTATIONS_PATH)) {
  for (const line of readFileSync(EXPECTATIONS_PATH, "utf8").split("\n")) {
    const t = line.trim();
    if (t && !t.startsWith("#")) expected.add(t.split(/\s+/)[0]);
  }
}

let pass = 0, skip = 0;
const failures = [];      // unexpected failures
const expectedFailures = []; // failures listed in expectations
const unexpectedPasses = [];

function* flatFiles(dir) {
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    if (entry.isFile() && entry.name.endsWith(".js") && !entry.name.endsWith("_FIXTURE.js"))
      yield join(dir, entry.name);
  }
}

function* allTestFiles() {
  for (const dir of TEST_DIRS) {
    const full = join(T262, dir);
    if (existsSync(full)) yield* walk(full, includeGenerated);
  }
  for (const dir of FLAT_TEST_DIRS) {
    const full = join(T262, dir);
    if (existsSync(full)) yield* flatFiles(full);
  }
}

{
  for (const file of allTestFiles()) {
    const rel = relative(T262, file);
    if (filter && !rel.includes(filter)) continue;
    const meta = parseFrontmatter(readFileSync(file, "utf8"));
    const r = runFile(file, meta, verbose);
    if (r.status === "SKIP") { skip++; continue; }
    const isExpectedFail = expected.has(rel);
    if (r.status === "PASS") {
      if (isExpectedFail) unexpectedPasses.push(rel);
      else pass++;
    } else {
      if (isExpectedFail) expectedFailures.push(rel);
      else failures.push({ rel, reason: r.reason });
    }
  }
}

console.log(`test262: ${pass} passed, ${expectedFailures.length} known failures, ${failures.length} unexpected failures, ${unexpectedPasses.length} unexpected passes, ${skip} skipped`);
if (failures.length) {
  console.log("\nUNEXPECTED FAILURES:");
  for (const f of failures) console.log(`  ${f.rel}\n      ${f.reason}`);
}
if (unexpectedPasses.length) {
  console.log("\nUNEXPECTED PASSES (remove from test/test262.expectations):");
  for (const p of unexpectedPasses) console.log(`  ${p}`);
}
process.exit(failures.length || unexpectedPasses.length ? 1 : 0);
