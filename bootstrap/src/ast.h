#ifndef ALPHA_AST_H
#define ALPHA_AST_H

#include "lexer.h"
#include <stdint.h>
#include <stdbool.h>

// Forward declare Type from types.h
typedef struct Type Type;

typedef enum {
    // Top-level
    NODE_PROGRAM,
    NODE_FN_DECL,
    NODE_STRUCT_DECL,
    NODE_ENUM_DECL,
    NODE_IMPL_BLOCK,
    NODE_TEST_DECL,
    NODE_IMPORT,

    // Statements
    NODE_LET_STMT,
    NODE_RETURN_STMT,
    NODE_EXPR_STMT,
    NODE_IF_STMT,
    NODE_WHILE_STMT,
    NODE_FOR_STMT,
    NODE_BLOCK,
    NODE_BREAK_STMT,
    NODE_CONTINUE_STMT,
    NODE_ASSIGN_STMT,
    NODE_DEFER_STMT,

    // Expressions
    NODE_BINARY_EXPR,
    NODE_UNARY_EXPR,
    NODE_CALL_EXPR,
    NODE_METHOD_CALL,
    NODE_FIELD_ACCESS,
    NODE_INDEX_EXPR,
    NODE_INT_LIT,
    NODE_FLOAT_LIT,
    NODE_STRING_LIT,
    NODE_BOOL_LIT,
    NODE_IDENT,
    NODE_STRUCT_LIT,
    NODE_ARRAY_LIT,
    NODE_NONE_LIT,
    NODE_REF_EXPR,
    NODE_DEREF_EXPR,
    NODE_LAMBDA,
    NODE_ENUM_VARIANT_EXPR,  // Shape::Circle(5.0)
    NODE_ENUM_VARIANT_DEF,   // Circle(f64) in enum declaration
    NODE_MATCH_EXPR,
    NODE_MATCH_ARM,

    // Types
    NODE_TYPE_BASIC,
    NODE_TYPE_REF,
    NODE_TYPE_ARRAY,
    NODE_TYPE_GENERIC,
} NodeType;

// Forward declare
typedef struct ASTNode ASTNode;

// Dynamic list of AST node pointers
typedef struct {
    ASTNode **items;
    int count;
    int capacity;
} NodeList;

// Function parameter / struct field
typedef struct {
    char *name;
    ASTNode *type;
    bool is_mut;
} Field;

typedef struct {
    Field *items;
    int count;
    int capacity;
} FieldList;

struct ASTNode {
    NodeType type;
    int line;
    int column;
    Type *resolved_type; // set by type checker

    union {
        // NODE_PROGRAM
        struct { NodeList decls; } program;

        // NODE_FN_DECL
        struct {
            char *name;
            FieldList params;
            ASTNode *return_type;
            ASTNode *body;
            bool is_pub;
            bool is_method;
            bool has_self;
            bool self_is_mut;
            NodeList type_params;   // generic: <T, U> (NODE_TYPE_BASIC nodes)
            NodeList examples;
            NodeList panics;
            NodeList requires;
            NodeList ensures;
            ASTNode *recover_block;
        } fn_decl;

        // NODE_STRUCT_DECL
        struct {
            char *name;
            FieldList fields;
            bool is_pub;
            NodeList invariants;
            NodeList type_params;   // generic: <A, B>
        } struct_decl;

        // NODE_ENUM_DECL
        struct {
            char *name;
            NodeList variants;
            bool is_pub;
        } enum_decl;

        // NODE_IMPL_BLOCK
        struct {
            char *type_name;
            NodeList methods;
        } impl_block;

        // NODE_TEST_DECL
        struct {
            char *name;
            ASTNode *body;
        } test_decl;

        // NODE_IMPORT
        struct {
            char *path;
        } import_decl;

        // NODE_BLOCK
        struct { NodeList stmts; } block;

        // NODE_LET_STMT
        struct {
            char *name;
            ASTNode *type_ann;
            ASTNode *init;
            bool is_mut;
        } let_stmt;

        // NODE_RETURN_STMT
        struct { ASTNode *value; } return_stmt;

        // NODE_EXPR_STMT
        struct { ASTNode *expr; } expr_stmt;

        // NODE_IF_STMT
        struct {
            ASTNode *condition;
            ASTNode *then_block;
            ASTNode *else_block;
        } if_stmt;

