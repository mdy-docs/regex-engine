/* Compiler: walks the AST (from re_parser.c) once and emits bytecode
 * directly into Program.code -- no separate optimization pass. Also home to
 * compile_into, the top-level entry point that drives lexer -> parser ->
 * validation -> compiler in sequence (declared in regexp.h; this is the
 * function regex_compile() in regex_wasm.c actually calls). See
 * docs/ARCHITECTURE.md's "Compiler" section, especially the notes on how
 * lookaround and lookbehind compile (out-of-line subroutine + recursive VM
 * call, and rtl=true reversing AST_CONCAT's emission order, respectively).
 *
 * Split out of what was originally a single file (src/regexp.c, itself a
 * verbatim copy of jsvm2's src/regexp.c) for maintainability -- see
 * CLAUDE.md/README.md's "Provenance" section for why that diverges from
 * upstream's layout.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucd.h"
#include "regexp.h"
#include "re_internal.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

/* Under /i the fold both sides of an OP_CHAR comparison get is fixed at
 * compile time (whether /u applies is a whole-pattern fact, and modifier
 * groups toggle only i/m/s), and both folds are idempotent -- so the
 * constant operand can be canonicalized once here, leaving the VM to fold
 * only the input character per dispatch. */
static uint32_t emit_char_operand(const Program* prog, uint32_t ch) {
    if (!prog->ignore_case) return ch;
    return prog->unicode ? unicode_casefold(ch) : annexb_canonicalize(ch);
}

/* Bounds-checked against MAX_OPCODES (a
 * confirmed heap-buffer-overflow: an unchecked pattern compiling to more
 * than MAX_OPCODES instructions wrote past prog->code[]). Once the limit is
 * hit, prog->error is set and every subsequent call clamps to the last
 * valid slot instead of indexing out of bounds; callers keep patching that
 * slot's fields (`prog->code[returned_idx].argN = ...`) after the fact,
 * which produces nonsensical-but-safe bytecode that's never actually run,
 * since a Program with prog->error set is discarded by regex_compile()
 * without ever reaching vm_execute_internal() (see src/regex_wasm.c). */
static int emit(Program* prog, RegexOpCode op, int arg1, int arg2, int arg3, int arg4, bool lazy) {
    if (prog->code_count >= MAX_OPCODES) {
        if (!prog->error) prog->error = "InternalError: pattern exceeds maximum compiled instruction count";
        return MAX_OPCODES - 1;
    }
    int idx = prog->code_count++;
    prog->code[idx] = (Instruction){op, arg1, arg2, arg3, arg4, lazy};
    return idx;
}

/* A character class containing multi-codepoint string alternatives (from
 * \q{...} or a Unicode "property of strings" like \p{RGI_Emoji_Flag_Sequence})
 * can't be matched by a single OP_CLASS instruction, which only ever tests
 * one code point against cls->ranges. Compile it instead as an alternation
 * -- each string in declaration order, then (if the class also has ordinary
 * single-codepoint ranges) a final OP_CLASS fallback -- reusing the VM's
 * existing, already-correct OP_SPLIT/backtracking machinery rather than
 * teaching OP_CLASS a second matching mode. Preferring strings before the
 * single-codepoint fallback mirrors how the Unicode property data itself is
 * structured (ranges and sequences are disjoint code point sets in every
 * property this engine generates); rtl mirrors AST_CONCAT's lookbehind
 * handling by emitting each string's code points in reverse. */
/* qsort comparator over {length, index} pairs: descending length, with the
 * original index as tiebreak so equal-length strings keep declaration
 * order (qsort itself is not stable). */
typedef struct { int length; int index; } StringOrder;

static int string_order_cmp(const void* pa, const void* pb) {
    const StringOrder* a = (const StringOrder*)pa;
    const StringOrder* b = (const StringOrder*)pb;
    if (a->length != b->length) return b->length - a->length;
    return a->index - b->index;
}

