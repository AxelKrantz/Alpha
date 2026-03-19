#include "lexer.h"
#include "error.h"
#include <string.h>
#include <ctype.h>

void lexer_init(Lexer *lexer, const char *source, const char *filename) {
    lexer->source = source;
    lexer->current = source;
    lexer->filename = filename;
    lexer->line = 1;
    lexer->column = 1;
    lexer->paren_depth = 0;
    lexer->last_type = TOK_EOF;
}

static char peek(Lexer *lexer) {
    return *lexer->current;
}

static char peek_next(Lexer *lexer) {
    if (*lexer->current == '\0') return '\0';
    return lexer->current[1];
}

static char advance(Lexer *lexer) {
    char c = *lexer->current;
    lexer->current++;
    lexer->column++;
    return c;
}

static bool match(Lexer *lexer, char expected) {
    if (*lexer->current == expected) {
        advance(lexer);
        return true;
    }
    return false;
}

static Token make_token(Lexer *lexer, TokenType type, const char *start, int line, int col) {
    Token tok;
    tok.type = type;
    tok.start = start;
    tok.length = (int)(lexer->current - start);
    tok.line = line;
    tok.column = col;
    tok.int_val = 0;
    return tok;
}

static Token error_token(Lexer *lexer, const char *msg) {
    Token tok;
    tok.type = TOK_ERROR;
    tok.start = msg;
    tok.length = (int)strlen(msg);
    tok.line = lexer->line;
    tok.column = lexer->column;
    tok.int_val = 0;
    return tok;
}

static void skip_whitespace(Lexer *lexer) {
    for (;;) {
        char c = peek(lexer);
        if (c == ' ' || c == '\t' || c == '\r') {
            advance(lexer);
        } else if (c == '/' && peek_next(lexer) == '/') {
            // Line comment - consume until end of line
            while (peek(lexer) != '\n' && peek(lexer) != '\0') {
                advance(lexer);
            }
        } else {
            return;
        }
    }
}

typedef struct {
    const char *keyword;
    TokenType type;
} Keyword;

static const Keyword keywords[] = {
    {"fn",       TOK_FN},
    {"let",      TOK_LET},
    {"mut",      TOK_MUT},
    {"return",   TOK_RETURN},
    {"if",       TOK_IF},
    {"else",     TOK_ELSE},
    {"match",    TOK_MATCH},
    {"struct",   TOK_STRUCT},
    {"impl",     TOK_IMPL},
    {"trait",    TOK_TRAIT},
    {"enum",     TOK_ENUM},
    {"while",    TOK_WHILE},
    {"for",      TOK_FOR},
    {"in",       TOK_IN},
    {"break",    TOK_BREAK},
    {"continue", TOK_CONTINUE},
    {"import",   TOK_IMPORT},
    {"pub",      TOK_PUB},
    {"self",     TOK_SELF},
    {"defer",    TOK_DEFER},
    {"test",     TOK_TEST},
    {"example",  TOK_EXAMPLE},
    {"requires", TOK_REQUIRES},
    {"ensures",  TOK_ENSURES},
    {"invariant",TOK_INVARIANT},
    {"panics",   TOK_PANICS},
    {"recover",  TOK_RECOVER},
    {"true",     TOK_TRUE},
    {"false",    TOK_FALSE},
    {"None",     TOK_NONE},
    {"none",     TOK_NONE},
    {"and",      TOK_AND},
    {"or",       TOK_OR},
    {"not",      TOK_NOT},
    {"type",     TOK_TYPE},
    {NULL,       TOK_ERROR},
};

static TokenType check_keyword(const char *start, int length) {
    for (int i = 0; keywords[i].keyword != NULL; i++) {
        int kw_len = (int)strlen(keywords[i].keyword);
        if (kw_len == length && memcmp(start, keywords[i].keyword, length) == 0) {
            return keywords[i].type;
        }
    }
    return TOK_IDENT;
}

static Token lex_identifier(Lexer *lexer) {
    const char *start = lexer->current - 1;
    int line = lexer->line;
    int col = lexer->column - 1;

    while (isalnum(peek(lexer)) || peek(lexer) == '_') {
        advance(lexer);
    }

    int length = (int)(lexer->current - start);
    TokenType type = check_keyword(start, length);

    return make_token(lexer, type, start, line, col);
}

