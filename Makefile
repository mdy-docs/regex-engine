CC ?= cc
CFLAGS ?= -Wall -Wextra -O2 -g -Iinclude
EMCC ?= emcc

WASM_OUT_DIR = dist
WASM_TARGET = $(WASM_OUT_DIR)/regex-engine.js
ENGINE_SRCS = src/re_lexer.c src/re_parser.c src/re_compiler.c src/re_vm.c
WASM_SRCS = $(ENGINE_SRCS) src/regex_wasm.c

.PHONY: all clean test test-asan test-wasm wasm demo

all: test/smoke

# Native smoke test -- exercises the actual regex_wasm.c shim API (compiled
# natively; EMSCRIPTEN_KEEPALIVE is a no-op outside __EMSCRIPTEN__) as a
# quick sanity check with no Emscripten toolchain required.
test/smoke: $(ENGINE_SRCS) src/regex_wasm.c test/smoke.c
	$(CC) $(CFLAGS) $(ENGINE_SRCS) src/regex_wasm.c test/smoke.c -o test/smoke

test: test/smoke
	./test/smoke

# Same smoke test under ASan+UBSan. The OOB-read regression tests
# (docs/IMPROVEMENTS.md #1.4/#1.5) exercise one-past-the-end reads on
# tightly-sized buffers -- a plain build can pass those by silently reading
# adjacent memory, so only this target actually proves them fixed.
# -fno-sanitize-recover: UBSan only *prints* its findings by default and
# exits 0, which would let undefined behavior sail through CI unnoticed --
# this makes the first finding fatal, like ASan's already are.
test/smoke-asan: $(ENGINE_SRCS) src/regex_wasm.c test/smoke.c
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(ENGINE_SRCS) src/regex_wasm.c test/smoke.c -o test/smoke-asan

test-asan: test/smoke-asan
	./test/smoke-asan

# End-to-end check of the actual compiled dist/regex-engine.wasm through its
# real JS glue (requires `make wasm` first, and Node).
test-wasm: wasm
	node test/node_smoke.mjs

# STACK_OVERFLOW_CHECK=2 -- deeply nested lookaround used to be a confirmed
# C-stack-overflow crash (docs/IMPROVEMENTS.md #1.1, fixed: the VM's
# backtrack stack/fail-cache/per-thread captures are heap-allocated and
# right-sized to the pattern's actual group count now, not fixed ~2.2MB
# C-stack locals on every recursive call). Parsing itself still recurses
# per nested paren group and is now hard-capped at MAX_AST_DEPTH=200
# (docs/IMPROVEMENTS.md #1.3) rather than unbounded, but 200 levels is
# still enough to want headroom under WASM specifically: Emscripten's
# default stack is only 64KB (confirmed: 8 nested lookaheads previously
# overflowed a 64KB stack during parsing alone, well under the 200 cap).
# STACK_SIZE is bumped to 8MB, matching a typical native default, so this
# build tolerates the full MAX_AST_DEPTH range same as a native embedder
# would. Kept as defense-in-depth even though #1.1 (the original, more
# severe reason for both flags) is fixed: STACK_OVERFLOW_CHECK turns any
# *future* stack issue into a catchable RuntimeError (see web/app.js)
# instead of silent linear-memory corruption, which is worth keeping
# regardless of what specifically motivated adding it.
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
	rm -rf test/smoke test/smoke.dSYM test/smoke-asan test/smoke-asan.dSYM $(WASM_OUT_DIR) web/regex-engine.js web/regex-engine.wasm