static void compile_class_with_strings(Program* prog, int class_id, bool rtl) {
    CharClass* cls = &prog->classes[class_id];
    /* Heap scratch sized to the actual string count -- these were fixed
     * [128] arrays back when the string set itself was capped at 128;
     * a full \p{RGI_Emoji} is 2604 alternatives now. */
    int* jmp_pcs = malloc(sizeof(int) * (size_t)cls->string_count);
    StringOrder* order = malloc(sizeof(StringOrder) * (size_t)cls->string_count);
    if (!jmp_pcs || !order) {
        fprintf(stderr, "Fatal Error: Out of memory\n");
        exit(EXIT_FAILURE);
    }
    int jmp_count = 0;
    /* Longest strings first: the spec's v-mode CharacterClass matcher
     * tries a class's string alternatives in descending length order, so
     * \q{ab|abc} against "abc" must consume all three units. The VM's
     * OP_SPLIT chain tries alternatives in emission order, so emission
     * order IS preference order. */
    for (int i = 0; i < cls->string_count; i++) {
        order[i].length = cls->strings[i].length;
        order[i].index = i;
    }
    qsort(order, (size_t)cls->string_count, sizeof(StringOrder), string_order_cmp);
    for (int i = 0; i < cls->string_count; i++) {
        bool has_more = (i < cls->string_count - 1) || (cls->range_count > 0);
        int split = has_more ? emit(prog, OP_SPLIT, 0, 0, 0, 0, false) : -1;
        if (split != -1) prog->code[split].arg1 = prog->code_count;
        StringSequence* seq = &cls->strings[order[i].index];
        if (rtl) {
            for (int k = seq->length - 1; k >= 0; k--) emit(prog, OP_CHAR, emit_char_operand(prog, seq->cps[k]), prog->ignore_case, 0, 0, false);
        } else {
            for (int k = 0; k < seq->length; k++) emit(prog, OP_CHAR, emit_char_operand(prog, seq->cps[k]), prog->ignore_case, 0, 0, false);
        }
        if (has_more) {
            jmp_pcs[jmp_count++] = emit(prog, OP_JMP, 0, 0, 0, 0, false);
            prog->code[split].arg2 = prog->code_count;
        }
    }
    if (cls->range_count > 0) emit(prog, OP_CLASS, class_id, 0, 0, 0, false);
    int end_pc = prog->code_count;
    for (int i = 0; i < jmp_count; i++) prog->code[jmp_pcs[i]].arg1 = end_pc;
    free(jmp_pcs);
    free(order);
}

/* Smallest and largest capture-group id *defined* inside this subtree --
 * AST_GROUP with id > 0 only (id 0 is a non-capturing group; AST_BACKREF's
 * id is a reference to a paren, not a paren). Because the parser assigns
 * ids in source order, a subtree's ids always form the contiguous range
 * [lo, hi] -- the same parenIndex/parenCount range ECMA-262's RepeatMatcher
 * is specified over. Recursion depth is bounded by MAX_AST_DEPTH. */
static void group_id_range(ASTNode* node, int* lo, int* hi) {
    if (!node) return;
    if (node->type == AST_GROUP && node->id > 0) {
        if (node->id < *lo) *lo = node->id;
        if (node->id > *hi) *hi = node->id;
    }
    group_id_range(node->left, lo, hi);
    group_id_range(node->right, lo, hi);
}

