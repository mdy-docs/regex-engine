/* Internal declarations shared between this engine's lexer/parser/compiler/
 * VM translation units (re_lexer.c, re_parser.c, re_compiler.c, re_vm.c).
 * Not part of the public API -- include/regexp.h is that. Nothing outside
 * src/ should include this header.
 *
 * These four files are a structural split of what was originally one
 * verbatim-copied-from-jsvm2 file (src/regexp.c); see docs/ARCHITECTURE.md
 * for how the pieces fit together and CLAUDE.md/README.md's "Provenance"
 * section for why the split diverges from jsvm2's own single-file layout.
 */
#ifndef RE_INTERNAL_H
#define RE_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "regexp.h"

/* ---- Lexer (re_lexer.c) --------------------------------------------------
 * Token/TokenType/Lexer are defined here rather than in re_lexer.c itself
 * because re_parser.c (every parse_* function takes a Lexer*) and
 * re_compiler.c (compile_into constructs one directly to bootstrap parsing)
 * both need the full definitions, not just a Lexer* pointer type. */

typedef enum {
    TOK_LITERAL, TOK_CLASS, TOK_STAR, TOK_PLUS, TOK_QUESTION,
    TOK_BOUNDS, TOK_OR, TOK_LPAREN, TOK_RPAREN, TOK_LOOKAHEAD, TOK_NEG_LOOKAHEAD, TOK_EOF,
    TOK_CARET, TOK_DOLLAR, TOK_BACKREF, TOK_NONCAP_GROUP, TOK_WORD_BOUNDARY, TOK_NON_WORD_BOUNDARY,
    TOK_NAMED_GROUP, TOK_NAMED_BACKREF, TOK_LOOKBEHIND, TOK_NEG_LOOKBEHIND, TOK_MODIFIER_GROUP
} TokenType;

typedef struct {
    TokenType type;
    uint32_t ch;
    int class_id;
    int min;
    int max;
    char name[MAX_GROUP_NAME];
    int flags_on;
    int flags_off;
} Token;

typedef struct {
    const uint16_t* src;
    int pos;
    Token current;
    Program* prog;
    bool has_named_groups;
    /* Live count of nested parse_alt() calls currently on the C stack --
     * both nested groups (parse_primary -> parse_alt) and chained
     * alternation (parse_alt -> parse_alt for each '|') recurse through
     * parse_alt, so checking *there*, before recursing further, is what
     * actually bounds the parser's own stack usage (unlike ASTNode.depth,
     * which is only known *after* a subtree is fully built -- too late to
     * stop the recursion that built it). See MAX_AST_DEPTH in
     * include/regexp.h. Zero-initialized for free by compile_into's
     * existing partial Lexer initializer. */
    int parse_depth;
} Lexer;

/* Advances the lexer past one token into lexer->current. Called from every
 * parse_* function in re_parser.c and from re_compiler.c's compile_into (to
 * bootstrap parsing with the first token). */
void next_token(Lexer* lexer);

/* ---- Parser (re_parser.c) ------------------------------------------------
 * ASTType/ASTNode/NameSet are shared for the same reason as Lexer/Token
 * above: re_compiler.c's compile_into holds an ASTNode* (parse_alt's
 * result) and constructs a NameSet directly when calling
 * validate_group_names. */

typedef enum {
    AST_LITERAL, AST_CLASS, AST_CONCAT, AST_ALT, AST_GROUP, AST_LOOKAHEAD, AST_NEG_LOOKAHEAD,
    AST_QUANTIFIER, AST_ASSERT_START, AST_ASSERT_END, AST_BACKREF, AST_NAMED_BACKREF,
    AST_WORD_BOUNDARY, AST_NON_WORD_BOUNDARY, AST_LOOKBEHIND, AST_NEG_LOOKBEHIND, AST_MODIFIER_GROUP
} ASTType;

typedef struct ASTNode {
    ASTType type; uint32_t ch; int id; int min; int max; bool lazy;
    char name[MAX_GROUP_NAME];
    int flags_on; int flags_off;
    struct ASTNode* left; struct ASTNode* right;
    /* Height of this node's subtree (leaves are 1); maintained incrementally
     * as the parser attaches children, and checked against MAX_AST_DEPTH
     * (see re_parser.c) to guard against a stack-overflow class of bug --
     * free_ast, validate_group_names,
     * validate_backrefs, validate_named_backrefs, and compile_node all
     * recurse through left/right with no depth limit of their own. */
    int depth;
} ASTNode;

typedef struct {
    char names[MAX_GROUPS][MAX_GROUP_NAME];
    int count;
} NameSet;

/* ---- VM (re_vm.c) --------------------------------------------------------
 * ECMA-262 Canonicalize for the non-Unicode case (simple uppercase mapping
 * with the non-ASCII -> ASCII fold exception). Lives in re_vm.c (it matches
 * input at run time); the compiler also calls it to pre-canonicalize
 * OP_CHAR's constant operand at emit time, so the VM folds only the input
 * side. unicode_casefold (ucd.h) is its /u-mode counterpart. */
uint32_t annexb_canonicalize(uint32_t ch);

/* Parses a full Disjunction starting at lexer->current, consuming tokens via
 * next_token as it goes. Entry point re_compiler.c's compile_into calls to
 * get an AST; also called recursively within the parser for group bodies. */
ASTNode* parse_alt(Lexer* lexer);

/* Frees an AST built by parse_alt. Called from re_compiler.c's compile_into
 * once it's done compiling the tree to bytecode. */
void free_ast(ASTNode* node);

/* Collects every capture group name into out_set, rejecting duplicates
 * within the same alternative (duplicates across mutually-exclusive
 * alternation branches are allowed, per ES2025). Called from re_compiler.c's
 * compile_into as a post-parse validation pass. */
bool validate_group_names(ASTNode* node, NameSet* out_set, const char** error);

#endif /* RE_INTERNAL_H */
