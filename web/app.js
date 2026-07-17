// Regex playground UI on top of the compiled WASM engine (baru-re.js /
// baru-re.wasm, built by `make wasm` from src/re_*.c + regex_wasm.c).
// This file only marshals data across the WASM boundary and renders results
// -- all matching happens inside the compiled C engine.
"use strict";

const MAX_MATCHES = 500; // safety cap for the "find all matches" loop below
const MARK_COLORS = ["#ffe58a", "#a8d8ff", "#c9f2c0", "#ffc9de", "#e0c8ff", "#ffd9ad"];

const els = {
  pattern: document.getElementById("pattern"),
  flagsDisplay: document.getElementById("flags-display"),
  subject: document.getElementById("subject"),
  status: document.getElementById("status"),
  highlighted: document.getElementById("highlighted"),
  matches: document.getElementById("matches"),
  presets: document.getElementById("presets"),
};
const flagCheckboxes = Array.from(document.querySelectorAll("[data-flag]"));

const PRESETS = [
  {
    label: "ISO date, positional groups",
    pattern: "(\\d{4})-(\\d{2})-(\\d{2})",
    flags: "g",
    subject: "Order #1057 shipped on 2026-07-15, invoiced 2026-07-17.",
  },
  {
    label: "Named groups",
    pattern: "(?<year>\\d{4})-(?<month>\\d{2})-(?<day>\\d{2})",
    flags: "g",
    subject: "Starts 2026-07-15, ends 2026-08-01.",
  },
  {
    label: "Lookbehind + lookahead",
    pattern: "(?<=\\$)\\d+(?=\\.\\d{2}\\b)",
    flags: "g",
    subject: "Subtotal $42.00, tax $3.50, total $45.50",
  },
  {
    label: "Unicode property escape (\\p{...})",
    pattern: "\\p{Script=Greek}+",
    flags: "gu",
    subject: "Hello Ελληνικά world, καλημέρα!",
  },
  {
    label: "Backreference, case-insensitive",
    pattern: "(\\w+) \\1",
    flags: "gi",
    subject: "the the quick brown FOX fox jumps",
  },
  {
    label: "Sticky tokenizer step",
    pattern: "[A-Za-z]+|\\d+|\\s+",
    flags: "y",
    subject: "abc123 def",
  },
  {
    label: "/v mode character class subtraction",
    pattern: "[\\p{Decimal_Number}--[0-9]]+",
    flags: "gv",
    subject: "٣٤٥ and ۹۸ are non-ASCII digits, 42 is not",
  },
  {
    label: "Deep lookaround nesting (used to crash at depth 3 — fixed)",
    pattern: "(?=".repeat(60) + "x" + ")".repeat(60),
    flags: "",
    subject: "x",
  },
];

let Module, cfn;
let liveBuffers = []; // ptrs malloc'd for the current run, freed before the next one

async function loadModule() {
  Module = await createBaruReModule();
  cfn = {
    flag_bit: Module.cwrap("regex_flag_bit", "number", ["number"]),
    compile: Module.cwrap("regex_compile", "number", ["number", "number", "number"]),
    last_error: Module.cwrap("regex_last_error", "string", []),
    group_count: Module.cwrap("regex_group_count", "number", ["number"]),
    group_name: Module.cwrap("regex_group_name", "string", ["number", "number"]),
    exec: Module.cwrap("regex_exec", "number", ["number", "number", "number", "number"]),
    captures_ptr: Module.cwrap("regex_captures_ptr", "number", ["number"]),
    free_handle: Module.cwrap("regex_free", null, ["number"]),
  };
}

function writeUtf16(str) {
  const ptr = Module._malloc((str.length + 1) * 2);
  const units = new Uint16Array(Module.HEAPU16.buffer, ptr, str.length + 1);
  for (let i = 0; i < str.length; i++) units[i] = str.charCodeAt(i);
  units[str.length] = 0;
  liveBuffers.push(ptr);
  return ptr;
}

function freeLiveBuffers() {
  for (const ptr of liveBuffers) Module._free(ptr);
  liveBuffers = [];
}

function currentFlags() {
  return flagCheckboxes.filter((cb) => cb.checked).map((cb) => cb.dataset.flag);
}

