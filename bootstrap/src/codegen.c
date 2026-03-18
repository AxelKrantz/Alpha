#include "codegen.h"
#include "types.h"
#include "error.h"
#include <string.h>

void codegen_init(CodeGen *gen, FILE *out) {
    gen->out = out;
    gen->indent = 0;
    gen->current_struct = NULL;
    gen->test_mode = false;
    gen->strip_contracts = false;
    gen->current_ensures = NULL;
    gen->current_fn_name = NULL;
    gen->current_invariants = NULL;
    gen->invariant_fields = NULL;
    gen->defer_stack = NULL;
    gen->defer_count = 0;
    gen->defer_cap = 0;
    gen->owned_vars = NULL;
    gen->owned_count = 0;
    gen->owned_cap = 0;
    gen->scope_depth = 0;
    gen->subst_param_names = NULL;
    gen->subst_concrete = NULL;
    gen->subst_count = 0;
    gen->type_table = NULL;
    gen->lambda_count = 0;
    gen->lambdas = NULL;
    gen->lambda_cap = 0;
}

static void emit_indent(CodeGen *gen) {
    for (int i = 0; i < gen->indent; i++) {
        fprintf(gen->out, "    ");
    }
}

static void emit_type(CodeGen *gen, ASTNode *type);
static void emit_expr(CodeGen *gen, ASTNode *node);
static void emit_stmt(CodeGen *gen, ASTNode *node);
static void emit_if(CodeGen *gen, ASTNode *node);
static void emit_decl(CodeGen *gen, ASTNode *node);

// ---- Helpers ----

// Emit a string value re-escaped for C
static void emit_c_string(FILE *out, const char *s);

// ---- Smart assert helpers ----

// Pretty-print an Alpha expression to a string (for assert error messages)
static void expr_pretty(FILE *f, ASTNode *node) {
    if (!node) return;
    switch (node->type) {
        case NODE_INT_LIT:
            fprintf(f, "%lld", (long long)node->int_lit.value);
            break;
        case NODE_FLOAT_LIT:
            fprintf(f, "%g", node->float_lit.value);
            break;
        case NODE_STRING_LIT:
            fprintf(f, "\"");
            emit_c_string(f, node->string_lit.value);
            fprintf(f, "\"");
            break;
        case NODE_BOOL_LIT:
            fprintf(f, "%s", node->bool_lit.value ? "true" : "false");
            break;
        case NODE_IDENT:
            fprintf(f, "%s", node->ident.name);
            break;
        case NODE_BINARY_EXPR:
            expr_pretty(f, node->binary.left);
            fprintf(f, " %s ", token_type_name(node->binary.op));
            expr_pretty(f, node->binary.right);
            break;
        case NODE_UNARY_EXPR:
            fprintf(f, "%s", token_type_name(node->unary.op));
            expr_pretty(f, node->unary.operand);
            break;
        case NODE_CALL_EXPR:
            expr_pretty(f, node->call.callee);
            fprintf(f, "(");
            for (int i = 0; i < node->call.args.count; i++) {
                if (i > 0) fprintf(f, ", ");
                expr_pretty(f, node->call.args.items[i]);
            }
            fprintf(f, ")");
            break;
        case NODE_FIELD_ACCESS:
            expr_pretty(f, node->field_access.object);
            fprintf(f, ".%s", node->field_access.field);
            break;
        case NODE_METHOD_CALL:
            expr_pretty(f, node->method_call.object);
            fprintf(f, ".%s(", node->method_call.method);
            for (int i = 0; i < node->method_call.args.count; i++) {
                if (i > 0) fprintf(f, ", ");
                expr_pretty(f, node->method_call.args.items[i]);
            }
            fprintf(f, ")");
            break;
        case NODE_INDEX_EXPR:
            expr_pretty(f, node->index_expr.object);
            fprintf(f, "[");
            expr_pretty(f, node->index_expr.index);
            fprintf(f, "]");
            break;
        case NODE_NONE_LIT:
            fprintf(f, "None");
            break;
        default:
            fprintf(f, "...");
            break;
    }
}

static char *expr_to_cstring(CodeGen *gen, ASTNode *node) {
    (void)gen;
    char *buf = NULL;
    size_t len = 0;
    FILE *mem = open_memstream(&buf, &len);
    expr_pretty(mem, node);
    fclose(mem);
    return buf;
}

// Escape a string for use inside a C string literal (handles ", \, %)
static char *escape_c_string(const char *s) {
    int len = (int)strlen(s);
    // Worst case: every char needs escaping
    char *out = malloc(len * 3 + 1);
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] == '"') { out[j++] = '\\'; out[j++] = '"'; }
        else if (s[i] == '\\') { out[j++] = '\\'; out[j++] = '\\'; }
        else if (s[i] == '%') { out[j++] = '%'; out[j++] = '%'; }
        else if (s[i] == '\n') { out[j++] = '\\'; out[j++] = 'n'; }
        else { out[j++] = s[i]; }
    }
    out[j] = '\0';
    return out;
}

// Emit a string value re-escaped for C
static void emit_c_string(FILE *out, const char *s) {
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '\n': fprintf(out, "\\n"); break;
            case '\t': fprintf(out, "\\t"); break;
            case '\r': fprintf(out, "\\r"); break;
            case '\\': fprintf(out, "\\\\"); break;
            case '"':  fprintf(out, "\\\""); break;
            default:   fputc(*p, out); break;
        }
    }
}

// Track a variable that owns heap memory (auto-freed at scope exit)
static void track_owned(CodeGen *gen, const char *name, const char *free_fn) {
    if (gen->owned_count >= gen->owned_cap) {
        gen->owned_cap = gen->owned_cap ? gen->owned_cap * 2 : 16;
        gen->owned_vars = realloc(gen->owned_vars, sizeof(gen->owned_vars[0]) * gen->owned_cap);
    }
    gen->owned_vars[gen->owned_count].name = strdup(name);
    gen->owned_vars[gen->owned_count].free_fn = free_fn;
    gen->owned_vars[gen->owned_count].scope_depth = gen->scope_depth;
    gen->owned_count++;
}

// Emit cleanup for owned variables at or below a given scope depth
static void emit_auto_cleanup_depth(CodeGen *gen, const char *skip_name, int max_depth) {
    for (int i = gen->owned_count - 1; i >= 0; i--) {
        if (gen->owned_vars[i].scope_depth > max_depth) continue;
        if (skip_name && strcmp(gen->owned_vars[i].name, skip_name) == 0) continue;
        emit_indent(gen);
        fprintf(gen->out, "%s(&%s);\n", gen->owned_vars[i].free_fn, gen->owned_vars[i].name);
    }
}

static void emit_auto_cleanup(CodeGen *gen, const char *skip_name) {
    // Only clean up variables at the function's top scope (depth 0)
    emit_auto_cleanup_depth(gen, skip_name, 0);
}

// Get the name of a simple return expression (for skip-on-return)
static const char *get_return_var_name(ASTNode *expr) {
    if (!expr) return NULL;
    if (expr->type == NODE_IDENT) return expr->ident.name;
    return NULL;
}

// Emit all deferred statements (in reverse order, like Go)
static void emit_defers(CodeGen *gen) {
    for (int i = gen->defer_count - 1; i >= 0; i--) {
        emit_stmt(gen, gen->defer_stack[i]);
    }
}

static bool is_comparison_op(TokenType op) {
    return op == TOK_EQ || op == TOK_NEQ ||
           op == TOK_LT || op == TOK_GT ||
           op == TOK_LEQ || op == TOK_GEQ;
}

static const char *comparison_op_str(TokenType op) {
    switch (op) {
        case TOK_EQ:  return "==";
        case TOK_NEQ: return "!=";
        case TOK_LT:  return "<";
        case TOK_GT:  return ">";
        case TOK_LEQ: return "<=";
        case TOK_GEQ: return ">=";
        default:      return "?";
    }
}

static bool expr_is_string(ASTNode *node) {
    if (node->type == NODE_STRING_LIT) return true;
    if (node->resolved_type && node->resolved_type->kind == TYPE_STR) return true;
    return false;
}

// ---- Type emission ----

static const char *map_basic_type(const char *name) {
    if (strcmp(name, "i8") == 0)   return "int8_t";
    if (strcmp(name, "i16") == 0)  return "int16_t";
    if (strcmp(name, "i32") == 0)  return "int32_t";
    if (strcmp(name, "i64") == 0)  return "int64_t";
    if (strcmp(name, "u8") == 0)   return "uint8_t";
    if (strcmp(name, "u16") == 0)  return "uint16_t";
    if (strcmp(name, "u32") == 0)  return "uint32_t";
    if (strcmp(name, "u64") == 0)  return "uint64_t";
    if (strcmp(name, "f32") == 0)  return "float";
    if (strcmp(name, "f64") == 0)  return "double";
    if (strcmp(name, "bool") == 0) return "bool";
    if (strcmp(name, "str") == 0)  return "const char*";
    if (strcmp(name, "void") == 0) return "void";
    return name; // struct types pass through
}

static void emit_type(CodeGen *gen, ASTNode *type) {
    if (!type) {
        fprintf(gen->out, "void");
        return;
    }

    switch (type->type) {
        case NODE_TYPE_BASIC:
            // Check if this is a type parameter being substituted
            if (gen->subst_count > 0) {
                for (int si = 0; si < gen->subst_count; si++) {
                    if (strcmp(type->type_basic.name, gen->subst_param_names[si]) == 0) {
                        fprintf(gen->out, "%s", type_to_c(gen->subst_concrete[si]));
                        goto type_done;
                    }
                }
            }
            fprintf(gen->out, "%s", map_basic_type(type->type_basic.name));
            type_done:
            break;

        case NODE_TYPE_REF:
            emit_type(gen, type->type_ref.inner);
            fprintf(gen->out, "*");
            break;

        case NODE_TYPE_ARRAY: {
            // Map to AlphaArr_suffix for dynamic arrays
            if (type->type_array.element_type && type->type_array.element_type->type == NODE_TYPE_BASIC) {
                const char *elem_name = type->type_array.element_type->type_basic.name;
                // Check for type parameter substitution
                if (gen->subst_count > 0) {
                    for (int si = 0; si < gen->subst_count; si++) {
                        if (strcmp(elem_name, gen->subst_param_names[si]) == 0) {
                            elem_name = type_array_suffix(gen->subst_concrete[si]);
                            break;
                        }
                    }
                }
                const char *suffix = "i64";
                if (strcmp(elem_name, "f64") == 0 || strcmp(elem_name, "f32") == 0) suffix = "f64";
                else if (strcmp(elem_name, "str") == 0) suffix = "str";
                else if (strcmp(elem_name, "bool") == 0) suffix = "bool";
                else if (strcmp(elem_name, "u8") == 0) suffix = "u8";
                else if (elem_name[0] >= 'A' && elem_name[0] <= 'Z') suffix = elem_name;
                else suffix = elem_name; // already substituted
                fprintf(gen->out, "AlphaArr_%s", suffix);
            } else {
                fprintf(gen->out, "AlphaArr_i64");
            }
            break;
        }

        case NODE_TYPE_GENERIC:
            if (strcmp(type->type_generic.name, "Option") == 0 && type->type_generic.type_args.count > 0) {
                ASTNode *inner_node = type->type_generic.type_args.items[0];
                const char *suffix = "i64";
                if (inner_node->type == NODE_TYPE_BASIC) {
                    const char *n = inner_node->type_basic.name;
                    if (strcmp(n, "f64") == 0) suffix = "f64";
                    else if (strcmp(n, "str") == 0) suffix = "str";
                    else if (strcmp(n, "bool") == 0) suffix = "bool";
                }
                fprintf(gen->out, "AlphaOpt_%s", suffix);
                break;
            }
            if (strcmp(type->type_generic.name, "Map") == 0 && type->type_generic.type_args.count > 0) {
                // Resolve value type suffix
                ASTNode *val_node = type->type_generic.type_args.items[0];
                const char *suffix = "str";
                if (val_node->type == NODE_TYPE_BASIC) {
                    const char *n = val_node->type_basic.name;
                    if (strcmp(n, "i64") == 0 || strcmp(n, "i32") == 0) suffix = "i64";
                    else if (strcmp(n, "f64") == 0) suffix = "f64";
                    else if (strcmp(n, "bool") == 0) suffix = "bool";
                    else if (strcmp(n, "str") == 0) suffix = "str";
                }
                fprintf(gen->out, "AlphaMap_%s", suffix);
            } else {
                fprintf(gen->out, "%s", map_basic_type(type->type_generic.name));
            }
            break;

        default:
            fprintf(gen->out, "/* unknown type */");
            break;
    }
}

// ---- Expression emission ----

static bool is_builtin_fn(const char *name) {
    return strcmp(name, "print") == 0 ||
           strcmp(name, "println") == 0 ||
           strcmp(name, "len") == 0 ||
           strcmp(name, "exit") == 0 ||
           strcmp(name, "assert") == 0 ||
           strcmp(name, "str_eq") == 0 ||
           strcmp(name, "str_concat") == 0 ||
           strcmp(name, "str_substr") == 0 ||
           strcmp(name, "str_char_at") == 0 ||
           strcmp(name, "str_from_int") == 0 ||
           strcmp(name, "str_contains") == 0 ||
           strcmp(name, "str_starts_with") == 0 ||
           strcmp(name, "char_to_str") == 0 ||
           strcmp(name, "file_read") == 0 ||
           strcmp(name, "file_write") == 0 ||
           strcmp(name, "i64_to_str") == 0 ||
           strcmp(name, "str_to_i64") == 0 ||
           strcmp(name, "sqrt") == 0 ||
           strcmp(name, "alloc") == 0 ||
           strcmp(name, "args_count") == 0 ||
           strcmp(name, "args_get") == 0 ||
           strcmp(name, "run_command") == 0 ||
           strcmp(name, "sb_new") == 0 ||
           strcmp(name, "sb_append") == 0 ||
           strcmp(name, "sb_append_char") == 0 ||
           strcmp(name, "sb_to_str") == 0 ||
           strcmp(name, "eprintln") == 0 ||
           strcmp(name, "env_get") == 0 ||
           strcmp(name, "panic") == 0 ||
           strcmp(name, "format") == 0 ||
           strcmp(name, "map_new") == 0 ||
           strcmp(name, "some") == 0 ||
           strcmp(name, "as_i64") == 0 ||
           strcmp(name, "as_f64") == 0 ||
           strcmp(name, "as_i32") == 0 ||
           strcmp(name, "as_u8") == 0 ||
           strcmp(name, "as_bool") == 0 ||
           strcmp(name, "assert_eq") == 0 ||
           strcmp(name, "assert_neq") == 0 ||
           strcmp(name, "assert_lt") == 0 ||
           strcmp(name, "assert_gt") == 0;
}

