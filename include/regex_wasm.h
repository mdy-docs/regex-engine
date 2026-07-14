/* Public API of the WASM shim (src/regex_wasm.c). See that file's top-of-
 * file comment for the full usage walkthrough and each function's contract.
 * This header exists so the shim's own translation unit and native callers
 * (e.g. test/smoke.c) share one declaration -- it plays no role in the WASM
 * build itself (Emscripten's EXPORTED_FUNCTIONS list in the Makefile is
 * what actually controls what's callable from JS). */
#ifndef REGEX_WASM_H
#define REGEX_WASM_H

#include <stdint.h>

int regex_flag_bit(int flag_char);
uintptr_t regex_compile(const uint16_t* pattern, int pattern_units, int flags);
const char* regex_last_error(void);
int regex_group_count(uintptr_t handle);
const char* regex_group_name(uintptr_t handle, int index);
int regex_exec(uintptr_t handle, const uint16_t* text, int text_units, int start_index);
const int32_t* regex_captures_ptr(uintptr_t handle);
void regex_free(uintptr_t handle);

#endif /* REGEX_WASM_H */
