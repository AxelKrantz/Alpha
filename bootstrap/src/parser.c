#include "parser.h"
#include "error.h"
#include <stdio.h>
#include <string.h>

// ---- Helpers ----

static void parser_advance(Parser *p) {
    p->previous = p->current;
    p->current = lexer_next(p->lexer);
    if (p->current.type == TOK_ERROR) {
        error_at(p->filename, p->current.line, p->current.column,
                 "%.*s", p->current.length, p->current.start);
    }
}

static bool parser_check(Parser *p, TokenType type) {
    return p->current.type == type;
}

static bool parser_match(Parser *p, TokenType type) {
    if (p->current.type == type) {
        parser_advance(p);
        return true;
    }
    return false;
}

static void parser_expect(Parser *p, TokenType type) {
    if (p->current.type == type) {
        parser_advance(p);
        return;
    }
    error_at(p->filename, p->current.line, p->current.column,
             "expected '%s', got '%s'", token_type_name(type),
             token_type_name(p->current.type));
}

static void skip_newlines(Parser *p) {
    while (p->current.type == TOK_NEWLINE) {
        parser_advance(p);
    }
}

static void expect_terminator(Parser *p) {
    if (p->current.type == TOK_NEWLINE) {
        parser_advance(p);
        skip_newlines(p);
        return;
    }
    if (p->current.type == TOK_EOF || p->current.type == TOK_RBRACE) {
        return;
    }
    error_at(p->filename, p->current.line, p->current.column,
             "expected newline or end of block, got '%s'",
             token_type_name(p->current.type));
}

static char *token_to_str(Token *tok) {
    return str_dup(tok->start, tok->length);
}

// ---- Forward declarations ----
static ASTNode *parse_expression(Parser *p);
static FieldList parse_params(Parser *p);
static ASTNode *parse_statement(Parser *p);
static ASTNode *parse_block(Parser *p);
static ASTNode *parse_type(Parser *p);

// ---- Type parsing ----

static ASTNode *parse_type(Parser *p) {
    // &type or &mut type
    if (parser_match(p, TOK_AMP)) {
        bool is_mut = parser_match(p, TOK_MUT);
        ASTNode *inner = parse_type(p);
        ASTNode *node = ast_new(NODE_TYPE_REF, p->previous.line, p->previous.column);
        node->type_ref.inner = inner;
        node->type_ref.is_mut = is_mut;
        return node;
    }

    // [type]
    if (parser_match(p, TOK_LBRACKET)) {
        ASTNode *elem = parse_type(p);
        parser_expect(p, TOK_RBRACKET);
        ASTNode *node = ast_new(NODE_TYPE_ARRAY, p->previous.line, p->previous.column);
        node->type_array.element_type = elem;
        return node;
    }

    // Named type, possibly generic: Type<T, U>
    parser_expect(p, TOK_IDENT);
    char *name = token_to_str(&p->previous);
    int line = p->previous.line;
    int col = p->previous.column;

    if (parser_check(p, TOK_LT)) {
        parser_advance(p);
        ASTNode *node = ast_new(NODE_TYPE_GENERIC, line, col);
        node->type_generic.name = name;
        node_list_init(&node->type_generic.type_args);

        if (!parser_check(p, TOK_GT)) {
            node_list_push(&node->type_generic.type_args, parse_type(p));
            while (parser_match(p, TOK_COMMA)) {
                node_list_push(&node->type_generic.type_args, parse_type(p));
            }
        }
        parser_expect(p, TOK_GT);
        return node;
    }

    ASTNode *node = ast_new(NODE_TYPE_BASIC, line, col);
    node->type_basic.name = name;
    return node;
}

// ---- Expression parsing (precedence climbing) ----

