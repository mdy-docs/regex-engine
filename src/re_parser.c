/* Parser: recursive-descent AST builder over the token stream next_token()
 * (re_lexer.c) produces. Grammar: parse_alt -> parse_concat -> 
 * parse_quantifier -> parse_primary, precedence-climbing style -- see
 * docs/ARCHITECTURE.md's "Parser" section for the full grammar and the
 * important caveat that parse_concat/parse_alt build linear (not balanced)
 * chains for flat sequences.
 *
 * Split out of what was originally a single file (src/regexp.c, itself a
 * verbatim copy of jsvm2's src/regexp.c) for maintainability -- see
 * CLAUDE.md/README.md's "Provenance" section for why that diverges from
 * upstream's layout, and docs/IMPROVEMENTS.md section 4 for the rationale.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "regexp.h"
#include "re_internal.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

static ASTNode* create_node(ASTType type) {
    ASTNode* node = calloc(1, sizeof(ASTNode));
    if (!node) {
        fprintf(stderr, "Fatal Error: Out of memory\n");
        exit(EXIT_FAILURE);
    }
    node->type = type;
    return node;
}

static ASTNode* parse_primary(Lexer* lexer) {
    ASTNode* node = NULL;
    if (lexer->current.type == TOK_LITERAL) { node = create_node(AST_LITERAL); node->ch = lexer->current.ch; next_token(lexer); } 
    else if (lexer->current.type == TOK_CLASS) { node = create_node(AST_CLASS); node->id = lexer->current.class_id; next_token(lexer); } 
    else if (lexer->current.type == TOK_CARET) { node = create_node(AST_ASSERT_START); next_token(lexer); } 
    else if (lexer->current.type == TOK_DOLLAR) { node = create_node(AST_ASSERT_END); next_token(lexer); } 
    else if (lexer->current.type == TOK_BACKREF) { node = create_node(AST_BACKREF); node->id = lexer->current.class_id; next_token(lexer); } 
    else if (lexer->current.type == TOK_NAMED_BACKREF) { node = create_node(AST_NAMED_BACKREF); strcpy(node->name, lexer->current.name); next_token(lexer); } 
    else if (lexer->current.type == TOK_WORD_BOUNDARY) { node = create_node(AST_WORD_BOUNDARY); next_token(lexer); } 
    else if (lexer->current.type == TOK_NON_WORD_BOUNDARY) { node = create_node(AST_NON_WORD_BOUNDARY); next_token(lexer); } 
    else if (lexer->current.type == TOK_LPAREN || lexer->current.type == TOK_LOOKAHEAD || lexer->current.type == TOK_NEG_LOOKAHEAD || lexer->current.type == TOK_LOOKBEHIND || lexer->current.type == TOK_NEG_LOOKBEHIND || lexer->current.type == TOK_NONCAP_GROUP || lexer->current.type == TOK_NAMED_GROUP || lexer->current.type == TOK_MODIFIER_GROUP) {
        bool is_la = (lexer->current.type == TOK_LOOKAHEAD);
        bool is_neg_la = (lexer->current.type == TOK_NEG_LOOKAHEAD);
        bool is_lb = (lexer->current.type == TOK_LOOKBEHIND);
        bool is_neg_lb = (lexer->current.type == TOK_NEG_LOOKBEHIND);
        bool is_noncap = (lexer->current.type == TOK_NONCAP_GROUP);
        bool is_named = (lexer->current.type == TOK_NAMED_GROUP);
        bool is_modifier = (lexer->current.type == TOK_MODIFIER_GROUP);
        char name[32] = {0};
        int flags_on = 0, flags_off = 0;
        if (is_named) strcpy(name, lexer->current.name);
        if (is_modifier) {
            flags_on = lexer->current.flags_on;
            flags_off = lexer->current.flags_off;
        }
        
        next_token(lexer);
        ASTNode* inner = parse_alt(lexer);
        if (lexer->current.type == TOK_RPAREN) {
            next_token(lexer);
        } else {
            lexer->prog->error = "SyntaxError: Unterminated group";
        }
        
        if (is_modifier) {
            node = create_node(AST_MODIFIER_GROUP);
            node->flags_on = flags_on;
            node->flags_off = flags_off;
        } else {
            node = create_node(is_la ? AST_LOOKAHEAD : (is_neg_la ? AST_NEG_LOOKAHEAD : (is_lb ? AST_LOOKBEHIND : (is_neg_lb ? AST_NEG_LOOKBEHIND : AST_GROUP))));
        }
        node->left = inner;
        if (!is_la && !is_neg_la && !is_lb && !is_neg_lb && !is_noncap && !is_modifier) {
            node->id = ++lexer->prog->group_count; 
            if (is_named) strcpy(lexer->prog->group_names[node->id], name);
            if (is_named) strcpy(node->name, name);
        }
    }
    return node;
}

static ASTNode* parse_quantifier(Lexer* lexer) {
    ASTNode* node = parse_primary(lexer);
    if (!node) return NULL;
    TokenType t = lexer->current.type;
    if (t == TOK_STAR || t == TOK_PLUS || t == TOK_QUESTION || t == TOK_BOUNDS) {
        /* Lookahead/lookbehind assertions cannot be quantified */
        if (node->type == AST_LOOKAHEAD || node->type == AST_NEG_LOOKAHEAD ||
            node->type == AST_LOOKBEHIND || node->type == AST_NEG_LOOKBEHIND) {
            lexer->prog->error = "SyntaxError: Invalid quantifier applied to assertion";
        }
        ASTNode* q = create_node(AST_QUANTIFIER);
        q->left = node;
        if (t == TOK_STAR)      { q->min = 0; q->max = -1; }
        else if (t == TOK_PLUS) { q->min = 1; q->max = -1; }
        else if (t == TOK_QUESTION){ q->min = 0; q->max = 1; }
        else if (t == TOK_BOUNDS)  { q->min = lexer->current.min; q->max = lexer->current.max; }
        next_token(lexer);
        if (lexer->current.type == TOK_QUESTION) { q->lazy = true; next_token(lexer); }
        node = q;
    }
    return node;
}

