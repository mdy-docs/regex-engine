#ifndef REGEXP_H
#define REGEXP_H

#include <stdbool.h>
#include <stdint.h>

/* 32768, not the original 16384: string-set classes compile to one
 * alternation branch per string (see re_compiler.c), and a full
 * \p{RGI_Emoji} -- 2604 sequences now that CharClass.strings is heap-sized
 * rather than capped at 128 -- needs ~17.5k instructions on its own. */
#define MAX_OPCODES 32768
#define MAX_GROUPS 255
/* 128, not the original 64: real-world patterns clear 64 (test262's
 * classic XML shallow-parsing regex builds 72 distinct classes in one
 * pattern). Each embedded CharClass is ~16KB (ranges[2048]), so this is
 * the main Program-size lever -- raising it further should wait for
 * heap-sized ranges like CharClass.strings already has. */
#define MAX_CLASSES 128
/* 256, not the original 16: every quantifier consumes a counter id, and
 * ordinary real-world patterns blow past 16 -- test262's classic XML
 * shallow-parsing regex (a single 907-char pattern) needs 75. Cheap to
 * raise now that the VM sizes ALL counter storage (per-thread arenas,
 * fail-cache side arrays, path-start snapshots) to the pattern's actual
 * counter_count on the heap -- this constant only caps the compile-time
 * id space. */
#define MAX_COUNTERS 256
#define CACHE_SIZE 8192
/* Capture-group name buffer size in bytes: 31 UTF-8 bytes + NUL. An engine
 * limit, not a spec one (ECMAScript puts no length cap on group names) --
 * over-long names fail compilation loudly.
 * Shared by Program.group_names, the parser/lexer's Token/ASTNode name
 * buffers, and the append bound in rx_name_append_utf8; sizing one without
 * the others would silently reintroduce a truncation bug. */
#define MAX_GROUP_NAME 32

/* Unlike the four MAX_* above (which size fixed arrays and are checked at
 * their respective allocation sites), this
 * bounds AST *height*, not a data structure's capacity: the parser
 * (re_parser.c) computes each node's subtree height as it's built and
 * rejects a pattern before its AST would grow deep enough to overflow the
 * C stack in one of the several functions that recurse through it with no
 * depth limit of their own (free_ast, validate_group_names,
 * validate_backrefs, validate_named_backrefs, compile_node).
 * Two changes made 400 safe where 200 once was the ceiling:
 * validate_group_names' two ~8KB NameSet locals per frame (ASan-observed
 * crash near depth ~247 on an 8MB stack) moved to the heap, and the
 * parser now builds BALANCED concat/alternation trees, so height tracks
 * real bracket nesting rather than pattern length. The remaining
 * recursions carry small frames -- the largest per-level cost is vm_run's
 * lookaround recursion (a cap_pairs-sized VLA, ~4KB worst case), which at
 * 400 levels stays ~1.6MB against the 8MB native and WASM (Makefile
 * STACK_SIZE) stacks. test262's classic suites nest 200 parens; 400
 * accommodates that with the same 2x headroom the old value had over
 * hand-written patterns. */
#define MAX_AST_DEPTH 400

#define REGEX_FLAG_IGNORECASE 1
#define REGEX_FLAG_GLOBAL 2
#define REGEX_FLAG_MULTILINE 4
#define REGEX_FLAG_DOTALL 8
#define REGEX_FLAG_STICKY 16
#define REGEX_FLAG_UNICODE 32
#define REGEX_FLAG_INDICES 64
#define REGEX_FLAG_UNICODE_SETS 128

/* The enum members ARE the short names: every use site already said OP_*
 * (via a full set of aliasing macros this header used to carry); the
 * REGEX_OP_* spellings were referenced nowhere else in this repo or by
 * any known consumer, so the double naming was collapsed. */
typedef enum {
    OP_CHAR, OP_CLASS, OP_SPLIT, OP_JMP, OP_SAVE, OP_LOOKAHEAD, OP_NEG_LOOKAHEAD, OP_MATCH,
    OP_INIT_COUNTER, OP_INC_COUNTER, OP_CHECK_COUNTER, OP_ASSERT_START, OP_ASSERT_END, OP_BACKREF, OP_NAMED_BACKREF, OP_WORD_BOUNDARY,
    OP_NON_WORD_BOUNDARY, OP_LOOKBEHIND, OP_NEG_LOOKBEHIND, OP_CLEAR_CAPTURES
} RegexOpCode;

typedef struct {
    RegexOpCode op;
    int arg1;
    int arg2;
    int arg3;  // Max bounds for OP_CHECK_COUNTER
    int arg4;  // Jump target for OP_CHECK_COUNTER
    bool lazy; // Lazy routing flag
} Instruction;

typedef struct {
    uint32_t start;
    uint32_t end;
} CodePointRange;

typedef struct {
    uint32_t cps[16]; // Max length of a string in a class (e.g. complex emojis)
    int length;
} StringSequence;

typedef struct {
    CodePointRange ranges[2048];
    int range_count;
    /* Heap-owned, right-sized string set (grown on demand; NULL iff
     * string_count == 0). This used to be a fixed strings[128] embedded
     * array, which silently truncated the large Unicode properties of
     * strings (RGI_Emoji alone has 2604 sequences). Ownership: a CharClass
     * owns its buffer; copies must deep-copy (re_lexer.c's
     * class_strings_push) or deliberately transfer ownership, and the
     * buffers are released by class_strings_free -- compile_into frees a
     * previous compile's buffers on re-entry, regex_free frees them at
     * teardown. A Program must be ZERO-INITIALIZED before its first
     * compile_into so those frees never see garbage pointers. */
    StringSequence* strings;
    int string_count;
    int string_cap;
    bool negated;
} CharClass;

