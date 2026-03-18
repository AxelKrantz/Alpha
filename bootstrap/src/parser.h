#ifndef ALPHA_PARSER_H
#define ALPHA_PARSER_H

#include "lexer.h"
#include "ast.h"

typedef struct {
    Lexer *lexer;
    Token current;
    Token previous;
    const char *filename;
    bool had_error;
} Parser;

void parser_init(Parser *parser, Lexer *lexer, const char *filename);
ASTNode *parser_parse(Parser *parser);

#endif