static void compile_node(ASTNode* node, Program* prog, bool rtl) {
    if (!node) return;
    /* Match-time flag decisions are baked into each instruction as it's
     * emitted (OP_CHAR/backrefs carry effective ignore_case in arg2, the
     * anchors carry effective multiline in arg1, the word boundaries carry
     * effective ignore_case in arg1 for /iu's fold-aware IsWordChar) rather
     * than read from
     * prog->* at match time -- the AST_MODIFIER_GROUP case below toggles
     * the prog fields only for the duration of this compile walk, so a
     * (?i:...) body's instructions must capture the toggled value while it
     * is in effect. dot_all needs no instruction bit: `.` is a CharClass
     * built at lex time, where re_parser.c does the equivalent toggle. */
    switch (node->type) {
        case AST_ASSERT_START: emit(prog, OP_ASSERT_START, prog->multiline, 0, 0, 0, false); break;
        case AST_ASSERT_END:   emit(prog, OP_ASSERT_END, prog->multiline, 0, 0, 0, false); break;
        case AST_WORD_BOUNDARY:emit(prog, OP_WORD_BOUNDARY, prog->ignore_case, 0, 0, 0, false); break;
        case AST_NON_WORD_BOUNDARY:emit(prog, OP_NON_WORD_BOUNDARY, prog->ignore_case, 0, 0, 0, false); break;
        case AST_BACKREF:      emit(prog, OP_BACKREF, node->id, prog->ignore_case, 0, 0, false); prog->has_backrefs = true; break;
        case AST_NAMED_BACKREF: {
            /* Emit OP_NAMED_BACKREF, not OP_BACKREF: with ES2025 duplicate
             * group names, which group a \k<name> refers to is only known
             * at match time (whichever same-named group actually
             * participated), and the VM's OP_NAMED_BACKREF implements
             * exactly that search. Binding to the first same-named id at
             * compile time -- as this used to -- made \k<x> silently match
             * empty whenever a *different* alternative's group had
             * captured (found via test262's duplicate-names tests). */
            int gid = -1;
            for (int i = 1; i <= prog->group_count; i++) {
                if (strcmp(prog->group_names[i], node->name) == 0) {
                    gid = i;
                    break;
                }
            }
            if (gid != -1) { emit(prog, OP_NAMED_BACKREF, gid, prog->ignore_case, 0, 0, false); prog->has_backrefs = true; }
            break;
        }
        case AST_MODIFIER_GROUP: {
            bool old_ignore_case = prog->ignore_case;
            bool old_multiline = prog->multiline;
            bool old_dot_all = prog->dot_all;

            if (node->flags_on & REGEX_FLAG_IGNORECASE) prog->ignore_case = true;
            if (node->flags_on & REGEX_FLAG_MULTILINE) prog->multiline = true;
            if (node->flags_on & REGEX_FLAG_DOTALL) prog->dot_all = true;
            if (node->flags_off & REGEX_FLAG_IGNORECASE) prog->ignore_case = false;
            if (node->flags_off & REGEX_FLAG_MULTILINE) prog->multiline = false;
            if (node->flags_off & REGEX_FLAG_DOTALL) prog->dot_all = false;

            compile_node(node->left, prog, rtl);

            prog->ignore_case = old_ignore_case; prog->multiline = old_multiline; prog->dot_all = old_dot_all;
            break;
        }
        case AST_LITERAL: emit(prog, OP_CHAR, emit_char_operand(prog, node->ch), prog->ignore_case, 0, 0, false); break;
        case AST_CLASS:
            if (prog->classes[node->id].string_count > 0) compile_class_with_strings(prog, node->id, rtl);
            else emit(prog, OP_CLASS, node->id, 0, 0, 0, false);
            break;
        case AST_CONCAT:
            if (rtl) {
                compile_node(node->right, prog, rtl); compile_node(node->left, prog, rtl);
            } else {
                compile_node(node->left, prog, rtl); compile_node(node->right, prog, rtl);
            }
            break;
        case AST_GROUP:
            if (rtl) {
                if (node->id > 0) emit(prog, OP_SAVE, node->id * 2 + 1, 0, 0, 0, false);
                compile_node(node->left, prog, rtl);
                if (node->id > 0) emit(prog, OP_SAVE, node->id * 2, 0, 0, 0, false);
            } else {
                if (node->id > 0) emit(prog, OP_SAVE, node->id * 2, 0, 0, 0, false);
                compile_node(node->left, prog, rtl);
                if (node->id > 0) emit(prog, OP_SAVE, node->id * 2 + 1, 0, 0, 0, false);
            }
            break;
        case AST_LOOKAHEAD:
        case AST_NEG_LOOKAHEAD: {
            int jmp = emit(prog, OP_JMP, 0, 0, 0, 0, false);
            int la_start = prog->code_count;
            compile_node(node->left, prog, false);
            emit(prog, OP_MATCH, 0, 0, 0, 0, false); 
            prog->code[jmp].arg1 = prog->code_count; 
            emit(prog, (node->type == AST_LOOKAHEAD) ? OP_LOOKAHEAD : OP_NEG_LOOKAHEAD, la_start, 0, 0, 0, false); break;
        }
        case AST_LOOKBEHIND:
        case AST_NEG_LOOKBEHIND: {
            int jmp = emit(prog, OP_JMP, 0, 0, 0, 0, false);
            int lb_start = prog->code_count;
            compile_node(node->left, prog, true);
            emit(prog, OP_MATCH, 0, 0, 0, 0, false); 
            prog->code[jmp].arg1 = prog->code_count; 
            emit(prog, (node->type == AST_LOOKBEHIND) ? OP_LOOKBEHIND : OP_NEG_LOOKBEHIND, lb_start, 0, 0, 0, false); break;
        }
        case AST_ALT: {
            int split = emit(prog, OP_SPLIT, 0, 0, 0, 0, false);
            prog->code[split].arg1 = prog->code_count;
            compile_node(node->left, prog, rtl);
            int jmp = emit(prog, OP_JMP, 0, 0, 0, 0, false);
            prog->code[split].arg2 = prog->code_count;
            compile_node(node->right, prog, rtl);
            prog->code[jmp].arg1 = prog->code_count; break;
        }
        case AST_QUANTIFIER: {
            /* Bounds-checked against MAX_COUNTERS (a
             * confirmed stack-buffer-overflow: a pattern with more
             * than MAX_COUNTERS bounded quantifiers wrote past the VM's
             * per-thread counters[MAX_COUNTERS]/counter_sp[MAX_COUNTERS]
             * arrays). No reserved-zero convention here (unlike group ids),
             * so counter_count >= MAX_COUNTERS is the exact overflow point;
             * clamping to the last valid slot keeps this safe the same way
             * emit()'s clamp does, for the same reason (prog->error set =>
             * this Program is discarded, never executed). */
            int counter_id;
            if (prog->counter_count < MAX_COUNTERS) {
                counter_id = prog->counter_count++;
            } else {
                if (!prog->error) prog->error = "InternalError: pattern exceeds maximum quantifier count";
                counter_id = MAX_COUNTERS - 1;
            }
            emit(prog, OP_INIT_COUNTER, counter_id, 0, 0, 0, false);
            int split_pc = prog->code_count;
            int check_idx = emit(prog, OP_CHECK_COUNTER, counter_id, node->min, node->max, 0, node->lazy);
            /* ECMA-262 RepeatMatcher: captures inside the repeated Atom
             * reset at the start of every iteration, so a branch that
             * doesn't participate in the final iteration ends up unset
             * rather than keeping a stale value from an earlier one
             * (OP_CLEAR_CAPTURES existed in
             * the VM but was never emitted). Placing the clear after
             * OP_CHECK_COUNTER is what preserves the "last successful
             * iteration wins" retention on exit: the exit thread
             * OP_CHECK_COUNTER pushes carries the captures as they were
             * *before* this instruction runs. */
            int clear_lo = MAX_GROUPS, clear_hi = 0;
            group_id_range(node->left, &clear_lo, &clear_hi);
            if (clear_hi > 0) emit(prog, OP_CLEAR_CAPTURES, clear_lo, clear_hi, 0, 0, false);
            compile_node(node->left, prog, rtl);
            emit(prog, OP_INC_COUNTER, counter_id, 0, 0, 0, false);
            emit(prog, OP_JMP, split_pc, 0, 0, 0, false);
            prog->code[check_idx].arg4 = prog->code_count; 
            break;
        }
    }
}

