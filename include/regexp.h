#ifndef REGEXP_H
#define REGEXP_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_OPCODES 16384
#define MAX_GROUPS 255
#define MAX_CLASSES 64
#define MAX_COUNTERS 16
#define CACHE_SIZE 8192
/* Capture-group name buffer size in bytes: 31 UTF-8 bytes + NUL. An engine
 * limit, not a spec one (ECMAScript puts no length cap on group names) --
 * over-long names fail compilation loudly (docs/IMPROVEMENTS.md #1.9).
 * Shared by Program.group_names, the parser/lexer's Token/ASTNode name
 * buffers, and the append bound in rx_name_append_utf8; sizing one without
 * the others would silently reintroduce a truncation bug. */
#define MAX_GROUP_NAME 32

/* Unlike the four MAX_* above (which size fixed arrays and are checked at
 * their respective allocation sites -- see docs/IMPROVEMENTS.md #1.2), this
 * bounds AST *height*, not a data structure's capacity: the parser
 * (re_parser.c) computes each node's subtree height as it's built and
 * rejects a pattern before its AST would grow deep enough to overflow the
 * C stack in one of the several functions that recurse through it with no
 * depth limit of their own (free_ast, validate_group_names,
 * validate_backrefs, validate_named_backrefs, compile_node -- see
 * docs/IMPROVEMENTS.md #1.3). validate_group_names is the most fragile of
 * those (two ~8KB NameSet locals per stack frame) and was observed via
 * ASan to crash around recursion depth ~247 on an 8MB stack; this leaves a
 * comfortable safety margin below that while still accommodating any
 * pattern a human would plausibly write by hand. */
#define MAX_AST_DEPTH 200

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
 * any known consumer, so the double naming was collapsed
 * (docs/IMPROVEMENTS.md section 4). */
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
    StringSequence strings[128];
    int string_count;
    bool negated;
} CharClass;

typedef struct {
    Instruction code[MAX_OPCODES];
    CharClass classes[MAX_CLASSES];
    char group_names[MAX_GROUPS][MAX_GROUP_NAME];
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
    const char* error;
} Program;

typedef struct {
    int start;
    int end;
} CaptureIndex;

void compile_into(Program* prog, const uint16_t* regex, int flags);

/* Reusable execution scratch (backtrack stacks, capture arenas, fail
 * caches -- one set per lookaround recursion depth, allocated lazily and
 * kept until freed). Create one per exec *call* and pass it to vm_execute
 * for every start position tried within that call: an unanchored search
 * over N code units re-enters the VM up to N times, and paying
 * allocation + fail-cache initialization per position instead of per call
 * dominated the runtime by orders of magnitude (see docs/IMPROVEMENTS.md
 * section 2). A context is tied to the group count of the Program it was
 * created for, and is not thread-safe -- one context per thread. */
typedef struct VMContext VMContext;
VMContext* vm_context_new(const Program* prog);
void vm_context_free(VMContext* ctx);
bool vm_execute(Program* prog, VMContext* ctx, int start_pc, int step, const uint16_t* original_text, const uint16_t* text_end, const uint16_t* search_start, const uint16_t** out_captures);

/* One-shot convenience wrapper: creates a context, runs vm_execute once,
 * frees the context. Fine for a single anchored evaluation; do NOT call
 * this per start position in a scan loop -- that's the exact per-position
 * overhead VMContext exists to remove. */
bool vm_execute_internal(Program* prog, int start_pc, int step, const uint16_t* original_text, const uint16_t* text_end, const uint16_t* search_start, const uint16_t** out_captures);
void vm_get_indices(const uint16_t* original_text, const uint16_t** captures, CaptureIndex* out_indices, int group_count);

#endif /* REGEXP_H */
