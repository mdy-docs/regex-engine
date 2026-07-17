/* libFuzzer harness: input bytes become (flags, pattern, text), driven
 * through the same shim API (regex_compile + regex_exec's scan loop) an
 * embedder uses. The invariant under test is purely "never crashes, never
 * trips ASan/UBSan" -- matching *correctness* is the smoke suite's and
 * test262's job. Build and run with
 * `make fuzz` (needs clang's -fsanitize=fuzzer, so CC must be clang).
 *
 * Input layout: byte 0 = flag bits (masked to real REGEX_FLAG_* values),
 * bytes 1-2 = little-endian split point, remaining bytes = UTF-16 code
 * units, pattern before the split and subject text after it. Raw bytes as
 * code units deliberately produces lone surrogates, NULs, and garbage --
 * the exact inputs sections 1.4/1.5 were about. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "regex_wasm.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 4) return 0;
    int flags = data[0];
    size_t split = (size_t)(data[1] | (data[2] << 8));
    const uint8_t* body = data + 3;
    size_t units = (size - 3) / 2;
    if (units == 0) return 0;
    split %= units + 1;

    uint16_t* pattern = malloc((split + 1) * sizeof(uint16_t));
    memcpy(pattern, body, split * sizeof(uint16_t));
    pattern[split] = 0;

    /* Tightly-sized, non-NUL-terminated on purpose: regex_exec's contract
     * allows it, and it's what makes one-past-the-end reads ASan-visible. */
    size_t text_units = units - split;
    uint16_t* text = malloc(text_units ? text_units * sizeof(uint16_t) : 1);
    memcpy(text, body + split * sizeof(uint16_t), text_units * sizeof(uint16_t));

    uintptr_t h = regex_compile(pattern, 0, flags);
    if (h) {
        regex_exec(h, text, (int)text_units, 0);
        regex_free(h);
    }
    free(pattern);
    free(text);
    return 0;
}

#ifdef FUZZ_STANDALONE
/* Driver for toolchains without libFuzzer (Apple clang ships no
 * libclang_rt.fuzzer_osx.a): a deterministic xorshift PRNG generates
 * inputs biased toward regex syntax bytes -- purely random bytes only ever
 * exercise the "unrecognized escape" paths, while a pool of
 * metacharacters reaches quantifiers, classes, groups, and property
 * escapes. Reproducible by seed: `./test/fuzz [iterations] [seed]`. */
#include <stdio.h>

static uint64_t rng_state;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int main(int argc, char** argv) {
    long iters = argc > 1 ? atol(argv[1]) : 200000;
    rng_state = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x62617275; /* "baru" */
    static const char pool[] =
        "\\^$.|?*+()[]{}<>=!:,-0123456789abkdDwWsSpPquLNv characters";
    uint8_t buf[512];
    for (long n = 0; n < iters; n++) {
        size_t size = 4 + (size_t)(rng() % 220);
        buf[0] = (uint8_t)rng();
        buf[1] = (uint8_t)rng();
        buf[2] = (uint8_t)rng();
        for (size_t i = 3; i + 1 < size; i += 2) {
            uint64_t r = rng();
            buf[i] = (r % 10 < 7) ? (uint8_t)pool[(r >> 8) % (sizeof(pool) - 1)] : (uint8_t)(r >> 8);
            /* High byte usually 0 (ASCII); occasionally random, which is
             * what produces lone surrogates and astral-adjacent units. */
            buf[i + 1] = (r % 10 < 8) ? 0 : (uint8_t)(r >> 16);
        }
        LLVMFuzzerTestOneInput(buf, size);
    }
    printf("fuzz: %ld inputs, no crashes\n", iters);
    return 0;
}
#endif