// Emit a smart assertion — decomposes comparisons, shows values on failure
static void emit_smart_assert(CodeGen *gen, ASTNode *expr) {
    int assert_line = expr->line;

    if (expr->type == NODE_BINARY_EXPR && is_comparison_op(expr->binary.op)) {
        char *left_c = expr_to_cstring(gen, expr->binary.left);
        char *right_c = expr_to_cstring(gen, expr->binary.right);
        char *left_esc = escape_c_string(left_c);
        char *right_esc = escape_c_string(right_c);
        const char *op = comparison_op_str(expr->binary.op);
        bool is_str = expr_is_string(expr->binary.left);

        fprintf(gen->out, "do { ");
        fprintf(gen->out, "alpha_test_assertions++; ");
        fprintf(gen->out, "__auto_type _al = (");
        emit_expr(gen, expr->binary.left);
        fprintf(gen->out, "); __auto_type _ar = (");
        emit_expr(gen, expr->binary.right);
        fprintf(gen->out, "); ");

        if (is_str && (expr->binary.op == TOK_EQ || expr->binary.op == TOK_NEQ)) {
            fprintf(gen->out, "if (!(strcmp(_al, _ar) %s 0)) { ", op);
        } else {
            fprintf(gen->out, "if (!(_al %s _ar)) { ", op);
        }

        fprintf(gen->out, "alpha_test_current_failed = 1; ");
        fprintf(gen->out, "fprintf(stderr, \"  FAIL  %%s:%%d: assert(%s %s %s)\\n\", alpha_test_file, %d); ",
                left_esc, op, right_esc, assert_line);
        fprintf(gen->out, "fprintf(stderr, \"         left:  \"); alpha_assert_print(_al); fprintf(stderr, \"\\n\"); ");
        fprintf(gen->out, "fprintf(stderr, \"         right: \"); alpha_assert_print(_ar); fprintf(stderr, \"\\n\"); ");
        fprintf(gen->out, "if (alpha_test_json_buf) { ");
        fprintf(gen->out, "if (alpha_test_json_buf->len > 0) sb_append(alpha_test_json_buf, \", \"); ");
        fprintf(gen->out, "sb_append(alpha_test_json_buf, \"{\\\"line\\\": \"); ");
        fprintf(gen->out, "alpha_sprint_i64(alpha_test_json_buf, %d); ", assert_line);
        fprintf(gen->out, "sb_append(alpha_test_json_buf, \", \\\"expr\\\": \\\"%s %s %s\\\"\"); ", left_esc, op, right_esc);
        fprintf(gen->out, "sb_append(alpha_test_json_buf, \", \\\"left\\\": \"); ");
        fprintf(gen->out, "alpha_sprint_val(alpha_test_json_buf, _al); ");
        fprintf(gen->out, "sb_append(alpha_test_json_buf, \", \\\"right\\\": \"); ");
        fprintf(gen->out, "alpha_sprint_val(alpha_test_json_buf, _ar); ");
        fprintf(gen->out, "sb_append(alpha_test_json_buf, \"}\"); } ");
        fprintf(gen->out, "} } while(0)");

        free(left_c); free(right_c); free(left_esc); free(right_esc);
    } else {
        char *expr_c = expr_to_cstring(gen, expr);
        char *expr_esc = escape_c_string(expr_c);

        fprintf(gen->out, "do { ");
        fprintf(gen->out, "alpha_test_assertions++; ");
        fprintf(gen->out, "if (!(");
        emit_expr(gen, expr);
        fprintf(gen->out, ")) { ");
        fprintf(gen->out, "alpha_test_current_failed = 1; ");
        fprintf(gen->out, "fprintf(stderr, \"  FAIL  %%s:%%d: assert(%s) was false\\n\", alpha_test_file, %d); ",
                expr_esc, assert_line);
        fprintf(gen->out, "if (alpha_test_json_buf) { ");
        fprintf(gen->out, "if (alpha_test_json_buf->len > 0) sb_append(alpha_test_json_buf, \", \"); ");
        fprintf(gen->out, "sb_append(alpha_test_json_buf, \"{\\\"line\\\": \"); ");
        fprintf(gen->out, "alpha_sprint_i64(alpha_test_json_buf, %d); ", assert_line);
        fprintf(gen->out, "sb_append(alpha_test_json_buf, \", \\\"expr\\\": \\\"%s\\\"}\"); } ", expr_esc);
        fprintf(gen->out, "} } while(0)");

        free(expr_c); free(expr_esc);
    }
}