static Token lex_number(Lexer *lexer) {
    const char *start = lexer->current - 1;
    int line = lexer->line;
    int col = lexer->column - 1;
    bool is_float = false;

    while (isdigit(peek(lexer)) || peek(lexer) == '_') {
        advance(lexer);
    }

    if (peek(lexer) == '.' && isdigit(peek_next(lexer))) {
        is_float = true;
        advance(lexer); // consume '.'
        while (isdigit(peek(lexer)) || peek(lexer) == '_') {
            advance(lexer);
        }
    }

    Token tok = make_token(lexer, is_float ? TOK_FLOAT_LIT : TOK_INT_LIT, start, line, col);

    if (is_float) {
        tok.float_val = strtod(start, NULL);
    } else {
        tok.int_val = strtoll(start, NULL, 10);
    }

    return tok;
}

static Token lex_string(Lexer *lexer) {
    const char *start = lexer->current - 1;
    int line = lexer->line;
    int col = lexer->column - 1;

    while (peek(lexer) != '"' && peek(lexer) != '\0') {
        if (peek(lexer) == '\n') {
            lexer->line++;
            lexer->column = 0;
        }
        if (peek(lexer) == '\\') {
            advance(lexer); // skip escape character
        }
        advance(lexer);
    }

    if (peek(lexer) == '\0') {
        return error_token(lexer, "unterminated string");
    }

    advance(lexer); // closing quote
    return make_token(lexer, TOK_STRING_LIT, start, line, col);
}

static Token lexer_next_raw(Lexer *lexer);

Token lexer_next(Lexer *lexer) {
    Token tok = lexer_next_raw(lexer);
    lexer->last_type = tok.type;
    return tok;
}

