#ifndef REGEXP_H
#define REGEXP_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_OPCODES 16384
#define MAX_GROUPS 255
#define MAX_CLASSES 64
#define MAX_COUNTERS 16
#define CACHE_SIZE 8192

#define REGEX_FLAG_IGNORECASE 1
#define REGEX_FLAG_GLOBAL 2
#define REGEX_FLAG_MULTILINE 4
#define REGEX_FLAG_DOTALL 8
#define REGEX_FLAG_STICKY 16
#define REGEX_FLAG_UNICODE 32
#define REGEX_FLAG_INDICES 64
#define REGEX_FLAG_UNICODE_SETS 128

typedef enum {
    REGEX_OP_CHAR, REGEX_OP_CLASS, REGEX_OP_SPLIT, REGEX_OP_JMP, REGEX_OP_SAVE, REGEX_OP_LOOKAHEAD, REGEX_OP_NEG_LOOKAHEAD, REGEX_OP_MATCH,
    REGEX_OP_INIT_COUNTER, REGEX_OP_INC_COUNTER, REGEX_OP_CHECK_COUNTER, REGEX_OP_ASSERT_START, REGEX_OP_ASSERT_END, REGEX_OP_BACKREF, REGEX_OP_NAMED_BACKREF, REGEX_OP_WORD_BOUNDARY,
    REGEX_OP_NON_WORD_BOUNDARY, REGEX_OP_LOOKBEHIND, REGEX_OP_NEG_LOOKBEHIND, REGEX_OP_CLEAR_CAPTURES
} RegexOpCode;

#define OP_CHAR REGEX_OP_CHAR
#define OP_CLASS REGEX_OP_CLASS
#define OP_SPLIT REGEX_OP_SPLIT
#define OP_JMP REGEX_OP_JMP
#define OP_SAVE REGEX_OP_SAVE
#define OP_LOOKAHEAD REGEX_OP_LOOKAHEAD
#define OP_NEG_LOOKAHEAD REGEX_OP_NEG_LOOKAHEAD
#define OP_MATCH REGEX_OP_MATCH
#define OP_INIT_COUNTER REGEX_OP_INIT_COUNTER
#define OP_INC_COUNTER REGEX_OP_INC_COUNTER
#define OP_CHECK_COUNTER REGEX_OP_CHECK_COUNTER
#define OP_ASSERT_START REGEX_OP_ASSERT_START
#define OP_ASSERT_END REGEX_OP_ASSERT_END
#define OP_BACKREF REGEX_OP_BACKREF
#define OP_NAMED_BACKREF REGEX_OP_NAMED_BACKREF
#define OP_WORD_BOUNDARY REGEX_OP_WORD_BOUNDARY
#define OP_NON_WORD_BOUNDARY REGEX_OP_NON_WORD_BOUNDARY
#define OP_LOOKBEHIND REGEX_OP_LOOKBEHIND
#define OP_NEG_LOOKBEHIND REGEX_OP_NEG_LOOKBEHIND
#define OP_CLEAR_CAPTURES REGEX_OP_CLEAR_CAPTURES

typedef struct {
    RegexOpCode op;
    int arg1; 
    int arg2; 
    int arg3;  // Max bounds for REGEX_OP_CHECK_COUNTER
    int arg4;  // Jump target for REGEX_OP_CHECK_COUNTER
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
    char group_names[MAX_GROUPS][32];
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
bool vm_execute_internal(Program* prog, int start_pc, int step, const uint16_t* original_text, const uint16_t* text_end, const uint16_t* search_start, const uint16_t** out_captures);
void vm_get_indices(const uint16_t* original_text, const uint16_t** captures, CaptureIndex* out_indices, int group_count);

#endif /* REGEXP_H */