static void validate_backrefs(ASTNode* node, int group_count, const char** error) {
    if (!node || *error) return;
    if (node->type == AST_BACKREF && node->id > group_count) {
        *error = "SyntaxError: Invalid backreference";
        return;
    }
    validate_backrefs(node->left, group_count, error);
    validate_backrefs(node->right, group_count, error);
}

/* Every named backreference must resolve to a group with that name. */
static void validate_named_backrefs(ASTNode* node, Program* prog, const char** error) {
    if (!node || *error) return;
    if (node->type == AST_NAMED_BACKREF) {
        bool found = false;
        for (int i = 1; i <= prog->group_count; i++) {
            if (strcmp(prog->group_names[i], node->name) == 0) { found = true; break; }
        }
        if (!found) { *error = "SyntaxError: Invalid named capture referenced"; return; }
    }
    validate_named_backrefs(node->left, prog, error);
    validate_named_backrefs(node->right, prog, error);
}

/* Whether the pattern text contains a named capture group '(?<name>' (not a
 * lookbehind '(?<=' / '(?<!'). Governs whether '\k' is a named backreference.
 * Character-class contents and escaped chars are skipped. */
static bool scan_has_named_group(const uint16_t* src) {
    bool in_class = false;
    for (int i = 0; src[i] != '\0'; i++) {
        uint16_t c = src[i];
        if (c == '\\') { if (src[i + 1] != '\0') i++; continue; }
        if (in_class) { if (c == ']') in_class = false; continue; }
        if (c == '[') { in_class = true; continue; }
        if (c == '(' && src[i + 1] == '?' && src[i + 2] == '<' &&
            src[i + 3] != '=' && src[i + 3] != '!') {
            return true;
        }
    }
    return false;
}