static ASTNode* parse_concat(Lexer* lexer) {
    ASTNode* node = parse_quantifier(lexer);
    while (lexer->current.type == TOK_LITERAL || lexer->current.type == TOK_CLASS || 
           lexer->current.type == TOK_LPAREN || lexer->current.type == TOK_LOOKAHEAD ||
           lexer->current.type == TOK_NEG_LOOKAHEAD ||
           lexer->current.type == TOK_LOOKBEHIND ||
           lexer->current.type == TOK_NEG_LOOKBEHIND ||
           lexer->current.type == TOK_CARET || lexer->current.type == TOK_DOLLAR || lexer->current.type == TOK_MODIFIER_GROUP ||
           lexer->current.type == TOK_BACKREF || 
           lexer->current.type == TOK_NAMED_BACKREF ||
           lexer->current.type == TOK_NONCAP_GROUP ||
           lexer->current.type == TOK_NAMED_GROUP ||
           lexer->current.type == TOK_WORD_BOUNDARY ||
           lexer->current.type == TOK_NON_WORD_BOUNDARY) {
        ASTNode* right = parse_quantifier(lexer);
        ASTNode* concat = create_node(AST_CONCAT);
        concat->left = node; concat->right = right;
        node = concat;
    }
    return node;
}

ASTNode* parse_alt(Lexer* lexer) {
    ASTNode* node = parse_concat(lexer);
    if (lexer->current.type == TOK_OR) {
        next_token(lexer);
        ASTNode* right = parse_alt(lexer);
        ASTNode* alt = create_node(AST_ALT);
        alt->left = node; alt->right = right;
        node = alt;
    }
    return node;
}

/* Non-static: called from re_compiler.c's compile_into. Declared in
 * re_internal.h. */
void free_ast(ASTNode* node) {
    if (!node) return;
    free_ast(node->left); free_ast(node->right); free(node);
}

/* Non-static: called from re_compiler.c's compile_into. Declared in
 * re_internal.h. */
bool validate_group_names(ASTNode* node, NameSet* out_set, const char** error) {
    if (!node) return true;
    if (node->type == AST_ALT) {
        NameSet left_set = {0}, right_set = {0};
        if (!validate_group_names(node->left, &left_set, error)) return false;
        if (!validate_group_names(node->right, &right_set, error)) return false;
        
        for (int i = 0; i < left_set.count; i++) {
            strcpy(out_set->names[out_set->count++], left_set.names[i]);
        }
        for (int i = 0; i < right_set.count; i++) {
            bool found = false;
            for (int j = 0; j < left_set.count; j++) {
                if (strcmp(right_set.names[i], left_set.names[j]) == 0) { found = true; break; }
            }
            if (!found && out_set->count < MAX_GROUPS) strcpy(out_set->names[out_set->count++], right_set.names[i]);
        }
        return true;
    } else {
        NameSet left_set = {0}, right_set = {0};
        if (!validate_group_names(node->left, &left_set, error)) return false;
        if (!validate_group_names(node->right, &right_set, error)) return false;
        
        for (int i = 0; i < left_set.count; i++) {
            strcpy(out_set->names[out_set->count++], left_set.names[i]);
        }
        for (int i = 0; i < right_set.count; i++) {
            for (int j = 0; j < left_set.count; j++) {
                if (strcmp(right_set.names[i], left_set.names[j]) == 0) {
                    *error = "SyntaxError: Duplicate capture group name";
                    return false;
                }
            }
            if (out_set->count < MAX_GROUPS) strcpy(out_set->names[out_set->count++], right_set.names[i]);
        }
        
        if (node->type == AST_GROUP && node->name[0] != '\0') {
            for (int i = 0; i < out_set->count; i++) {
                if (strcmp(out_set->names[i], node->name) == 0) {
                    *error = "SyntaxError: Duplicate capture group name";
                    return false;
                }
            }
            if (out_set->count < MAX_GROUPS) strcpy(out_set->names[out_set->count++], node->name);
        }
        return true;
    }
}
