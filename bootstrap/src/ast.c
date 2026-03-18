#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ASTNode *ast_new(NodeType type, int line, int col) {
    ASTNode *node = calloc(1, sizeof(ASTNode));
    if (!node) {
        fprintf(stderr, "fatal: out of memory\n");
        exit(1);
    }
    node->type = type;
    node->line = line;
    node->column = col;
    return node;
}

void node_list_init(NodeList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void node_list_push(NodeList *list, ASTNode *node) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        list->items = realloc(list->items, sizeof(ASTNode *) * list->capacity);
        if (!list->items) {
            fprintf(stderr, "fatal: out of memory\n");
            exit(1);
        }
    }
    list->items[list->count++] = node;
}

void field_list_init(FieldList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void field_list_push(FieldList *list, Field field) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        list->items = realloc(list->items, sizeof(Field) * list->capacity);
        if (!list->items) {
            fprintf(stderr, "fatal: out of memory\n");
            exit(1);
        }
    }
    list->items[list->count++] = field;
}

char *str_dup(const char *s, int len) {
    char *dup = malloc(len + 1);
    if (!dup) {
        fprintf(stderr, "fatal: out of memory\n");
        exit(1);
    }
    memcpy(dup, s, len);
    dup[len] = '\0';
    return dup;
}

// Duplicate string with escape sequence processing (\n, \t, \r, \\, \", \0)
char *str_dup_unescape(const char *s, int len) {
    char *buf = malloc(len + 1);
    if (!buf) { fprintf(stderr, "fatal: out of memory\n"); exit(1); }
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] == '\\' && i + 1 < len) {
            i++;
            switch (s[i]) {
                case 'n':  buf[j++] = '\n'; break;
                case 't':  buf[j++] = '\t'; break;
                case 'r':  buf[j++] = '\r'; break;
                case '\\': buf[j++] = '\\'; break;
                case '"':  buf[j++] = '"';  break;
                case '0':  buf[j++] = '\0'; break;
                default:   buf[j++] = '\\'; buf[j++] = s[i]; break;
            }
        } else {
            buf[j++] = s[i];
        }
    }
    buf[j] = '\0';
    return buf;
}