static void emit_builtin_call(CodeGen *gen, const char *name, NodeList *args) {
    if (strcmp(name, "print") == 0 || strcmp(name, "println") == 0) {
        bool newline = strcmp(name, "println") == 0;
        if (args->count == 0) {
            if (newline) fprintf(gen->out, "printf(\"\\n\")");
            return;
        }
        // Check first arg type for format string
        ASTNode *arg = args->items[0];
        if (arg->type == NODE_STRING_LIT) {
            if (args->count == 1) {
                fprintf(gen->out, "printf(\"%%s%s\", \"", newline ? "\\n" : "");
                emit_c_string(gen->out, arg->string_lit.value);
                fprintf(gen->out, "\")");
            } else {
                // String with format args
                fprintf(gen->out, "printf(\"");
                emit_c_string(gen->out, arg->string_lit.value);
                fprintf(gen->out, "%s\"", newline ? "\\n" : "");
                for (int i = 1; i < args->count; i++) {
                    fprintf(gen->out, ", ");
                    emit_expr(gen, args->items[i]);
                }
                fprintf(gen->out, ")");
            }
        } else if (arg->type == NODE_INT_LIT) {
            fprintf(gen->out, "printf(\"%%lld%s\", (long long)", newline ? "\\n" : "");
            emit_expr(gen, arg);
            fprintf(gen->out, ")");
        } else if (arg->type == NODE_FLOAT_LIT) {
            fprintf(gen->out, "printf(\"%%f%s\", ", newline ? "\\n" : "");
            emit_expr(gen, arg);
            fprintf(gen->out, ")");
        } else if (arg->type == NODE_BOOL_LIT) {
            fprintf(gen->out, "printf(\"%%s%s\", ", newline ? "\\n" : "");
            emit_expr(gen, arg);
            fprintf(gen->out, " ? \"true\" : \"false\")");
        } else {
            // Use resolved type if available, otherwise fall back to _Generic
            if (arg->resolved_type && arg->resolved_type->kind != TYPE_UNKNOWN) {
                const char *fmt;
                bool cast_needed = false;
                switch (arg->resolved_type->kind) {
                    case TYPE_F32:
                    case TYPE_F64:  fmt = "%g"; break;
                    case TYPE_STR:  fmt = "%s"; break;
                    case TYPE_BOOL:
                        fprintf(gen->out, "printf(\"%%s%s\", ", newline ? "\\n" : "");
                        emit_expr(gen, arg);
                        fprintf(gen->out, " ? \"true\" : \"false\")");
                        return;
                    default:        fmt = "%lld"; cast_needed = true; break;
                }
                fprintf(gen->out, "printf(\"%s%s\", ", fmt, newline ? "\\n" : "");
                if (cast_needed) fprintf(gen->out, "(long long)");
                emit_expr(gen, arg);
                fprintf(gen->out, ")");
            } else {
                fprintf(gen->out, "alpha_print(");
                emit_expr(gen, arg);
                fprintf(gen->out, ", %s)", newline ? "1" : "0");
            }
        }
        return;
    }

    if (strcmp(name, "len") == 0) {
        if (args->count == 1 && args->items[0]->type == NODE_STRING_LIT) {
            fprintf(gen->out, "strlen(");
            emit_expr(gen, args->items[0]);
            fprintf(gen->out, ")");
        } else {
            fprintf(gen->out, "strlen(");
            emit_expr(gen, args->items[0]);
            fprintf(gen->out, ")");
        }
        return;
    }

    if (strcmp(name, "exit") == 0) {
        fprintf(gen->out, "exit(");
        if (args->count > 0) emit_expr(gen, args->items[0]);
        else fprintf(gen->out, "0");
        fprintf(gen->out, ")");
        return;
    }

    if (strcmp(name, "panic") == 0) {
        if (args->count > 0 && args->items[0]->type == NODE_STRING_LIT) {
            fprintf(gen->out, "__alpha_panic(\"");
            emit_c_string(gen->out, args->items[0]->string_lit.value);
            fprintf(gen->out, "\")");
        } else if (args->count > 0) {
            fprintf(gen->out, "__alpha_panic(");
            emit_expr(gen, args->items[0]);
            fprintf(gen->out, ")");
        } else {
            fprintf(gen->out, "__alpha_panic(\"panic\")");
        }
        return;
    }
    if (strcmp(name, "env_get") == 0 && args->count == 1) {
        fprintf(gen->out, "(getenv(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ") ? getenv(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ") : \"\")");
        return;
    }
    // Type casts
    if (strcmp(name, "as_i64") == 0 && args->count == 1) {
        fprintf(gen->out, "((int64_t)("); emit_expr(gen, args->items[0]); fprintf(gen->out, "))");
        return;
    }
    if (strcmp(name, "as_f64") == 0 && args->count == 1) {
        fprintf(gen->out, "((double)("); emit_expr(gen, args->items[0]); fprintf(gen->out, "))");
        return;
    }
    if (strcmp(name, "as_i32") == 0 && args->count == 1) {
        fprintf(gen->out, "((int32_t)("); emit_expr(gen, args->items[0]); fprintf(gen->out, "))");
        return;
    }
    if (strcmp(name, "as_u8") == 0 && args->count == 1) {
        fprintf(gen->out, "((uint8_t)("); emit_expr(gen, args->items[0]); fprintf(gen->out, "))");
        return;
    }
    if (strcmp(name, "as_bool") == 0 && args->count == 1) {
        fprintf(gen->out, "((bool)("); emit_expr(gen, args->items[0]); fprintf(gen->out, "))");
        return;
    }

    // some() and map_new are handled directly in emit_expr for NODE_CALL_EXPR

    if (strcmp(name, "format") == 0 && args->count >= 1) {
        // format("hello {}, age {}", name, age) -> alpha_format("hello %s, age %lld", name, age)
        // Parse the format string at compile time and replace {} with C format specifiers
        ASTNode *fmt_arg = args->items[0];
        if (fmt_arg->type == NODE_STRING_LIT) {
            const char *fmt = fmt_arg->string_lit.value;
            // Build C format string by replacing {} with type-appropriate specifiers
            fprintf(gen->out, "alpha_format(\"");
            int arg_idx = 1;
            for (const char *p = fmt; *p; p++) {
                if (*p == '{' && *(p+1) == '}') {
                    // Substitute format specifier based on argument type
                    if (arg_idx < args->count) {
                        Type *at = args->items[arg_idx]->resolved_type;
                        if (at && (at->kind == TYPE_F64 || at->kind == TYPE_F32))
                            fprintf(gen->out, "%%g");
                        else if (at && at->kind == TYPE_STR)
                            fprintf(gen->out, "%%s");
                        else if (at && at->kind == TYPE_BOOL)
                            fprintf(gen->out, "%%s");
                        else
                            fprintf(gen->out, "%%lld");
                    }
                    arg_idx++;
                    p++; // skip }
                } else if (*p == '"') {
                    fprintf(gen->out, "\\\"");
                } else if (*p == '\\') {
                    fprintf(gen->out, "\\\\");
                } else if (*p == '%') {
                    fprintf(gen->out, "%%%%");
                } else {
                    fputc(*p, gen->out);
                }
            }
            fprintf(gen->out, "\"");
            for (int i = 1; i < args->count; i++) {
                fprintf(gen->out, ", ");
                Type *at = args->items[i]->resolved_type;
                if (at && at->kind == TYPE_BOOL) {
                    fprintf(gen->out, "(");
                    emit_expr(gen, args->items[i]);
                    fprintf(gen->out, ") ? \"true\" : \"false\"");
                } else if (at && type_is_integer(at)) {
                    fprintf(gen->out, "(long long)(");
                    emit_expr(gen, args->items[i]);
                    fprintf(gen->out, ")");
                } else {
                    emit_expr(gen, args->items[i]);
                }
            }
            fprintf(gen->out, ")");
        } else {
            // Dynamic format string — just pass through
            fprintf(gen->out, "alpha_format(");
            emit_expr(gen, fmt_arg);
            for (int i = 1; i < args->count; i++) {
                fprintf(gen->out, ", ");
                emit_expr(gen, args->items[i]);
            }
            fprintf(gen->out, ")");
        }
        return;
    }

    if (strcmp(name, "assert") == 0 && args->count > 0) {
        emit_smart_assert(gen, args->items[0]);
        return;
    }

    // Legacy assertion builtins — forward to smart assert with synthetic expression
    if (strcmp(name, "assert_eq") == 0 && args->count == 2) {
        ASTNode *syn = ast_new(NODE_BINARY_EXPR, args->items[0]->line, args->items[0]->column);
        syn->binary.op = TOK_EQ; syn->binary.left = args->items[0]; syn->binary.right = args->items[1];
        emit_smart_assert(gen, syn);
        return;
    }
    if (strcmp(name, "assert_neq") == 0 && args->count == 2) {
        ASTNode *syn = ast_new(NODE_BINARY_EXPR, args->items[0]->line, args->items[0]->column);
        syn->binary.op = TOK_NEQ; syn->binary.left = args->items[0]; syn->binary.right = args->items[1];
        emit_smart_assert(gen, syn);
        return;
    }
    if (strcmp(name, "assert_lt") == 0 && args->count == 2) {
        ASTNode *syn = ast_new(NODE_BINARY_EXPR, args->items[0]->line, args->items[0]->column);
        syn->binary.op = TOK_LT; syn->binary.left = args->items[0]; syn->binary.right = args->items[1];
        emit_smart_assert(gen, syn);
        return;
    }
    if (strcmp(name, "assert_gt") == 0 && args->count == 2) {
        ASTNode *syn = ast_new(NODE_BINARY_EXPR, args->items[0]->line, args->items[0]->column);
        syn->binary.op = TOK_GT; syn->binary.left = args->items[0]; syn->binary.right = args->items[1];
        emit_smart_assert(gen, syn);
        return;
    }

    if (strcmp(name, "str_eq") == 0 && args->count == 2) {
        fprintf(gen->out, "(strcmp(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ", ");
        emit_expr(gen, args->items[1]);
        fprintf(gen->out, ") == 0)");
        return;
    }

    if (strcmp(name, "str_concat") == 0 && args->count == 2) {
        fprintf(gen->out, "alpha_str_concat(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ", ");
        emit_expr(gen, args->items[1]);
        fprintf(gen->out, ")");
        return;
    }

    if (strcmp(name, "str_substr") == 0 && args->count == 3) {
        fprintf(gen->out, "alpha_str_substr(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ", ");
        emit_expr(gen, args->items[1]);
        fprintf(gen->out, ", ");
        emit_expr(gen, args->items[2]);
        fprintf(gen->out, ")");
        return;
    }

    if (strcmp(name, "str_char_at") == 0 && args->count == 2) {
        fprintf(gen->out, "(int64_t)((unsigned char)(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ")[");
        emit_expr(gen, args->items[1]);
        fprintf(gen->out, "])");
        return;
    }

    if (strcmp(name, "str_contains") == 0 && args->count == 2) {
        fprintf(gen->out, "(strstr(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ", ");
        emit_expr(gen, args->items[1]);
        fprintf(gen->out, ") != NULL)");
        return;
    }

    if (strcmp(name, "str_starts_with") == 0 && args->count == 2) {
        fprintf(gen->out, "(strncmp(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ", ");
        emit_expr(gen, args->items[1]);
        fprintf(gen->out, ", strlen(");
        emit_expr(gen, args->items[1]);
        fprintf(gen->out, ")) == 0)");
        return;
    }

    if (strcmp(name, "char_to_str") == 0 && args->count == 1) {
        fprintf(gen->out, "alpha_char_to_str(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ")");
        return;
    }

    if (strcmp(name, "i64_to_str") == 0 && args->count == 1) {
        fprintf(gen->out, "alpha_i64_to_str(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ")");
        return;
    }

    if (strcmp(name, "str_to_i64") == 0 && args->count == 1) {
        fprintf(gen->out, "strtoll(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ", NULL, 10)");
        return;
    }

    if (strcmp(name, "file_read") == 0 && args->count == 1) {
        fprintf(gen->out, "alpha_file_read(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ")");
        return;
    }

    if (strcmp(name, "file_write") == 0 && args->count == 2) {
        fprintf(gen->out, "alpha_file_write(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ", ");
        emit_expr(gen, args->items[1]);
        fprintf(gen->out, ")");
        return;
    }

    if (strcmp(name, "sqrt") == 0 && args->count == 1) {
        fprintf(gen->out, "sqrt(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ")");
        return;
    }

    if (strcmp(name, "alloc") == 0 && args->count == 1) {
        fprintf(gen->out, "calloc(1, ");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ")");
        return;
    }

    if (strcmp(name, "args_count") == 0) {
        fprintf(gen->out, "args_count()");
        return;
    }

    if (strcmp(name, "args_get") == 0 && args->count == 1) {
        fprintf(gen->out, "args_get(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ")");
        return;
    }

    if (strcmp(name, "run_command") == 0 && args->count == 1) {
        fprintf(gen->out, "run_command(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ")");
        return;
    }

    if (strcmp(name, "sb_new") == 0) {
        fprintf(gen->out, "sb_new()");
        return;
    }

    if (strcmp(name, "sb_append") == 0 && args->count == 2) {
        fprintf(gen->out, "sb_append(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ", ");
        emit_expr(gen, args->items[1]);
        fprintf(gen->out, ")");
        return;
    }

    if (strcmp(name, "sb_append_char") == 0 && args->count == 2) {
        fprintf(gen->out, "sb_append_char(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ", (char)");
        emit_expr(gen, args->items[1]);
        fprintf(gen->out, ")");
        return;
    }

    if (strcmp(name, "sb_to_str") == 0 && args->count == 1) {
        fprintf(gen->out, "sb_to_str(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ")");
        return;
    }

    if (strcmp(name, "eprintln") == 0 && args->count >= 1) {
        ASTNode *arg = args->items[0];
        if (arg->type == NODE_STRING_LIT) {
            fprintf(gen->out, "fprintf(stderr, \"%%s\\n\", \"");
            emit_c_string(gen->out, arg->string_lit.value);
            fprintf(gen->out, "\")");
        } else {
            fprintf(gen->out, "fprintf(stderr, \"%%s\\n\", ");
            emit_expr(gen, arg);
            fprintf(gen->out, ")");
        }
        return;
    }

    // Test assertion builtins
    if (strcmp(name, "assert_eq") == 0 && args->count == 2) {
        fprintf(gen->out, "alpha_assert_eq(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ", ");
        emit_expr(gen, args->items[1]);
        fprintf(gen->out, ", \"%s\", %d)", gen->current_struct ? gen->current_struct : "", args->items[0]->line);
        return;
    }
    if (strcmp(name, "assert_neq") == 0 && args->count == 2) {
        fprintf(gen->out, "alpha_assert_neq(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ", ");
        emit_expr(gen, args->items[1]);
        fprintf(gen->out, ", \"%s\", %d)", gen->current_struct ? gen->current_struct : "", args->items[0]->line);
        return;
    }
    if (strcmp(name, "assert_lt") == 0 && args->count == 2) {
        fprintf(gen->out, "alpha_assert_lt(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ", ");
        emit_expr(gen, args->items[1]);
        fprintf(gen->out, ", \"%s\", %d)", gen->current_struct ? gen->current_struct : "", args->items[0]->line);
        return;
    }
    if (strcmp(name, "assert_gt") == 0 && args->count == 2) {
        fprintf(gen->out, "alpha_assert_gt(");
        emit_expr(gen, args->items[0]);
        fprintf(gen->out, ", ");
        emit_expr(gen, args->items[1]);
        fprintf(gen->out, ", \"%s\", %d)", gen->current_struct ? gen->current_struct : "", args->items[0]->line);
        return;
    }
}

static void emit_expr(CodeGen *gen, ASTNode *node) {
    switch (node->type) {
        case NODE_INT_LIT:
            fprintf(gen->out, "%lldLL", (long long)node->int_lit.value);
            break;

        case NODE_FLOAT_LIT:
            fprintf(gen->out, "%f", node->float_lit.value);
            break;

        case NODE_STRING_LIT: {
            // Re-escape for C output
            fprintf(gen->out, "\"");
            for (const char *p = node->string_lit.value; *p; p++) {
                switch (*p) {
                    case '\n': fprintf(gen->out, "\\n"); break;
                    case '\t': fprintf(gen->out, "\\t"); break;
                    case '\r': fprintf(gen->out, "\\r"); break;
                    case '\\': fprintf(gen->out, "\\\\"); break;
                    case '"':  fprintf(gen->out, "\\\""); break;
                    case '\0': fprintf(gen->out, "\\0"); break;
                    default:   fputc(*p, gen->out); break;
                }
            }
            fprintf(gen->out, "\"");
            break;
        }

        case NODE_BOOL_LIT:
            fprintf(gen->out, "%s", node->bool_lit.value ? "true" : "false");
            break;

        case NODE_NONE_LIT:
            // If the resolved type is Option, emit typed none
            if (node->resolved_type && node->resolved_type->kind == TYPE_OPTION) {
                const char *suffix = node->resolved_type->option_info.inner_type
                    ? type_array_suffix(node->resolved_type->option_info.inner_type) : "i64";
                fprintf(gen->out, "AlphaOpt_%s_none()", suffix);
            } else {
                fprintf(gen->out, "NULL");
            }
            break;

        case NODE_IDENT:
            fprintf(gen->out, "%s", node->ident.name);
            break;

        case NODE_BINARY_EXPR:
            // Special case: string == and != use strcmp
            if ((node->binary.op == TOK_EQ || node->binary.op == TOK_NEQ) &&
                ((node->binary.left->resolved_type &&
                  node->binary.left->resolved_type->kind == TYPE_STR) ||
                 node->binary.left->type == NODE_STRING_LIT)) {
                fprintf(gen->out, "(strcmp(");
                emit_expr(gen, node->binary.left);
                fprintf(gen->out, ", ");
                emit_expr(gen, node->binary.right);
                fprintf(gen->out, ") %s 0)", node->binary.op == TOK_EQ ? "==" : "!=");
                break;
            }
            // Special case: string + string = concatenation
            if (node->binary.op == TOK_PLUS &&
                ((node->binary.left->resolved_type &&
                  node->binary.left->resolved_type->kind == TYPE_STR) ||
                 node->binary.left->type == NODE_STRING_LIT)) {
                fprintf(gen->out, "alpha_str_concat(");
                emit_expr(gen, node->binary.left);
                fprintf(gen->out, ", ");
                emit_expr(gen, node->binary.right);
                fprintf(gen->out, ")");
                break;
            }
            fprintf(gen->out, "(");
            emit_expr(gen, node->binary.left);
            switch (node->binary.op) {
                case TOK_PLUS:    fprintf(gen->out, " + ");  break;
                case TOK_MINUS:   fprintf(gen->out, " - ");  break;
                case TOK_STAR:    fprintf(gen->out, " * ");  break;
                case TOK_SLASH:   fprintf(gen->out, " / ");  break;
                case TOK_PERCENT: fprintf(gen->out, " %% "); break;
                case TOK_EQ:      fprintf(gen->out, " == "); break;
                case TOK_NEQ:     fprintf(gen->out, " != "); break;
                case TOK_LT:      fprintf(gen->out, " < ");  break;
                case TOK_GT:      fprintf(gen->out, " > ");  break;
                case TOK_LEQ:     fprintf(gen->out, " <= "); break;
                case TOK_GEQ:     fprintf(gen->out, " >= "); break;
                case TOK_AND:     fprintf(gen->out, " && "); break;
                case TOK_OR:      fprintf(gen->out, " || "); break;
                default:          fprintf(gen->out, " ? ");  break;
            }
            emit_expr(gen, node->binary.right);
            fprintf(gen->out, ")");
            break;

        case NODE_UNARY_EXPR:
            fprintf(gen->out, "(");
            switch (node->unary.op) {
                case TOK_MINUS: fprintf(gen->out, "-"); break;
                case TOK_NOT:
                case TOK_BANG:  fprintf(gen->out, "!"); break;
                default: break;
            }
            emit_expr(gen, node->unary.operand);
            fprintf(gen->out, ")");
            break;

        case NODE_CALL_EXPR: {
            // some(value) — wrap in Option
            if (node->call.callee->type == NODE_IDENT &&
                strcmp(node->call.callee->ident.name, "some") == 0 &&
                node->call.args.count == 1) {
                const char *suffix = "i64";
                if (node->resolved_type && node->resolved_type->kind == TYPE_OPTION &&
                    node->resolved_type->option_info.inner_type) {
                    suffix = type_array_suffix(node->resolved_type->option_info.inner_type);
                } else if (node->call.args.items[0]->resolved_type) {
                    suffix = type_array_suffix(node->call.args.items[0]->resolved_type);
                }
                fprintf(gen->out, "AlphaOpt_%s_some(", suffix);
                emit_expr(gen, node->call.args.items[0]);
                fprintf(gen->out, ")");
                break;
            }
            // map_new() — needs resolved_type to determine value type
            if (node->call.callee->type == NODE_IDENT &&
                strcmp(node->call.callee->ident.name, "map_new") == 0) {
                const char *suffix = "str";
                if (node->resolved_type && node->resolved_type->kind == TYPE_MAP &&
                    node->resolved_type->map_info.value_type) {
                    suffix = type_array_suffix(node->resolved_type->map_info.value_type);
                }
                fprintf(gen->out, "AlphaMap_%s_new()", suffix);
                break;
            }
            // Check for built-in functions
            if (node->call.callee->type == NODE_IDENT &&
                is_builtin_fn(node->call.callee->ident.name)) {
                emit_builtin_call(gen, node->call.callee->ident.name, &node->call.args);
                break;
            }
            // Generic call: use mangled name
            if (node->call.mono_name) {
                fprintf(gen->out, "%s(", node->call.mono_name);
            } else {
                emit_expr(gen, node->call.callee);
                fprintf(gen->out, "(");
            }
            for (int i = 0; i < node->call.args.count; i++) {
                if (i > 0) fprintf(gen->out, ", ");
                emit_expr(gen, node->call.args.items[i]);
            }
            fprintf(gen->out, ")");
            break;
        }

        case NODE_METHOD_CALL: {
            Type *obj_type = node->method_call.object->resolved_type;
            const char *method = node->method_call.method;

            // Array methods: push, pop, clear, clone, free
            if (obj_type && obj_type->kind == TYPE_ARRAY) {
                const char *suffix = type_array_suffix(obj_type->array_info.element);
                if (strcmp(method, "push") == 0) {
                    fprintf(gen->out, "AlphaArr_%s_push(&", suffix);
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ", ");
                    emit_expr(gen, node->method_call.args.items[0]);
                    fprintf(gen->out, ")");
                } else if (strcmp(method, "pop") == 0) {
                    fprintf(gen->out, "AlphaArr_%s_pop(&", suffix);
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ")");
                } else if (strcmp(method, "clear") == 0) {
                    fprintf(gen->out, "AlphaArr_%s_clear(&", suffix);
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ")");
                } else if (strcmp(method, "clone") == 0) {
                    fprintf(gen->out, "AlphaArr_%s_clone(&", suffix);
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ")");
                } else if (strcmp(method, "free") == 0) {
                    fprintf(gen->out, "AlphaArr_%s_free(&", suffix);
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ")");
                } else if (strcmp(method, "map") == 0 || strcmp(method, "filter") == 0 ||
                           strcmp(method, "any") == 0 || strcmp(method, "all") == 0 ||
                           strcmp(method, "count") == 0) {
                    fprintf(gen->out, "AlphaArr_%s_%s(&", suffix, method);
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ", ");
                    emit_expr(gen, node->method_call.args.items[0]);
                    fprintf(gen->out, ")");
                } else if (strcmp(method, "reduce") == 0) {
                    fprintf(gen->out, "AlphaArr_%s_reduce(&", suffix);
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ", ");
                    emit_expr(gen, node->method_call.args.items[0]);
                    fprintf(gen->out, ", ");
                    emit_expr(gen, node->method_call.args.items[1]);
                    fprintf(gen->out, ")");
                } else if (strcmp(method, "join") == 0 && strcmp(suffix, "str") == 0) {
                    fprintf(gen->out, "alpha_str_join(&");
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ", ");
                    emit_expr(gen, node->method_call.args.items[0]);
                    fprintf(gen->out, ")");
                } else {
                    fprintf(gen->out, "/* unknown array method '%s' */0", method);
                }
                break;
            }

            // Option methods: is_some, is_none, unwrap, unwrap_or
            if (obj_type && obj_type->kind == TYPE_OPTION) {
                const char *suffix = obj_type->option_info.inner_type
                    ? type_array_suffix(obj_type->option_info.inner_type) : "i64";
                if (strcmp(method, "is_some") == 0) {
                    fprintf(gen->out, "(");
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ").has_value");
                } else if (strcmp(method, "is_none") == 0) {
                    fprintf(gen->out, "(!(");
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ").has_value)");
                } else if (strcmp(method, "unwrap") == 0) {
                    fprintf(gen->out, "AlphaOpt_%s_unwrap(", suffix);
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ")");
                } else if (strcmp(method, "unwrap_or") == 0) {
                    fprintf(gen->out, "AlphaOpt_%s_unwrap_or(", suffix);
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ", ");
                    emit_expr(gen, node->method_call.args.items[0]);
                    fprintf(gen->out, ")");
                }
                break;
            }

            // Map methods: set, get, has, delete, keys
            if (obj_type && obj_type->kind == TYPE_MAP) {
                const char *suffix = obj_type->map_info.value_type
                    ? type_array_suffix(obj_type->map_info.value_type) : "str";
                if (strcmp(method, "set") == 0) {
                    fprintf(gen->out, "AlphaMap_%s_set(&", suffix);
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ", ");
                    emit_expr(gen, node->method_call.args.items[0]);
                    fprintf(gen->out, ", ");
                    emit_expr(gen, node->method_call.args.items[1]);
                    fprintf(gen->out, ")");
                } else if (strcmp(method, "get") == 0) {
                    fprintf(gen->out, "AlphaMap_%s_get(&", suffix);
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ", ");
                    emit_expr(gen, node->method_call.args.items[0]);
                    // Default value
                    if (node->method_call.args.count > 1) {
                        fprintf(gen->out, ", ");
                        emit_expr(gen, node->method_call.args.items[1]);
                    } else {
                        fprintf(gen->out, ", 0");
                    }
                    fprintf(gen->out, ")");
                } else if (strcmp(method, "has") == 0) {
                    fprintf(gen->out, "AlphaMap_%s_has(&", suffix);
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ", ");
                    emit_expr(gen, node->method_call.args.items[0]);
                    fprintf(gen->out, ")");
                } else if (strcmp(method, "delete") == 0) {
                    fprintf(gen->out, "AlphaMap_%s_delete(&", suffix);
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ", ");
                    emit_expr(gen, node->method_call.args.items[0]);
                    fprintf(gen->out, ")");
                } else if (strcmp(method, "keys") == 0) {
                    fprintf(gen->out, "AlphaMap_%s_keys(&", suffix);
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ")");
                }
                break;
            }

            // String methods: contains, starts_with, substr, char_at, split
            if (obj_type && obj_type->kind == TYPE_STR) {
                if (strcmp(method, "contains") == 0) {
                    fprintf(gen->out, "(strstr(");
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ", ");
                    emit_expr(gen, node->method_call.args.items[0]);
                    fprintf(gen->out, ") != NULL)");
                } else if (strcmp(method, "starts_with") == 0) {
                    fprintf(gen->out, "alpha_str_starts_with(");
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ", ");
                    emit_expr(gen, node->method_call.args.items[0]);
                    fprintf(gen->out, ")");
                } else if (strcmp(method, "substr") == 0) {
                    fprintf(gen->out, "alpha_str_substr(");
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ", ");
                    emit_expr(gen, node->method_call.args.items[0]);
                    fprintf(gen->out, ", ");
                    emit_expr(gen, node->method_call.args.items[1]);
                    fprintf(gen->out, ")");
                } else if (strcmp(method, "char_at") == 0) {
                    fprintf(gen->out, "(int64_t)((unsigned char)(");
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ")[");
                    emit_expr(gen, node->method_call.args.items[0]);
                    fprintf(gen->out, "])");
                } else if (strcmp(method, "split") == 0) {
                    fprintf(gen->out, "alpha_str_split(");
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ", ");
                    emit_expr(gen, node->method_call.args.items[0]);
                    fprintf(gen->out, ")");
                } else if (strcmp(method, "trim") == 0) {
                    fprintf(gen->out, "alpha_str_trim(");
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ")");
                } else if (strcmp(method, "replace") == 0) {
                    fprintf(gen->out, "alpha_str_replace(");
                    emit_expr(gen, node->method_call.object);
                    fprintf(gen->out, ", ");
                    emit_expr(gen, node->method_call.args.items[0]);
                    fprintf(gen->out, ", ");
                    emit_expr(gen, node->method_call.args.items[1]);
                    fprintf(gen->out, ")");
                } else {
                    fprintf(gen->out, "/* unknown string method '%s' */0", method);
                }
                break;
            }

            // User-defined methods: TypeName_method(&object, args...)
            const char *type_name = "unknown";
            if (obj_type && obj_type->kind == TYPE_STRUCT && obj_type->name) {
                type_name = obj_type->name;
            }
            fprintf(gen->out, "%s_%s(", type_name, method);
            fprintf(gen->out, "&");
            emit_expr(gen, node->method_call.object);
            for (int i = 0; i < node->method_call.args.count; i++) {
                fprintf(gen->out, ", ");
                emit_expr(gen, node->method_call.args.items[i]);
            }
            fprintf(gen->out, ")");
            break;
        }

        case NODE_FIELD_ACCESS: {
            Type *obj_type = node->field_access.object->resolved_type;
            const char *field = node->field_access.field;

            // Map .len field
            if (obj_type && obj_type->kind == TYPE_MAP && strcmp(field, "len") == 0) {
                emit_expr(gen, node->field_access.object);
                fprintf(gen->out, ".len");
                break;
            }

            // Array .len field
            if (obj_type && obj_type->kind == TYPE_ARRAY && strcmp(field, "len") == 0) {
                emit_expr(gen, node->field_access.object);
                fprintf(gen->out, ".len");
                break;
            }

            // String .len field
            if (obj_type && obj_type->kind == TYPE_STR && strcmp(field, "len") == 0) {
                fprintf(gen->out, "(int64_t)strlen(");
                emit_expr(gen, node->field_access.object);
                fprintf(gen->out, ")");
                break;
            }

            // Pointer dereference (self in methods)
            bool is_ptr = false;
            if (node->field_access.object->type == NODE_IDENT &&
                strcmp(node->field_access.object->ident.name, "self") == 0) {
                is_ptr = true;
            }
            if (obj_type && obj_type->kind == TYPE_REF) {
                is_ptr = true;
            }
            emit_expr(gen, node->field_access.object);
            fprintf(gen->out, "%s%s", is_ptr ? "->" : ".", field);
            break;
        }

        case NODE_INDEX_EXPR: {
            Type *obj_type = node->index_expr.object->resolved_type;
            if (obj_type && obj_type->kind == TYPE_ARRAY) {
                emit_expr(gen, node->index_expr.object);
                fprintf(gen->out, ".data[");
                emit_expr(gen, node->index_expr.index);
                fprintf(gen->out, "]");
            } else {
                // String indexing or raw pointer
                emit_expr(gen, node->index_expr.object);
                fprintf(gen->out, "[");
                emit_expr(gen, node->index_expr.index);
                fprintf(gen->out, "]");
            }
            break;
        }

        case NODE_STRUCT_LIT:
            fprintf(gen->out, "(%s){", node->struct_lit.name);
            for (int i = 0; i < node->struct_lit.field_inits.count; i++) {
                if (i > 0) fprintf(gen->out, ", ");
                fprintf(gen->out, ".%s = ", node->struct_lit.field_inits.items[i].name);
                emit_expr(gen, node->struct_lit.values.items[i]);
            }
            fprintf(gen->out, "}");
            break;

        case NODE_ARRAY_LIT: {
            // Determine element type from resolved type or first element
            Type *arr_type = node->resolved_type;
            const char *suffix = "i64";
            if (arr_type && arr_type->kind == TYPE_ARRAY && arr_type->array_info.element) {
                suffix = type_array_suffix(arr_type->array_info.element);
            } else if (node->array_lit.elements.count > 0) {
                Type *elem = node->array_lit.elements.items[0]->resolved_type;
                if (elem) suffix = type_array_suffix(elem);
            }

            if (node->array_lit.elements.count == 0) {
                // Empty array
                fprintf(gen->out, "AlphaArr_%s_new()", suffix);
            } else {
                // Non-empty: create, push each element
                // Use a statement expression (GCC/Clang extension)
                fprintf(gen->out, "({ AlphaArr_%s _arr = AlphaArr_%s_new(); ", suffix, suffix);
                for (int i = 0; i < node->array_lit.elements.count; i++) {
                    fprintf(gen->out, "AlphaArr_%s_push(&_arr, ", suffix);
                    emit_expr(gen, node->array_lit.elements.items[i]);
                    fprintf(gen->out, "); ");
                }
                fprintf(gen->out, "_arr; })");
            }
            break;
        }

        case NODE_REF_EXPR:
            fprintf(gen->out, "&");
            emit_expr(gen, node->ref_expr.operand);
            break;

        case NODE_DEREF_EXPR:
            fprintf(gen->out, "(*");
            emit_expr(gen, node->deref_expr.operand);
            fprintf(gen->out, ")");
            break;

        case NODE_LAMBDA: {
            // Assign a unique ID and register for hoisting
            int id = gen->lambda_count++;
            node->lambda.id = id;
            if (gen->lambda_count > gen->lambda_cap) {
                gen->lambda_cap = gen->lambda_cap ? gen->lambda_cap * 2 : 16;
                gen->lambdas = realloc(gen->lambdas, sizeof(ASTNode *) * gen->lambda_cap);
            }
            gen->lambdas[id] = node;
            // Emit just the function name — the actual function is hoisted
            fprintf(gen->out, "__alpha_lambda_%d", id);
            break;
        }

        default:
            fprintf(gen->out, "/* unhandled expr */");
            break;
    }
}

