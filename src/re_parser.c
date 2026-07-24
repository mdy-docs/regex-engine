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
 * upstream's layout.
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
    node->depth = 1; /* a freshly-created node is a leaf until finish_node()
                       * recomputes this from its children, if any get attached */
    return node;
}

static int node_depth(ASTNode* n) { return n ? n->depth : 0; }

/* Call once a node's left/right (or just left, for single-child wrappers
 * like AST_GROUP/AST_QUANTIFIER) have been attached, so its own depth can
 * be (re)computed from them. Rejects the pattern (once, via prog->error --
 * "first failure wins" like every other resource-limit check in this
 * engine) as soon as MAX_AST_DEPTH is exceeded, rather than letting the
 * tree keep growing -- see MAX_AST_DEPTH's comment in include/regexp.h for
 * why this matters and what it protects. Safe to keep calling after the
 * limit trips: node->depth stays accurate (so a parent combining an
 * already-too-deep child computes correctly, rather than silently
 * under-counting), and every parse_* caller's own error-propagation
 * (ultimately via next_token() short-circuiting to TOK_EOF once
 * prog->error is set) unwinds parsing shortly after regardless. */
static ASTNode* finish_node(Lexer* lexer, ASTNode* node) {
    int d = 1 + (node_depth(node->left) > node_depth(node->right) ? node_depth(node->left) : node_depth(node->right));
    node->depth = d;
    if (d > MAX_AST_DEPTH && !lexer->prog->error) {
        lexer->prog->error = "InternalError: pattern too deeply nested or too long to compile safely";
    }
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
        char name[MAX_GROUP_NAME] = {0};
        int flags_on = 0, flags_off = 0;
        if (is_named) strcpy(name, lexer->current.name);
        if (is_modifier) {
            flags_on = lexer->current.flags_on;
            flags_off = lexer->current.flags_off;
        }
        
        /* The capture-group id must be claimed HERE, at the opening paren,
         * before the body is parsed -- ECMA-262 numbers groups by the
         * position of '(' in source order, so in `((a)b)` the outer group
         * is 1 and the inner is 2. Assigning after parse_alt returns (as
         * this originally did, and jsvm2 upstream still does) numbers
         * nested groups by *closing* paren instead -- backwards from every
         * real JS engine, visibly wrong in both reported capture indices
         * and numeric-backreference resolution the moment groups nest.
         *
         * Bounds-checked against MAX_GROUPS (a confirmed
         * heap-buffer-overflow: an unchecked pattern
         * with more than MAX_GROUPS capture groups wrote past
         * prog->group_names[] and, at match time, past the VM's
         * captures[MAX_GROUPS*2] array). The safe ceiling is
         * MAX_GROUPS-1, not MAX_GROUPS: group id 0 is reserved for
         * "whole match" everywhere else in the engine, so it already
         * occupies one of the MAX_GROUPS slots even though no capture
         * group is ever assigned id 0 -- allowing group_count to reach
         * MAX_GROUPS itself would let a later id*2+1 index one past the
         * end of captures[MAX_GROUPS*2]. */
        bool is_capture = !is_la && !is_neg_la && !is_lb && !is_neg_lb && !is_noncap && !is_modifier;
        int gid = 0;
        if (is_capture) {
            if (lexer->prog->group_count < MAX_GROUPS - 1) {
                gid = ++lexer->prog->group_count;
                if (is_named) strcpy(lexer->prog->group_names[gid], name);
            } else {
                if (!lexer->prog->error) lexer->prog->error = "InternalError: pattern exceeds maximum capture group count";
            }
        }

        /* Modifier groups must take effect while the body is LEXED, not just
         * while it's compiled: `.` and character classes are built into
         * CharClass tables by next_token itself, reading prog->dot_all /
         * prog->ignore_case at that moment -- so (?s:.) and (?i:[a-z]) only
         * work if the flags are toggled around the body's parse (during
         * which its tokens are produced), then restored. compile_node's
         * AST_MODIFIER_GROUP does the same dance for what's decided at
         * compile time; match-time decisions ride along inside the emitted
         * instructions (see re_compiler.c). */
        bool saved_i = lexer->prog->ignore_case;
        bool saved_m = lexer->prog->multiline;
        bool saved_s = lexer->prog->dot_all;
        if (is_modifier) {
            if (flags_on & REGEX_FLAG_IGNORECASE) lexer->prog->ignore_case = true;
            if (flags_on & REGEX_FLAG_MULTILINE) lexer->prog->multiline = true;
            if (flags_on & REGEX_FLAG_DOTALL) lexer->prog->dot_all = true;
            if (flags_off & REGEX_FLAG_IGNORECASE) lexer->prog->ignore_case = false;
            if (flags_off & REGEX_FLAG_MULTILINE) lexer->prog->multiline = false;
            if (flags_off & REGEX_FLAG_DOTALL) lexer->prog->dot_all = false;
        }

        next_token(lexer);
        ASTNode* inner = parse_alt(lexer);
        /* Restore BEFORE consuming past the ')': the next_token below lexes
         * the first token AFTER the group, which must see the outer flags. */
        if (is_modifier) {
            lexer->prog->ignore_case = saved_i;
            lexer->prog->multiline = saved_m;
            lexer->prog->dot_all = saved_s;
        }
        if (lexer->current.type == TOK_RPAREN) {
            next_token(lexer);
        } else if (!lexer->prog->error) {
            /* First failure wins, like every other error site: once any
             * error is set, next_token short-circuits to TOK_EOF, so this
             * branch is reached for EVERY open group on the way out --
             * unguarded, it masked the real error (e.g. the class-count
             * limit surfaced as "Unterminated group"). */
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
        node = finish_node(lexer, node);
        if (is_capture) {
            node->id = gid;
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
        node = finish_node(lexer, q);
    }
    return node;
}

/* Builds a BALANCED binary tree over items[lo..hi] instead of the linear
 * chain the parser originally produced. Concatenation and alternation are
 * both associative in every way this engine observes them -- compile_node
 * emits CONCAT children in sequence order (either direction under rtl) and
 * ALT children in preference order regardless of association, and
 * validate_group_names' cross-branch rules compose -- so tree shape is
 * free to choose. Linear chains made AST height equal the ATOM COUNT,
 * which turned MAX_AST_DEPTH's C-stack guard into a hard ~200-atom limit
 * on flat patterns (a 300-character literal string failed to compile);
 * balanced, a million atoms are ~20 levels and the guard constrains only
 * real nesting. Recursion depth here is O(log n). */
static ASTNode* build_balanced(Lexer* lexer, ASTType type, ASTNode** items, int lo, int hi) {
    if (lo >= hi) return items[lo];
    int mid = lo + (hi - lo) / 2;
    ASTNode* n = create_node(type);
    n->left = build_balanced(lexer, type, items, lo, mid);
    n->right = build_balanced(lexer, type, items, mid + 1, hi);
    return finish_node(lexer, n);
}

static ASTNode** items_push(ASTNode** items, int* count, int* cap, ASTNode* node) {
    if (*count == *cap) {
        *cap *= 2;
        ASTNode** grown = realloc(items, sizeof(ASTNode*) * (size_t)*cap);
        if (!grown) {
            fprintf(stderr, "Fatal Error: Out of memory\n");
            exit(EXIT_FAILURE);
        }
        items = grown;
    }
    items[(*count)++] = node;
    return items;
}

static bool starts_atom(TokenType t) {
    return t == TOK_LITERAL || t == TOK_CLASS ||
           t == TOK_LPAREN || t == TOK_LOOKAHEAD ||
           t == TOK_NEG_LOOKAHEAD ||
           t == TOK_LOOKBEHIND ||
           t == TOK_NEG_LOOKBEHIND ||
           t == TOK_CARET || t == TOK_DOLLAR || t == TOK_MODIFIER_GROUP ||
           t == TOK_BACKREF ||
           t == TOK_NAMED_BACKREF ||
           t == TOK_NONCAP_GROUP ||
           t == TOK_NAMED_GROUP ||
           t == TOK_WORD_BOUNDARY ||
           t == TOK_NON_WORD_BOUNDARY;
}

static ASTNode* parse_concat(Lexer* lexer) {
    ASTNode* node = parse_quantifier(lexer);
    if (!starts_atom(lexer->current.type)) return node;
    int cap = 8, count = 0;
    ASTNode** items = malloc(sizeof(ASTNode*) * (size_t)cap);
    if (!items) {
        fprintf(stderr, "Fatal Error: Out of memory\n");
        exit(EXIT_FAILURE);
    }
    items = items_push(items, &count, &cap, node);
    while (starts_atom(lexer->current.type)) {
        ASTNode* right = parse_quantifier(lexer);
        if (!right) break;
        items = items_push(items, &count, &cap, right);
    }
    node = build_balanced(lexer, AST_CONCAT, items, 0, count - 1);
    free(items);
    return node;
}

ASTNode* parse_alt(Lexer* lexer) {
    /* Both nested groups (parse_primary calls parse_alt for the group body)
     * and chained alternation (parse_alt calls itself for each '|') recurse
     * through here on the C stack -- checked *before* recursing further,
     * since ASTNode.depth (via finish_node below) is only known *after* a
     * subtree is fully built, which is too late to stop the recursion that
     * built it. See MAX_AST_DEPTH's comment in include/regexp.h. */
    lexer->parse_depth++;
    if (lexer->parse_depth > MAX_AST_DEPTH) {
        if (!lexer->prog->error) lexer->prog->error = "InternalError: pattern too deeply nested or too long to compile safely";
        lexer->parse_depth--;
        /* A harmless placeholder: the caller (parse_primary, or top-level
         * compile_into) unwinds quickly once prog->error is set, since
         * next_token() starts short-circuiting to TOK_EOF immediately --
         * what's returned here barely matters structurally, it just needs
         * to be a valid, non-NULL node for callers that dereference it
         * (e.g. parse_quantifier checking node->type). */
        return create_node(AST_LITERAL);
    }
    ASTNode* node = parse_concat(lexer);
    if (lexer->current.type == TOK_OR) {
        /* Chained '|' is collected iteratively and built balanced (see
         * build_balanced) -- the old one-recursion-per-alternative shape
         * both consumed a parse_depth level per '|' and made AST height
         * equal the alternative count. NULL items are legitimate: an empty
         * alternative (/a|/) has no concat node and matches empty. */
        int cap = 8, count = 0;
        ASTNode** items = malloc(sizeof(ASTNode*) * (size_t)cap);
        if (!items) {
            fprintf(stderr, "Fatal Error: Out of memory\n");
            exit(EXIT_FAILURE);
        }
        items = items_push(items, &count, &cap, node);
        while (lexer->current.type == TOK_OR) {
            next_token(lexer);
            items = items_push(items, &count, &cap, parse_concat(lexer));
        }
        node = build_balanced(lexer, AST_ALT, items, 0, count - 1);
        free(items);
    }
    lexer->parse_depth--;
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
    /* The two child NameSets are HEAP-allocated: as ~8KB stack locals they
     * made this the most stack-hungry recursion over the AST (observed
     * crashing near depth ~247 on an 8MB stack), which is what originally
     * forced MAX_AST_DEPTH down to 200 -- a depth real patterns hit
     * (test262 exercises 200 nested groups). With small frames here, the
     * depth cap could be raised; see MAX_AST_DEPTH in include/regexp.h. */
    NameSet* sets = calloc(2, sizeof(NameSet));
    if (!sets) {
        fprintf(stderr, "Fatal Error: Out of memory\n");
        exit(EXIT_FAILURE);
    }
    NameSet* left_set = &sets[0];
    NameSet* right_set = &sets[1];
    bool ok = false;
    if (!validate_group_names(node->left, left_set, error)) goto done;
    if (!validate_group_names(node->right, right_set, error)) goto done;

    if (node->type == AST_ALT) {
        for (int i = 0; i < left_set->count; i++) {
            strcpy(out_set->names[out_set->count++], left_set->names[i]);
        }
        for (int i = 0; i < right_set->count; i++) {
            bool found = false;
            for (int j = 0; j < left_set->count; j++) {
                if (strcmp(right_set->names[i], left_set->names[j]) == 0) { found = true; break; }
            }
            if (!found && out_set->count < MAX_GROUPS) strcpy(out_set->names[out_set->count++], right_set->names[i]);
        }
        ok = true;
    } else {
        for (int i = 0; i < left_set->count; i++) {
            strcpy(out_set->names[out_set->count++], left_set->names[i]);
        }
        for (int i = 0; i < right_set->count; i++) {
            for (int j = 0; j < left_set->count; j++) {
                if (strcmp(right_set->names[i], left_set->names[j]) == 0) {
                    *error = "SyntaxError: Duplicate capture group name";
                    goto done;
                }
            }
            if (out_set->count < MAX_GROUPS) strcpy(out_set->names[out_set->count++], right_set->names[i]);
        }

        if (node->type == AST_GROUP && node->name[0] != '\0') {
            for (int i = 0; i < out_set->count; i++) {
                if (strcmp(out_set->names[i], node->name) == 0) {
                    *error = "SyntaxError: Duplicate capture group name";
                    goto done;
                }
            }
            if (out_set->count < MAX_GROUPS) strcpy(out_set->names[out_set->count++], node->name);
        }
        ok = true;
    }
done:
    free(sets);
    return ok;
}
