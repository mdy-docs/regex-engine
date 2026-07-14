CC ?= cc
CFLAGS ?= -Wall -Wextra -O2 -g -Iinclude
EMCC ?= emcc

WASM_OUT_DIR = dist
WASM_TARGET = $(WASM_OUT_DIR)/regex-engine.js
WASM_SRCS = src/regexp.c src/regex_wasm.c

.PHONY: all clean test test-wasm wasm

all: test/smoke

# Native smoke test -- exercises the actual regex_wasm.c shim API (compiled
# natively; EMSCRIPTEN_KEEPALIVE is a no-op outside __EMSCRIPTEN__) as a
# quick sanity check with no Emscripten toolchain required.
test/smoke: src/regexp.c src/regex_wasm.c test/smoke.c
	$(CC) $(CFLAGS) src/regexp.c src/regex_wasm.c test/smoke.c -o test/smoke

test: test/smoke
	./test/smoke

# End-to-end check of the actual compiled dist/regex-engine.wasm through its
# real JS glue (requires `make wasm` first, and Node).
test-wasm: wasm
	node test/node_smoke.mjs

wasm:
	@mkdir -p $(WASM_OUT_DIR)
	$(EMCC) -O2 -Iinclude $(WASM_SRCS) -o $(WASM_TARGET) \
		-s WASM=1 \
		-s MODULARIZE=1 \
		-s EXPORT_NAME=createRegexEngineModule \
		-s ENVIRONMENT=web,node \
		-s ALLOW_MEMORY_GROWTH=1 \
		-s EXPORTED_FUNCTIONS='["_malloc","_free","_regex_compile","_regex_exec","_regex_free","_regex_last_error","_regex_group_count","_regex_group_name","_regex_captures_ptr","_regex_flag_bit"]' \
		-s EXPORTED_RUNTIME_METHODS='["cwrap","ccall","UTF8ToString","HEAPU16","HEAP16","HEAPU8","HEAP32"]' \
		--no-entry
	@echo "WASM build generated: $(WASM_TARGET) and $(WASM_OUT_DIR)/regex-engine.wasm"

clean:
	rm -rf test/smoke test/smoke.dSYM $(WASM_OUT_DIR)
