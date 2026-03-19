#include "checker.h"
#include "error.h"
#include <stdio.h>
#include <string.h>

// Levenshtein distance for suggestions
static int edit_distance(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 30 || lb > 30) return 999;
    int dp[31][31];
    for (int i = 0; i <= la; i++) dp[i][0] = i;
    for (int j = 0; j <= lb; j++) dp[0][j] = j;
    for (int i = 1; i <= la; i++)
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i-1] != b[j-1]) ? 1 : 0;
            dp[i][j] = dp[i-1][j] + 1;
            if (dp[i][j-1] + 1 < dp[i][j]) dp[i][j] = dp[i][j-1] + 1;
            if (dp[i-1][j-1] + cost < dp[i][j]) dp[i][j] = dp[i-1][j-1] + cost;
        }
    return dp[la][lb];
}

// Find the closest matching name in scope
static const char *find_similar(TypeTable *tt, const char *name) {
    const char *best = NULL;
    int best_dist = 3; // max distance to suggest
    for (Scope *scope = tt->current_scope; scope; scope = scope->parent) {
        for (int i = 0; i < scope->count; i++) {
            int d = edit_distance(name, scope->symbols[i].name);
            if (d < best_dist) {
                best_dist = d;
                best = scope->symbols[i].name;
            }
        }
    }
    return best;
}

void checker_init(Checker *c, const char *filename) {
    type_table_init(&c->types);
    c->filename = filename;
    c->had_error = false;
    c->loop_depth = 0;
    c->current_ret = NULL;
    c->impls = NULL;
    c->impl_count = 0;
    c->impl_cap = 0;
}

static void check_warning(Checker *c, int line, int col, int length, const char *fmt, ...) {
    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (g_diag) {
        diag_emit(g_diag, DIAG_WARNING, line, col, length, NULL, "%s", msg);
    } else {
        fprintf(stderr, "%s:%d:%d: warning: %s\n", c->filename, line, col, msg);
    }
}

// Check if a name is a builtin function
static bool is_builtin_name(const char *name) {
    static const char *builtins[] = {
        "print", "println", "len", "exit", "assert", "eprintln",
        "str_eq", "str_concat", "str_substr", "str_char_at", "str_contains",
        "str_starts_with", "char_to_str", "i64_to_str", "str_to_i64",
        "file_read", "file_write", "sqrt", "alloc",
        "args_count", "args_get", "run_command",
        "sb_new", "sb_append", "sb_append_char", "sb_to_str",
        "assert_eq", "assert_neq", "assert_lt", "assert_gt",
        "format", "map_new", "some", "ok", "err",
        "as_i64", "as_f64", "as_i32", "as_u8", "as_bool",
        "env_get", "panic",
        NULL
    };
    for (int i = 0; builtins[i]; i++) {
        if (strcmp(name, builtins[i]) == 0) return true;
    }
    return false;
}

static void check_error(Checker *c, int line, int col, const char *fmt, ...) {
    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    if (g_diag) {
        diag_emit(g_diag, DIAG_ERROR, line, col, 0, NULL, "%s", msg);
    } else {
        fprintf(stderr, "%s:%d:%d: type error: %s\n", c->filename, line, col, msg);
    }
    c->had_error = true;
}

// ---- Resolve AST type node to Type* ----

static Type *resolve_type_node(Checker *c, ASTNode *node) {
    if (!node) return c->types.t_void;

    switch (node->type) {
        case NODE_TYPE_BASIC:
            return type_resolve_name(&c->types, node->type_basic.name);

        case NODE_TYPE_REF:
            return type_new_ref(&c->types,
                resolve_type_node(c, node->type_ref.inner),
                node->type_ref.is_mut);

        case NODE_TYPE_ARRAY:
            return type_new_array(&c->types,
                resolve_type_node(c, node->type_array.element_type));

        case NODE_TYPE_GENERIC:
            // Handle Result<T>
            if (strcmp(node->type_generic.name, "Result") == 0 && node->type_generic.type_args.count > 0) {
                Type *ok_type = resolve_type_node(c, node->type_generic.type_args.items[0]);
                return type_new_result(&c->types, ok_type);
            }
            // Handle Option<T>
            if (strcmp(node->type_generic.name, "Option") == 0 && node->type_generic.type_args.count > 0) {
                Type *inner = resolve_type_node(c, node->type_generic.type_args.items[0]);
                return type_new_option(&c->types, inner);
            }
            // Handle Map<ValueType>
            if (strcmp(node->type_generic.name, "Map") == 0 && node->type_generic.type_args.count > 0) {
                Type *val_type = resolve_type_node(c, node->type_generic.type_args.items[0]);
                return type_new_map(&c->types, val_type);
            }
            return type_resolve_name(&c->types, node->type_generic.name);

        default:
            return c->types.t_unknown;
    }
}

// Resolve type node in generic context (type param names -> TYPE_PARAM)
static Type *resolve_type_node_generic(Checker *c, ASTNode *node, char **param_names, int param_count) {
    if (!node) return c->types.t_void;
    if (node->type == NODE_TYPE_BASIC) {
        // Check if it's a type parameter
        for (int i = 0; i < param_count; i++) {
            if (strcmp(node->type_basic.name, param_names[i]) == 0) {
                return type_new_param(&c->types, param_names[i], i);
            }
        }
        return type_resolve_name(&c->types, node->type_basic.name);
    }
    if (node->type == NODE_TYPE_ARRAY) {
        return type_new_array(&c->types,
            resolve_type_node_generic(c, node->type_array.element_type, param_names, param_count));
    }
    if (node->type == NODE_TYPE_REF) {
        return type_new_ref(&c->types,
            resolve_type_node_generic(c, node->type_ref.inner, param_names, param_count),
            node->type_ref.is_mut);
    }
    if (node->type == NODE_TYPE_GENERIC) {
        // Handle Option<T>, Map<T> etc
        if (strcmp(node->type_generic.name, "Option") == 0 && node->type_generic.type_args.count > 0)
            return type_new_option(&c->types, resolve_type_node_generic(c, node->type_generic.type_args.items[0], param_names, param_count));
        if (strcmp(node->type_generic.name, "Map") == 0 && node->type_generic.type_args.count > 0)
            return type_new_map(&c->types, resolve_type_node_generic(c, node->type_generic.type_args.items[0], param_names, param_count));
        return type_resolve_name(&c->types, node->type_generic.name);
    }
    return resolve_type_node(c, node);
}