// ---- If/else helper (recursive) ----

static void emit_if(CodeGen *gen, ASTNode *node) {
    fprintf(gen->out, "if (");
    emit_expr(gen, node->if_stmt.condition);
    fprintf(gen->out, ") {\n");
    gen->indent++; gen->scope_depth++;
    for (int i = 0; i < node->if_stmt.then_block->block.stmts.count; i++) {
        emit_stmt(gen, node->if_stmt.then_block->block.stmts.items[i]);
    }
    gen->indent--; gen->scope_depth--;
    emit_indent(gen);
    fprintf(gen->out, "}");
    if (node->if_stmt.else_block) {
        if (node->if_stmt.else_block->type == NODE_IF_STMT) {
            fprintf(gen->out, " else ");
            emit_if(gen, node->if_stmt.else_block);
        } else {
            fprintf(gen->out, " else {\n");
            gen->indent++; gen->scope_depth++;
            for (int i = 0; i < node->if_stmt.else_block->block.stmts.count; i++) {
                emit_stmt(gen, node->if_stmt.else_block->block.stmts.items[i]);
            }
            gen->indent--; gen->scope_depth--;
            emit_indent(gen);
            fprintf(gen->out, "}");
        }
    }
}

// ---- Statement emission ----

static void emit_stmt(CodeGen *gen, ASTNode *node) {
    switch (node->type) {
        case NODE_LET_STMT:
            emit_indent(gen);
            // Use resolved type from type checker if available
            if (node->resolved_type && node->resolved_type->kind != TYPE_UNKNOWN) {
                fprintf(gen->out, "%s", type_to_c(node->resolved_type));
            } else if (node->let_stmt.type_ann) {
                emit_type(gen, node->let_stmt.type_ann);
            } else {
                // Fallback: infer from literal or use __auto_type
                if (node->let_stmt.init) {
                    switch (node->let_stmt.init->type) {
                        case NODE_INT_LIT:    fprintf(gen->out, "int64_t"); break;
                        case NODE_FLOAT_LIT:  fprintf(gen->out, "double"); break;
                        case NODE_STRING_LIT: fprintf(gen->out, "const char*"); break;
                        case NODE_BOOL_LIT:   fprintf(gen->out, "bool"); break;
                        case NODE_STRUCT_LIT: fprintf(gen->out, "%s", node->let_stmt.init->struct_lit.name); break;
                        default:              fprintf(gen->out, "__auto_type"); break;
                    }
                } else {
                    fprintf(gen->out, "int64_t");
                }
            }
            fprintf(gen->out, " %s", node->let_stmt.name);
            if (node->let_stmt.init) {
                fprintf(gen->out, " = ");
                emit_expr(gen, node->let_stmt.init);
            }
            fprintf(gen->out, ";\n");

            // Auto-track arrays and maps for cleanup
            if (node->resolved_type) {
                if (node->resolved_type->kind == TYPE_ARRAY) {
                    const char *suffix = type_array_suffix(node->resolved_type->array_info.element);
                    char free_fn[128];
                    snprintf(free_fn, sizeof(free_fn), "AlphaArr_%s_free", suffix);
                    track_owned(gen, node->let_stmt.name, strdup(free_fn));
                } else if (node->resolved_type->kind == TYPE_MAP) {
                    const char *suffix = node->resolved_type->map_info.value_type
                        ? type_array_suffix(node->resolved_type->map_info.value_type) : "str";
                    char free_fn[128];
                    snprintf(free_fn, sizeof(free_fn), "AlphaMap_%s_free", suffix);
                    track_owned(gen, node->let_stmt.name, strdup(free_fn));
                }
            }
            break;

        case NODE_RETURN_STMT:
            emit_indent(gen);
            if (node->return_stmt.value && gen->current_ensures) {
                // Capture result, check ensures, then return
                fprintf(gen->out, "{ __auto_type result = (");
                emit_expr(gen, node->return_stmt.value);
                fprintf(gen->out, ");\n");
                gen->indent++;
                for (int ei = 0; ei < gen->current_ensures->count; ei++) {
                    ASTNode *ens = gen->current_ensures->items[ei];
                    char *es = expr_to_cstring(gen, ens);
                    char *ee = escape_c_string(es);
                    emit_indent(gen);
                    fprintf(gen->out, "if (!(");
                    emit_expr(gen, ens);
                    fprintf(gen->out, ")) {\n");
                    gen->indent++;
                    emit_indent(gen);
                    fprintf(gen->out, "fprintf(stderr, \"contract violation: ensures %s failed in %s\\n\");\n",
                            ee, gen->current_fn_name);
                    emit_indent(gen);
                    fprintf(gen->out, "fprintf(stderr, \"  result = \"); alpha_assert_print(result); fprintf(stderr, \"\\n\");\n");
                    emit_indent(gen);
                    fprintf(gen->out, "__alpha_panic(\"contract violation\");\n");
                    gen->indent--;
                    emit_indent(gen);
                    fprintf(gen->out, "}\n");
                    free(es); free(ee);
                }
                emit_indent(gen);
                fprintf(gen->out, "return result; }\n");
                gen->indent--;
            } else if (node->return_stmt.value) {
                bool need_cleanup = gen->defer_count > 0 || gen->owned_count > 0;
                if (need_cleanup) {
                    const char *skip = get_return_var_name(node->return_stmt.value);
                    fprintf(gen->out, "{ __auto_type __ret = (");
                    emit_expr(gen, node->return_stmt.value);
                    fprintf(gen->out, ");\n");
                    gen->indent++;
                    emit_auto_cleanup(gen, skip);
                    emit_defers(gen);
                    emit_indent(gen);
                    fprintf(gen->out, "return __ret; }\n");
                    gen->indent--;
                } else {
                    fprintf(gen->out, "return ");
                    emit_expr(gen, node->return_stmt.value);
                    fprintf(gen->out, ";\n");
                }
            } else {
                if (gen->defer_count > 0 || gen->owned_count > 0) {
                    emit_auto_cleanup(gen, NULL);
                    emit_defers(gen);
                    emit_indent(gen);
                }
                fprintf(gen->out, "return;\n");
            }
            break;

        case NODE_EXPR_STMT:
            emit_indent(gen);
            emit_expr(gen, node->expr_stmt.expr);
            fprintf(gen->out, ";\n");
            break;

        case NODE_IF_STMT:
            emit_indent(gen);
            emit_if(gen, node);
            fprintf(gen->out, "\n");
            break;

        case NODE_WHILE_STMT:
            emit_indent(gen);
            fprintf(gen->out, "while (");
            emit_expr(gen, node->while_stmt.condition);
            fprintf(gen->out, ") {\n");
            gen->indent++; gen->scope_depth++;
            for (int i = 0; i < node->while_stmt.body->block.stmts.count; i++) {
                emit_stmt(gen, node->while_stmt.body->block.stmts.items[i]);
            }
            gen->indent--; gen->scope_depth--;
            emit_indent(gen);
            fprintf(gen->out, "}\n");
            break;

        case NODE_FOR_STMT: {
            ASTNode *iterable = node->for_stmt.iterable;
            const char *var = node->for_stmt.var_name;

            // Range: for i in 0..10
            if (iterable->type == NODE_BINARY_EXPR && iterable->binary.op == TOK_DOTDOT) {
                emit_indent(gen);
                fprintf(gen->out, "for (int64_t %s = ", var);
                emit_expr(gen, iterable->binary.left);
                fprintf(gen->out, "; %s < ", var);
                emit_expr(gen, iterable->binary.right);
                fprintf(gen->out, "; %s++) {\n", var);
                gen->indent++; gen->scope_depth++;
                for (int i = 0; i < node->for_stmt.body->block.stmts.count; i++) {
                    emit_stmt(gen, node->for_stmt.body->block.stmts.items[i]);
                }
                gen->indent--; gen->scope_depth--;
                emit_indent(gen);
                fprintf(gen->out, "}\n");
            }
            // Array: for item in array
            else if (iterable->resolved_type && iterable->resolved_type->kind == TYPE_ARRAY) {
                Type *elem = iterable->resolved_type->array_info.element;
                // In generic context, use __auto_type since resolved types may not be concrete
                const char *elem_c;
                if (gen->subst_count > 0) {
                    elem_c = "__auto_type";
                } else {
                    elem_c = elem ? type_to_c(elem) : "int64_t";
                }
                emit_indent(gen);
                fprintf(gen->out, "for (int64_t _fi = 0; _fi < (");
                emit_expr(gen, iterable);
                fprintf(gen->out, ").len; _fi++) {\n");
                gen->indent++; gen->scope_depth++;
                emit_indent(gen);
                fprintf(gen->out, "%s %s = (", elem_c, var);
                emit_expr(gen, iterable);
                fprintf(gen->out, ").data[_fi];\n");
                for (int i = 0; i < node->for_stmt.body->block.stmts.count; i++) {
                    emit_stmt(gen, node->for_stmt.body->block.stmts.items[i]);
                }
                gen->indent--; gen->scope_depth--;
                emit_indent(gen);
                fprintf(gen->out, "}\n");
            }
            // Fallback: treat as range 0..n
            else {
                emit_indent(gen);
                fprintf(gen->out, "for (int64_t %s = 0; %s < ", var, var);
                emit_expr(gen, iterable);
                fprintf(gen->out, "; %s++) {\n", var);
                gen->indent++; gen->scope_depth++;
                for (int i = 0; i < node->for_stmt.body->block.stmts.count; i++) {
                    emit_stmt(gen, node->for_stmt.body->block.stmts.items[i]);
                }
                gen->indent--; gen->scope_depth--;
                emit_indent(gen);
                fprintf(gen->out, "}\n");
            }
            break;
        }

        case NODE_ASSIGN_STMT:
            emit_indent(gen);
            emit_expr(gen, node->assign_stmt.target);
            switch (node->assign_stmt.op) {
                case TOK_ASSIGN:       fprintf(gen->out, " = ");  break;
                case TOK_PLUS_ASSIGN:  fprintf(gen->out, " += "); break;
                case TOK_MINUS_ASSIGN: fprintf(gen->out, " -= "); break;
                case TOK_STAR_ASSIGN:  fprintf(gen->out, " *= "); break;
                case TOK_SLASH_ASSIGN: fprintf(gen->out, " /= "); break;
                default:               fprintf(gen->out, " = ");  break;
            }
            emit_expr(gen, node->assign_stmt.value);
            fprintf(gen->out, ";\n");
            break;

        case NODE_DEFER_STMT:
            // Push to defer stack — emitted at function return
            if (gen->defer_count >= gen->defer_cap) {
                gen->defer_cap = gen->defer_cap ? gen->defer_cap * 2 : 8;
                gen->defer_stack = realloc(gen->defer_stack, sizeof(ASTNode *) * gen->defer_cap);
            }
            gen->defer_stack[gen->defer_count++] = node->defer_stmt.stmt;
            break;

        case NODE_BREAK_STMT:
            emit_indent(gen);
            fprintf(gen->out, "break;\n");
            break;

        case NODE_CONTINUE_STMT:
            emit_indent(gen);
            fprintf(gen->out, "continue;\n");
            break;

        case NODE_BLOCK:
            for (int i = 0; i < node->block.stmts.count; i++) {
                emit_stmt(gen, node->block.stmts.items[i]);
            }
            break;

        case NODE_MATCH_EXPR: {
            // Emit match as if/else if chain
            emit_indent(gen);
            fprintf(gen->out, "{\n");
            gen->indent++;

            // Evaluate subject once into a temp
            emit_indent(gen);
            fprintf(gen->out, "__auto_type __match_val = ");
            emit_expr(gen, node->match_expr.subject);
            fprintf(gen->out, ";\n");

            bool first = true;
            for (int i = 0; i < node->match_expr.arms.count; i++) {
                ASTNode *arm = node->match_expr.arms.items[i];

                if (!arm->match_arm.pattern) {
                    // Wildcard arm (_) — emit as else
                    emit_indent(gen);
                    if (!first) fprintf(gen->out, "else ");
                    fprintf(gen->out, "{\n");
                } else {
                    emit_indent(gen);
                    if (first) {
                        fprintf(gen->out, "if (__match_val == ");
                    } else {
                        fprintf(gen->out, "else if (__match_val == ");
                    }
                    emit_expr(gen, arm->match_arm.pattern);
                    fprintf(gen->out, ") {\n");
                }
                first = false;

                gen->indent++; gen->scope_depth++;
                if (arm->match_arm.body->type == NODE_BLOCK) {
                    for (int j = 0; j < arm->match_arm.body->block.stmts.count; j++) {
                        emit_stmt(gen, arm->match_arm.body->block.stmts.items[j]);
                    }
                } else {
                    emit_indent(gen);
                    emit_expr(gen, arm->match_arm.body);
                    fprintf(gen->out, ";\n");
                }
                gen->indent--; gen->scope_depth--;

                emit_indent(gen);
                fprintf(gen->out, "}\n");
            }

            gen->indent--;
            emit_indent(gen);
            fprintf(gen->out, "}\n");
            break;
        }

        default:
            emit_indent(gen);
            fprintf(gen->out, "/* unhandled statement */\n");
            break;
    }
}

