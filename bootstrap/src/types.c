#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *my_strdup(const char *s) {
    int len = (int)strlen(s);
    char *d = malloc(len + 1);
    memcpy(d, s, len + 1);
    return d;
}

void type_table_init(TypeTable *tt) {
    // Create built-in singletons
    tt->t_void    = calloc(1, sizeof(Type)); tt->t_void->kind = TYPE_VOID;
    tt->t_bool    = calloc(1, sizeof(Type)); tt->t_bool->kind = TYPE_BOOL;
    tt->t_i8      = calloc(1, sizeof(Type)); tt->t_i8->kind = TYPE_I8;
    tt->t_i16     = calloc(1, sizeof(Type)); tt->t_i16->kind = TYPE_I16;
    tt->t_i32     = calloc(1, sizeof(Type)); tt->t_i32->kind = TYPE_I32;
    tt->t_i64     = calloc(1, sizeof(Type)); tt->t_i64->kind = TYPE_I64;
    tt->t_u8      = calloc(1, sizeof(Type)); tt->t_u8->kind = TYPE_U8;
    tt->t_u16     = calloc(1, sizeof(Type)); tt->t_u16->kind = TYPE_U16;
    tt->t_u32     = calloc(1, sizeof(Type)); tt->t_u32->kind = TYPE_U32;
    tt->t_u64     = calloc(1, sizeof(Type)); tt->t_u64->kind = TYPE_U64;
    tt->t_f32     = calloc(1, sizeof(Type)); tt->t_f32->kind = TYPE_F32;
    tt->t_f64     = calloc(1, sizeof(Type)); tt->t_f64->kind = TYPE_F64;
    tt->t_str     = calloc(1, sizeof(Type)); tt->t_str->kind = TYPE_STR;
    tt->t_unknown = calloc(1, sizeof(Type)); tt->t_unknown->kind = TYPE_UNKNOWN;

    tt->user_types = NULL;
    tt->user_type_count = 0;
    tt->user_type_cap = 0;
    tt->current_scope = NULL;

    // Push global scope
    scope_push(tt);
}

Type *type_new(TypeTable *tt, TypeKind kind, const char *name) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = kind;
    if (name) t->name = my_strdup(name);

    if (tt->user_type_count >= tt->user_type_cap) {
        tt->user_type_cap = tt->user_type_cap == 0 ? 32 : tt->user_type_cap * 2;
        tt->user_types = realloc(tt->user_types, sizeof(Type *) * tt->user_type_cap);
    }
    tt->user_types[tt->user_type_count++] = t;
    return t;
}

Type *type_resolve_name(TypeTable *tt, const char *name) {
    if (!name) return tt->t_unknown;

    // Built-in types
    if (strcmp(name, "void") == 0) return tt->t_void;
    if (strcmp(name, "bool") == 0) return tt->t_bool;
    if (strcmp(name, "i8") == 0)   return tt->t_i8;
    if (strcmp(name, "i16") == 0)  return tt->t_i16;
    if (strcmp(name, "i32") == 0)  return tt->t_i32;
    if (strcmp(name, "i64") == 0)  return tt->t_i64;
    if (strcmp(name, "u8") == 0)   return tt->t_u8;
    if (strcmp(name, "u16") == 0)  return tt->t_u16;
    if (strcmp(name, "u32") == 0)  return tt->t_u32;
    if (strcmp(name, "u64") == 0)  return tt->t_u64;
    if (strcmp(name, "f32") == 0)  return tt->t_f32;
    if (strcmp(name, "f64") == 0)  return tt->t_f64;
    if (strcmp(name, "str") == 0)  return tt->t_str;

    // User-defined types
    for (int i = 0; i < tt->user_type_count; i++) {
        if (tt->user_types[i]->name && strcmp(tt->user_types[i]->name, name) == 0) {
            return tt->user_types[i];
        }
    }

    return tt->t_unknown;
}