// Unify a type pattern (may contain TYPE_PARAM) against a concrete type
static bool unify_types(Type *pattern, Type *concrete, Type **type_args, int param_count) {
    if (!pattern || !concrete) return true;
    if (pattern->kind == TYPE_PARAM) {
        int idx = pattern->param_info.index;
        if (idx < 0 || idx >= param_count) return false;
        if (type_args[idx] == NULL) {
            type_args[idx] = concrete;
            return true;
        }
        return type_equals(type_args[idx], concrete);
    }
    if (pattern->kind == TYPE_ARRAY && concrete->kind == TYPE_ARRAY) {
        return unify_types(pattern->array_info.element, concrete->array_info.element, type_args, param_count);
    }
    if (pattern->kind == TYPE_OPTION && concrete->kind == TYPE_OPTION) {
        return unify_types(pattern->option_info.inner_type, concrete->option_info.inner_type, type_args, param_count);
    }
    if (pattern->kind == TYPE_REF && concrete->kind == TYPE_REF) {
        return unify_types(pattern->ref_info.inner, concrete->ref_info.inner, type_args, param_count);
    }
    return true; // non-parameterized types are compatible
}

// ---- Register impl block ----

static void register_impl(Checker *c, const char *struct_name, ASTNode *impl_node) {
    if (c->impl_count >= c->impl_cap) {
        c->impl_cap = c->impl_cap == 0 ? 16 : c->impl_cap * 2;
        c->impls = realloc(c->impls, sizeof(c->impls[0]) * c->impl_cap);
    }
    char *name = malloc(strlen(struct_name) + 1);
    strcpy(name, struct_name);
    c->impls[c->impl_count].struct_name = name;
    c->impls[c->impl_count].impl_node = impl_node;
    c->impl_count++;
}

// Look up a method for a struct type
static ASTNode *find_method(Checker *c, const char *struct_name, const char *method_name) {
    for (int i = 0; i < c->impl_count; i++) {
        if (strcmp(c->impls[i].struct_name, struct_name) != 0) continue;
        ASTNode *impl = c->impls[i].impl_node;
        // Handle both NODE_IMPL_BLOCK and NODE_IMPL_TRAIT
        NodeList *methods;
        if (impl->type == NODE_IMPL_TRAIT) methods = &impl->impl_trait.methods;
        else methods = &impl->impl_block.methods;
        for (int j = 0; j < methods->count; j++) {
            ASTNode *m = methods->items[j];
            if (strcmp(m->fn_decl.name, method_name) == 0) {
                return m;
            }
        }
    }
    return NULL;
}

// ---- Check expressions ----

static Type *check_expr(Checker *c, ASTNode *node);
static void check_stmt(Checker *c, ASTNode *node);
static void check_block(Checker *c, ASTNode *node);