void compile_into(Program* prog, const uint16_t* regex, int flags) {
    /* Program is ~2MB; a full `= {0}` zeroing (and return-by-value copying)
     * dominated RegExp construction cost. Only the bookkeeping needs
     * initialisation: code[]/classes[] entries are fully written before use
     * and never read past their counts; group_names IS read by name lookups
     * for unnamed groups, so it stays zeroed. Class string sets are heap-
     * owned (see CharClass in regexp.h), so a re-compile into the same
     * Program must release the previous compile's buffers first -- which
     * also means a Program must be ZERO-initialized (calloc) before its
     * FIRST compile_into, or this loop would free garbage pointers. */
    for (int i = 0; i < prog->class_count; i++) class_strings_free(&prog->classes[i]);
    memset(prog->group_names, 0, sizeof(prog->group_names));
    memset(prog->name_chain, 0, sizeof(prog->name_chain));
    prog->code_count = 0;
    prog->class_count = 0;
    prog->group_count = 0;
    prog->counter_count = 0;
    prog->has_backrefs = false;
    prog->error = NULL;
    prog->ignore_case = (flags & REGEX_FLAG_IGNORECASE) != 0;
    prog->multiline = (flags & REGEX_FLAG_MULTILINE) != 0;
    prog->dot_all = (flags & REGEX_FLAG_DOTALL) != 0;
    prog->sticky = (flags & REGEX_FLAG_STICKY) != 0;
    prog->unicode = (flags & REGEX_FLAG_UNICODE) != 0 || (flags & REGEX_FLAG_UNICODE_SETS) != 0;
    prog->unicode_sets = (flags & REGEX_FLAG_UNICODE_SETS) != 0;
    prog->has_indices = (flags & REGEX_FLAG_INDICES) != 0;
    Lexer lexer = {regex, 0, {0}, prog, false};
    lexer.has_named_groups = scan_has_named_group(regex);
    next_token(&lexer);
    ASTNode* ast = parse_alt(&lexer);

    if (!prog->error && lexer.current.type != TOK_EOF) {
        if (lexer.current.type == TOK_STAR || lexer.current.type == TOK_PLUS || lexer.current.type == TOK_QUESTION || lexer.current.type == TOK_BOUNDS) {
            prog->error = "SyntaxError: Nothing to repeat";
        } else if (lexer.current.type == TOK_RPAREN) {
            prog->error = "SyntaxError: Unmatched ')'";
        } else {
            prog->error = "SyntaxError: Trailing unparsed tokens";
        }
    }

    if (!prog->error) {
        NameSet set = {0};
        validate_group_names(ast, &set, &prog->error);
    }

    if (!prog->error) {
        validate_named_backrefs(ast, prog, &prog->error);
    }

    /* Spec-wise an out-of-range numeric backreference is an early error only
     * under /u -- Annex B instead re-reads small ones as octal/identity
     * escapes -- but this engine has no such fallback: the lexer tokenizes
     * backslash-digits as TOK_BACKREF in every mode, so an unvalidated id
     * indexes captures[] far out of bounds at match time
     * (confirmed OOB read via /(a)\999/).
     * Validating unconditionally trades exact Annex B semantics for a
     * SyntaxError, which is strictly safer than the UB it replaces. */
    if (!prog->error) {
        validate_backrefs(ast, prog->group_count, &prog->error);
    }

    if (!prog->error) {
        /* Same-name chains for OP_NAMED_BACKREF (see name_chain in
         * regexp.h); group_names is complete once parsing is done, so this
         * runs before any instruction that consumes the chain is emitted. */
        for (int i = 1; i <= prog->group_count; i++) {
            if (!prog->group_names[i][0]) continue;
            for (int j = i + 1; j <= prog->group_count; j++) {
                if (strcmp(prog->group_names[i], prog->group_names[j]) == 0) {
                    prog->name_chain[i] = j;
                    break;
                }
            }
        }
        emit(prog, OP_SAVE, 0, 0, 0, 0, false);
        compile_node(ast, prog, false);
        emit(prog, OP_SAVE, 1, 0, 0, 0, false);
        emit(prog, OP_MATCH, 0, 0, 0, 0, false);
    }
    free_ast(ast);
}
