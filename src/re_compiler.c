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
 * upstream's layout, and docs/IMPROVEMENTS.md section 4 for the rationale.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "regexp.h"
#include "re_internal.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

static int emit(Program* prog, RegexOpCode op, int arg1, int arg2, int arg3, int arg4, bool lazy) {
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
static void compile_class_with_strings(Program* prog, int class_id, bool rtl) {
    CharClass* cls = &prog->classes[class_id];
    int jmp_pcs[128];
    int jmp_count = 0;
    for (int i = 0; i < cls->string_count; i++) {
        bool has_more = (i < cls->string_count - 1) || (cls->range_count > 0);
        int split = has_more ? emit(prog, OP_SPLIT, 0, 0, 0, 0, false) : -1;
        if (split != -1) prog->code[split].arg1 = prog->code_count;
        StringSequence* seq = &cls->strings[i];
        if (rtl) {
            for (int k = seq->length - 1; k >= 0; k--) emit(prog, OP_CHAR, seq->cps[k], 0, 0, 0, false);
        } else {
            for (int k = 0; k < seq->length; k++) emit(prog, OP_CHAR, seq->cps[k], 0, 0, 0, false);
        }
        if (has_more) {
            jmp_pcs[jmp_count++] = emit(prog, OP_JMP, 0, 0, 0, 0, false);
            prog->code[split].arg2 = prog->code_count;
        }
    }
    if (cls->range_count > 0) emit(prog, OP_CLASS, class_id, 0, 0, 0, false);
    int end_pc = prog->code_count;
    for (int i = 0; i < jmp_count; i++) prog->code[jmp_pcs[i]].arg1 = end_pc;
}

static void compile_node(ASTNode* node, Program* prog, bool rtl) {
    if (!node) return;
    switch (node->type) {
        case AST_ASSERT_START: emit(prog, OP_ASSERT_START, 0, 0, 0, 0, false); break;
        case AST_ASSERT_END:   emit(prog, OP_ASSERT_END, 0, 0, 0, 0, false); break;
        case AST_WORD_BOUNDARY:emit(prog, OP_WORD_BOUNDARY, 0, 0, 0, 0, false); break;
        case AST_NON_WORD_BOUNDARY:emit(prog, OP_NON_WORD_BOUNDARY, 0, 0, 0, 0, false); break;
        case AST_BACKREF:      emit(prog, OP_BACKREF, node->id, 0, 0, 0, false); break;
        case AST_NAMED_BACKREF: {
            int gid = -1;
            for (int i = 1; i <= prog->group_count; i++) {
                if (strcmp(prog->group_names[i], node->name) == 0) {
                    gid = i;
                    break;
                }
            }
            if (gid != -1) emit(prog, OP_BACKREF, gid, 0, 0, 0, false);
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
        case AST_LITERAL: emit(prog, OP_CHAR, node->ch, 0, 0, 0, false); break;
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
            int counter_id = prog->counter_count++;
            emit(prog, OP_INIT_COUNTER, counter_id, 0, 0, 0, false);
            int split_pc = prog->code_count;
            int check_idx = emit(prog, OP_CHECK_COUNTER, counter_id, node->min, node->max, 0, node->lazy);
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
     * for unnamed groups, so it stays zeroed. */
    memset(prog->group_names, 0, sizeof(prog->group_names));
    prog->code_count = 0;
    prog->class_count = 0;
    prog->group_count = 0;
    prog->counter_count = 0;
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

    /* In unicode mode, backreferences to non-existent groups are early errors */
    if (!prog->error && prog->unicode) {
        validate_backrefs(ast, prog->group_count, &prog->error);
    }

    if (!prog->error) {
        emit(prog, OP_SAVE, 0, 0, 0, 0, false);
        compile_node(ast, prog, false);
        emit(prog, OP_SAVE, 1, 0, 0, 0, false);
        emit(prog, OP_MATCH, 0, 0, 0, 0, false);
    }
    free_ast(ast);
}