static Token lexer_next_raw(Lexer *lexer) {
    skip_whitespace(lexer);

    if (*lexer->current == '\0') {
        return make_token(lexer, TOK_EOF, lexer->current, lexer->line, lexer->column);
    }

    int line = lexer->line;
    int col = lexer->column;
    char c = advance(lexer);

    // Newlines
    if (c == '\n') {
        lexer->line++;
        lexer->column = 1;

        // Skip consecutive newlines
        while (peek(lexer) == '\n' || peek(lexer) == '\r' ||
               peek(lexer) == ' ' || peek(lexer) == '\t') {
            if (peek(lexer) == '\n') {
                lexer->line++;
                lexer->column = 0;
            }
            advance(lexer);
        }

        // Suppress newlines inside grouping
        if (lexer->paren_depth > 0) {
            return lexer_next(lexer);
        }

        // Line continuation: suppress newlines after binary operators and commas
        switch (lexer->last_type) {
            case TOK_PLUS: case TOK_MINUS: case TOK_STAR: case TOK_SLASH:
            case TOK_PERCENT: case TOK_ASSIGN: case TOK_EQ: case TOK_NEQ:
            case TOK_LT: case TOK_GT: case TOK_LEQ: case TOK_GEQ:
            case TOK_AND: case TOK_OR: case TOK_COMMA: case TOK_DOT:
            case TOK_AMP: case TOK_PIPE: case TOK_PIPE_ARROW: case TOK_ARROW: case TOK_FAT_ARROW:
            case TOK_PLUS_ASSIGN: case TOK_MINUS_ASSIGN:
            case TOK_STAR_ASSIGN: case TOK_SLASH_ASSIGN:
            case TOK_COLON:
                return lexer_next(lexer);
            default:
                break;
        }

        Token tok;
        tok.type = TOK_NEWLINE;
        tok.start = "\n";
        tok.length = 1;
        tok.line = line;
        tok.column = col;
        tok.int_val = 0;
        return tok;
    }

    // Identifiers and keywords
    if (isalpha(c) || c == '_') {
        return lex_identifier(lexer);
    }

    // Numbers
    if (isdigit(c)) {
        return lex_number(lexer);
    }

    // Strings (including triple-quoted multi-line)
    if (c == '"') {
        // Check for triple-quote """
        if (peek(lexer) == '"' && peek_next(lexer) == '"') {
            advance(lexer); advance(lexer); // consume the other two quotes
            const char *content_start = lexer->current;
            int sline = lexer->line;
            int scol = lexer->column;
            // Read until """
            while (lexer->current[0] != '\0') {
                if (lexer->current[0] == '"' && lexer->current[1] == '"' && lexer->current[2] == '"') {
                    Token tok;
                    tok.type = TOK_STRING_LIT;
                    tok.start = content_start - 1; // -1 because parser does start+1
                    tok.length = (int)(lexer->current - content_start) + 2; // +2 because parser does length-2
                    tok.line = sline;
                    tok.column = scol;
                    tok.int_val = 0;
                    advance(lexer); advance(lexer); advance(lexer); // consume closing """
                    return tok;
                }
                if (*lexer->current == '\n') { lexer->line++; lexer->column = 0; }
                advance(lexer);
            }
            return error_token(lexer, "unterminated triple-quoted string");
        }
        return lex_string(lexer);
    }

    const char *start = lexer->current - 1;

    // Operators and punctuation
    switch (c) {
        case '+':
            if (match(lexer, '=')) return make_token(lexer, TOK_PLUS_ASSIGN, start, line, col);
            return make_token(lexer, TOK_PLUS, start, line, col);
        case '-':
            if (match(lexer, '>')) return make_token(lexer, TOK_ARROW, start, line, col);
            if (match(lexer, '=')) return make_token(lexer, TOK_MINUS_ASSIGN, start, line, col);
            return make_token(lexer, TOK_MINUS, start, line, col);
        case '*':
            if (match(lexer, '=')) return make_token(lexer, TOK_STAR_ASSIGN, start, line, col);
            return make_token(lexer, TOK_STAR, start, line, col);
        case '/':
            if (match(lexer, '=')) return make_token(lexer, TOK_SLASH_ASSIGN, start, line, col);
            return make_token(lexer, TOK_SLASH, start, line, col);
        case '%':
            return make_token(lexer, TOK_PERCENT, start, line, col);
        case '=':
            if (match(lexer, '=')) return make_token(lexer, TOK_EQ, start, line, col);
            if (match(lexer, '>')) return make_token(lexer, TOK_FAT_ARROW, start, line, col);
            return make_token(lexer, TOK_ASSIGN, start, line, col);
        case '?':
            return make_token(lexer, TOK_QUESTION, start, line, col);
        case '!':
            if (match(lexer, '=')) return make_token(lexer, TOK_NEQ, start, line, col);
            return make_token(lexer, TOK_BANG, start, line, col);
        case '<':
            if (match(lexer, '=')) return make_token(lexer, TOK_LEQ, start, line, col);
            return make_token(lexer, TOK_LT, start, line, col);
        case '>':
            if (match(lexer, '=')) return make_token(lexer, TOK_GEQ, start, line, col);
            return make_token(lexer, TOK_GT, start, line, col);
        case '&':
            return make_token(lexer, TOK_AMP, start, line, col);
        case '|':
            if (match(lexer, '>')) return make_token(lexer, TOK_PIPE_ARROW, start, line, col);
            return make_token(lexer, TOK_PIPE, start, line, col);
        case '.':
            if (match(lexer, '.')) return make_token(lexer, TOK_DOTDOT, start, line, col);
            return make_token(lexer, TOK_DOT, start, line, col);
        case ',':
            return make_token(lexer, TOK_COMMA, start, line, col);
        case ':':
            if (match(lexer, ':')) return make_token(lexer, TOK_COLONCOLON, start, line, col);
            return make_token(lexer, TOK_COLON, start, line, col);
        case '(':
            lexer->paren_depth++;
            return make_token(lexer, TOK_LPAREN, start, line, col);
        case ')':
            lexer->paren_depth--;
            return make_token(lexer, TOK_RPAREN, start, line, col);
        case '{':
            return make_token(lexer, TOK_LBRACE, start, line, col);
        case '}':
            return make_token(lexer, TOK_RBRACE, start, line, col);
        case '[':
            lexer->paren_depth++;
            return make_token(lexer, TOK_LBRACKET, start, line, col);
        case ']':
            lexer->paren_depth--;
            return make_token(lexer, TOK_RBRACKET, start, line, col);
    }

    return error_token(lexer, "unexpected character");
}