static ASTNode *parse_primary(Parser *p) {
    int line = p->current.line;
    int col = p->current.column;

    // Integer literal
    if (parser_match(p, TOK_INT_LIT)) {
        ASTNode *node = ast_new(NODE_INT_LIT, line, col);
        node->int_lit.value = p->previous.int_val;
        return node;
    }

    // Float literal
    if (parser_match(p, TOK_FLOAT_LIT)) {
        ASTNode *node = ast_new(NODE_FLOAT_LIT, line, col);
        node->float_lit.value = p->previous.float_val;
        return node;
    }

    // String literal
    if (parser_match(p, TOK_STRING_LIT)) {
        ASTNode *node = ast_new(NODE_STRING_LIT, line, col);
        // Strip quotes
        node->string_lit.value = str_dup_unescape(p->previous.start + 1, p->previous.length - 2);
        return node;
    }

    // Bool literals
    if (parser_match(p, TOK_TRUE)) {
        ASTNode *node = ast_new(NODE_BOOL_LIT, line, col);
        node->bool_lit.value = true;
        return node;
    }
    if (parser_match(p, TOK_FALSE)) {
        ASTNode *node = ast_new(NODE_BOOL_LIT, line, col);
        node->bool_lit.value = false;
        return node;
    }

    // None
    if (parser_match(p, TOK_NONE)) {
        return ast_new(NODE_NONE_LIT, line, col);
    }

    // self
    if (parser_match(p, TOK_SELF)) {
        ASTNode *node = ast_new(NODE_IDENT, line, col);
        node->ident.name = str_dup("self", 4);
        return node;
    }

    // Identifier (may be struct literal or just variable)
    if (parser_match(p, TOK_IDENT)) {
        char *name = token_to_str(&p->previous);

        // Enum variant: Name::Variant or Name::Variant(args)
        if (parser_check(p, TOK_COLONCOLON)) {
            parser_advance(p);
            parser_expect(p, TOK_IDENT);
            char *variant = token_to_str(&p->previous);

            ASTNode *node = ast_new(NODE_ENUM_VARIANT_EXPR, line, col);
            node->enum_variant_expr.enum_name = name;
            node->enum_variant_expr.variant_name = variant;
            node_list_init(&node->enum_variant_expr.args);

            if (parser_match(p, TOK_LPAREN)) {
                if (!parser_check(p, TOK_RPAREN)) {
                    node_list_push(&node->enum_variant_expr.args, parse_expression(p));
                    while (parser_match(p, TOK_COMMA)) {
                        node_list_push(&node->enum_variant_expr.args, parse_expression(p));
                    }
                }
                parser_expect(p, TOK_RPAREN);
            }
            return node;
        }

        // Check for struct literal: Name { field: value, ... }
        if (parser_check(p, TOK_LBRACE)) {
            // Peek further to check if it's Name { ident: ...
            // For now, we use a heuristic: if the identifier starts with uppercase
            if (name[0] >= 'A' && name[0] <= 'Z') {
                parser_advance(p); // consume '{'
                skip_newlines(p);
                ASTNode *node = ast_new(NODE_STRUCT_LIT, line, col);
                node->struct_lit.name = name;
                field_list_init(&node->struct_lit.field_inits);
                node_list_init(&node->struct_lit.values);

                while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
                    parser_expect(p, TOK_IDENT);
                    char *field_name = token_to_str(&p->previous);
                    parser_expect(p, TOK_COLON);

                    ASTNode *value = parse_expression(p);

                    Field f;
                    f.name = field_name;
                    f.type = NULL;
                    f.is_mut = false;
                    field_list_push(&node->struct_lit.field_inits, f);
                    node_list_push(&node->struct_lit.values, value);

                    if (!parser_match(p, TOK_COMMA)) {
                        skip_newlines(p);
                        break;
                    }
                    skip_newlines(p);
                }
                parser_expect(p, TOK_RBRACE);
                return node;
            }
        }

        ASTNode *node = ast_new(NODE_IDENT, line, col);
        node->ident.name = name;
        return node;
    }

    // Array literal [a, b, c]
    if (parser_match(p, TOK_LBRACKET)) {
        ASTNode *node = ast_new(NODE_ARRAY_LIT, line, col);
        node_list_init(&node->array_lit.elements);

        if (!parser_check(p, TOK_RBRACKET)) {
            node_list_push(&node->array_lit.elements, parse_expression(p));
            while (parser_match(p, TOK_COMMA)) {
                skip_newlines(p);
                if (parser_check(p, TOK_RBRACKET)) break;
                node_list_push(&node->array_lit.elements, parse_expression(p));
            }
        }
        parser_expect(p, TOK_RBRACKET);
        return node;
    }

    // Grouped expression (expr)
    if (parser_match(p, TOK_LPAREN)) {
        ASTNode *expr = parse_expression(p);
        parser_expect(p, TOK_RPAREN);
        return expr;
    }

    // Lambda: fn(params) -> ret { body }
    if (parser_match(p, TOK_FN)) {
        FieldList params = parse_params(p);
        ASTNode *ret_type = NULL;
        if (parser_match(p, TOK_ARROW)) {
            ret_type = parse_type(p);
        }
        ASTNode *body = parse_block(p);
        ASTNode *node = ast_new(NODE_LAMBDA, line, col);
        node->lambda.params = params;
        node->lambda.return_type = ret_type;
        node->lambda.body = body;
        node->lambda.id = -1; // assigned later
        return node;
    }

    error_at(p->filename, line, col, "expected expression, got '%s'",
             token_type_name(p->current.type));
    return NULL; // unreachable
}