typedef struct {
    Instruction code[MAX_OPCODES];
    CharClass classes[MAX_CLASSES];
    char group_names[MAX_GROUPS][MAX_GROUP_NAME];
    /* name_chain[i] = the next group id > i sharing group i's name (0 =
     * none). Built once by compile_into after parsing; OP_NAMED_BACKREF
     * walks it to find the participating group among ES2025 duplicate
     * names, replacing the strcmp scan over every group name the VM used
     * to do per execution. */
    int name_chain[MAX_GROUPS];
    int code_count;
    int class_count;
    int group_count;
    int counter_count;
    bool ignore_case;
    bool multiline;
    bool dot_all;
    bool sticky;
    bool unicode;
    bool has_indices;
    bool unicode_sets;
    /* Set by the compiler when any (numeric or named) backreference is
     * emitted. The VM's fail cache keys on (pc, sp, counters) but NOT on
     * capture state, and captures feed back into matching only through
     * backreferences -- so memoizing failures is unsound exactly when this
     * is set (a cached failure from one capture history wrongly suppressed
     * a viable thread with another: /(?:(x)|x)*\1y/ vs "xy"). The VM skips
     * the cache for such patterns; the step budget remains their defense
     * against catastrophic backtracking. */
    bool has_backrefs;
    /* First-unit scan filter, computed by compile_into from the finished
     * bytecode. When scan_filter is true, a match can only START at a
     * position whose first UTF-16 code unit is admitted: units < 128 are
     * looked up in the scan_ascii bitmap, units >= 128 (including all
     * surrogates) are admitted iff scan_non_ascii. An unanchored scan loop
     * can skip inadmissible positions without entering the VM at all --
     * regex_wasm.c's regex_exec does exactly this; native scan loops may
     * too. The filter OVER-approximates (skipping is only ever safe, never
     * required); it is left false whenever a conservative answer isn't
     * cheap -- patterns that can match empty, or whose first consuming
     * opcode is a backreference or lookaround. */
    bool scan_filter;
    bool scan_non_ascii;
    uint8_t scan_ascii[16];
    const char* error;
} Program;

typedef struct {
    int start;
    int end;
} CaptureIndex;

void compile_into(Program* prog, const uint16_t* regex, int flags);

/* Releases one class's heap-owned string set (safe on an empty class; the
 * pointer is NULLed). A host that drives compile_into directly must call
 * this for classes[0..class_count) before discarding a Program -- the
 * regex_wasm.c shim's regex_free does exactly that. */
void class_strings_free(CharClass* cls);

/* Reusable execution scratch (backtrack stacks, capture arenas, fail
 * caches -- one set per lookaround recursion depth, allocated lazily and
 * kept until freed). Create one per exec *call* and pass it to vm_execute
 * for every start position tried within that call: an unanchored search
 * over N code units re-enters the VM up to N times, and paying
 * allocation + fail-cache initialization per position instead of per call
 * dominated the runtime by orders of magnitude. A context is tied to the group count of the Program it was
 * created for, and is not thread-safe -- one context per thread. */
typedef struct VMContext VMContext;
VMContext* vm_context_new(const Program* prog);
void vm_context_free(VMContext* ctx);

/* Hard step budget: the maximum number of VM instructions this context may
 * execute across every vm_execute entry (including lookaround recursion)
 * for the context's whole lifetime. 0 (the default) means unlimited --
 * existing consumers are unaffected unless they opt in. When the budget is
 * exhausted the in-flight match is abandoned (vm_execute returns false,
 * like the VM_STACK_MAX limit) and vm_context_budget_exhausted() reports
 * true, letting a host distinguish "no match" from "gave up" and surface a
 * catchable error instead of hanging. This is the engine's defense against
 * catastrophic backtracking that the fail cache does not catch: the cache
 * is direct-mapped (CACHE_SIZE slots, collisions evict) and is
 * generation-cleared across lookaround boundaries, so pathological
 * patterns -- /(a+)+$/ against a few hundred 'a's is enough -- still
 * backtrack exponentially without a budget. Embedders running untrusted
 * patterns should always set one; a budget linear in the subject length
 * (e.g. 1e6 + 2000/unit) bounds superlinear blowup while leaving orders of
 * magnitude of headroom for legitimate matching. */
void vm_context_set_step_budget(VMContext* ctx, uint64_t max_steps);
bool vm_context_budget_exhausted(const VMContext* ctx);
bool vm_execute(Program* prog, VMContext* ctx, int start_pc, int step, const uint16_t* original_text, const uint16_t* text_end, const uint16_t* search_start, const uint16_t** out_captures);

/* One-shot convenience wrapper: creates a context, runs vm_execute once,
 * frees the context. Fine for a single anchored evaluation; do NOT call
 * this per start position in a scan loop -- that's the exact per-position
 * overhead VMContext exists to remove. */
bool vm_execute_internal(Program* prog, int start_pc, int step, const uint16_t* original_text, const uint16_t* text_end, const uint16_t* search_start, const uint16_t** out_captures);
void vm_get_indices(const uint16_t* original_text, const uint16_t** captures, CaptureIndex* out_indices, int group_count);

#endif /* REGEXP_H */
