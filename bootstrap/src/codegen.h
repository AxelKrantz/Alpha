#ifndef ALPHA_CODEGEN_H
#define ALPHA_CODEGEN_H

#include "ast.h"
#include <stdio.h>

typedef struct {
    FILE *out;
    int indent;
    const char *current_struct;
    const char *test_filename;  // set for test mode
    bool test_mode;
    bool strip_contracts;
    // Contract tracking for ensures
    NodeList *current_ensures;
    const char *current_fn_name;
    // Struct invariant tracking
    NodeList *current_invariants;
    const FieldList *invariant_fields;
    // Defer tracking
    ASTNode **defer_stack;
    int defer_count;
    int defer_cap;
    // Auto-cleanup tracking: variables that own heap memory
    struct { char *name; const char *free_fn; int scope_depth; } *owned_vars;
    int owned_count;
    int owned_cap;
    int scope_depth;
    // Generics
    char **subst_param_names;
    Type **subst_concrete;
    int subst_count;
    void *type_table; // TypeTable* for monomorphization
    // Lambda tracking
    int lambda_count;
    ASTNode **lambdas;     // collected during emit, hoisted to top
    int lambda_cap;
} CodeGen;

void codegen_init(CodeGen *gen, FILE *out);
void codegen_emit(CodeGen *gen, ASTNode *program);

#endif