static ASTNode *parse_postfix(Parser *p) {
    ASTNode *expr = parse_primary(p);

    for (;;) {
        int line = p->current.line;
        int col = p->current.column;

        // Function call: expr(args)
        if (parser_match(p, TOK_LPAREN)) {
            ASTNode *node = ast_new(NODE_CALL_EXPR, line, col);
            node->call.callee = expr;
            node_list_init(&node->call.args);

            if (!parser_check(p, TOK_RPAREN)) {
                node_list_push(&node->call.args, parse_expression(p));
                while (parser_match(p, TOK_COMMA)) {
                    skip_newlines(p);
                    node_list_push(&node->call.args, parse_expression(p));
                }
            }
            parser_expect(p, TOK_RPAREN);
            expr = node;
            continue;
        }

        // Field access / method call: expr.field or expr.method(args)
        if (parser_match(p, TOK_DOT)) {
            parser_expect(p, TOK_IDENT);
            char *name = token_to_str(&p->previous);

            // Method call
            if (parser_check(p, TOK_LPAREN)) {
                parser_advance(p);
                ASTNode *node = ast_new(NODE_METHOD_CALL, line, col);
                node->method_call.object = expr;
                node->method_call.method = name;
                node_list_init(&node->method_call.args);

                if (!parser_check(p, TOK_RPAREN)) {
                    node_list_push(&node->method_call.args, parse_expression(p));
                    while (parser_match(p, TOK_COMMA)) {
                        skip_newlines(p);
                        node_list_push(&node->method_call.args, parse_expression(p));
                    }
                }
                parser_expect(p, TOK_RPAREN);
                expr = node;
                continue;
            }

            // Field access
            ASTNode *node = ast_new(NODE_FIELD_ACCESS, line, col);
            node->field_access.object = expr;
            node->field_access.field = name;
            expr = node;
            continue;
        }

        // Index: expr[index]
        if (parser_match(p, TOK_LBRACKET)) {
            ASTNode *index = parse_expression(p);
            parser_expect(p, TOK_RBRACKET);

            ASTNode *node = ast_new(NODE_INDEX_EXPR, line, col);
            node->index_expr.object = expr;
            node->index_expr.index = index;
            expr = node;
            continue;
        }

        // Try operator: expr?
        if (parser_match(p, TOK_QUESTION)) {
            ASTNode *node = ast_new(NODE_TRY_EXPR, line, col);
            node->try_expr.operand = expr;
            expr = node;
            continue;
        }

        break;
    }

    return expr;
}

static ASTNode *parse_unary(Parser *p) {
    int line = p->current.line;
    int col = p->current.column;

    if (parser_match(p, TOK_MINUS) || parser_match(p, TOK_NOT) || parser_match(p, TOK_BANG)) {
        TokenType op = p->previous.type;
        ASTNode *operand = parse_unary(p);
        ASTNode *node = ast_new(NODE_UNARY_EXPR, line, col);
        node->unary.op = op;
        node->unary.operand = operand;
        return node;
    }

    // Reference: &expr or &mut expr
    if (parser_match(p, TOK_AMP)) {
        bool is_mut = parser_match(p, TOK_MUT);
        ASTNode *operand = parse_unary(p);
        ASTNode *node = ast_new(NODE_REF_EXPR, line, col);
        node->ref_expr.operand = operand;
        node->ref_expr.is_mut = is_mut;
        return node;
    }

    // Dereference: *expr
    if (parser_match(p, TOK_STAR)) {
        ASTNode *operand = parse_unary(p);
        ASTNode *node = ast_new(NODE_DEREF_EXPR, line, col);
        node->deref_expr.operand = operand;
        return node;
    }

    return parse_postfix(p);
}

static ASTNode *parse_multiplication(Parser *p) {
    ASTNode *left = parse_unary(p);

    while (parser_check(p, TOK_STAR) || parser_check(p, TOK_SLASH) || parser_check(p, TOK_PERCENT)) {
        int line = p->current.line;
        int col = p->current.column;
        parser_advance(p);
        TokenType op = p->previous.type;
        ASTNode *right = parse_unary(p);

        ASTNode *node = ast_new(NODE_BINARY_EXPR, line, col);
        node->binary.op = op;
        node->binary.left = left;
        node->binary.right = right;
        left = node;
    }

    return left;
}

static ASTNode *parse_addition(Parser *p) {
    ASTNode *left = parse_multiplication(p);

    while (parser_check(p, TOK_PLUS) || parser_check(p, TOK_MINUS)) {
        int line = p->current.line;
        int col = p->current.column;
        parser_advance(p);
        TokenType op = p->previous.type;
        ASTNode *right = parse_multiplication(p);

        ASTNode *node = ast_new(NODE_BINARY_EXPR, line, col);
        node->binary.op = op;
        node->binary.left = left;
        node->binary.right = right;
        left = node;
    }

    return left;
}

static ASTNode *parse_comparison(Parser *p) {
    ASTNode *left = parse_addition(p);

    if (parser_check(p, TOK_EQ) || parser_check(p, TOK_NEQ) ||
        parser_check(p, TOK_LT) || parser_check(p, TOK_GT) ||
        parser_check(p, TOK_LEQ) || parser_check(p, TOK_GEQ)) {
        int line = p->current.line;
        int col = p->current.column;
        parser_advance(p);
        TokenType op = p->previous.type;
        ASTNode *right = parse_addition(p);

        ASTNode *node = ast_new(NODE_BINARY_EXPR, line, col);
        node->binary.op = op;
        node->binary.left = left;
        node->binary.right = right;
        return node;
    }

    return left;
}

