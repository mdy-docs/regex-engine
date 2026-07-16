CC ?= cc
CFLAGS ?= -Wall -Wextra -O2 -g -Iinclude
EMCC ?= emcc

WASM_OUT_DIR = dist
WASM_TARGET = $(WASM_OUT_DIR)/regex-engine.js
WASM_SRCS = src/regexp.c src/regex_wasm.c

.PHONY: all clean test test-wasm wasm demo

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

# STACK_OVERFLOW_CHECK=2 -- the native engine has confirmed C-stack-overflow
# crashes on some inputs (deeply nested lookaround, very long/deeply-nested
# patterns in general -- parsing recurses per paren group, not just
# lookaround; see docs/IMPROVEMENTS.md #1.1/#1.3). Without this flag a WASM
# stack overflow can silently corrupt linear memory instead of trapping;
# with it, it becomes a catchable RuntimeError a JS host can recover from
# (see web/app.js). STACK_SIZE is bumped from Emscripten's 64KB default
# (which made the crash threshold far shallower under WASM than natively --
# confirmed: 8 nested lookaheads overflowed a 64KB stack during parsing
# alone, before any matching happened) to 8MB, roughly matching a typical
# native default thread stack, so the demo tolerates the same depth a
# native embedder would before hitting this.
wasm:
	@mkdir -p $(WASM_OUT_DIR)
	$(EMCC) -O2 -Iinclude $(WASM_SRCS) -o $(WASM_TARGET) \
		-s WASM=1 \
		-s MODULARIZE=1 \
		-s EXPORT_NAME=createRegexEngineModule \
		-s ENVIRONMENT=web,node \
		-s ALLOW_MEMORY_GROWTH=1 \
		-s STACK_OVERFLOW_CHECK=2 \
		-s STACK_SIZE=8388608 \
		-s EXPORTED_FUNCTIONS='["_malloc","_free","_regex_compile","_regex_exec","_regex_free","_regex_last_error","_regex_group_count","_regex_group_name","_regex_captures_ptr","_regex_flag_bit"]' \
		-s EXPORTED_RUNTIME_METHODS='["cwrap","ccall","UTF8ToString","HEAPU16","HEAP16","HEAPU8","HEAP32"]' \
		--no-entry
	@echo "WASM build generated: $(WASM_TARGET) and $(WASM_OUT_DIR)/regex-engine.wasm"

# Builds the wasm artifacts and copies them next to web/'s HTML/CSS/JS so the
# playground demo (web/index.html) can be opened via a local static server,
# e.g. `python3 -m http.server -d web`. The .github/workflows/pages.yml
# workflow does the equivalent for the published GitHub Pages deployment --
# neither commits the built .js/.wasm into git (see .gitignore).
demo: wasm
	cp $(WASM_OUT_DIR)/regex-engine.js $(WASM_OUT_DIR)/regex-engine.wasm web/
	@echo "Demo artifacts copied into web/. Serve it with e.g.:"
	@echo "  python3 -m http.server -d web"

clean:
	rm -rf test/smoke test/smoke.dSYM $(WASM_OUT_DIR) web/regex-engine.js web/regex-engine.wasm