        // NODE_WHILE_STMT
        struct {
            ASTNode *condition;
            ASTNode *body;
        } while_stmt;

        // NODE_FOR_STMT
        struct {
            char *var_name;
            ASTNode *iterable;
            ASTNode *body;
        } for_stmt;

        // NODE_DEFER_STMT
        struct { ASTNode *stmt; } defer_stmt;

        // NODE_ASSIGN_STMT
        struct {
            ASTNode *target;
            ASTNode *value;
            TokenType op; // TOK_ASSIGN, TOK_PLUS_ASSIGN, etc.
        } assign_stmt;

        // NODE_BINARY_EXPR
        struct {
            TokenType op;
            ASTNode *left;
            ASTNode *right;
        } binary;

        // NODE_UNARY_EXPR
        struct {
            TokenType op;
            ASTNode *operand;
        } unary;

        // NODE_CALL_EXPR
        struct {
            ASTNode *callee;
            NodeList args;
            char *mono_name; // set by checker for generic calls: "first_i64"
        } call;

        // NODE_METHOD_CALL
        struct {
            ASTNode *object;
            char *method;
            NodeList args;
        } method_call;

        // NODE_FIELD_ACCESS
        struct {
            ASTNode *object;
            char *field;
        } field_access;

        // NODE_INDEX_EXPR
        struct {
            ASTNode *object;
            ASTNode *index;
        } index_expr;

        // NODE_INT_LIT
        struct { int64_t value; } int_lit;

        // NODE_FLOAT_LIT
        struct { double value; } float_lit;

        // NODE_STRING_LIT
        struct { char *value; } string_lit;

        // NODE_BOOL_LIT
        struct { bool value; } bool_lit;

        // NODE_IDENT
        struct { char *name; } ident;

        // NODE_STRUCT_LIT
        struct {
            char *name;
            FieldList field_inits; // name + init (stored as type field)
            NodeList values;       // corresponding values
        } struct_lit;

        // NODE_ARRAY_LIT
        struct { NodeList elements; } array_lit;

        // NODE_REF_EXPR
        struct {
            ASTNode *operand;
            bool is_mut;
        } ref_expr;

        // NODE_DEREF_EXPR
        struct { ASTNode *operand; } deref_expr;

        // NODE_LAMBDA
        struct {
            FieldList params;
            ASTNode *return_type;
            ASTNode *body;
            int id;
            // Captures (filled by checker)
            char **capture_names;
            Type **capture_types;
            int capture_count;
        } lambda;

        // NODE_ENUM_VARIANT_EXPR: Shape::Circle(5.0)
        struct {
            char *enum_name;    // "Shape"
            char *variant_name; // "Circle"
            NodeList args;      // [5.0]
        } enum_variant_expr;

        // NODE_ENUM_VARIANT_DEF: Circle(f64) in enum decl
        struct {
            char *name;         // "Circle"
            NodeList field_types; // [NODE_TYPE_BASIC("f64")]
        } enum_variant_def;

        // NODE_MATCH_EXPR
        struct {
            ASTNode *subject;
            NodeList arms; // list of NODE_MATCH_ARM
        } match_expr;

        // NODE_MATCH_ARM
        struct {
            ASTNode *pattern;    // NULL = wildcard, NODE_ENUM_VARIANT_EXPR for destructuring
            ASTNode *body;
            char **bind_names;   // destructured variable names: [r] or [w, h]
            int bind_count;
        } match_arm;

        // NODE_TYPE_BASIC
        struct { char *name; } type_basic;

        // NODE_TYPE_REF
        struct {
            ASTNode *inner;
            bool is_mut;
        } type_ref;

        // NODE_TYPE_ARRAY
        struct {
            ASTNode *element_type;
        } type_array;

        // NODE_TYPE_GENERIC
        struct {
            char *name;
            NodeList type_args;
        } type_generic;
    };
};

// Constructors
ASTNode *ast_new(NodeType type, int line, int col);
void node_list_init(NodeList *list);
void node_list_push(NodeList *list, ASTNode *node);
void field_list_init(FieldList *list);
void field_list_push(FieldList *list, Field field);
char *str_dup(const char *s, int len);
char *str_dup_unescape(const char *s, int len);

#endif