static ASTNode *parse_and(Parser *p) {
    ASTNode *left = parse_comparison(p);

    while (parser_match(p, TOK_AND)) {
        int line = p->previous.line;
        int col = p->previous.column;
        ASTNode *right = parse_comparison(p);

        ASTNode *node = ast_new(NODE_BINARY_EXPR, line, col);
        node->binary.op = TOK_AND;
        node->binary.left = left;
        node->binary.right = right;
        left = node;
    }

    return left;
}

static ASTNode *parse_or(Parser *p) {
    ASTNode *left = parse_and(p);

    while (parser_match(p, TOK_OR)) {
        int line = p->previous.line;
        int col = p->previous.column;
        ASTNode *right = parse_and(p);

        ASTNode *node = ast_new(NODE_BINARY_EXPR, line, col);
        node->binary.op = TOK_OR;
        node->binary.left = left;
        node->binary.right = right;
        left = node;
    }

    return left;
}

static ASTNode *parse_expression(Parser *p) {
    ASTNode *left = parse_or(p);

    // Pipe operator: expr |> fn becomes fn(expr)
    while (parser_match(p, TOK_PIPE_ARROW)) {
        int line = p->previous.line;
        int col = p->previous.column;
        ASTNode *right = parse_or(p);
        // Transform: left |> right into right(left)
        // If right is a call expr like f(a), make it f(a, left) — wait, simpler:
        // left |> f becomes f(left)
        if (right->type == NODE_IDENT) {
            ASTNode *call = ast_new(NODE_CALL_EXPR, line, col);
            call->call.callee = right;
            node_list_init(&call->call.args);
            call->call.mono_name = NULL;
            node_list_push(&call->call.args, left);
            left = call;
        } else if (right->type == NODE_CALL_EXPR) {
            // left |> f(a, b) becomes f(left, a, b)
            // Prepend left to args
            NodeList new_args;
            node_list_init(&new_args);
            node_list_push(&new_args, left);
            for (int i = 0; i < right->call.args.count; i++) {
                node_list_push(&new_args, right->call.args.items[i]);
            }
            right->call.args = new_args;
            left = right;
        } else {
            left = right; // fallback
        }
    }

    // Range operator: expr..expr
    if (parser_check(p, TOK_DOTDOT)) {
        int line = p->current.line;
        int col = p->current.column;
        parser_advance(p);
        ASTNode *right = parse_or(p);
        ASTNode *node = ast_new(NODE_BINARY_EXPR, line, col);
        node->binary.op = TOK_DOTDOT;
        node->binary.left = left;
        node->binary.right = right;
        return node;
    }

    return left;
}

// ---- Statement parsing ----

static ASTNode *parse_block(Parser *p) {
    int line = p->current.line;
    int col = p->current.column;
    parser_expect(p, TOK_LBRACE);
    skip_newlines(p);

    ASTNode *node = ast_new(NODE_BLOCK, line, col);
    node_list_init(&node->block.stmts);

    while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
        node_list_push(&node->block.stmts, parse_statement(p));
        skip_newlines(p);
    }

    parser_expect(p, TOK_RBRACE);
    return node;
}

static ASTNode *parse_let(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    bool is_mut = parser_match(p, TOK_MUT);

    parser_expect(p, TOK_IDENT);
    char *name = token_to_str(&p->previous);

    ASTNode *type_ann = NULL;
    if (parser_match(p, TOK_COLON)) {
        type_ann = parse_type(p);
    }

    ASTNode *init = NULL;
    if (parser_match(p, TOK_ASSIGN)) {
        init = parse_expression(p);
    }

    ASTNode *node = ast_new(NODE_LET_STMT, line, col);
    node->let_stmt.name = name;
    node->let_stmt.type_ann = type_ann;
    node->let_stmt.init = init;
    node->let_stmt.is_mut = is_mut;

    expect_terminator(p);
    return node;
}

static ASTNode *parse_return(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    ASTNode *value = NULL;
    if (!parser_check(p, TOK_NEWLINE) && !parser_check(p, TOK_RBRACE) &&
        !parser_check(p, TOK_EOF)) {
        value = parse_expression(p);
    }

    ASTNode *node = ast_new(NODE_RETURN_STMT, line, col);
    node->return_stmt.value = value;

    expect_terminator(p);
    return node;
}

static ASTNode *parse_if(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    ASTNode *condition = parse_expression(p);
    ASTNode *then_block = parse_block(p);

    ASTNode *else_block = NULL;
    skip_newlines(p);
    if (parser_match(p, TOK_ELSE)) {
        if (parser_check(p, TOK_IF)) {
            parser_advance(p);
            else_block = parse_if(p);
        } else {
            else_block = parse_block(p);
        }
    }

    ASTNode *node = ast_new(NODE_IF_STMT, line, col);
    node->if_stmt.condition = condition;
    node->if_stmt.then_block = then_block;
    node->if_stmt.else_block = else_block;
    return node;
}