function flagsMask(flagChars) {
  let mask = 0;
  for (const f of flagChars) mask |= cfn.flag_bit(f.charCodeAt(0));
  return mask;
}

function isUnicodeMode(flagChars) {
  return flagChars.includes("u") || flagChars.includes("v");
}

function escapeHtml(s) {
  return s.replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

// Enforce u/v mutual exclusion in the UI, same as the real RegExp constructor
// (both set is a SyntaxError in JS).
for (const cb of flagCheckboxes) {
  cb.addEventListener("change", () => {
    if (cb.dataset.flag === "u" && cb.checked) flagCheckboxes.find((c) => c.dataset.flag === "v").checked = false;
    if (cb.dataset.flag === "v" && cb.checked) flagCheckboxes.find((c) => c.dataset.flag === "u").checked = false;
    scheduleRun();
  });
}

function setStatus(text, kind) {
  els.status.textContent = text;
  els.status.className = "status" + (kind ? " " + kind : "");
}

// Finds every match by repeatedly calling regex_exec, advancing past each
// match (advancing by one unit -- or one code point under u/v -- on a
// zero-length match to avoid looping forever), mirroring how a JS host
// would implement String.prototype.matchAll on top of this low-level API
// (see README.md "Integrating into another WASM package").
function findAllMatches(handle, textPtr, textUnits, groupCount, unicodeMode, sticky) {
  const matches = [];
  let start = 0;
  while (start <= textUnits && matches.length < MAX_MATCHES) {
    const matched = cfn.exec(handle, textPtr, textUnits, start);
    if (!matched) break;
    const caps = new Int32Array(Module.HEAPU16.buffer, cfn.captures_ptr(handle), (groupCount + 1) * 2).slice();
    matches.push(caps);
    const end = caps[1];
    if (end === caps[0]) {
      if (unicodeMode) {
        const units = new Uint16Array(Module.HEAPU16.buffer, textPtr, textUnits);
        const isLead = end < textUnits && units[end] >= 0xd800 && units[end] <= 0xdbff;
        const isTrail = end + 1 < textUnits && units[end + 1] >= 0xdc00 && units[end + 1] <= 0xdfff;
        start = isLead && isTrail ? end + 2 : end + 1;
      } else {
        start = end + 1;
      }
    } else {
      start = end;
    }
    if (sticky && !matches.length) break;
  }
  return matches;
}

function renderHighlighted(subject, matches) {
  if (matches.length === 0) {
    els.highlighted.textContent = subject;
    return;
  }
  let html = "";
  let cursor = 0;
  matches.forEach((caps, i) => {
    const [s, e] = caps;
    html += escapeHtml(subject.slice(cursor, s));
    const color = MARK_COLORS[i % MARK_COLORS.length];
    html += `<mark style="background:${color}" title="match ${i + 1}">${escapeHtml(subject.slice(s, e))}</mark>`;
    cursor = e;
  });
  html += escapeHtml(subject.slice(cursor));
  els.highlighted.innerHTML = html;
}

function renderMatchTables(matches, handle, groupCount, subject) {
  if (matches.length === 0) {
    els.matches.innerHTML = '<p class="no-matches">No matches.</p>';
    return;
  }
  const names = [];
  for (let i = 1; i <= groupCount; i++) names[i] = cfn.group_name(handle, i);

  let html = "";
  matches.forEach((caps, mIdx) => {
    const color = MARK_COLORS[mIdx % MARK_COLORS.length];
    html += `<table class="match-table"><caption><span class="swatch" style="background:${color}"></span>Match ${mIdx + 1} of ${matches.length}</caption>`;
    html += "<thead><tr><th>Group</th><th>Name</th><th>Text</th><th>Range</th></tr></thead><tbody>";
    for (let g = 0; g <= groupCount; g++) {
      const s = caps[g * 2], e = caps[g * 2 + 1];
      const label = g === 0 ? "0 (whole match)" : String(g);
      const name = g > 0 && names[g] ? names[g] : "";
      if (s === -1) {
        html += `<tr><td>${label}</td><td>${escapeHtml(name)}</td><td class="unmatched">(unmatched)</td><td>&mdash;</td></tr>`;
      } else {
        html += `<tr><td>${label}</td><td>${escapeHtml(name)}</td><td>${escapeHtml(subject.slice(s, e))}</td><td>[${s}, ${e})</td></tr>`;
      }
    }
    html += "</tbody></table>";
  });
  els.matches.innerHTML = html;
}

let runToken = 0;
function scheduleRun() {
  const token = ++runToken;
  clearTimeout(scheduleRun._t);
  scheduleRun._t = setTimeout(() => {
    if (token === runToken) run();
  }, 150);
}

async function run() {
  const pattern = els.pattern.value;
  const flagChars = currentFlags();
  els.flagsDisplay.textContent = flagChars.join("");
  const subject = els.subject.value;
  const unicodeMode = isUnicodeMode(flagChars);
  const global = flagChars.includes("g");
  const sticky = flagChars.includes("y");

  freeLiveBuffers();
  let handle = 0;
  try {
    const patPtr = writeUtf16(pattern);
    handle = cfn.compile(patPtr, 0, flagsMask(flagChars));
    if (!handle) {
      setStatus("SyntaxError: " + cfn.last_error(), "error");
      els.highlighted.textContent = subject;
      els.matches.innerHTML = "";
      return;
    }
    const groupCount = cfn.group_count(handle);
    const textPtr = writeUtf16(subject);

    const matches = global
      ? findAllMatches(handle, textPtr, subject.length, groupCount, unicodeMode, sticky)
      : (() => {
          const m = cfn.exec(handle, textPtr, subject.length, 0);
          if (!m) return [];
          return [new Int32Array(Module.HEAPU16.buffer, cfn.captures_ptr(handle), (groupCount + 1) * 2).slice()];
        })();

    const names = groupCount > 0 ? `, ${groupCount} capture group${groupCount === 1 ? "" : "s"}` : "";
    setStatus(
      matches.length
        ? `Compiled OK${names} — ${matches.length} match${matches.length === 1 ? "" : "es"}${matches.length === MAX_MATCHES ? ` (capped at ${MAX_MATCHES})` : ""}`
        : `Compiled OK${names} — no match`,
      matches.length ? "ok" : ""
    );
    renderHighlighted(subject, matches);
    renderMatchTables(matches, handle, groupCount, subject);
  } catch (err) {
    // A WASM trap. The two confirmed, reliably-reproducible ways to hit
    // this (deeply nested lookaround exhausting the stack -- #1.1; very
    // long/deeply-nested patterns overflowing the parser/AST-walker
    // recursion -- #1.3) are both fixed as of docs/IMPROVEMENTS.md's
    // current state -- this catch is now defense-in-depth against
    // whatever's left (an OOB read landing somewhere WASM's linear-memory
    // bounds checking actually traps on, say) rather than a known,
    // demonstrable path. The module instance is left in an undefined
    // state per Emscripten's own guidance after an abort, so we discard
    // it and load a fresh one rather than keep making calls into it.
    console.error(err);
    setStatus(
      "Engine crashed evaluating this pattern (unexpected -- see the footer below). Reloading the WASM instance…",
      "crash"
    );
    els.matches.innerHTML = "";
    liveBuffers = []; // the old module's memory is going away with it
    await loadModule();
    setStatus(
      "Engine reloaded after an unexpected crash — see docs/IMPROVEMENTS.md linked below for known (non-crashing) issues.",
      "crash"
    );
  } finally {
    if (handle) {
      try { cfn.free_handle(handle); } catch { /* module may have been replaced above */ }
    }
  }
}

function populatePresets() {
  for (const p of PRESETS) {
    const opt = document.createElement("option");
    opt.textContent = p.label;
    opt.value = p.label;
    els.presets.appendChild(opt);
  }
  els.presets.addEventListener("change", () => {
    const preset = PRESETS.find((p) => p.label === els.presets.value);
    if (!preset) return;
    els.pattern.value = preset.pattern;
    els.subject.value = preset.subject;
    for (const cb of flagCheckboxes) cb.checked = preset.flags.includes(cb.dataset.flag);
    run();
  });
}

els.pattern.addEventListener("input", scheduleRun);
els.subject.addEventListener("input", scheduleRun);

populatePresets();
setStatus("Loading WASM module…");
loadModule().then(run);