Type *type_new_ref(TypeTable *tt, Type *inner, bool is_mut) {
    Type *t = type_new(tt, TYPE_REF, NULL);
    t->ref_info.inner = inner;
    t->ref_info.is_mut = is_mut;
    return t;
}

Type *type_new_array(TypeTable *tt, Type *element) {
    Type *t = type_new(tt, TYPE_ARRAY, NULL);
    t->array_info.element = element;
    return t;
}

Type *type_new_fn(TypeTable *tt, Type **params, int param_count, Type *ret) {
    Type *t = type_new(tt, TYPE_FN, NULL);
    t->fn_info.params = params;
    t->fn_info.param_count = param_count;
    t->fn_info.ret = ret;
    return t;
}

// ---- Scopes ----

void scope_push(TypeTable *tt) {
    Scope *scope = calloc(1, sizeof(Scope));
    scope->parent = tt->current_scope;
    tt->current_scope = scope;
}

void scope_pop(TypeTable *tt) {
    if (tt->current_scope) {
        tt->current_scope = tt->current_scope->parent;
    }
}

void scope_define(TypeTable *tt, const char *name, Type *type, bool is_mut) {
    scope_define_at(tt, name, type, is_mut, 0, 0);
}

void scope_define_at(TypeTable *tt, const char *name, Type *type, bool is_mut, int line, int col) {
    Scope *scope = tt->current_scope;
    if (!scope) return;

    if (scope->count >= scope->capacity) {
        scope->capacity = scope->capacity == 0 ? 16 : scope->capacity * 2;
        scope->symbols = realloc(scope->symbols, sizeof(Symbol) * scope->capacity);
    }
    Symbol *sym = &scope->symbols[scope->count++];
    sym->name = my_strdup(name);
    sym->type = type;
    sym->is_mut = is_mut;
    sym->used = false;
    sym->line = line;
    sym->col = col;
}

Symbol *scope_lookup(TypeTable *tt, const char *name) {
    for (Scope *scope = tt->current_scope; scope; scope = scope->parent) {
        for (int i = scope->count - 1; i >= 0; i--) {
            if (strcmp(scope->symbols[i].name, name) == 0) {
                return &scope->symbols[i];
            }
        }
    }
    return NULL;
}

// ---- Utilities ----

bool type_is_integer(Type *t) {
    return t->kind >= TYPE_I8 && t->kind <= TYPE_U64;
}

bool type_is_float(Type *t) {
    return t->kind == TYPE_F32 || t->kind == TYPE_F64;
}

bool type_is_numeric(Type *t) {
    return type_is_integer(t) || type_is_float(t);
}

bool type_equals(Type *a, Type *b) {
    if (a == b) return true;
    if (a->kind != b->kind) return false;
    if (a->kind == TYPE_STRUCT || a->kind == TYPE_ENUM) {
        return a->name && b->name && strcmp(a->name, b->name) == 0;
    }
    if (a->kind == TYPE_REF) {
        return a->ref_info.is_mut == b->ref_info.is_mut &&
               type_equals(a->ref_info.inner, b->ref_info.inner);
    }
    if (a->kind == TYPE_ARRAY) {
        return type_equals(a->array_info.element, b->array_info.element);
    }
    return true;
}

const char *type_kind_name(TypeKind kind) {
    switch (kind) {
        case TYPE_VOID:    return "void";
        case TYPE_BOOL:    return "bool";
        case TYPE_I8:      return "i8";
        case TYPE_I16:     return "i16";
        case TYPE_I32:     return "i32";
        case TYPE_I64:     return "i64";
        case TYPE_U8:      return "u8";
        case TYPE_U16:     return "u16";
        case TYPE_U32:     return "u32";
        case TYPE_U64:     return "u64";
        case TYPE_F32:     return "f32";
        case TYPE_F64:     return "f64";
        case TYPE_STR:     return "str";
        case TYPE_STRUCT:  return "struct";
        case TYPE_ENUM:    return "enum";
        case TYPE_ARRAY:   return "array";
        case TYPE_REF:     return "ref";
        case TYPE_FN:      return "fn";
        case TYPE_MAP:     return "map";
        case TYPE_OPTION:  return "option";
        case TYPE_UNKNOWN: return "unknown";
    }
    return "?";
}