static ASTNode *parse_while(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    ASTNode *condition = parse_expression(p);
    ASTNode *body = parse_block(p);

    ASTNode *node = ast_new(NODE_WHILE_STMT, line, col);
    node->while_stmt.condition = condition;
    node->while_stmt.body = body;
    return node;
}

static ASTNode *parse_for(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    parser_expect(p, TOK_IDENT);
    char *var_name = token_to_str(&p->previous);

    parser_expect(p, TOK_IN);

    ASTNode *iterable = parse_expression(p);
    ASTNode *body = parse_block(p);

    ASTNode *node = ast_new(NODE_FOR_STMT, line, col);
    node->for_stmt.var_name = var_name;
    node->for_stmt.iterable = iterable;
    node->for_stmt.body = body;
    return node;
}

static ASTNode *parse_match(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    ASTNode *subject = parse_expression(p);
    parser_expect(p, TOK_LBRACE);
    skip_newlines(p);

    ASTNode *node = ast_new(NODE_MATCH_EXPR, line, col);
    node->match_expr.subject = subject;
    node_list_init(&node->match_expr.arms);

    while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
        int arm_line = p->current.line;
        int arm_col = p->current.column;

        // Parse pattern: _ is wildcard, otherwise an expression
        ASTNode *pattern = NULL;
        if (parser_check(p, TOK_IDENT) && p->current.length == 1 && p->current.start[0] == '_') {
            parser_advance(p); // consume _
            pattern = NULL;    // wildcard
        } else {
            pattern = parse_expression(p);
        }

        parser_expect(p, TOK_FAT_ARROW);

        // Parse body: block or single expression
        ASTNode *body;
        if (parser_check(p, TOK_LBRACE)) {
            body = parse_block(p);
        } else {
            body = parse_expression(p);
        }

        ASTNode *arm = ast_new(NODE_MATCH_ARM, arm_line, arm_col);
        arm->match_arm.pattern = pattern;
        arm->match_arm.body = body;
        arm->match_arm.bind_names = NULL;
        arm->match_arm.bind_count = 0;

        // Extract binding names from enum variant pattern
        if (pattern && pattern->type == NODE_ENUM_VARIANT_EXPR) {
            int bc = pattern->enum_variant_expr.args.count;
            if (bc > 0) {
                arm->match_arm.bind_names = malloc(sizeof(char *) * bc);
                arm->match_arm.bind_count = bc;
                for (int bi = 0; bi < bc; bi++) {
                    ASTNode *arg = pattern->enum_variant_expr.args.items[bi];
                    if (arg->type == NODE_IDENT) {
                        arm->match_arm.bind_names[bi] = arg->ident.name;
                    } else {
                        arm->match_arm.bind_names[bi] = "_";
                    }
                }
            }
        }
        node_list_push(&node->match_expr.arms, arm);

        // Comma or newline separator
        parser_match(p, TOK_COMMA);
        skip_newlines(p);
    }

    parser_expect(p, TOK_RBRACE);
    return node;
}

static ASTNode *parse_statement(Parser *p) {
    skip_newlines(p);

    if (parser_match(p, TOK_LET)) return parse_let(p);
    if (parser_match(p, TOK_RETURN)) return parse_return(p);
    if (parser_match(p, TOK_IF)) return parse_if(p);
    if (parser_match(p, TOK_WHILE)) return parse_while(p);
    if (parser_match(p, TOK_FOR)) return parse_for(p);
    if (parser_match(p, TOK_MATCH)) return parse_match(p);
    if (parser_match(p, TOK_DEFER)) {
        int line = p->previous.line;
        int col = p->previous.column;
        ASTNode *stmt = parse_statement(p);
        ASTNode *node = ast_new(NODE_DEFER_STMT, line, col);
        node->defer_stmt.stmt = stmt;
        return node;
    }
    if (parser_match(p, TOK_BREAK)) {
        ASTNode *node = ast_new(NODE_BREAK_STMT, p->previous.line, p->previous.column);
        expect_terminator(p);
        return node;
    }
    if (parser_match(p, TOK_CONTINUE)) {
        ASTNode *node = ast_new(NODE_CONTINUE_STMT, p->previous.line, p->previous.column);
        expect_terminator(p);
        return node;
    }

    // Expression statement (may be assignment)
    int line = p->current.line;
    int col = p->current.column;
    ASTNode *expr = parse_expression(p);

    // Check for assignment
    if (parser_check(p, TOK_ASSIGN) || parser_check(p, TOK_PLUS_ASSIGN) ||
        parser_check(p, TOK_MINUS_ASSIGN) || parser_check(p, TOK_STAR_ASSIGN) ||
        parser_check(p, TOK_SLASH_ASSIGN)) {
        parser_advance(p);
        TokenType op = p->previous.type;
        ASTNode *value = parse_expression(p);

        ASTNode *node = ast_new(NODE_ASSIGN_STMT, line, col);
        node->assign_stmt.target = expr;
        node->assign_stmt.value = value;
        node->assign_stmt.op = op;

        expect_terminator(p);
        return node;
    }

    ASTNode *node = ast_new(NODE_EXPR_STMT, line, col);
    node->expr_stmt.expr = expr;
    expect_terminator(p);
    return node;
}

