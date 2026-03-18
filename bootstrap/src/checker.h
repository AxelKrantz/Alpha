#ifndef ALPHA_CHECKER_H
#define ALPHA_CHECKER_H

#include "ast.h"
#include "types.h"

typedef struct {
    TypeTable types;
    const char *filename;
    bool had_error;
    int loop_depth;       // for break/continue validation
    Type *current_ret;    // expected return type of current function (NULL = void)

    // Impl method registry: maps struct_name -> list of methods
    struct {
        char *struct_name;
        ASTNode *impl_node;
    } *impls;
    int impl_count;
    int impl_cap;
} Checker;

void checker_init(Checker *c, const char *filename);
void checker_check(Checker *c, ASTNode *program);
TypeTable *checker_get_types(Checker *c);

#endif
