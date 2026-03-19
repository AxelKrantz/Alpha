#ifndef ALPHA_LEXER_H
#define ALPHA_LEXER_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    // Literals
    TOK_INT_LIT,
    TOK_FLOAT_LIT,
    TOK_STRING_LIT,

    // Identifier
    TOK_IDENT,

    // Keywords
    TOK_FN,
    TOK_LET,
    TOK_MUT,
    TOK_RETURN,
    TOK_IF,
    TOK_ELSE,
    TOK_MATCH,
    TOK_STRUCT,
    TOK_IMPL,
    TOK_TRAIT,
    TOK_ENUM,
    TOK_WHILE,
    TOK_FOR,
    TOK_IN,
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_IMPORT,
    TOK_PUB,
    TOK_SELF,
    TOK_TRUE,
    TOK_FALSE,
    TOK_NONE,
    TOK_DEFER,
    TOK_TEST,
    TOK_EXAMPLE,
    TOK_REQUIRES,
    TOK_ENSURES,
    TOK_INVARIANT,
    TOK_PANICS,
    TOK_RECOVER,
    TOK_RESULT,
    TOK_AND,
    TOK_OR,
    TOK_NOT,
    TOK_TYPE,

    // Operators
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_ASSIGN,
    TOK_EQ,
    TOK_NEQ,
    TOK_LT,
    TOK_GT,
    TOK_LEQ,
    TOK_GEQ,
    TOK_AMP,
    TOK_PIPE,
    TOK_ARROW,
    TOK_FAT_ARROW,
    TOK_DOT,
    TOK_DOTDOT,
    TOK_BANG,
    TOK_QUESTION,
    TOK_PLUS_ASSIGN,
    TOK_MINUS_ASSIGN,
    TOK_STAR_ASSIGN,
    TOK_SLASH_ASSIGN,

    // Delimiters
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_COMMA,
    TOK_COLON,
    TOK_COLONCOLON,

    // Special
    TOK_NEWLINE,
    TOK_EOF,
    TOK_ERROR,
} TokenType;

typedef struct {
    TokenType type;
    const char *start;
    int length;
    int line;
    int column;
    union {
        int64_t int_val;
        double float_val;
    };
} Token;

typedef struct {
    const char *source;
    const char *current;
    const char *filename;
    int line;
    int column;
    int paren_depth;   // tracks () [] {} nesting to suppress newlines
    TokenType last_type; // for line continuation after operators
} Lexer;

void lexer_init(Lexer *lexer, const char *source, const char *filename);
Token lexer_next(Lexer *lexer);
Token lexer_peek(Lexer *lexer);
const char *token_type_name(TokenType type);

#endif