// ---- Declaration parsing ----

static FieldList parse_params(Parser *p) {
    FieldList params;
    field_list_init(&params);

    parser_expect(p, TOK_LPAREN);

    // Handle self parameter
    if (parser_check(p, TOK_SELF) || parser_check(p, TOK_AMP)) {
        // &self or &mut self or self
        Field self_param;
        self_param.is_mut = false;
        if (parser_match(p, TOK_AMP)) {
            self_param.is_mut = parser_match(p, TOK_MUT);
            parser_expect(p, TOK_SELF);
        } else {
            parser_expect(p, TOK_SELF);
        }
        self_param.name = str_dup("self", 4);
        self_param.type = NULL;
        field_list_push(&params, self_param);

        if (parser_match(p, TOK_COMMA)) {
            // More params follow
        } else {
            parser_expect(p, TOK_RPAREN);
            return params;
        }
    }

    if (!parser_check(p, TOK_RPAREN)) {
        do {
            Field param;
            param.is_mut = false;

            parser_expect(p, TOK_IDENT);
            param.name = token_to_str(&p->previous);

            parser_expect(p, TOK_COLON);
            param.type = parse_type(p);

            field_list_push(&params, param);
        } while (parser_match(p, TOK_COMMA));
    }

    parser_expect(p, TOK_RPAREN);
    return params;
}

static ASTNode *parse_fn_decl(Parser *p, bool is_pub) {
    int line = p->previous.line;
    int col = p->previous.column;

    parser_expect(p, TOK_IDENT);
    char *name = token_to_str(&p->previous);

    // Parse optional type parameters: <T, U>
    NodeList fn_type_params;
    node_list_init(&fn_type_params);
    if (parser_check(p, TOK_LT)) {
        parser_advance(p);
        do {
            parser_expect(p, TOK_IDENT);
            ASTNode *tp = ast_new(NODE_TYPE_BASIC, p->previous.line, p->previous.column);
            tp->type_basic.name = token_to_str(&p->previous);
            node_list_push(&fn_type_params, tp);
        } while (parser_match(p, TOK_COMMA));
        parser_expect(p, TOK_GT);
    }

    FieldList params = parse_params(p);

    ASTNode *return_type = NULL;
    if (parser_match(p, TOK_ARROW)) {
        return_type = parse_type(p);
    }

    // Parse contract/example/panics/recover clauses between signature and body
    NodeList examples, panics_list, requires_list, ensures_list;
    node_list_init(&examples);
    node_list_init(&panics_list);
    node_list_init(&requires_list);
    node_list_init(&ensures_list);
    ASTNode *recover_block = NULL;

    skip_newlines(p);
    while (parser_check(p, TOK_EXAMPLE) || parser_check(p, TOK_PANICS) ||
           parser_check(p, TOK_REQUIRES) || parser_check(p, TOK_ENSURES) ||
           parser_check(p, TOK_RECOVER)) {
        if (parser_match(p, TOK_EXAMPLE)) {
            node_list_push(&examples, parse_expression(p));
        } else if (parser_match(p, TOK_PANICS)) {
            node_list_push(&panics_list, parse_expression(p));
        } else if (parser_match(p, TOK_REQUIRES)) {
            node_list_push(&requires_list, parse_expression(p));
        } else if (parser_match(p, TOK_ENSURES)) {
            node_list_push(&ensures_list, parse_expression(p));
        } else if (parser_match(p, TOK_RECOVER)) {
            recover_block = parse_block(p);
        }
        skip_newlines(p);
    }

    ASTNode *body = parse_block(p);

    ASTNode *node = ast_new(NODE_FN_DECL, line, col);
    node->fn_decl.name = name;
    node->fn_decl.params = params;
    node->fn_decl.return_type = return_type;
    node->fn_decl.body = body;
    node->fn_decl.is_pub = is_pub;
    node->fn_decl.is_method = false;
    node->fn_decl.has_self = false;
    node->fn_decl.self_is_mut = false;
    node->fn_decl.type_params = fn_type_params;
    node->fn_decl.examples = examples;
    node->fn_decl.panics = panics_list;
    node->fn_decl.requires = requires_list;
    node->fn_decl.ensures = ensures_list;
    node->fn_decl.recover_block = recover_block;

    // Check if first param is self
    if (params.count > 0 && strcmp(params.items[0].name, "self") == 0) {
        node->fn_decl.has_self = true;
        node->fn_decl.self_is_mut = params.items[0].is_mut;
    }

    return node;
}

