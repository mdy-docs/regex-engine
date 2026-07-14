// End-to-end sanity check of the actual compiled dist/regex-engine.wasm
// through its real JS glue (not the native shim test) -- run with:
//   node test/node_smoke.mjs
// after `make wasm`.
import createRegexEngineModule from "../dist/regex-engine.js";

const Module = await createRegexEngineModule();

const regex_flag_bit = Module.cwrap("regex_flag_bit", "number", ["number"]);
const regex_compile = Module.cwrap("regex_compile", "number", ["number", "number", "number"]);
const regex_last_error = Module.cwrap("regex_last_error", "string", []);
const regex_group_count = Module.cwrap("regex_group_count", "number", ["number"]);
const regex_group_name = Module.cwrap("regex_group_name", "string", ["number", "number"]);
const regex_exec = Module.cwrap("regex_exec", "number", ["number", "number", "number", "number"]);
const regex_captures_ptr = Module.cwrap("regex_captures_ptr", "number", ["number"]);
const regex_free = Module.cwrap("regex_free", null, ["number"]);

let failures = 0;
function check(cond, what) {
  if (cond) {
    console.log(`[PASS] ${what}`);
  } else {
    console.log(`[FAIL] ${what}`);
    failures++;
  }
}

// Writes a JS string into WASM memory as NUL-terminated UTF-16 code units,
// returns [ptr, unitCount] (unitCount excludes the NUL). Caller must _free(ptr).
function writeUtf16(str) {
  const ptr = Module._malloc((str.length + 1) * 2);
  const units = new Uint16Array(Module.HEAPU16.buffer, ptr, str.length + 1);
  for (let i = 0; i < str.length; i++) units[i] = str.charCodeAt(i);
  units[str.length] = 0;
  return [ptr, str.length];
}

function readCaptures(handle, groupCount) {
  const ptr = regex_captures_ptr(handle);
  return new Int32Array(Module.HEAPU16.buffer, ptr, (groupCount + 1) * 2);
}

// --- basic match + capture groups, case-insensitive ---
{
  const [patPtr] = writeUtf16("(\\d+)-([a-z]+)");
  const flags = regex_flag_bit("i".charCodeAt(0));
  const h = regex_compile(patPtr, 0, flags);
  check(h !== 0, "compile (\\d+)-([a-z]+) succeeds");
  check(regex_group_count(h) === 2, "group_count is 2");

  const text = "order 42-ABC done";
  const [textPtr, textLen] = writeUtf16(text);
  const matched = regex_exec(h, textPtr, textLen, 0);
  check(matched === 1, "matches '42-ABC' under /i");

  const caps = readCaptures(h, 2);
  check(caps[0] === 6 && caps[1] === 12, "whole-match span is [6,12)");
  check(caps[2] === 6 && caps[3] === 8, "group 1 span is [6,8) ('42')");
  check(caps[4] === 9 && caps[5] === 12, "group 2 span is [9,12) ('ABC')");
  check(text.slice(caps[0], caps[1]) === "42-ABC", "slice reconstructs the whole match");

  Module._free(patPtr);
  Module._free(textPtr);
  regex_free(h);
}

// --- named groups ---
{
  const [patPtr] = writeUtf16("(?<year>\\d{4})-(?<month>\\d{2})");
  const h = regex_compile(patPtr, 0, 0);
  check(regex_group_name(h, 1) === "year", "group 1 named 'year'");
  check(regex_group_name(h, 2) === "month", "group 2 named 'month'");

  const [textPtr, textLen] = writeUtf16("2026-07");
  check(regex_exec(h, textPtr, textLen, 0) === 1, "matches '2026-07'");

  Module._free(patPtr);
  Module._free(textPtr);
  regex_free(h);
}

// --- compile error path ---
{
  const [patPtr] = writeUtf16("(unclosed");
  const h = regex_compile(patPtr, 0, 0);
  check(h === 0, "unbalanced paren fails to compile");
  check(regex_last_error().length > 0, "regex_last_error() is non-empty");
  Module._free(patPtr);
}

// --- non-ASCII / surrogate pair handling under /u ---
{
  const [patPtr] = writeUtf16(".");
  const flags = regex_flag_bit("u".charCodeAt(0));
  const h = regex_compile(patPtr, 0, flags);
  const text = "a\u{1F600}b"; // emoji is a surrogate pair
  const [textPtr, textLen] = writeUtf16(text);
  const matched = regex_exec(h, textPtr, textLen, 1);
  check(matched === 1, "/u '.' matches starting at the emoji");
  const caps = readCaptures(h, 0);
  check(caps[0] === 1 && caps[1] === 3, "/u '.' consumes the full surrogate pair, not half of it");

  Module._free(patPtr);
  Module._free(textPtr);
  regex_free(h);
}

if (failures === 0) {
  console.log("\nAll node smoke tests passed.");
  process.exit(0);
} else {
  console.log(`\n${failures} node smoke test(s) FAILED.`);
  process.exit(1);
}