// ---- Declaration emission ----

static void emit_fn_decl(CodeGen *gen, ASTNode *node) {
    // Return type (main always returns int in C)
    if (strcmp(node->fn_decl.name, "main") == 0) {
        fprintf(gen->out, "int");
    } else {
        emit_type(gen, node->fn_decl.return_type);
    }

    // Function name (mangled for methods)
    if (node->fn_decl.is_method && gen->current_struct) {
        fprintf(gen->out, " %s_%s(", gen->current_struct, node->fn_decl.name);
    } else if (strcmp(node->fn_decl.name, "main") == 0) {
        fprintf(gen->out, " main(");
    } else {
        fprintf(gen->out, " %s(", node->fn_decl.name);
    }

    // Parameters
    bool first = true;
    for (int i = 0; i < node->fn_decl.params.count; i++) {
        Field *param = &node->fn_decl.params.items[i];

        // Skip self in param list — it becomes the first C param
        if (strcmp(param->name, "self") == 0) {
            if (!first) fprintf(gen->out, ", ");
            if (gen->current_struct) {
                fprintf(gen->out, "%s* self", gen->current_struct);
            } else {
                fprintf(gen->out, "void* self");
            }
            first = false;
            continue;
        }

        if (!first) fprintf(gen->out, ", ");
        if (param->type) {
            emit_type(gen, param->type);
        } else {
            fprintf(gen->out, "int64_t");
        }
        fprintf(gen->out, " %s", param->name);
        first = false;
    }

    // main() gets argc/argv in C
    if (strcmp(node->fn_decl.name, "main") == 0 && node->fn_decl.params.count == 0) {
        fprintf(gen->out, "int argc, char** argv");
    }

    fprintf(gen->out, ") {\n");

    // Save argc/argv for main
    if (strcmp(node->fn_decl.name, "main") == 0) {
        fprintf(gen->out, "    alpha_argc = argc;\n");
        fprintf(gen->out, "    alpha_argv = argv;\n");
    }

    // Requires checks (contract preconditions)
    gen->indent = 1;
    if (!gen->strip_contracts && node->fn_decl.requires.count > 0) {
        for (int i = 0; i < node->fn_decl.requires.count; i++) {
            ASTNode *req = node->fn_decl.requires.items[i];
            char *expr_str = expr_to_cstring(gen, req);
            char *expr_esc = escape_c_string(expr_str);

            emit_indent(gen);
            fprintf(gen->out, "if (!(");
            emit_expr(gen, req);
            fprintf(gen->out, ")) {\n");
            gen->indent++;
            emit_indent(gen);
            fprintf(gen->out, "fprintf(stderr, \"contract violation: requires %s failed in %s\\n\");\n",
                    expr_esc, node->fn_decl.name);
            // Print argument values (skip complex types like arrays/structs)
            for (int j = 0; j < node->fn_decl.params.count; j++) {
                Field *param = &node->fn_decl.params.items[j];
                if (strcmp(param->name, "self") == 0) continue;
                // Only print primitive types
                if (param->type && (param->type->type == NODE_TYPE_ARRAY ||
                    (param->type->type == NODE_TYPE_BASIC &&
                     param->type->type_basic.name[0] >= 'A' &&
                     param->type->type_basic.name[0] <= 'Z'))) continue;
                emit_indent(gen);
                fprintf(gen->out, "fprintf(stderr, \"  %s = \"); alpha_assert_print(%s); fprintf(stderr, \"\\n\");\n",
                        param->name, param->name);
            }
            emit_indent(gen);
            fprintf(gen->out, "__alpha_panic(\"contract violation\");\n");
            gen->indent--;
            emit_indent(gen);
            fprintf(gen->out, "}\n");
            free(expr_str); free(expr_esc);
        }
    }

    // Save/reset cleanup state for this function
    int saved_defer_count = gen->defer_count;
    int saved_owned_count = gen->owned_count;
    int saved_scope_depth = gen->scope_depth;
    gen->defer_count = 0;
    gen->owned_count = 0;
    gen->scope_depth = 0;

    // Set up ensures tracking
    NodeList *saved_ensures = gen->current_ensures;
    const char *saved_fn_name = gen->current_fn_name;
    gen->current_ensures = (node->fn_decl.ensures.count > 0 && !gen->strip_contracts)
        ? &node->fn_decl.ensures : NULL;
    gen->current_fn_name = node->fn_decl.name;

    // Recover block: wrap body in setjmp
    if (node->fn_decl.recover_block) {
        emit_indent(gen);
        fprintf(gen->out, "jmp_buf __recover_buf;\n");
        emit_indent(gen);
        fprintf(gen->out, "jmp_buf* __saved_recover = __alpha_recover_jmp;\n");
        emit_indent(gen);
        fprintf(gen->out, "__alpha_recover_jmp = &__recover_buf;\n");
        emit_indent(gen);
        fprintf(gen->out, "if (setjmp(__recover_buf) != 0) {\n");
        gen->indent++;
        emit_indent(gen);
        fprintf(gen->out, "__alpha_recover_jmp = __saved_recover;\n");
        // Emit recover block body
        for (int i = 0; i < node->fn_decl.recover_block->block.stmts.count; i++) {
            emit_stmt(gen, node->fn_decl.recover_block->block.stmts.items[i]);
        }
        gen->indent--;
        emit_indent(gen);
        fprintf(gen->out, "}\n");
    }

    // Body
    if (node->fn_decl.body) {
        for (int i = 0; i < node->fn_decl.body->block.stmts.count; i++) {
            emit_stmt(gen, node->fn_decl.body->block.stmts.items[i]);
        }
    }

    // Restore recover state
    if (node->fn_decl.recover_block) {
        emit_indent(gen);
        fprintf(gen->out, "__alpha_recover_jmp = __saved_recover;\n");
    }

    gen->current_ensures = saved_ensures;
    gen->current_fn_name = saved_fn_name;

    // Auto-cleanup and defers at end of function
    if (gen->owned_count > 0) {
        emit_auto_cleanup(gen, NULL);
    }
    if (gen->defer_count > 0) {
        emit_defers(gen);
    }
    gen->defer_count = saved_defer_count;
    gen->owned_count = saved_owned_count;
    gen->scope_depth = saved_scope_depth;

    // Auto-return 0 for main
    if (strcmp(node->fn_decl.name, "main") == 0 && !node->fn_decl.return_type) {
        emit_indent(gen);
        fprintf(gen->out, "return 0;\n");
    }

    fprintf(gen->out, "}\n\n");
    gen->indent = 0;
}

static void emit_struct_decl(CodeGen *gen, ASTNode *node) {
    fprintf(gen->out, "typedef struct {\n");
    gen->indent = 1;
    for (int i = 0; i < node->struct_decl.fields.count; i++) {
        Field *field = &node->struct_decl.fields.items[i];
        emit_indent(gen);
        emit_type(gen, field->type);
        fprintf(gen->out, " %s;\n", field->name);
    }
    gen->indent = 0;
    fprintf(gen->out, "} %s;\n\n", node->struct_decl.name);
}

static void emit_enum_decl(CodeGen *gen, ASTNode *node) {
    fprintf(gen->out, "typedef enum {\n");
    gen->indent = 1;
    for (int i = 0; i < node->enum_decl.variants.count; i++) {
        emit_indent(gen);
        fprintf(gen->out, "%s_%s", node->enum_decl.name,
                node->enum_decl.variants.items[i]->ident.name);
        if (i < node->enum_decl.variants.count - 1) fprintf(gen->out, ",");
        fprintf(gen->out, "\n");
    }
    gen->indent = 0;
    fprintf(gen->out, "} %s;\n\n", node->enum_decl.name);
}

static void emit_decl(CodeGen *gen, ASTNode *node) {
    switch (node->type) {
        case NODE_FN_DECL:
            emit_fn_decl(gen, node);
            break;

        case NODE_STRUCT_DECL:
            emit_struct_decl(gen, node);
            break;

        case NODE_ENUM_DECL:
            emit_enum_decl(gen, node);
            break;

        case NODE_IMPL_BLOCK: {
            const char *saved = gen->current_struct;
            gen->current_struct = node->impl_block.type_name;
            for (int i = 0; i < node->impl_block.methods.count; i++) {
                emit_fn_decl(gen, node->impl_block.methods.items[i]);
            }
            gen->current_struct = saved;
            break;
        }

        default:
            fprintf(gen->out, "/* unhandled declaration */\n");
            break;
    }
}

// ---- Forward declarations ----

static void emit_forward_decls(CodeGen *gen, ASTNode *program) {
    for (int i = 0; i < program->program.decls.count; i++) {
        ASTNode *decl = program->program.decls.items[i];

        if (decl->type == NODE_FN_DECL && strcmp(decl->fn_decl.name, "main") != 0
            && decl->fn_decl.type_params.count == 0) {
            emit_type(gen, decl->fn_decl.return_type);
            fprintf(gen->out, " %s(", decl->fn_decl.name);
            for (int j = 0; j < decl->fn_decl.params.count; j++) {
                if (j > 0) fprintf(gen->out, ", ");
                Field *param = &decl->fn_decl.params.items[j];
                if (param->type) {
                    emit_type(gen, param->type);
                } else {
                    fprintf(gen->out, "int64_t");
                }
                fprintf(gen->out, " %s", param->name);
            }
            fprintf(gen->out, ");\n");
        }

        if (decl->type == NODE_IMPL_BLOCK) {
            for (int m = 0; m < decl->impl_block.methods.count; m++) {
                ASTNode *method = decl->impl_block.methods.items[m];
                emit_type(gen, method->fn_decl.return_type);
                fprintf(gen->out, " %s_%s(", decl->impl_block.type_name, method->fn_decl.name);
                bool first = true;
                for (int j = 0; j < method->fn_decl.params.count; j++) {
                    Field *param = &method->fn_decl.params.items[j];
                    if (!first) fprintf(gen->out, ", ");
                    if (strcmp(param->name, "self") == 0) {
                        fprintf(gen->out, "%s* self", decl->impl_block.type_name);
                    } else {
                        if (param->type) emit_type(gen, param->type);
                        else fprintf(gen->out, "int64_t");
                        fprintf(gen->out, " %s", param->name);
                    }
                    first = false;
                }
                fprintf(gen->out, ");\n");
            }
        }
    }
    fprintf(gen->out, "\n");
}

// ---- Public API ----