static ASTNode *parse_struct_decl(Parser *p, bool is_pub) {
    int line = p->previous.line;
    int col = p->previous.column;

    parser_expect(p, TOK_IDENT);
    char *name = token_to_str(&p->previous);

    // Parse optional type parameters: <A, B>
    NodeList struct_type_params;
    node_list_init(&struct_type_params);
    if (parser_check(p, TOK_LT)) {
        parser_advance(p);
        do {
            parser_expect(p, TOK_IDENT);
            ASTNode *tp = ast_new(NODE_TYPE_BASIC, p->previous.line, p->previous.column);
            tp->type_basic.name = token_to_str(&p->previous);
            node_list_push(&struct_type_params, tp);
        } while (parser_match(p, TOK_COMMA));
        parser_expect(p, TOK_GT);
    }

    parser_expect(p, TOK_LBRACE);
    skip_newlines(p);

    FieldList fields;
    field_list_init(&fields);
    NodeList invariants;
    node_list_init(&invariants);

    while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
        if (parser_match(p, TOK_INVARIANT)) {
            node_list_push(&invariants, parse_expression(p));
            parser_match(p, TOK_COMMA);
            skip_newlines(p);
            continue;
        }

        Field field;
        field.is_mut = false;

        parser_expect(p, TOK_IDENT);
        field.name = token_to_str(&p->previous);

        parser_expect(p, TOK_COLON);
        field.type = parse_type(p);

        field_list_push(&fields, field);

        parser_match(p, TOK_COMMA);
        skip_newlines(p);
    }

    parser_expect(p, TOK_RBRACE);

    ASTNode *node = ast_new(NODE_STRUCT_DECL, line, col);
    node->struct_decl.name = name;
    node->struct_decl.invariants = invariants;
    node->struct_decl.type_params = struct_type_params;
    node->struct_decl.fields = fields;
    node->struct_decl.is_pub = is_pub;
    return node;
}

static ASTNode *parse_enum_decl(Parser *p, bool is_pub) {
    int line = p->previous.line;
    int col = p->previous.column;

    parser_expect(p, TOK_IDENT);
    char *name = token_to_str(&p->previous);

    parser_expect(p, TOK_LBRACE);
    skip_newlines(p);

    ASTNode *node = ast_new(NODE_ENUM_DECL, line, col);
    node->enum_decl.name = name;
    node_list_init(&node->enum_decl.variants);
    node->enum_decl.is_pub = is_pub;

    while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
        parser_expect(p, TOK_IDENT);
        char *vname = token_to_str(&p->previous);
        int vline = p->previous.line;
        int vcol = p->previous.column;

        // Check for associated data: Variant(Type, Type)
        if (parser_check(p, TOK_LPAREN)) {
            parser_advance(p);
            ASTNode *vdef = ast_new(NODE_ENUM_VARIANT_DEF, vline, vcol);
            vdef->enum_variant_def.name = vname;
            node_list_init(&vdef->enum_variant_def.field_types);
            if (!parser_check(p, TOK_RPAREN)) {
                node_list_push(&vdef->enum_variant_def.field_types, parse_type(p));
                while (parser_match(p, TOK_COMMA)) {
                    node_list_push(&vdef->enum_variant_def.field_types, parse_type(p));
                }
            }
            parser_expect(p, TOK_RPAREN);
            node_list_push(&node->enum_decl.variants, vdef);
        } else {
            // Simple variant (no data)
            ASTNode *variant = ast_new(NODE_IDENT, vline, vcol);
            variant->ident.name = vname;
            node_list_push(&node->enum_decl.variants, variant);
        }

        if (!parser_match(p, TOK_COMMA)) {
            skip_newlines(p);
        } else {
            skip_newlines(p);
        }
    }

    parser_expect(p, TOK_RBRACE);
    return node;
}

static ASTNode *parse_impl_block(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    parser_expect(p, TOK_IDENT);
    char *first_name = token_to_str(&p->previous);

    // Check for "impl Trait for Type"
    if (parser_match(p, TOK_FOR)) {
        parser_expect(p, TOK_IDENT);
        char *type_name = token_to_str(&p->previous);

        parser_expect(p, TOK_LBRACE);
        skip_newlines(p);

        ASTNode *node = ast_new(NODE_IMPL_TRAIT, line, col);
        node->impl_trait.trait_name = first_name;
        node->impl_trait.type_name = type_name;
        node_list_init(&node->impl_trait.methods);

        while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
            skip_newlines(p);
            bool is_pub = parser_match(p, TOK_PUB);
            parser_expect(p, TOK_FN);
            ASTNode *method = parse_fn_decl(p, is_pub);
            method->fn_decl.is_method = true;
            node_list_push(&node->impl_trait.methods, method);
            skip_newlines(p);
        }
        parser_expect(p, TOK_RBRACE);
        return node;
    }

    parser_expect(p, TOK_LBRACE);
    skip_newlines(p);

    ASTNode *node = ast_new(NODE_IMPL_BLOCK, line, col);
    node->impl_block.type_name = first_name;
    node_list_init(&node->impl_block.methods);

    while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
        skip_newlines(p);
        bool is_pub = parser_match(p, TOK_PUB);
        parser_expect(p, TOK_FN);

        ASTNode *method = parse_fn_decl(p, is_pub);
        method->fn_decl.is_method = true;
        node_list_push(&node->impl_block.methods, method);

        skip_newlines(p);
    }

    parser_expect(p, TOK_RBRACE);
    return node;
}