static Type *check_expr(Checker *c, ASTNode *node) {
    if (!node) return c->types.t_void;

    Type *result = c->types.t_unknown;

    switch (node->type) {
        case NODE_INT_LIT:
            result = c->types.t_i64;
            break;

        case NODE_FLOAT_LIT:
            result = c->types.t_f64;
            break;

        case NODE_STRING_LIT:
            result = c->types.t_str;
            break;

        case NODE_BOOL_LIT:
            result = c->types.t_bool;
            break;

        case NODE_NONE_LIT:
            // If in a function returning Option<T>, resolve to that type
            if (c->current_ret && c->current_ret->kind == TYPE_OPTION) {
                result = c->current_ret;
            } else {
                result = c->types.t_void;
            }
            break;

        case NODE_IDENT: {
            Symbol *sym = scope_lookup(&c->types, node->ident.name);
            if (sym) {
                sym->used = true;
                result = sym->type;
            } else {
                // Undeclared variable — try to suggest a fix
                const char *similar = find_similar(&c->types, node->ident.name);
                int name_len = (int)strlen(node->ident.name);
                if (similar) {
                    char suggestion[128];
                    snprintf(suggestion, sizeof(suggestion), "did you mean '%s'?", similar);
                    diag_emit(g_diag, DIAG_ERROR, node->line, node->column, name_len,
                              suggestion, "unknown variable '%s'", node->ident.name);
                } else {
                    diag_emit(g_diag, DIAG_ERROR, node->line, node->column, name_len,
                              NULL, "unknown variable '%s'", node->ident.name);
                }
                c->had_error = true;
                result = c->types.t_unknown;
            }
            break;
        }

        case NODE_BINARY_EXPR: {
            Type *left = check_expr(c, node->binary.left);
            Type *right = check_expr(c, node->binary.right);

            switch (node->binary.op) {
                case TOK_PLUS:
                case TOK_MINUS:
                case TOK_STAR:
                case TOK_SLASH:
                case TOK_PERCENT:
                    // If either side is float, result is float
                    if (type_is_float(left) || type_is_float(right)) {
                        result = (left->kind == TYPE_F32 && right->kind == TYPE_F32)
                            ? c->types.t_f32 : c->types.t_f64;
                    } else if (type_is_integer(left) || type_is_integer(right)) {
                        result = c->types.t_i64;
                    } else {
                        result = left;
                    }
                    break;

                case TOK_EQ:
                case TOK_NEQ:
                case TOK_LT:
                case TOK_GT:
                case TOK_LEQ:
                case TOK_GEQ:
                case TOK_AND:
                case TOK_OR:
                    result = c->types.t_bool;
                    break;

                default:
                    result = left;
                    break;
            }
            break;
        }

        case NODE_UNARY_EXPR: {
            Type *operand = check_expr(c, node->unary.operand);
            if (node->unary.op == TOK_NOT || node->unary.op == TOK_BANG) {
                result = c->types.t_bool;
            } else {
                result = operand;
            }
            break;
        }

        case NODE_CALL_EXPR: {
            // Check callee
            if (node->call.callee->type == NODE_IDENT) {
                const char *name = node->call.callee->ident.name;

                // Built-in functions
                if (strcmp(name, "print") == 0 || strcmp(name, "println") == 0) {
                    // Check args
                    for (int i = 0; i < node->call.args.count; i++) {
                        check_expr(c, node->call.args.items[i]);
                    }
                    result = c->types.t_void;
                    break;
                }
                if (strcmp(name, "len") == 0) {
                    for (int i = 0; i < node->call.args.count; i++) {
                        check_expr(c, node->call.args.items[i]);
                    }
                    result = c->types.t_i64;
                    break;
                }
                if (strcmp(name, "exit") == 0 || strcmp(name, "assert") == 0 ||
                    strcmp(name, "file_write") == 0 || strcmp(name, "sb_append") == 0 ||
                    strcmp(name, "sb_append_char") == 0 || strcmp(name, "eprintln") == 0 ||
                    strcmp(name, "assert_eq") == 0 || strcmp(name, "assert_neq") == 0 ||
                    strcmp(name, "assert_lt") == 0 || strcmp(name, "assert_gt") == 0) {
                    for (int i = 0; i < node->call.args.count; i++) {
                        check_expr(c, node->call.args.items[i]);
                    }
                    result = c->types.t_void;
                    break;
                }
                if (strcmp(name, "str_eq") == 0 || strcmp(name, "str_contains") == 0 ||
                    strcmp(name, "str_starts_with") == 0) {
                    for (int i = 0; i < node->call.args.count; i++) {
                        check_expr(c, node->call.args.items[i]);
                    }
                    result = c->types.t_bool;
                    break;
                }
                if (strcmp(name, "str_concat") == 0 || strcmp(name, "str_substr") == 0 ||
                    strcmp(name, "char_to_str") == 0 || strcmp(name, "i64_to_str") == 0 ||
                    strcmp(name, "file_read") == 0 || strcmp(name, "args_get") == 0 ||
                    strcmp(name, "sb_to_str") == 0 || strcmp(name, "format") == 0 ||
                    strcmp(name, "env_get") == 0) {
                    for (int i = 0; i < node->call.args.count; i++) {
                        check_expr(c, node->call.args.items[i]);
                    }
                    result = c->types.t_str;
                    break;
                }
                if (strcmp(name, "str_char_at") == 0 || strcmp(name, "str_to_i64") == 0 ||
                    strcmp(name, "args_count") == 0 || strcmp(name, "run_command") == 0) {
                    for (int i = 0; i < node->call.args.count; i++) {
                        check_expr(c, node->call.args.items[i]);
                    }
                    result = c->types.t_i64;
                    break;
                }
                if (strcmp(name, "sb_new") == 0) {
                    result = c->types.t_unknown; // opaque pointer
                    break;
                }
                if (strcmp(name, "ok") == 0 && node->call.args.count == 1) {
                    Type *inner = check_expr(c, node->call.args.items[0]);
                    result = type_new_result(&c->types, inner);
                    node->resolved_type = result;
                    return result;
                }
                if (strcmp(name, "err") == 0 && node->call.args.count == 1) {
                    check_expr(c, node->call.args.items[0]);
                    // Return type depends on context — default to Result<str>
                    if (c->current_ret && c->current_ret->kind == TYPE_RESULT) {
                        result = c->current_ret;
                    } else {
                        result = type_new_result(&c->types, c->types.t_str);
                    }
                    node->resolved_type = result;
                    return result;
                }
                if (strcmp(name, "some") == 0 && node->call.args.count == 1) {
                    Type *inner = check_expr(c, node->call.args.items[0]);
                    result = type_new_option(&c->types, inner);
                    node->resolved_type = result;
                    return result;
                }
                if (strcmp(name, "map_new") == 0) {
                    // Default to map<str, str>; will be refined by type annotation
                    result = type_new_map(&c->types, c->types.t_str);
                    break;
                }
                if (strcmp(name, "as_i64") == 0 || strcmp(name, "as_i32") == 0 ||
                    strcmp(name, "as_u8") == 0) {
                    for (int i = 0; i < node->call.args.count; i++) check_expr(c, node->call.args.items[i]);
                    result = strcmp(name, "as_u8") == 0 ? c->types.t_u8 : c->types.t_i64;
                    break;
                }
                if (strcmp(name, "as_f64") == 0) {
                    for (int i = 0; i < node->call.args.count; i++) check_expr(c, node->call.args.items[i]);
                    result = c->types.t_f64;
                    break;
                }
                if (strcmp(name, "as_bool") == 0) {
                    for (int i = 0; i < node->call.args.count; i++) check_expr(c, node->call.args.items[i]);
                    result = c->types.t_bool;
                    break;
                }
                if (strcmp(name, "sqrt") == 0) {
                    for (int i = 0; i < node->call.args.count; i++) {
                        check_expr(c, node->call.args.items[i]);
                    }
                    result = c->types.t_f64;
                    break;
                }

                // Check all args first (needed for type inference)
                for (int i = 0; i < node->call.args.count; i++) {
                    check_expr(c, node->call.args.items[i]);
                }

                // Generic function?
                GenericDef *gdef = find_generic_def(&c->types, name);
                if (gdef && !gdef->is_struct) {
                    ASTNode *fn_ast = (ASTNode *)gdef->ast_node;
                    int tpc = gdef->type_param_count;

                    // Resolve param types in generic context
                    Type **type_args = calloc(tpc, sizeof(Type *));

                    // Unify each parameter with call arg type
                    for (int i = 0; i < fn_ast->fn_decl.params.count && i < node->call.args.count; i++) {
                        Type *param_pattern = resolve_type_node_generic(c,
                            fn_ast->fn_decl.params.items[i].type,
                            gdef->type_param_names, tpc);
                        Type *arg_type = node->call.args.items[i]->resolved_type;
                        if (arg_type) unify_types(param_pattern, arg_type, type_args, tpc);
                    }

                    // Check all type args were inferred
                    bool all_inferred = true;
                    for (int i = 0; i < tpc; i++) {
                        if (!type_args[i]) { all_inferred = false; break; }
                    }

                    if (all_inferred) {
                        // Find or create monomorphized instance
                        MonoInstance *mi = find_mono_instance(&c->types, name, type_args, tpc);
                        if (!mi) mi = add_mono_instance(&c->types, name, type_args, tpc);

                        // Set mangled name on the call for codegen
                        node->call.mono_name = mi->mangled_name;

                        // Compute return type
                        Type *ret_pattern = resolve_type_node_generic(c,
                            fn_ast->fn_decl.return_type,
                            gdef->type_param_names, tpc);
                        result = type_substitute(&c->types, ret_pattern, tpc, type_args);
                    } else {
                        check_error(c, node->line, node->column,
                            "cannot infer type parameters for generic function '%s'", name);
                    }
                    free(type_args);
                    break;
                }

                // User function
                Symbol *sym = scope_lookup(&c->types, name);
                if (sym && sym->type->kind == TYPE_FN) {
                    sym->used = true;
                    result = sym->type->fn_info.ret;

                    // Check argument count
                    int expected = sym->type->fn_info.param_count;
                    int got = node->call.args.count;
                    if (expected != got) {
                        check_error(c, node->line, node->column,
                            "'%s' expects %d argument%s, got %d",
                            name, expected, expected == 1 ? "" : "s", got);
                    }
                } else if (!is_builtin_name(name)) {
                    // Unknown function
                    const char *similar = find_similar(&c->types, name);
                    int name_len = (int)strlen(name);
                    if (similar) {
                        char suggestion[128];
                        snprintf(suggestion, sizeof(suggestion), "did you mean '%s'?", similar);
                        diag_emit(g_diag, DIAG_ERROR, node->line, node->column, name_len,
                                  suggestion, "unknown function '%s'", name);
                    } else {
                        diag_emit(g_diag, DIAG_ERROR, node->line, node->column, name_len,
                                  NULL, "unknown function '%s'", name);
                    }
                    c->had_error = true;
                    result = c->types.t_unknown;
                } else {
                    result = c->types.t_unknown;
                }
            } else {
                check_expr(c, node->call.callee);
                result = c->types.t_unknown;
            }

            // Check all args
            for (int i = 0; i < node->call.args.count; i++) {
                check_expr(c, node->call.args.items[i]);
            }
            break;
        }

        case NODE_METHOD_CALL: {
            Type *obj_type = check_expr(c, node->method_call.object);
            const char *method = node->method_call.method;

            // Array methods
            if (obj_type->kind == TYPE_ARRAY) {
                if (strcmp(method, "push") == 0) result = c->types.t_void;
                else if (strcmp(method, "pop") == 0) result = obj_type->array_info.element ? obj_type->array_info.element : c->types.t_unknown;
                else if (strcmp(method, "clear") == 0) result = c->types.t_void;
                else if (strcmp(method, "clone") == 0) result = obj_type;
                else if (strcmp(method, "free") == 0) result = c->types.t_void;
                else if (strcmp(method, "join") == 0) result = c->types.t_str;
                else if (strcmp(method, "map") == 0) result = obj_type;
                else if (strcmp(method, "filter") == 0) result = obj_type;
                else if (strcmp(method, "reduce") == 0) result = obj_type->array_info.element ? obj_type->array_info.element : c->types.t_unknown;
                else if (strcmp(method, "any") == 0 || strcmp(method, "all") == 0) result = c->types.t_bool;
                else if (strcmp(method, "count") == 0) result = c->types.t_i64;
                else check_error(c, node->line, node->column, "no method '%s' on array", method);
                for (int i = 0; i < node->method_call.args.count; i++)
                    check_expr(c, node->method_call.args.items[i]);
                break;
            }

            // Result methods
            if (obj_type->kind == TYPE_RESULT) {
                Type *ok_t = obj_type->result_info.ok_type ? obj_type->result_info.ok_type : c->types.t_unknown;
                if (strcmp(method, "is_ok") == 0 || strcmp(method, "is_err") == 0) result = c->types.t_bool;
                else if (strcmp(method, "unwrap") == 0 || strcmp(method, "unwrap_or") == 0) result = ok_t;
                else if (strcmp(method, "error") == 0) result = c->types.t_str;
                else check_error(c, node->line, node->column, "no method '%s' on Result", method);
                for (int i = 0; i < node->method_call.args.count; i++)
                    check_expr(c, node->method_call.args.items[i]);
                break;
            }

            // Option methods
            if (obj_type->kind == TYPE_OPTION) {
                Type *inner = obj_type->option_info.inner_type ? obj_type->option_info.inner_type : c->types.t_unknown;
                if (strcmp(method, "is_some") == 0 || strcmp(method, "is_none") == 0) result = c->types.t_bool;
                else if (strcmp(method, "unwrap") == 0) result = inner;
                else if (strcmp(method, "unwrap_or") == 0) result = inner;
                else check_error(c, node->line, node->column, "no method '%s' on Option", method);
                for (int i = 0; i < node->method_call.args.count; i++)
                    check_expr(c, node->method_call.args.items[i]);
                break;
            }

            // Map methods
            if (obj_type->kind == TYPE_MAP) {
                Type *vt = obj_type->map_info.value_type ? obj_type->map_info.value_type : c->types.t_str;
                if (strcmp(method, "set") == 0) result = c->types.t_void;
                else if (strcmp(method, "get") == 0) result = vt;
                else if (strcmp(method, "has") == 0) result = c->types.t_bool;
                else if (strcmp(method, "delete") == 0) result = c->types.t_void;
                else if (strcmp(method, "keys") == 0) result = type_new_array(&c->types, c->types.t_str);
                else check_error(c, node->line, node->column, "no method '%s' on map", method);
                for (int i = 0; i < node->method_call.args.count; i++)
                    check_expr(c, node->method_call.args.items[i]);
                break;
            }

            // String methods
            if (obj_type->kind == TYPE_STR) {
                if (strcmp(method, "contains") == 0 || strcmp(method, "starts_with") == 0) result = c->types.t_bool;
                else if (strcmp(method, "substr") == 0 || strcmp(method, "trim") == 0 ||
                         strcmp(method, "replace") == 0) result = c->types.t_str;
                else if (strcmp(method, "char_at") == 0) result = c->types.t_i64;
                else if (strcmp(method, "split") == 0) result = type_new_array(&c->types, c->types.t_str);
                else check_error(c, node->line, node->column, "no method '%s' on str", method);
                for (int i = 0; i < node->method_call.args.count; i++)
                    check_expr(c, node->method_call.args.items[i]);
                break;
            }

            // User-defined struct methods
            if (obj_type->kind == TYPE_STRUCT && obj_type->name) {
                ASTNode *m = find_method(c, obj_type->name, method);
                if (m) {
                    result = m->fn_decl.return_type
                        ? resolve_type_node(c, m->fn_decl.return_type)
                        : c->types.t_void;
                } else {
                    check_error(c, node->line, node->column,
                        "no method '%s' on type '%s'", method, obj_type->name);
                }
            }

            for (int i = 0; i < node->method_call.args.count; i++) {
                check_expr(c, node->method_call.args.items[i]);
            }
            break;
        }

        case NODE_FIELD_ACCESS: {
            Type *obj_type = check_expr(c, node->field_access.object);

            // Array .len
            if (obj_type->kind == TYPE_ARRAY && strcmp(node->field_access.field, "len") == 0) {
                result = c->types.t_i64;
                break;
            }
            // Map .len
            if (obj_type->kind == TYPE_MAP && strcmp(node->field_access.field, "len") == 0) {
                result = c->types.t_i64;
                break;
            }
            // String .len
            if (obj_type->kind == TYPE_STR && strcmp(node->field_access.field, "len") == 0) {
                result = c->types.t_i64;
                break;
            }

            if (obj_type->kind == TYPE_STRUCT) {
                // Look up field
                bool found = false;
                for (int i = 0; i < obj_type->struct_info.field_count; i++) {
                    if (strcmp(obj_type->struct_info.fields[i].name, node->field_access.field) == 0) {
                        result = obj_type->struct_info.fields[i].type;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    check_error(c, node->line, node->column,
                        "no field '%s' on type '%s'",
                        node->field_access.field,
                        obj_type->name ? obj_type->name : "struct");
                }
            }
            break;
        }

        case NODE_INDEX_EXPR: {
            Type *obj = check_expr(c, node->index_expr.object);
            check_expr(c, node->index_expr.index);
            if (obj->kind == TYPE_ARRAY && obj->array_info.element) {
                result = obj->array_info.element;
            } else if (obj->kind == TYPE_STR) {
                result = c->types.t_i64; // character access
            } else {
                result = c->types.t_unknown;
            }
            break;
        }

        case NODE_STRUCT_LIT: {
            Type *t = type_resolve_name(&c->types, node->struct_lit.name);
            if (t->kind == TYPE_UNKNOWN) {
                check_error(c, node->line, node->column,
                    "unknown type '%s'", node->struct_lit.name);
            }
            for (int i = 0; i < node->struct_lit.values.count; i++) {
                Type *val_type = check_expr(c, node->struct_lit.values.items[i]);
                // Propagate struct field type to empty array literals
                if (t->kind == TYPE_STRUCT && i < node->struct_lit.field_inits.count) {
                    const char *field_name = node->struct_lit.field_inits.items[i].name;
                    for (int j = 0; j < t->struct_info.field_count; j++) {
                        if (strcmp(t->struct_info.fields[j].name, field_name) == 0) {
                            Type *field_type = t->struct_info.fields[j].type;
                            if (field_type->kind == TYPE_ARRAY &&
                                node->struct_lit.values.items[i]->type == NODE_ARRAY_LIT &&
                                node->struct_lit.values.items[i]->array_lit.elements.count == 0) {
                                node->struct_lit.values.items[i]->resolved_type = field_type;
                            }
                            break;
                        }
                    }
                }
            }
            result = t;
            break;
        }

        case NODE_ARRAY_LIT: {
            Type *elem_type = c->types.t_unknown;
            for (int i = 0; i < node->array_lit.elements.count; i++) {
                Type *t = check_expr(c, node->array_lit.elements.items[i]);
                if (i == 0) elem_type = t;
            }
            result = type_new_array(&c->types, elem_type);
            break;
        }

        case NODE_REF_EXPR: {
            Type *inner = check_expr(c, node->ref_expr.operand);
            result = type_new_ref(&c->types, inner, node->ref_expr.is_mut);
            break;
        }

        case NODE_TRY_EXPR: {
            Type *inner = check_expr(c, node->try_expr.operand);
            if (inner->kind == TYPE_RESULT && inner->result_info.ok_type) {
                result = inner->result_info.ok_type;
            } else if (inner->kind == TYPE_OPTION && inner->option_info.inner_type) {
                result = inner->option_info.inner_type;
            } else {
                result = c->types.t_unknown;
            }
            break;
        }

        case NODE_ENUM_VARIANT_EXPR: {
            // Resolve to the enum type
            result = type_resolve_name(&c->types, node->enum_variant_expr.enum_name);
            for (int i = 0; i < node->enum_variant_expr.args.count; i++) {
                check_expr(c, node->enum_variant_expr.args.items[i]);
            }
            break;
        }

        case NODE_DEREF_EXPR: {
            Type *inner = check_expr(c, node->deref_expr.operand);
            if (inner->kind == TYPE_REF) {
                result = inner->ref_info.inner;
            } else {
                result = c->types.t_unknown;
            }
            break;
        }

        case NODE_LAMBDA: {
            // Collect param names for capture detection
            int param_count = node->lambda.params.count;

            // Type-check lambda body with params in scope
            scope_push(&c->types);
            for (int i = 0; i < param_count; i++) {
                Field *param = &node->lambda.params.items[i];
                Type *pt = param->type ? resolve_type_node(c, param->type) : c->types.t_unknown;
                scope_define(&c->types, param->name, pt, false);
                Symbol *sym = scope_lookup(&c->types, param->name);
                if (sym) sym->used = true;
            }
            if (node->lambda.body) {
                for (int i = 0; i < node->lambda.body->block.stmts.count; i++) {
                    check_stmt(c, node->lambda.body->block.stmts.items[i]);
                }
            }
            scope_pop(&c->types);

            // Detect captures: scan body for identifiers not in param list
            // that exist in enclosing scope
            node->lambda.capture_names = NULL;
            node->lambda.capture_types = NULL;
            node->lambda.capture_count = 0;

            // Simple capture detection: walk body looking for NODE_IDENT
            // This is a recursive walk - for simplicity, check top-level expressions
            if (node->lambda.body) {
                for (int si = 0; si < node->lambda.body->block.stmts.count; si++) {
                    ASTNode *stmt = node->lambda.body->block.stmts.items[si];
                    // Walk statement looking for identifiers
                    // For now, check common patterns: return expr, expr stmt
                    ASTNode *expr = NULL;
                    if (stmt->type == NODE_RETURN_STMT) expr = stmt->return_stmt.value;
                    else if (stmt->type == NODE_EXPR_STMT) expr = stmt->expr_stmt.expr;

                    // Simple recursive scan of expression tree
                    // Use a stack-based approach for common cases
                    ASTNode *scan_stack[64];
                    int scan_top = 0;
                    if (expr) scan_stack[scan_top++] = expr;

                    while (scan_top > 0) {
                        ASTNode *e = scan_stack[--scan_top];
                        if (!e) continue;

                        if (e->type == NODE_IDENT) {
                            const char *name = e->ident.name;
                            // Check if it's a param
                            bool is_param = false;
                            for (int pi = 0; pi < param_count; pi++) {
                                if (strcmp(node->lambda.params.items[pi].name, name) == 0) {
                                    is_param = true; break;
                                }
                            }
                            if (!is_param) {
                                // Check if it's in enclosing scope
                                Symbol *sym = scope_lookup(&c->types, name);
                                if (sym && sym->type->kind != TYPE_FN) {
                                    // Check not already captured
                                    bool already = false;
                                    for (int ci = 0; ci < node->lambda.capture_count; ci++) {
                                        if (strcmp(node->lambda.capture_names[ci], name) == 0) {
                                            already = true; break;
                                        }
                                    }
                                    if (!already) {
                                        int cc = node->lambda.capture_count;
                                        node->lambda.capture_names = realloc(node->lambda.capture_names, sizeof(char*) * (cc + 1));
                                        node->lambda.capture_types = realloc(node->lambda.capture_types, sizeof(Type*) * (cc + 1));
                                        node->lambda.capture_names[cc] = (char*)name;
                                        node->lambda.capture_types[cc] = sym->type;
                                        node->lambda.capture_count = cc + 1;
                                    }
                                }
                            }
                        }

                        // Push children onto scan stack
                        if (e->type == NODE_BINARY_EXPR && scan_top < 62) {
                            scan_stack[scan_top++] = e->binary.left;
                            scan_stack[scan_top++] = e->binary.right;
                        } else if (e->type == NODE_UNARY_EXPR && scan_top < 63) {
                            scan_stack[scan_top++] = e->unary.operand;
                        } else if (e->type == NODE_CALL_EXPR && scan_top < 60) {
                            scan_stack[scan_top++] = e->call.callee;
                            for (int ai = 0; ai < e->call.args.count && scan_top < 63; ai++)
                                scan_stack[scan_top++] = e->call.args.items[ai];
                        } else if (e->type == NODE_FIELD_ACCESS && scan_top < 63) {
                            scan_stack[scan_top++] = e->field_access.object;
                        } else if (e->type == NODE_METHOD_CALL && scan_top < 60) {
                            scan_stack[scan_top++] = e->method_call.object;
                            for (int ai = 0; ai < e->method_call.args.count && scan_top < 63; ai++)
                                scan_stack[scan_top++] = e->method_call.args.items[ai];
                        } else if (e->type == NODE_INDEX_EXPR && scan_top < 62) {
                            scan_stack[scan_top++] = e->index_expr.object;
                            scan_stack[scan_top++] = e->index_expr.index;
                        }
                    }
                }
            }

            result = c->types.t_unknown;
            break;
        }

        case NODE_MATCH_EXPR: {
            check_expr(c, node->match_expr.subject);
            for (int i = 0; i < node->match_expr.arms.count; i++) {
                ASTNode *arm = node->match_expr.arms.items[i];
                ASTNode *pat = arm->match_arm.pattern;

                // For enum variant patterns, don't check as expression
                // (the args are binding names, not variables)
                if (pat && pat->type != NODE_ENUM_VARIANT_EXPR) {
                    check_expr(c, pat);
                }

                // Check body with bindings in scope
                scope_push(&c->types);

                // Define destructured bindings with resolved types
                if (pat && pat->type == NODE_ENUM_VARIANT_EXPR && arm->match_arm.bind_count > 0) {
                    // Find the variant's field types from the pattern
                    // Look up the enum variant expression's args — they contain
                    // the binding names. The actual types come from the enum decl.
                    // We need to find the enum decl AST node.
                    // For now, search registered enums for field types
                    const char *ename = pat->enum_variant_expr.enum_name;
                    const char *vname = pat->enum_variant_expr.variant_name;

                    for (int bi = 0; bi < arm->match_arm.bind_count; bi++) {
                        if (strcmp(arm->match_arm.bind_names[bi], "_") == 0) continue;
                        // Default to unknown — __auto_type handles it in C
                        scope_define(&c->types, arm->match_arm.bind_names[bi],
                                     c->types.t_unknown, false);
                        Symbol *sym = scope_lookup(&c->types, arm->match_arm.bind_names[bi]);
                        if (sym) sym->used = true;
                    }
                }

                if (arm->match_arm.body) {
                    if (arm->match_arm.body->type == NODE_BLOCK) {
                        // Don't use check_block (it pushes another scope)
                        for (int si = 0; si < arm->match_arm.body->block.stmts.count; si++) {
                            check_stmt(c, arm->match_arm.body->block.stmts.items[si]);
                        }
                    } else {
                        check_expr(c, arm->match_arm.body);
                    }
                }
                scope_pop(&c->types);
            }
            result = c->types.t_void;
            break;
        }

        default:
            break;
    }

    node->resolved_type = result;
    return result;
}

// ---- Check statements ----

static void check_stmt(Checker *c, ASTNode *node) {
    if (!node) return;

    switch (node->type) {
        case NODE_LET_STMT: {
            Type *declared_type = NULL;
            if (node->let_stmt.type_ann) {
                declared_type = resolve_type_node(c, node->let_stmt.type_ann);
            }

            Type *init_type = NULL;
            if (node->let_stmt.init) {
                init_type = check_expr(c, node->let_stmt.init);
                // Propagate declared map type
                if (declared_type && declared_type->kind == TYPE_MAP &&
                    init_type && init_type->kind == TYPE_MAP) {
                    node->let_stmt.init->resolved_type = declared_type;
                    init_type = declared_type;
                }
                // Propagate declared type to empty array literals
                if (declared_type && declared_type->kind == TYPE_ARRAY &&
                    node->let_stmt.init->type == NODE_ARRAY_LIT &&
                    node->let_stmt.init->array_lit.elements.count == 0) {
                    node->let_stmt.init->resolved_type = declared_type;
                    init_type = declared_type;
                }
            }

            // Determine final type
            Type *final_type;
            if (declared_type) {
                final_type = declared_type;
            } else if (init_type) {
                final_type = init_type;
            } else {
                final_type = c->types.t_unknown;
            }

            node->resolved_type = final_type;
            scope_define_at(&c->types, node->let_stmt.name, final_type,
                            node->let_stmt.is_mut, node->line, node->column);
            break;
        }

        case NODE_RETURN_STMT:
            if (node->return_stmt.value) {
                check_expr(c, node->return_stmt.value);
            }
            break;

        case NODE_EXPR_STMT:
            check_expr(c, node->expr_stmt.expr);
            break;

        case NODE_IF_STMT: {
            Type *cond = check_expr(c, node->if_stmt.condition);
            if (cond->kind != TYPE_BOOL && cond->kind != TYPE_UNKNOWN) {
                // Allow numeric types in conditions (truthy)
                if (!type_is_numeric(cond)) {
                    check_error(c, node->line, node->column,
                        "if condition must be bool, got %s", type_kind_name(cond->kind));
                }
            }
            check_block(c, node->if_stmt.then_block);
            if (node->if_stmt.else_block) {
                if (node->if_stmt.else_block->type == NODE_IF_STMT) {
                    check_stmt(c, node->if_stmt.else_block);
                } else {
                    check_block(c, node->if_stmt.else_block);
                }
            }
            break;
        }

        case NODE_WHILE_STMT:
            check_expr(c, node->while_stmt.condition);
            c->loop_depth++;
            check_block(c, node->while_stmt.body);
            c->loop_depth--;
            break;

        case NODE_FOR_STMT: {
            Type *iter_type = check_expr(c, node->for_stmt.iterable);
            scope_push(&c->types);

            // Determine loop variable type:
            // - for i in 0..10 → i64 (range)
            // - for item in array → element type
            Type *var_type = c->types.t_i64;
            if (iter_type->kind == TYPE_ARRAY && iter_type->array_info.element) {
                var_type = iter_type->array_info.element;
            }
            scope_define(&c->types, node->for_stmt.var_name, var_type, false);

            c->loop_depth++;
            check_block(c, node->for_stmt.body);
            c->loop_depth--;
            scope_pop(&c->types);
            break;
        }

        case NODE_ASSIGN_STMT: {
            check_expr(c, node->assign_stmt.target);
            check_expr(c, node->assign_stmt.value);

            // Mutability check
            if (node->assign_stmt.target->type == NODE_IDENT) {
                Symbol *sym = scope_lookup(&c->types, node->assign_stmt.target->ident.name);
                if (sym && !sym->is_mut) {
                    int name_len = (int)strlen(node->assign_stmt.target->ident.name);
                    diag_emit(g_diag, DIAG_ERROR, node->line, node->column, name_len,
                              "declare with 'let mut' to make it mutable",
                              "cannot assign to immutable variable '%s'",
                              node->assign_stmt.target->ident.name);
                    c->had_error = true;
                }
            }
            break;
        }

        case NODE_MATCH_EXPR:
            check_expr(c, node);
            break;

        case NODE_DEFER_STMT:
            check_stmt(c, node->defer_stmt.stmt);
            break;

        case NODE_BREAK_STMT:
            if (c->loop_depth == 0) {
                check_error(c, node->line, node->column, "'break' outside of loop");
            }
            break;
        case NODE_CONTINUE_STMT:
            if (c->loop_depth == 0) {
                check_error(c, node->line, node->column, "'continue' outside of loop");
            }
            break;

        default:
            // Try as expression
            check_expr(c, node);
            break;
    }
}

static void check_unused_in_scope(Checker *c) {
    Scope *scope = c->types.current_scope;
    if (!scope) return;
    for (int i = 0; i < scope->count; i++) {
        Symbol *sym = &scope->symbols[i];
        if (!sym->used && sym->line > 0 && sym->type->kind != TYPE_FN) {
            // Skip special names: self, _, loop vars, etc.
            if (strcmp(sym->name, "self") == 0) continue;
            if (sym->name[0] == '_') continue;
            check_warning(c, sym->line, sym->col, (int)strlen(sym->name),
                "unused variable '%s'", sym->name);
        }
    }
}

static void check_block(Checker *c, ASTNode *node) {
    if (!node || node->type != NODE_BLOCK) return;
    scope_push(&c->types);
    for (int i = 0; i < node->block.stmts.count; i++) {
        check_stmt(c, node->block.stmts.items[i]);
    }
    check_unused_in_scope(c);
    scope_pop(&c->types);
}

// ---- Check declarations (first pass: register, second pass: check bodies) ----

static void register_decl(Checker *c, ASTNode *node) {
    switch (node->type) {
        case NODE_STRUCT_DECL: {
            Type *t = type_new(&c->types, TYPE_STRUCT, node->struct_decl.name);
            t->struct_info.field_count = node->struct_decl.fields.count;
            t->struct_info.fields = calloc(node->struct_decl.fields.count, sizeof(TypeField));
            for (int i = 0; i < node->struct_decl.fields.count; i++) {
                t->struct_info.fields[i].name = node->struct_decl.fields.items[i].name;
                t->struct_info.fields[i].type =
                    resolve_type_node(c, node->struct_decl.fields.items[i].type);
            }
            break;
        }

        case NODE_ENUM_DECL: {
            Type *t = type_new(&c->types, TYPE_ENUM, node->enum_decl.name);
            t->enum_info.variant_count = node->enum_decl.variants.count;
            t->enum_info.variants = calloc(node->enum_decl.variants.count, sizeof(char *));
            for (int i = 0; i < node->enum_decl.variants.count; i++) {
                t->enum_info.variants[i] = node->enum_decl.variants.items[i]->ident.name;
            }
            break;
        }

        case NODE_FN_DECL: {
            // Generic functions: register as template, don't create concrete type
            if (node->fn_decl.type_params.count > 0) {
                int tpc = node->fn_decl.type_params.count;
                char **names = malloc(sizeof(char *) * tpc);
                for (int i = 0; i < tpc; i++)
                    names[i] = node->fn_decl.type_params.items[i]->type_basic.name;
                register_generic_def(&c->types, node->fn_decl.name, names, tpc, node, false);
                break;
            }
            // Build function type
            int param_count = node->fn_decl.params.count;
            Type **params = NULL;
            if (param_count > 0) {
                params = calloc(param_count, sizeof(Type *));
                for (int i = 0; i < param_count; i++) {
                    if (node->fn_decl.params.items[i].type) {
                        params[i] = resolve_type_node(c, node->fn_decl.params.items[i].type);
                    } else {
                        params[i] = c->types.t_unknown;
                    }
                }
            }
            Type *ret = node->fn_decl.return_type
                ? resolve_type_node(c, node->fn_decl.return_type)
                : c->types.t_void;

            Type *fn_type = type_new_fn(&c->types, params, param_count, ret);
            scope_define(&c->types, node->fn_decl.name, fn_type, false);
            break;
        }

        case NODE_IMPL_BLOCK: {
            register_impl(c, node->impl_block.type_name, node);
            // Register methods as TypeName_method functions
            for (int i = 0; i < node->impl_block.methods.count; i++) {
                ASTNode *method = node->impl_block.methods.items[i];
                char mangled[256];
                snprintf(mangled, sizeof(mangled), "%s_%s",
                    node->impl_block.type_name, method->fn_decl.name);

                int param_count = method->fn_decl.params.count;
                Type **params = calloc(param_count, sizeof(Type *));
                for (int j = 0; j < param_count; j++) {
                    if (strcmp(method->fn_decl.params.items[j].name, "self") == 0) {
                        params[j] = type_resolve_name(&c->types, node->impl_block.type_name);
                    } else if (method->fn_decl.params.items[j].type) {
                        params[j] = resolve_type_node(c, method->fn_decl.params.items[j].type);
                    } else {
                        params[j] = c->types.t_unknown;
                    }
                }
                Type *ret = method->fn_decl.return_type
                    ? resolve_type_node(c, method->fn_decl.return_type)
                    : c->types.t_void;

                Type *fn_type = type_new_fn(&c->types, params, param_count, ret);
                scope_define(&c->types, mangled, fn_type, false);
            }
            break;
        }

        case NODE_IMPL_TRAIT: {
            // impl Trait for Type — register methods same as impl block
            register_impl(c, node->impl_trait.type_name, node);
            for (int i = 0; i < node->impl_trait.methods.count; i++) {
                ASTNode *method = node->impl_trait.methods.items[i];
                char mangled[256];
                snprintf(mangled, sizeof(mangled), "%s_%s",
                    node->impl_trait.type_name, method->fn_decl.name);

                int param_count = method->fn_decl.params.count;
                Type **params = calloc(param_count, sizeof(Type *));
                for (int j = 0; j < param_count; j++) {
                    if (strcmp(method->fn_decl.params.items[j].name, "self") == 0) {
                        params[j] = type_resolve_name(&c->types, node->impl_trait.type_name);
                    } else if (method->fn_decl.params.items[j].type) {
                        params[j] = resolve_type_node(c, method->fn_decl.params.items[j].type);
                    } else {
                        params[j] = c->types.t_unknown;
                    }
                }
                Type *ret = method->fn_decl.return_type
                    ? resolve_type_node(c, method->fn_decl.return_type)
                    : c->types.t_void;
                Type *fn_type = type_new_fn(&c->types, params, param_count, ret);
                scope_define(&c->types, mangled, fn_type, false);
            }
            break;
        }

        case NODE_LET_STMT: {
            // Global variable
            Type *declared = NULL;
            if (node->let_stmt.type_ann) {
                declared = resolve_type_node(c, node->let_stmt.type_ann);
            }
            Type *init_type = NULL;
            if (node->let_stmt.init) {
                init_type = check_expr(c, node->let_stmt.init);
                // Propagate declared type to empty array/map literals
                if (declared && declared->kind == TYPE_ARRAY &&
                    node->let_stmt.init->type == NODE_ARRAY_LIT &&
                    node->let_stmt.init->array_lit.elements.count == 0) {
                    node->let_stmt.init->resolved_type = declared;
                    init_type = declared;
                }
                if (declared && declared->kind == TYPE_MAP &&
                    init_type && init_type->kind == TYPE_MAP) {
                    node->let_stmt.init->resolved_type = declared;
                    init_type = declared;
                }
            }
            Type *final_type = declared ? declared : (init_type ? init_type : c->types.t_unknown);
            node->resolved_type = final_type;
            scope_define(&c->types, node->let_stmt.name, final_type, node->let_stmt.is_mut);
            break;
        }

        default:
            break;
    }
}

// Check if a block definitely returns (simple analysis)
static bool block_returns(ASTNode *block) {
    if (!block || block->type != NODE_BLOCK) return false;
    if (block->block.stmts.count == 0) return false;
    ASTNode *last = block->block.stmts.items[block->block.stmts.count - 1];
    if (last->type == NODE_RETURN_STMT) return true;
    if (last->type == NODE_IF_STMT && last->if_stmt.else_block) {
        return block_returns(last->if_stmt.then_block) &&
               (last->if_stmt.else_block->type == NODE_IF_STMT
                   ? block_returns(last->if_stmt.else_block->if_stmt.then_block)
                   : block_returns(last->if_stmt.else_block));
    }
    return false;
}

static void check_fn_body(Checker *c, ASTNode *node, const char *self_struct) {
    scope_push(&c->types);

    // Track return type for this function
    Type *saved_ret = c->current_ret;
    c->current_ret = node->fn_decl.return_type
        ? resolve_type_node(c, node->fn_decl.return_type)
        : c->types.t_void;

    // Define parameters in scope (mark as used since they're inputs)
    for (int i = 0; i < node->fn_decl.params.count; i++) {
        Field *param = &node->fn_decl.params.items[i];
        Type *param_type;
        if (strcmp(param->name, "self") == 0 && self_struct) {
            param_type = type_resolve_name(&c->types, self_struct);
        } else if (param->type) {
            param_type = resolve_type_node(c, param->type);
        } else {
            param_type = c->types.t_unknown;
        }
        scope_define(&c->types, param->name, param_type, param->is_mut);
        // Mark params as used — they're function inputs
        Symbol *sym = scope_lookup(&c->types, param->name);
        if (sym) sym->used = true;
    }

    // Check recover block (if any) — has access to same scope + return type
    if (node->fn_decl.recover_block) {
        for (int i = 0; i < node->fn_decl.recover_block->block.stmts.count; i++) {
            check_stmt(c, node->fn_decl.recover_block->block.stmts.items[i]);
        }
    }

    // Check body
    if (node->fn_decl.body) {
        for (int i = 0; i < node->fn_decl.body->block.stmts.count; i++) {
            check_stmt(c, node->fn_decl.body->block.stmts.items[i]);
        }
    }

    // Check for missing return in non-void functions
    if (c->current_ret->kind != TYPE_VOID && c->current_ret->kind != TYPE_UNKNOWN) {
        if (node->fn_decl.body && !block_returns(node->fn_decl.body)) {
            // Don't warn for main()
            if (strcmp(node->fn_decl.name, "main") != 0) {
                check_warning(c, node->line, node->column, (int)strlen(node->fn_decl.name),
                    "function '%s' may not return a value on all paths",
                    node->fn_decl.name);
            }
        }
    }

    // Check for unused variables in function scope
    check_unused_in_scope(c);

    c->current_ret = saved_ret;
    scope_pop(&c->types);
}

// ---- Public API ----

void checker_check(Checker *c, ASTNode *program) {
    // First pass: register all type and function declarations
    for (int i = 0; i < program->program.decls.count; i++) {
        register_decl(c, program->program.decls.items[i]);
    }

    // Second pass: check function bodies
    for (int i = 0; i < program->program.decls.count; i++) {
        ASTNode *decl = program->program.decls.items[i];
        if (decl->type == NODE_FN_DECL) {
            check_fn_body(c, decl, NULL);
        } else if (decl->type == NODE_IMPL_BLOCK) {
            for (int j = 0; j < decl->impl_block.methods.count; j++) {
                check_fn_body(c, decl->impl_block.methods.items[j],
                    decl->impl_block.type_name);
            }
        } else if (decl->type == NODE_IMPL_TRAIT) {
            for (int j = 0; j < decl->impl_trait.methods.count; j++) {
                check_fn_body(c, decl->impl_trait.methods.items[j],
                    decl->impl_trait.type_name);
            }
        } else if (decl->type == NODE_TEST_DECL) {
            check_block(c, decl->test_decl.body);
        }
    }

    // Error reporting now handled by main via DiagContext
}

TypeTable *checker_get_types(Checker *c) {
    return &c->types;
}