void codegen_emit(CodeGen *gen, ASTNode *program) {
    // Preamble
    fprintf(gen->out, "/* Generated by Alpha compiler */\n");
    fprintf(gen->out, "#include <stdio.h>\n");
    fprintf(gen->out, "#include <stdlib.h>\n");
    fprintf(gen->out, "#include <stdint.h>\n");
    fprintf(gen->out, "#include <stdbool.h>\n");
    fprintf(gen->out, "#include <string.h>\n");
    fprintf(gen->out, "#include <assert.h>\n");
    fprintf(gen->out, "#include <math.h>\n");
    fprintf(gen->out, "#include <stdarg.h>\n");
    fprintf(gen->out, "#include <setjmp.h>\n");
    fprintf(gen->out, "\n");

    // Runtime helper functions
    fprintf(gen->out, "static char* alpha_str_concat(const char* a, const char* b) {\n");
    fprintf(gen->out, "    size_t la = strlen(a), lb = strlen(b);\n");
    fprintf(gen->out, "    char* r = malloc(la + lb + 1);\n");
    fprintf(gen->out, "    memcpy(r, a, la); memcpy(r + la, b, lb); r[la + lb] = 0;\n");
    fprintf(gen->out, "    return r;\n");
    fprintf(gen->out, "}\n");

    fprintf(gen->out, "static char* alpha_str_substr(const char* s, int64_t start, int64_t len) {\n");
    fprintf(gen->out, "    char* r = malloc(len + 1);\n");
    fprintf(gen->out, "    memcpy(r, s + start, len); r[len] = 0;\n");
    fprintf(gen->out, "    return r;\n");
    fprintf(gen->out, "}\n");

    fprintf(gen->out, "static char* alpha_char_to_str(int64_t c) {\n");
    fprintf(gen->out, "    char* r = malloc(2); r[0] = (char)c; r[1] = 0;\n");
    fprintf(gen->out, "    return r;\n");
    fprintf(gen->out, "}\n");

    fprintf(gen->out, "static char* alpha_i64_to_str(int64_t v) {\n");
    fprintf(gen->out, "    char* r = malloc(21);\n");
    fprintf(gen->out, "    snprintf(r, 21, \"%%lld\", (long long)v);\n");
    fprintf(gen->out, "    return r;\n");
    fprintf(gen->out, "}\n");

    fprintf(gen->out, "static char* alpha_file_read(const char* path) {\n");
    fprintf(gen->out, "    FILE* f = fopen(path, \"rb\");\n");
    fprintf(gen->out, "    if (!f) { fprintf(stderr, \"error: cannot open '%%s'\\n\", path); exit(1); }\n");
    fprintf(gen->out, "    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);\n");
    fprintf(gen->out, "    char* r = malloc(sz + 1); fread(r, 1, sz, f); r[sz] = 0; fclose(f);\n");
    fprintf(gen->out, "    return r;\n");
    fprintf(gen->out, "}\n");

    fprintf(gen->out, "static void alpha_file_write(const char* path, const char* data) {\n");
    fprintf(gen->out, "    FILE* f = fopen(path, \"wb\");\n");
    fprintf(gen->out, "    if (!f) { fprintf(stderr, \"error: cannot write '%%s'\\n\", path); exit(1); }\n");
    fprintf(gen->out, "    fwrite(data, 1, strlen(data), f); fclose(f);\n");
    fprintf(gen->out, "}\n");
    fprintf(gen->out, "static bool alpha_str_starts_with(const char* s, const char* prefix) {\n");
    fprintf(gen->out, "    return strncmp(s, prefix, strlen(prefix)) == 0;\n");
    fprintf(gen->out, "}\n");
    fprintf(gen->out, "static const char* alpha_str_trim(const char* s) {\n");
    fprintf(gen->out, "    while (*s == ' ' || *s == '\\t' || *s == '\\n' || *s == '\\r') s++;\n");
    fprintf(gen->out, "    int64_t len = strlen(s); while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\\t' || s[len-1] == '\\n' || s[len-1] == '\\r')) len--;\n");
    fprintf(gen->out, "    char* r = malloc(len + 1); memcpy(r, s, len); r[len] = 0; return r;\n");
    fprintf(gen->out, "}\n");
    fprintf(gen->out, "\n");

    // Command-line args support
    fprintf(gen->out, "static int alpha_argc = 0;\n");
    fprintf(gen->out, "static char** alpha_argv = NULL;\n");
    fprintf(gen->out, "static int64_t args_count(void) { return alpha_argc; }\n");
    fprintf(gen->out, "static const char* args_get(int64_t i) { return (i < alpha_argc) ? alpha_argv[i] : \"\"; }\n");
    fprintf(gen->out, "static int64_t run_command(const char* cmd) { return system(cmd); }\n");
    fprintf(gen->out, "\n");

    // String builder
    fprintf(gen->out, "typedef struct { char* data; int64_t len; int64_t cap; } StringBuilder;\n");
    fprintf(gen->out, "static StringBuilder* sb_new(void) {\n");
    fprintf(gen->out, "    StringBuilder* sb = calloc(1, sizeof(StringBuilder));\n");
    fprintf(gen->out, "    sb->cap = 256; sb->data = malloc(sb->cap); sb->data[0] = 0;\n");
    fprintf(gen->out, "    return sb;\n");
    fprintf(gen->out, "}\n");
    fprintf(gen->out, "static void sb_append(StringBuilder* sb, const char* s) {\n");
    fprintf(gen->out, "    int64_t slen = strlen(s);\n");
    fprintf(gen->out, "    while (sb->len + slen + 1 > sb->cap) { sb->cap *= 2; sb->data = realloc(sb->data, sb->cap); }\n");
    fprintf(gen->out, "    memcpy(sb->data + sb->len, s, slen); sb->len += slen; sb->data[sb->len] = 0;\n");
    fprintf(gen->out, "}\n");
    fprintf(gen->out, "static void sb_append_char(StringBuilder* sb, char c) {\n");
    fprintf(gen->out, "    if (sb->len + 2 > sb->cap) { sb->cap *= 2; sb->data = realloc(sb->data, sb->cap); }\n");
    fprintf(gen->out, "    sb->data[sb->len++] = c; sb->data[sb->len] = 0;\n");
    fprintf(gen->out, "}\n");
    fprintf(gen->out, "static const char* sb_to_str(StringBuilder* sb) { return sb->data; }\n");

    // String functions that depend on StringBuilder
    fprintf(gen->out, "static const char* alpha_str_replace(const char* s, const char* from, const char* to) {\n");
    fprintf(gen->out, "    StringBuilder* sb = sb_new(); int64_t fl = strlen(from), sl = strlen(s);\n");
    fprintf(gen->out, "    for (int64_t i = 0; i < sl; ) {\n");
    fprintf(gen->out, "        if (i + fl <= sl && strncmp(s + i, from, fl) == 0) { sb_append(sb, to); i += fl; }\n");
    fprintf(gen->out, "        else { sb_append_char(sb, s[i]); i++; }\n");
    fprintf(gen->out, "    } return sb->data;\n");
    fprintf(gen->out, "}\n\n");

    // Test framework runtime
    fprintf(gen->out, "static int alpha_test_pass = 0;\n");
    fprintf(gen->out, "static int alpha_test_fail = 0;\n");
    fprintf(gen->out, "static int alpha_test_assertions = 0;\n");
    fprintf(gen->out, "static const char* alpha_test_current = \"\";\n");
    fprintf(gen->out, "static const char* alpha_test_file = \"\";\n");
    fprintf(gen->out, "static int alpha_test_current_failed = 0;\n");
    fprintf(gen->out, "static int alpha_test_json = 0;\n");
    fprintf(gen->out, "static StringBuilder* alpha_test_json_buf = NULL;\n\n");

    // Generic assert_eq using _Generic for nice value printing
    fprintf(gen->out, "static void alpha_assert_print_i64(int64_t v) { fprintf(stderr, \"%%lld\", (long long)v); }\n");
    fprintf(gen->out, "static void alpha_assert_print_f64(double v) { fprintf(stderr, \"%%g\", v); }\n");
    fprintf(gen->out, "static void alpha_assert_print_str(const char* v) { fprintf(stderr, \"\\\"%%s\\\"\", v); }\n");
    fprintf(gen->out, "static void alpha_assert_print_bool(bool v) { fprintf(stderr, \"%%s\", v ? \"true\" : \"false\"); }\n");
    fprintf(gen->out, "#define alpha_assert_print(v) _Generic((v), \\\n");
    fprintf(gen->out, "    int64_t: alpha_assert_print_i64, \\\n");
    fprintf(gen->out, "    int: alpha_assert_print_i64, \\\n");
    fprintf(gen->out, "    double: alpha_assert_print_f64, \\\n");
    fprintf(gen->out, "    float: alpha_assert_print_f64, \\\n");
    fprintf(gen->out, "    char*: alpha_assert_print_str, \\\n");
    fprintf(gen->out, "    const char*: alpha_assert_print_str, \\\n");
    fprintf(gen->out, "    _Bool: alpha_assert_print_bool, \\\n");
    fprintf(gen->out, "    default: alpha_assert_print_i64)(v)\n\n");

    // Sprint variants: write values to StringBuilder (for JSON)
    fprintf(gen->out, "static void alpha_sprint_i64(StringBuilder* sb, int64_t v) { char b[32]; snprintf(b, 32, \"%%lld\", (long long)v); sb_append(sb, b); }\n");
    fprintf(gen->out, "static void alpha_sprint_f64(StringBuilder* sb, double v) { char b[32]; snprintf(b, 32, \"%%g\", v); sb_append(sb, b); }\n");
    fprintf(gen->out, "static void alpha_sprint_str(StringBuilder* sb, const char* v) { sb_append(sb, \"\\\"\"); sb_append(sb, v); sb_append(sb, \"\\\"\"); }\n");
    fprintf(gen->out, "static void alpha_sprint_bool(StringBuilder* sb, bool v) { sb_append(sb, v ? \"true\" : \"false\"); }\n");
    fprintf(gen->out, "#define alpha_sprint_val(sb, v) _Generic((v), \\\n");
    fprintf(gen->out, "    int64_t: alpha_sprint_i64, int: alpha_sprint_i64, \\\n");
    fprintf(gen->out, "    double: alpha_sprint_f64, float: alpha_sprint_f64, \\\n");
    fprintf(gen->out, "    char*: alpha_sprint_str, const char*: alpha_sprint_str, \\\n");
    fprintf(gen->out, "    _Bool: alpha_sprint_bool, \\\n");
    fprintf(gen->out, "    default: alpha_sprint_i64)((sb), (v))\n\n");

    // Generic equality check for strings vs other types
    fprintf(gen->out, "static bool alpha_generic_eq_i64(int64_t a, int64_t b) { return a == b; }\n");
    fprintf(gen->out, "static bool alpha_generic_eq_f64(double a, double b) { return a == b; }\n");
    fprintf(gen->out, "static bool alpha_generic_eq_str(const char* a, const char* b) { return strcmp(a, b) == 0; }\n");
    fprintf(gen->out, "static bool alpha_generic_eq_bool(bool a, bool b) { return a == b; }\n");
    fprintf(gen->out, "#define alpha_generic_eq(a, b) _Generic((a), \\\n");
    fprintf(gen->out, "    char*: alpha_generic_eq_str, const char*: alpha_generic_eq_str, \\\n");
    fprintf(gen->out, "    double: alpha_generic_eq_f64, float: alpha_generic_eq_f64, \\\n");
    fprintf(gen->out, "    _Bool: alpha_generic_eq_bool, \\\n");
    fprintf(gen->out, "    default: alpha_generic_eq_i64)((a), (b))\n\n");

    // Assertion macros
    fprintf(gen->out, "#define alpha_assert_eq(left, right, file, line) do { \\\n");
    fprintf(gen->out, "    alpha_test_assertions++; \\\n");
    fprintf(gen->out, "    __auto_type _l = (left); __auto_type _r = (right); \\\n");
    fprintf(gen->out, "    if (!alpha_generic_eq(_l, _r)) { \\\n");
    fprintf(gen->out, "        alpha_test_current_failed = 1; \\\n");
    fprintf(gen->out, "        fprintf(stderr, \"  FAIL  %%s:%%d: assert_eq failed\\n\", alpha_test_file, line); \\\n");
    fprintf(gen->out, "        fprintf(stderr, \"         left:  \"); alpha_assert_print(_l); fprintf(stderr, \"\\n\"); \\\n");
    fprintf(gen->out, "        fprintf(stderr, \"         right: \"); alpha_assert_print(_r); fprintf(stderr, \"\\n\"); \\\n");
    fprintf(gen->out, "    } \\\n");
    fprintf(gen->out, "} while(0)\n\n");

    fprintf(gen->out, "#define alpha_assert_neq(left, right, file, line) do { \\\n");
    fprintf(gen->out, "    alpha_test_assertions++; \\\n");
    fprintf(gen->out, "    __auto_type _l = (left); __auto_type _r = (right); \\\n");
    fprintf(gen->out, "    if (alpha_generic_eq(_l, _r)) { \\\n");
    fprintf(gen->out, "        alpha_test_current_failed = 1; \\\n");
    fprintf(gen->out, "        fprintf(stderr, \"  FAIL  %%s:%%d: assert_neq failed (values are equal: \", alpha_test_file, line); \\\n");
    fprintf(gen->out, "        alpha_assert_print(_l); fprintf(stderr, \")\\n\"); \\\n");
    fprintf(gen->out, "    } \\\n");
    fprintf(gen->out, "} while(0)\n\n");

    fprintf(gen->out, "#define alpha_assert_lt(left, right, file, line) do { \\\n");
    fprintf(gen->out, "    alpha_test_assertions++; \\\n");
    fprintf(gen->out, "    __auto_type _l = (left); __auto_type _r = (right); \\\n");
    fprintf(gen->out, "    if (!(_l < _r)) { \\\n");
    fprintf(gen->out, "        alpha_test_current_failed = 1; \\\n");
    fprintf(gen->out, "        fprintf(stderr, \"  FAIL  %%s:%%d: assert_lt failed\\n\", alpha_test_file, line); \\\n");
    fprintf(gen->out, "        fprintf(stderr, \"         left:  \"); alpha_assert_print(_l); fprintf(stderr, \"\\n\"); \\\n");
    fprintf(gen->out, "        fprintf(stderr, \"         right: \"); alpha_assert_print(_r); fprintf(stderr, \"\\n\"); \\\n");
    fprintf(gen->out, "    } \\\n");
    fprintf(gen->out, "} while(0)\n\n");

    fprintf(gen->out, "#define alpha_assert_gt(left, right, file, line) do { \\\n");
    fprintf(gen->out, "    alpha_test_assertions++; \\\n");
    fprintf(gen->out, "    __auto_type _l = (left); __auto_type _r = (right); \\\n");
    fprintf(gen->out, "    if (!(_l > _r)) { \\\n");
    fprintf(gen->out, "        alpha_test_current_failed = 1; \\\n");
    fprintf(gen->out, "        fprintf(stderr, \"  FAIL  %%s:%%d: assert_gt failed\\n\", alpha_test_file, line); \\\n");
    fprintf(gen->out, "        fprintf(stderr, \"         left:  \"); alpha_assert_print(_l); fprintf(stderr, \"\\n\"); \\\n");
    fprintf(gen->out, "        fprintf(stderr, \"         right: \"); alpha_assert_print(_r); fprintf(stderr, \"\\n\"); \\\n");
    fprintf(gen->out, "    } \\\n");
    fprintf(gen->out, "} while(0)\n\n");

    fprintf(gen->out, "static inline void alpha_print_i64(int64_t v, int nl) { printf(nl ? \"%%lld\\n\" : \"%%lld\", (long long)v); }\n");
    fprintf(gen->out, "static inline void alpha_print_f64(double v, int nl) { printf(nl ? \"%%g\\n\" : \"%%g\", v); }\n");
    fprintf(gen->out, "static inline void alpha_print_str(const char* v, int nl) { printf(nl ? \"%%s\\n\" : \"%%s\", v); }\n");
    fprintf(gen->out, "static inline void alpha_print_bool(bool v, int nl) { printf(nl ? \"%%s\\n\" : \"%%s\", v ? \"true\" : \"false\"); }\n");
    fprintf(gen->out, "#define alpha_print(val, nl) _Generic((val), \\\n");
    fprintf(gen->out, "    int64_t: alpha_print_i64, \\\n");
    fprintf(gen->out, "    int: alpha_print_i64, \\\n");
    fprintf(gen->out, "    double: alpha_print_f64, \\\n");
    fprintf(gen->out, "    float: alpha_print_f64, \\\n");
    fprintf(gen->out, "    char*: alpha_print_str, \\\n");
    fprintf(gen->out, "    const char*: alpha_print_str, \\\n");
    fprintf(gen->out, "    _Bool: alpha_print_bool, \\\n");
    fprintf(gen->out, "    default: alpha_print_i64)((val), (nl))\n");
    fprintf(gen->out, "\n");

    // Recover (panic recovery) support
    fprintf(gen->out, "static jmp_buf* __alpha_recover_jmp = NULL;\n");
    fprintf(gen->out, "static void __alpha_panic(const char* msg) {\n");
    fprintf(gen->out, "    if (__alpha_recover_jmp) longjmp(*__alpha_recover_jmp, 1);\n");
    fprintf(gen->out, "    fprintf(stderr, \"%%s\\n\", msg); abort();\n");
    fprintf(gen->out, "}\n\n");

    // Dynamic array types
    fprintf(gen->out, "#define ALPHA_ARRAY_DECL(T, S) \\\n");
    fprintf(gen->out, "typedef struct { T* data; int64_t len; int64_t cap; } AlphaArr_##S; \\\n");
    fprintf(gen->out, "static inline AlphaArr_##S AlphaArr_##S##_new(void) { return (AlphaArr_##S){0,0,0}; } \\\n");
    fprintf(gen->out, "static inline void AlphaArr_##S##_push(AlphaArr_##S* a, T v) { \\\n");
    fprintf(gen->out, "  if (a->len >= a->cap) { a->cap = a->cap ? a->cap*2 : 8; a->data = realloc(a->data, sizeof(T)*a->cap); } \\\n");
    fprintf(gen->out, "  a->data[a->len++] = v; } \\\n");
    fprintf(gen->out, "static inline T AlphaArr_##S##_pop(AlphaArr_##S* a) { return a->data[--a->len]; } \\\n");
    fprintf(gen->out, "static inline void AlphaArr_##S##_clear(AlphaArr_##S* a) { a->len = 0; } \\\n");
    fprintf(gen->out, "static inline AlphaArr_##S AlphaArr_##S##_clone(AlphaArr_##S* a) { \\\n");
    fprintf(gen->out, "  AlphaArr_##S b = {malloc(sizeof(T)*a->cap), a->len, a->cap}; \\\n");
    fprintf(gen->out, "  memcpy(b.data, a->data, sizeof(T)*a->len); return b; } \\\n");
    fprintf(gen->out, "static inline void AlphaArr_##S##_free(AlphaArr_##S* a) { free(a->data); a->data=0; a->len=0; a->cap=0; }\n");
    fprintf(gen->out, "\n");
    fprintf(gen->out, "ALPHA_ARRAY_DECL(int64_t, i64)\n");
    fprintf(gen->out, "ALPHA_ARRAY_DECL(double, f64)\n");
    fprintf(gen->out, "ALPHA_ARRAY_DECL(const char*, str)\n");
    fprintf(gen->out, "ALPHA_ARRAY_DECL(bool, bool)\n");
    fprintf(gen->out, "ALPHA_ARRAY_DECL(uint8_t, u8)\n");
    fprintf(gen->out, "\n");

    // Array higher-order methods (same-type only for now)
    fprintf(gen->out, "#define ALPHA_ARRAY_HO(T, S) \\\n");
    fprintf(gen->out, "static inline AlphaArr_##S AlphaArr_##S##_map(AlphaArr_##S* a, T (*f)(T)) { \\\n");
    fprintf(gen->out, "  AlphaArr_##S r = AlphaArr_##S##_new(); \\\n");
    fprintf(gen->out, "  for (int64_t i = 0; i < a->len; i++) AlphaArr_##S##_push(&r, f(a->data[i])); return r; } \\\n");
    fprintf(gen->out, "static inline AlphaArr_##S AlphaArr_##S##_filter(AlphaArr_##S* a, bool (*f)(T)) { \\\n");
    fprintf(gen->out, "  AlphaArr_##S r = AlphaArr_##S##_new(); \\\n");
    fprintf(gen->out, "  for (int64_t i = 0; i < a->len; i++) if (f(a->data[i])) AlphaArr_##S##_push(&r, a->data[i]); return r; } \\\n");
    fprintf(gen->out, "static inline T AlphaArr_##S##_reduce(AlphaArr_##S* a, T init, T (*f)(T, T)) { \\\n");
    fprintf(gen->out, "  T acc = init; for (int64_t i = 0; i < a->len; i++) acc = f(acc, a->data[i]); return acc; } \\\n");
    fprintf(gen->out, "static inline bool AlphaArr_##S##_any(AlphaArr_##S* a, bool (*f)(T)) { \\\n");
    fprintf(gen->out, "  for (int64_t i = 0; i < a->len; i++) if (f(a->data[i])) return true; return false; } \\\n");
    fprintf(gen->out, "static inline bool AlphaArr_##S##_all(AlphaArr_##S* a, bool (*f)(T)) { \\\n");
    fprintf(gen->out, "  for (int64_t i = 0; i < a->len; i++) if (!f(a->data[i])) return false; return true; } \\\n");
    fprintf(gen->out, "static inline int64_t AlphaArr_##S##_count(AlphaArr_##S* a, bool (*f)(T)) { \\\n");
    fprintf(gen->out, "  int64_t n = 0; for (int64_t i = 0; i < a->len; i++) if (f(a->data[i])) n++; return n; }\n");
    fprintf(gen->out, "\n");
    fprintf(gen->out, "ALPHA_ARRAY_HO(int64_t, i64)\n");
    fprintf(gen->out, "ALPHA_ARRAY_HO(double, f64)\n");
    fprintf(gen->out, "ALPHA_ARRAY_HO(const char*, str)\n");
    fprintf(gen->out, "ALPHA_ARRAY_HO(bool, bool)\n");
    fprintf(gen->out, "\n");

    // Hash map (string keys, type-specific values)
    fprintf(gen->out, "#define ALPHA_MAP_DECL(VT, S) \\\n");
    fprintf(gen->out, "typedef struct { char** keys; VT* vals; int64_t len; int64_t cap; } AlphaMap_##S; \\\n");
    fprintf(gen->out, "static inline AlphaMap_##S AlphaMap_##S##_new(void) { return (AlphaMap_##S){0,0,0,0}; } \\\n");
    fprintf(gen->out, "static inline int64_t AlphaMap_##S##_find(AlphaMap_##S* m, const char* k) { \\\n");
    fprintf(gen->out, "  for (int64_t i = 0; i < m->len; i++) if (m->keys[i] && strcmp(m->keys[i], k) == 0) return i; \\\n");
    fprintf(gen->out, "  return -1; } \\\n");
    fprintf(gen->out, "static inline void AlphaMap_##S##_set(AlphaMap_##S* m, const char* k, VT v) { \\\n");
    fprintf(gen->out, "  int64_t i = AlphaMap_##S##_find(m, k); \\\n");
    fprintf(gen->out, "  if (i >= 0) { m->vals[i] = v; return; } \\\n");
    fprintf(gen->out, "  if (m->len >= m->cap) { m->cap = m->cap ? m->cap*2 : 8; \\\n");
    fprintf(gen->out, "    m->keys = realloc(m->keys, sizeof(char*)*m->cap); \\\n");
    fprintf(gen->out, "    m->vals = realloc(m->vals, sizeof(VT)*m->cap); } \\\n");
    fprintf(gen->out, "  m->keys[m->len] = strdup(k); m->vals[m->len] = v; m->len++; } \\\n");
    fprintf(gen->out, "static inline VT AlphaMap_##S##_get(AlphaMap_##S* m, const char* k, VT def) { \\\n");
    fprintf(gen->out, "  int64_t i = AlphaMap_##S##_find(m, k); return (i >= 0) ? m->vals[i] : def; } \\\n");
    fprintf(gen->out, "static inline bool AlphaMap_##S##_has(AlphaMap_##S* m, const char* k) { \\\n");
    fprintf(gen->out, "  return AlphaMap_##S##_find(m, k) >= 0; } \\\n");
    fprintf(gen->out, "static inline void AlphaMap_##S##_delete(AlphaMap_##S* m, const char* k) { \\\n");
    fprintf(gen->out, "  int64_t i = AlphaMap_##S##_find(m, k); if (i >= 0) { \\\n");
    fprintf(gen->out, "    free(m->keys[i]); m->keys[i] = m->keys[m->len-1]; m->vals[i] = m->vals[m->len-1]; m->len--; } } \\\n");
    fprintf(gen->out, "static inline AlphaArr_str AlphaMap_##S##_keys(AlphaMap_##S* m) { \\\n");
    fprintf(gen->out, "  AlphaArr_str a = AlphaArr_str_new(); \\\n");
    fprintf(gen->out, "  for (int64_t i = 0; i < m->len; i++) AlphaArr_str_push(&a, m->keys[i]); return a; } \\\n");
    fprintf(gen->out, "static inline void AlphaMap_##S##_free(AlphaMap_##S* m) { \\\n");
    fprintf(gen->out, "  for (int64_t i = 0; i < m->len; i++) free(m->keys[i]); \\\n");
    fprintf(gen->out, "  free(m->keys); free(m->vals); m->keys=0; m->vals=0; m->len=0; m->cap=0; }\n");
    fprintf(gen->out, "\n");
    fprintf(gen->out, "ALPHA_MAP_DECL(int64_t, i64)\n");
    fprintf(gen->out, "ALPHA_MAP_DECL(double, f64)\n");
    fprintf(gen->out, "ALPHA_MAP_DECL(const char*, str)\n");
    fprintf(gen->out, "ALPHA_MAP_DECL(bool, bool)\n");
    fprintf(gen->out, "\n");

    // Option type
    fprintf(gen->out, "#define ALPHA_OPTION_DECL(T, S) \\\n");
    fprintf(gen->out, "typedef struct { bool has_value; T value; } AlphaOpt_##S; \\\n");
    fprintf(gen->out, "static inline AlphaOpt_##S AlphaOpt_##S##_some(T v) { return (AlphaOpt_##S){true, v}; } \\\n");
    fprintf(gen->out, "static inline AlphaOpt_##S AlphaOpt_##S##_none(void) { AlphaOpt_##S o = {false}; return o; } \\\n");
    fprintf(gen->out, "static inline T AlphaOpt_##S##_unwrap(AlphaOpt_##S o) { \\\n");
    fprintf(gen->out, "  if (!o.has_value) { __alpha_panic(\"unwrap called on none\"); } return o.value; } \\\n");
    fprintf(gen->out, "static inline T AlphaOpt_##S##_unwrap_or(AlphaOpt_##S o, T def) { return o.has_value ? o.value : def; }\n");
    fprintf(gen->out, "\n");
    fprintf(gen->out, "ALPHA_OPTION_DECL(int64_t, i64)\n");
    fprintf(gen->out, "ALPHA_OPTION_DECL(double, f64)\n");
    fprintf(gen->out, "ALPHA_OPTION_DECL(const char*, str)\n");
    fprintf(gen->out, "ALPHA_OPTION_DECL(bool, bool)\n");
    fprintf(gen->out, "\n");

    // String split and join (after array types are defined)
    fprintf(gen->out, "static AlphaArr_str alpha_str_split(const char* s, const char* delim) {\n");
    fprintf(gen->out, "    AlphaArr_str a = AlphaArr_str_new();\n");
    fprintf(gen->out, "    int64_t dl = strlen(delim), sl = strlen(s);\n");
    fprintf(gen->out, "    int64_t start = 0;\n");
    fprintf(gen->out, "    for (int64_t i = 0; i <= sl; i++) {\n");
    fprintf(gen->out, "        if (i == sl || (dl > 0 && i + dl <= sl && strncmp(s + i, delim, dl) == 0)) {\n");
    fprintf(gen->out, "            char* part = malloc(i - start + 1);\n");
    fprintf(gen->out, "            memcpy(part, s + start, i - start); part[i - start] = 0;\n");
    fprintf(gen->out, "            AlphaArr_str_push(&a, part);\n");
    fprintf(gen->out, "            if (i < sl) i += dl - 1;\n");
    fprintf(gen->out, "            start = i + 1;\n");
    fprintf(gen->out, "        }\n");
    fprintf(gen->out, "    } return a;\n");
    fprintf(gen->out, "}\n");

    fprintf(gen->out, "static const char* alpha_str_join(AlphaArr_str* a, const char* sep) {\n");
    fprintf(gen->out, "    StringBuilder* sb = sb_new();\n");
    fprintf(gen->out, "    for (int64_t i = 0; i < a->len; i++) {\n");
    fprintf(gen->out, "        if (i > 0) sb_append(sb, sep);\n");
    fprintf(gen->out, "        sb_append(sb, a->data[i]);\n");
    fprintf(gen->out, "    } return sb->data;\n");
    fprintf(gen->out, "}\n\n");

    // String formatting
    fprintf(gen->out, "static const char* alpha_format(const char* fmt, ...) {\n");
    fprintf(gen->out, "    va_list args; va_start(args, fmt);\n");
    fprintf(gen->out, "    int n = vsnprintf(NULL, 0, fmt, args); va_end(args);\n");
    fprintf(gen->out, "    char* buf = malloc(n + 1);\n");
    fprintf(gen->out, "    va_start(args, fmt); vsnprintf(buf, n + 1, fmt, args); va_end(args);\n");
    fprintf(gen->out, "    return buf;\n");
    fprintf(gen->out, "}\n\n");

    // Need stdarg for format
    // (already included via the preamble headers, but let's make sure)

    // First pass: emit struct/enum declarations
    for (int i = 0; i < program->program.decls.count; i++) {
        ASTNode *decl = program->program.decls.items[i];
        if (decl->type == NODE_STRUCT_DECL || decl->type == NODE_ENUM_DECL) {
            emit_decl(gen, decl);
        }
    }

    // Emit array types for user structs
    for (int i = 0; i < program->program.decls.count; i++) {
        ASTNode *decl = program->program.decls.items[i];
        if (decl->type == NODE_STRUCT_DECL) {
            fprintf(gen->out, "ALPHA_ARRAY_DECL(%s, %s)\n", decl->struct_decl.name, decl->struct_decl.name);
        }
    }
    fprintf(gen->out, "\n");

    // Emit global variables
    for (int i = 0; i < program->program.decls.count; i++) {
        ASTNode *decl = program->program.decls.items[i];
        if (decl->type == NODE_LET_STMT) {
            gen->indent = 0;
            // Global arrays/maps must use {0} initializer (not function calls)
            if (decl->let_stmt.init && decl->let_stmt.init->type == NODE_ARRAY_LIT &&
                decl->let_stmt.init->array_lit.elements.count == 0) {
                if (decl->resolved_type && decl->resolved_type->kind != TYPE_UNKNOWN) {
                    fprintf(gen->out, "%s", type_to_c(decl->resolved_type));
                } else {
                    fprintf(gen->out, "AlphaArr_i64");
                }
                fprintf(gen->out, " %s = {0};\n", decl->let_stmt.name);
            } else {
                emit_stmt(gen, decl);
            }
        }
    }
    fprintf(gen->out, "\n");

    // Pre-scan for lambdas (collect all NODE_LAMBDA nodes)
    // First pass the AST to assign IDs, then emit lambda functions
    {
        // Scan function: walks all nodes looking for lambdas
        // We do this by emitting all functions to /dev/null first to collect lambdas
        FILE *saved_out = gen->out;
        gen->out = fopen("/dev/null", "w");
        for (int i = 0; i < program->program.decls.count; i++) {
            ASTNode *decl = program->program.decls.items[i];
            if (decl->type == NODE_FN_DECL || decl->type == NODE_IMPL_BLOCK) {
                emit_decl(gen, decl);
            }
        }
        fclose(gen->out);
        gen->out = saved_out;

        // Now emit the collected lambda functions
        for (int i = 0; i < gen->lambda_count; i++) {
            ASTNode *lam = gen->lambdas[i];
            // Emit: static RET __alpha_lambda_N(PARAMS) { BODY }
            if (lam->lambda.return_type) {
                emit_type(gen, lam->lambda.return_type);
            } else {
                fprintf(gen->out, "void");
            }
            fprintf(gen->out, " __alpha_lambda_%d(", i);
            bool first = true;
            for (int j = 0; j < lam->lambda.params.count; j++) {
                if (!first) fprintf(gen->out, ", ");
                first = false;
                Field *param = &lam->lambda.params.items[j];
                if (param->type) emit_type(gen, param->type);
                else fprintf(gen->out, "int64_t");
                fprintf(gen->out, " %s", param->name);
            }
            if (lam->lambda.params.count == 0) fprintf(gen->out, "void");
            fprintf(gen->out, ") {\n");
            gen->indent = 1;
            if (lam->lambda.body) {
                for (int j = 0; j < lam->lambda.body->block.stmts.count; j++) {
                    emit_stmt(gen, lam->lambda.body->block.stmts.items[j]);
                }
            }
            gen->indent = 0;
            fprintf(gen->out, "}\n\n");
        }
        // Reset lambda count so the real pass reuses the same IDs
        gen->lambda_count = 0;
    }

    // Forward declarations for functions
    emit_forward_decls(gen, program);

    // Emit monomorphized generic functions
    if (gen->type_table) {
        TypeTable *tt = (TypeTable *)gen->type_table;
        for (int mi = 0; mi < tt->mono_instance_count; mi++) {
            MonoInstance *inst = &tt->mono_instances[mi];
            GenericDef *gdef = find_generic_def(tt, inst->generic_name);
            if (!gdef || gdef->is_struct) continue;

            ASTNode *fn = (ASTNode *)gdef->ast_node;

            // Set substitution context
            gen->subst_param_names = gdef->type_param_names;
            gen->subst_concrete = inst->concrete_types;
            gen->subst_count = gdef->type_param_count;

            // Emit return type
            if (fn->fn_decl.return_type) {
                emit_type(gen, fn->fn_decl.return_type);
            } else {
                fprintf(gen->out, "void");
            }
            fprintf(gen->out, " %s(", inst->mangled_name);

            // Emit params
            bool first = true;
            for (int j = 0; j < fn->fn_decl.params.count; j++) {
                if (!first) fprintf(gen->out, ", ");
                first = false;
                Field *param = &fn->fn_decl.params.items[j];
                if (param->type) emit_type(gen, param->type);
                else fprintf(gen->out, "int64_t");
                fprintf(gen->out, " %s", param->name);
            }
            if (fn->fn_decl.params.count == 0) fprintf(gen->out, "void");
            fprintf(gen->out, ") {\n");

            // Emit body
            gen->indent = 1;
            int saved_defer = gen->defer_count;
            int saved_owned = gen->owned_count;
            gen->defer_count = 0;
            gen->owned_count = 0;

            if (fn->fn_decl.body) {
                for (int j = 0; j < fn->fn_decl.body->block.stmts.count; j++) {
                    emit_stmt(gen, fn->fn_decl.body->block.stmts.items[j]);
                }
            }

            if (gen->owned_count > 0) emit_auto_cleanup(gen, NULL);
            if (gen->defer_count > 0) emit_defers(gen);
            gen->defer_count = saved_defer;
            gen->owned_count = saved_owned;

            gen->indent = 0;
            fprintf(gen->out, "}\n\n");

            // Clear substitution
            gen->subst_count = 0;
        }
    }

    // Emit functions and impl blocks
    for (int i = 0; i < program->program.decls.count; i++) {
        ASTNode *decl = program->program.decls.items[i];
        // Skip generic templates — monomorphized versions are emitted below
        if (decl->type == NODE_FN_DECL && decl->fn_decl.type_params.count > 0) continue;
        if (decl->type == NODE_STRUCT_DECL && decl->struct_decl.type_params.count > 0) continue;
        // In test mode, skip the user's main()
        if (gen->test_mode && decl->type == NODE_FN_DECL &&
            strcmp(decl->fn_decl.name, "main") == 0) {
            continue;
        }
        if (decl->type == NODE_FN_DECL || decl->type == NODE_IMPL_BLOCK) {
            emit_decl(gen, decl);
        }
    }

    // In test mode: emit test functions and a test runner main
    if (gen->test_mode) {
        // Count tests and examples
        int test_count = 0;
        int example_fn_count = 0;
        for (int i = 0; i < program->program.decls.count; i++) {
            ASTNode *d = program->program.decls.items[i];
            if (d->type == NODE_TEST_DECL) test_count++;
            if (d->type == NODE_FN_DECL && d->fn_decl.examples.count > 0) example_fn_count++;
            if (d->type == NODE_FN_DECL && d->fn_decl.panics.count > 0) example_fn_count++;
        }

        if (test_count == 0 && example_fn_count == 0) return;

        // Emit each test as a void function
        int test_idx = 0;
        for (int i = 0; i < program->program.decls.count; i++) {
            ASTNode *decl = program->program.decls.items[i];
            if (decl->type != NODE_TEST_DECL) continue;

            fprintf(gen->out, "static void alpha_test_%d(void) {\n", test_idx);
            gen->indent = 1;
            for (int j = 0; j < decl->test_decl.body->block.stmts.count; j++) {
                emit_stmt(gen, decl->test_decl.body->block.stmts.items[j]);
            }
            gen->indent = 0;
            fprintf(gen->out, "}\n\n");
            test_idx++;
        }

        // Emit example test functions (from function contracts)
        int example_idx = 0;
        for (int i = 0; i < program->program.decls.count; i++) {
            ASTNode *decl = program->program.decls.items[i];
            if (decl->type != NODE_FN_DECL || decl->fn_decl.examples.count == 0) continue;

            fprintf(gen->out, "static void alpha_example_%d(void) {\n", example_idx);
            gen->indent = 1;
            for (int j = 0; j < decl->fn_decl.examples.count; j++) {
                emit_indent(gen);
                emit_smart_assert(gen, decl->fn_decl.examples.items[j]);
                fprintf(gen->out, ";\n");
            }
            gen->indent = 0;
            fprintf(gen->out, "}\n\n");
            example_idx++;
        }

        // Emit panics test functions (anti-examples that must crash)
        int panics_idx = 0;
        for (int i = 0; i < program->program.decls.count; i++) {
            ASTNode *decl = program->program.decls.items[i];
            if (decl->type != NODE_FN_DECL || decl->fn_decl.panics.count == 0) continue;

            for (int j = 0; j < decl->fn_decl.panics.count; j++) {
                fprintf(gen->out, "static void alpha_panics_%d(void) {\n", panics_idx);
                gen->indent = 1;
                emit_indent(gen);
                // Use setjmp to catch the panic
                fprintf(gen->out, "jmp_buf __pb; jmp_buf* __saved = __alpha_recover_jmp;\n");
                emit_indent(gen);
                fprintf(gen->out, "__alpha_recover_jmp = &__pb;\n");
                emit_indent(gen);
                fprintf(gen->out, "if (setjmp(__pb) != 0) {\n");
                gen->indent++; gen->scope_depth++;
                emit_indent(gen);
                fprintf(gen->out, "__alpha_recover_jmp = __saved;\n");
                emit_indent(gen);
                fprintf(gen->out, "return; /* panicked as expected */\n");
                gen->indent--; gen->scope_depth--;
                emit_indent(gen);
                fprintf(gen->out, "}\n");
                emit_indent(gen);
                emit_expr(gen, decl->fn_decl.panics.items[j]);
                fprintf(gen->out, ";\n");
                emit_indent(gen);
                fprintf(gen->out, "__alpha_recover_jmp = __saved;\n");
                emit_indent(gen);
                fprintf(gen->out, "alpha_test_current_failed = 1;\n");
                emit_indent(gen);
                fprintf(gen->out, "fprintf(stderr, \"  FAIL  %%s:%%d: expected panic but call succeeded\\n\", alpha_test_file, %d);\n",
                        decl->fn_decl.panics.items[j]->line);
                gen->indent = 0;
                fprintf(gen->out, "}\n\n");
                panics_idx++;
            }
        }

        // Emit test runner main
        fprintf(gen->out, "int main(int argc, char** argv) {\n");
        fprintf(gen->out, "    alpha_argc = argc;\n");
        fprintf(gen->out, "    alpha_argv = argv;\n");
        fprintf(gen->out, "    alpha_test_file = \"%s\";\n", gen->current_struct ? gen->current_struct : "");
        fprintf(gen->out, "    int json = 0;\n");
        fprintf(gen->out, "    int json_first = 1;\n");
        fprintf(gen->out, "    for (int i = 1; i < argc; i++) {\n");
        fprintf(gen->out, "        if (strcmp(argv[i], \"--json\") == 0) json = 1;\n");
        fprintf(gen->out, "    }\n");
        fprintf(gen->out, "    alpha_test_json = json;\n\n");

        fprintf(gen->out, "    if (json) printf(\"{\\\"file\\\": \\\"%%s\\\", \\\"tests\\\": [\", \"%s\");\n\n",
                gen->current_struct ? gen->current_struct : "");

        // Run each test
        test_idx = 0;
        for (int i = 0; i < program->program.decls.count; i++) {
            ASTNode *decl = program->program.decls.items[i];
            if (decl->type != NODE_TEST_DECL) continue;

            fprintf(gen->out, "    // Test: %s\n", decl->test_decl.name);
            fprintf(gen->out, "    alpha_test_current = \"%s\";\n", decl->test_decl.name);
            fprintf(gen->out, "    alpha_test_current_failed = 0;\n");
            fprintf(gen->out, "    alpha_test_json_buf = sb_new();\n");
            fprintf(gen->out, "    alpha_test_%d();\n", test_idx);
            fprintf(gen->out, "    if (alpha_test_current_failed) {\n");
            fprintf(gen->out, "        alpha_test_fail++;\n");
            fprintf(gen->out, "        if (!json) fprintf(stderr, \"  FAIL  %%s\\n\", alpha_test_current);\n");
            fprintf(gen->out, "        if (json) {\n");
            fprintf(gen->out, "            if (!json_first) printf(\", \"); json_first = 0;\n");
            fprintf(gen->out, "            printf(\"{\\\"name\\\": \\\"%%s\\\", \\\"status\\\": \\\"fail\\\", \\\"failures\\\": [\", alpha_test_current);\n");
            fprintf(gen->out, "            printf(\"%%s\", sb_to_str(alpha_test_json_buf));\n");
            fprintf(gen->out, "            printf(\"]}\");\n");
            fprintf(gen->out, "        }\n");
            fprintf(gen->out, "    } else {\n");
            fprintf(gen->out, "        alpha_test_pass++;\n");
            fprintf(gen->out, "        if (!json) fprintf(stderr, \"  PASS  %%s\\n\", alpha_test_current);\n");
            fprintf(gen->out, "        if (json) { if (!json_first) printf(\", \"); json_first = 0; printf(\"{\\\"name\\\": \\\"%%s\\\", \\\"status\\\": \\\"pass\\\"}\", alpha_test_current); }\n");
            fprintf(gen->out, "    }\n\n");
            test_idx++;
        }

        // Run example tests (from function contracts)
        example_idx = 0;
        for (int i = 0; i < program->program.decls.count; i++) {
            ASTNode *decl = program->program.decls.items[i];
            if (decl->type != NODE_FN_DECL || decl->fn_decl.examples.count == 0) continue;

            fprintf(gen->out, "    alpha_test_current = \"%s\";\n", decl->fn_decl.name);
            fprintf(gen->out, "    alpha_test_current_failed = 0;\n");
            fprintf(gen->out, "    alpha_test_json_buf = sb_new();\n");
            fprintf(gen->out, "    alpha_example_%d();\n", example_idx);
            fprintf(gen->out, "    if (alpha_test_current_failed) {\n");
            fprintf(gen->out, "        alpha_test_fail++;\n");
            fprintf(gen->out, "        if (!json) fprintf(stderr, \"  FAIL  %%s\\n\", alpha_test_current);\n");
            fprintf(gen->out, "        if (json) {\n");
            fprintf(gen->out, "            if (!json_first) printf(\", \"); json_first = 0;\n");
            fprintf(gen->out, "            printf(\"{\\\"name\\\": \\\"%%s\\\", \\\"status\\\": \\\"fail\\\", \\\"failures\\\": [\", alpha_test_current);\n");
            fprintf(gen->out, "            printf(\"%%s\", sb_to_str(alpha_test_json_buf));\n");
            fprintf(gen->out, "            printf(\"]}\");\n");
            fprintf(gen->out, "        }\n");
            fprintf(gen->out, "    } else {\n");
            fprintf(gen->out, "        alpha_test_pass++;\n");
            fprintf(gen->out, "        if (!json) fprintf(stderr, \"  PASS  %%s\\n\", alpha_test_current);\n");
            fprintf(gen->out, "        if (json) { if (!json_first) printf(\", \"); json_first = 0; printf(\"{\\\"name\\\": \\\"%%s\\\", \\\"status\\\": \\\"pass\\\"}\", alpha_test_current); }\n");
            fprintf(gen->out, "    }\n\n");
            example_idx++;
        }

        // Run panics tests (anti-examples)
        panics_idx = 0;
        for (int i = 0; i < program->program.decls.count; i++) {
            ASTNode *decl = program->program.decls.items[i];
            if (decl->type != NODE_FN_DECL || decl->fn_decl.panics.count == 0) continue;

            fprintf(gen->out, "    alpha_test_current = \"%s (panics)\";\n", decl->fn_decl.name);
            fprintf(gen->out, "    alpha_test_current_failed = 0;\n");
            fprintf(gen->out, "    alpha_test_json_buf = sb_new();\n");

            for (int j = 0; j < decl->fn_decl.panics.count; j++) {
                fprintf(gen->out, "    alpha_test_assertions++;\n");
                fprintf(gen->out, "    alpha_panics_%d();\n", panics_idx);
                panics_idx++;
            }

            fprintf(gen->out, "    if (alpha_test_current_failed) {\n");
            fprintf(gen->out, "        alpha_test_fail++;\n");
            fprintf(gen->out, "        if (!json) fprintf(stderr, \"  FAIL  %%s\\n\", alpha_test_current);\n");
            fprintf(gen->out, "        if (json) { if (!json_first) printf(\", \"); json_first = 0; printf(\"{\\\"name\\\": \\\"%%s\\\", \\\"status\\\": \\\"fail\\\"}\", alpha_test_current); }\n");
            fprintf(gen->out, "    } else {\n");
            fprintf(gen->out, "        alpha_test_pass++;\n");
            fprintf(gen->out, "        if (!json) fprintf(stderr, \"  PASS  %%s\\n\", alpha_test_current);\n");
            fprintf(gen->out, "        if (json) { if (!json_first) printf(\", \"); json_first = 0; printf(\"{\\\"name\\\": \\\"%%s\\\", \\\"status\\\": \\\"pass\\\"}\", alpha_test_current); }\n");
            fprintf(gen->out, "    }\n\n");
        }

        // Summary
        fprintf(gen->out, "    if (json) {\n");
        fprintf(gen->out, "        printf(\"], \\\"passed\\\": %%d, \\\"failed\\\": %%d, \\\"assertions\\\": %%d}\\n\",\n");
        fprintf(gen->out, "               alpha_test_pass, alpha_test_fail, alpha_test_assertions);\n");
        fprintf(gen->out, "    } else {\n");
        fprintf(gen->out, "        fprintf(stderr, \"\\n\");\n");
        fprintf(gen->out, "        if (alpha_test_fail == 0) {\n");
        fprintf(gen->out, "            fprintf(stderr, \"  %%d/%%d tests passed (%%d assertions)\\n\",\n");
        fprintf(gen->out, "                    alpha_test_pass, alpha_test_pass + alpha_test_fail, alpha_test_assertions);\n");
        fprintf(gen->out, "        } else {\n");
        fprintf(gen->out, "            fprintf(stderr, \"  %%d/%%d tests passed, %%d FAILED (%%d assertions)\\n\",\n");
        fprintf(gen->out, "                    alpha_test_pass, alpha_test_pass + alpha_test_fail,\n");
        fprintf(gen->out, "                    alpha_test_fail, alpha_test_assertions);\n");
        fprintf(gen->out, "        }\n");
        fprintf(gen->out, "    }\n");
        fprintf(gen->out, "    return alpha_test_fail > 0 ? 1 : 0;\n");
        fprintf(gen->out, "}\n");
    }
}