const char *type_to_c(Type *t) {
    if (!t) return "void";
    switch (t->kind) {
        case TYPE_VOID:    return "void";
        case TYPE_BOOL:    return "bool";
        case TYPE_I8:      return "int8_t";
        case TYPE_I16:     return "int16_t";
        case TYPE_I32:     return "int32_t";
        case TYPE_I64:     return "int64_t";
        case TYPE_U8:      return "uint8_t";
        case TYPE_U16:     return "uint16_t";
        case TYPE_U32:     return "uint32_t";
        case TYPE_U64:     return "uint64_t";
        case TYPE_F32:     return "float";
        case TYPE_F64:     return "double";
        case TYPE_STR:     return "const char*";
        case TYPE_STRUCT:  return t->name ? t->name : "void";
        case TYPE_ENUM:    return t->name ? t->name : "int";
        case TYPE_ARRAY: {
            static char arr_buf[128];
            if (t->array_info.element) {
                const char *elem = type_to_c(t->array_info.element);
                // Map element C type to suffix
                const char *suffix = "i64";
                if (t->array_info.element->kind == TYPE_F64) suffix = "f64";
                else if (t->array_info.element->kind == TYPE_F32) suffix = "f64";
                else if (t->array_info.element->kind == TYPE_STR) suffix = "str";
                else if (t->array_info.element->kind == TYPE_BOOL) suffix = "bool";
                else if (t->array_info.element->kind == TYPE_U8) suffix = "u8";
                else if (t->array_info.element->kind == TYPE_STRUCT && t->array_info.element->name)
                    suffix = t->array_info.element->name;
                snprintf(arr_buf, sizeof(arr_buf), "AlphaArr_%s", suffix);
                (void)elem;
            } else {
                snprintf(arr_buf, sizeof(arr_buf), "AlphaArr_i64");
            }
            return arr_buf;
        }
        case TYPE_REF:     return "void*";
        case TYPE_FN:      return "void*";
        case TYPE_OPTION: {
            static char opt_buf[128];
            const char *suffix = t->option_info.inner_type ? type_array_suffix(t->option_info.inner_type) : "i64";
            snprintf(opt_buf, sizeof(opt_buf), "AlphaOpt_%s", suffix);
            return opt_buf;
        }
        case TYPE_MAP: {
            static char map_buf[128];
            const char *suffix = t->map_info.value_type ? type_array_suffix(t->map_info.value_type) : "i64";
            snprintf(map_buf, sizeof(map_buf), "AlphaMap_%s", suffix);
            return map_buf;
        }
        case TYPE_UNKNOWN: return "int64_t";
    }
    return "void";
}

Type *type_new_map(TypeTable *tt, Type *value_type) {
    Type *t = type_new(tt, TYPE_MAP, NULL);
    t->map_info.value_type = value_type;
    return t;
}

Type *type_new_option(TypeTable *tt, Type *inner_type) {
    Type *t = type_new(tt, TYPE_OPTION, NULL);
    t->option_info.inner_type = inner_type;
    return t;
}

const char *type_array_suffix(Type *elem) {
    if (!elem) return "i64";
    switch (elem->kind) {
        case TYPE_I64: case TYPE_I32: case TYPE_I16: case TYPE_I8: return "i64";
        case TYPE_U64: case TYPE_U32: case TYPE_U16: return "i64";
        case TYPE_U8: return "u8";
        case TYPE_F64: case TYPE_F32: return "f64";
        case TYPE_STR: return "str";
        case TYPE_BOOL: return "bool";
        case TYPE_STRUCT: return elem->name ? elem->name : "i64";
        default: return "i64";
    }
}