Token lexer_peek(Lexer *lexer) {
    // Save state
    const char *saved_current = lexer->current;
    int saved_line = lexer->line;
    int saved_column = lexer->column;
    int saved_depth = lexer->paren_depth;

    Token tok = lexer_next(lexer);

    // Restore state
    lexer->current = saved_current;
    lexer->line = saved_line;
    lexer->column = saved_column;
    lexer->paren_depth = saved_depth;

    return tok;
}

const char *token_type_name(TokenType type) {
    switch (type) {
        case TOK_INT_LIT: return "integer";
        case TOK_FLOAT_LIT: return "float";
        case TOK_STRING_LIT: return "string";
        case TOK_IDENT: return "identifier";
        case TOK_FN: return "fn";
        case TOK_LET: return "let";
        case TOK_MUT: return "mut";
        case TOK_RETURN: return "return";
        case TOK_IF: return "if";
        case TOK_ELSE: return "else";
        case TOK_MATCH: return "match";
        case TOK_STRUCT: return "struct";
        case TOK_IMPL: return "impl";
        case TOK_TRAIT: return "trait";
        case TOK_ENUM: return "enum";
        case TOK_WHILE: return "while";
        case TOK_FOR: return "for";
        case TOK_IN: return "in";
        case TOK_BREAK: return "break";
        case TOK_CONTINUE: return "continue";
        case TOK_IMPORT: return "import";
        case TOK_PUB: return "pub";
        case TOK_SELF: return "self";
        case TOK_DEFER: return "defer";
        case TOK_TEST: return "test";
        case TOK_EXAMPLE: return "example";
        case TOK_REQUIRES: return "requires";
        case TOK_ENSURES: return "ensures";
        case TOK_INVARIANT: return "invariant";
        case TOK_PANICS: return "panics";
        case TOK_RECOVER: return "recover";
        case TOK_RESULT: return "result";
        case TOK_TRUE: return "true";
        case TOK_FALSE: return "false";
        case TOK_NONE: return "None";
        case TOK_AND: return "and";
        case TOK_OR: return "or";
        case TOK_NOT: return "not";
        case TOK_TYPE: return "type";
        case TOK_PLUS: return "+";
        case TOK_MINUS: return "-";
        case TOK_STAR: return "*";
        case TOK_SLASH: return "/";
        case TOK_PERCENT: return "%";
        case TOK_ASSIGN: return "=";
        case TOK_EQ: return "==";
        case TOK_NEQ: return "!=";
        case TOK_LT: return "<";
        case TOK_GT: return ">";
        case TOK_LEQ: return "<=";
        case TOK_GEQ: return ">=";
        case TOK_AMP: return "&";
        case TOK_PIPE: return "|";
        case TOK_PIPE_ARROW: return "|>";
        case TOK_ARROW: return "->";
        case TOK_FAT_ARROW: return "=>";
        case TOK_DOT: return ".";
        case TOK_DOTDOT: return "..";
        case TOK_BANG: return "!";
        case TOK_QUESTION: return "?";
        case TOK_PLUS_ASSIGN: return "+=";
        case TOK_MINUS_ASSIGN: return "-=";
        case TOK_STAR_ASSIGN: return "*=";
        case TOK_SLASH_ASSIGN: return "/=";
        case TOK_LPAREN: return "(";
        case TOK_RPAREN: return ")";
        case TOK_LBRACE: return "{";
        case TOK_RBRACE: return "}";
        case TOK_LBRACKET: return "[";
        case TOK_RBRACKET: return "]";
        case TOK_COMMA: return ",";
        case TOK_COLON: return ":";
        case TOK_COLONCOLON: return "::";
        case TOK_NEWLINE: return "newline";
        case TOK_EOF: return "EOF";
        case TOK_ERROR: return "error";
    }
    return "unknown";
}