static ASTNode *parse_declaration(Parser *p) {
    skip_newlines(p);

    bool is_pub = parser_match(p, TOK_PUB);

    if (parser_match(p, TOK_FN)) return parse_fn_decl(p, is_pub);
    if (parser_match(p, TOK_STRUCT)) return parse_struct_decl(p, is_pub);
    if (parser_match(p, TOK_ENUM)) return parse_enum_decl(p, is_pub);
    if (parser_match(p, TOK_TRAIT)) {
        int tline = p->previous.line;
        int tcol = p->previous.column;
        parser_expect(p, TOK_IDENT);
        char *tname = token_to_str(&p->previous);
        parser_expect(p, TOK_LBRACE);
        skip_newlines(p);

        ASTNode *tnode = ast_new(NODE_TRAIT_DECL, tline, tcol);
        tnode->trait_decl.name = tname;
        node_list_init(&tnode->trait_decl.methods);

        while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
            skip_newlines(p);
            if (parser_check(p, TOK_RBRACE)) break;
            parser_expect(p, TOK_FN);
            parser_expect(p, TOK_IDENT);
            char *mname = token_to_str(&p->previous);
            int mline = p->previous.line;
            int mcol = p->previous.column;
            FieldList mparams = parse_params(p);
            ASTNode *mret = NULL;
            if (parser_match(p, TOK_ARROW)) mret = parse_type(p);

            ASTNode *sig = ast_new(NODE_FN_DECL, mline, mcol);
            sig->fn_decl.name = mname;
            sig->fn_decl.params = mparams;
            sig->fn_decl.return_type = mret;
            sig->fn_decl.body = NULL;
            node_list_init(&sig->fn_decl.type_params);
            node_list_init(&sig->fn_decl.examples);
            node_list_init(&sig->fn_decl.panics);
            node_list_init(&sig->fn_decl.requires);
            node_list_init(&sig->fn_decl.ensures);
            node_list_push(&tnode->trait_decl.methods, sig);
            skip_newlines(p);
        }
        parser_expect(p, TOK_RBRACE);
        return tnode;
    }
    if (parser_match(p, TOK_LET)) return parse_let(p); // global variable
    if (parser_match(p, TOK_IMPL)) {
        if (is_pub) {
            error_at(p->filename, p->previous.line, p->previous.column,
                     "impl blocks cannot be pub");
        }
        return parse_impl_block(p);
    }
    if (parser_match(p, TOK_IMPORT)) {
        int line = p->previous.line;
        int col = p->previous.column;
        parser_expect(p, TOK_STRING_LIT);
        char *path = str_dup(p->previous.start + 1, p->previous.length - 2);
        ASTNode *node = ast_new(NODE_IMPORT, line, col);
        node->import_decl.path = path;
        expect_terminator(p);
        return node;
    }
    if (parser_match(p, TOK_TEST)) {
        int line = p->previous.line;
        int col = p->previous.column;
        parser_expect(p, TOK_STRING_LIT);
        char *name = str_dup(p->previous.start + 1, p->previous.length - 2);
        ASTNode *body = parse_block(p);
        ASTNode *node = ast_new(NODE_TEST_DECL, line, col);
        node->test_decl.name = name;
        node->test_decl.body = body;
        return node;
    }

    error_at(p->filename, p->current.line, p->current.column,
             "expected declaration (fn, struct, enum, let, impl, test), got '%s'",
             token_type_name(p->current.type));
    return NULL;
}

// ---- Public API ----

void parser_init(Parser *parser, Lexer *lexer, const char *filename) {
    parser->lexer = lexer;
    parser->filename = filename;
    parser->current = lexer_next(lexer);
    parser->previous = parser->current;
}

ASTNode *parser_parse(Parser *parser) {
    ASTNode *program = ast_new(NODE_PROGRAM, 1, 1);
    node_list_init(&program->program.decls);

    skip_newlines(parser);

    while (!parser_check(parser, TOK_EOF)) {
        node_list_push(&program->program.decls, parse_declaration(parser));
        skip_newlines(parser);
    }

    return program;
}
