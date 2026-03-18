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
    tt->generic_defs = NULL;
    tt->generic_def_count = 0;
    tt->generic_def_cap = 0;
    tt->mono_instances = NULL;
    tt->mono_instance_count = 0;
    tt->mono_instance_cap = 0;

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
        case TYPE_PARAM:   return "T";
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
        case TYPE_PARAM:   return "/* TYPE_PARAM */void";
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

Type *type_new_param(TypeTable *tt, const char *name, int index) {
    Type *t = type_new(tt, TYPE_PARAM, name);
    t->param_info.index = index;
    return t;
}

// ---- Generics ----

void register_generic_def(TypeTable *tt, const char *name, char **param_names, int param_count, void *ast_node, bool is_struct) {
    if (tt->generic_def_count >= tt->generic_def_cap) {
        tt->generic_def_cap = tt->generic_def_cap ? tt->generic_def_cap * 2 : 16;
        tt->generic_defs = realloc(tt->generic_defs, sizeof(GenericDef) * tt->generic_def_cap);
    }
    GenericDef *def = &tt->generic_defs[tt->generic_def_count++];
    def->name = my_strdup(name);
    def->type_param_names = param_names;
    def->type_param_count = param_count;
    def->ast_node = ast_node;
    def->is_struct = is_struct;
}

GenericDef *find_generic_def(TypeTable *tt, const char *name) {
    for (int i = 0; i < tt->generic_def_count; i++) {
        if (strcmp(tt->generic_defs[i].name, name) == 0)
            return &tt->generic_defs[i];
    }
    return NULL;
}

static const char *type_mangle_suffix(Type *t) {
    static char buf[128];
    if (!t) return "void";
    switch (t->kind) {
        case TYPE_I64: case TYPE_I32: case TYPE_I16: case TYPE_I8: return "i64";
        case TYPE_U64: case TYPE_U32: case TYPE_U16: return "i64";
        case TYPE_U8: return "u8";
        case TYPE_F64: case TYPE_F32: return "f64";
        case TYPE_STR: return "str";
        case TYPE_BOOL: return "bool";
        case TYPE_STRUCT: return t->name ? t->name : "struct";
        case TYPE_ARRAY:
            snprintf(buf, sizeof(buf), "arr_%s", type_mangle_suffix(t->array_info.element));
            return buf;
        case TYPE_OPTION:
            snprintf(buf, sizeof(buf), "opt_%s", type_mangle_suffix(t->option_info.inner_type));
            return buf;
        default: return "unknown";
    }
}

char *mangle_generic_name(const char *base, Type **types, int count) {
    char buf[512];
    int pos = snprintf(buf, sizeof(buf), "%s", base);
    for (int i = 0; i < count; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "_%s", type_mangle_suffix(types[i]));
    }
    return my_strdup(buf);
}

MonoInstance *find_mono_instance(TypeTable *tt, const char *name, Type **types, int count) {
    for (int i = 0; i < tt->mono_instance_count; i++) {
        MonoInstance *mi = &tt->mono_instances[i];
        if (strcmp(mi->generic_name, name) != 0) continue;
        if (mi->type_count != count) continue;
        bool match = true;
        for (int j = 0; j < count; j++) {
            if (!type_equals(mi->concrete_types[j], types[j])) { match = false; break; }
        }
        if (match) return mi;
    }
    return NULL;
}

MonoInstance *add_mono_instance(TypeTable *tt, const char *name, Type **types, int count) {
    if (tt->mono_instance_count >= tt->mono_instance_cap) {
        tt->mono_instance_cap = tt->mono_instance_cap ? tt->mono_instance_cap * 2 : 16;
        tt->mono_instances = realloc(tt->mono_instances, sizeof(MonoInstance) * tt->mono_instance_cap);
    }
    MonoInstance *mi = &tt->mono_instances[tt->mono_instance_count++];
    mi->generic_name = my_strdup(name);
    mi->concrete_types = malloc(sizeof(Type *) * count);
    memcpy(mi->concrete_types, types, sizeof(Type *) * count);
    mi->type_count = count;
    mi->mangled_name = mangle_generic_name(name, types, count);
    return mi;
}

Type *type_substitute(TypeTable *tt, Type *t, int param_count, Type **concrete) {
    if (!t) return NULL;
    if (t->kind == TYPE_PARAM) {
        int idx = t->param_info.index;
        if (idx >= 0 && idx < param_count) return concrete[idx];
        return t;
    }
    if (t->kind == TYPE_ARRAY) {
        Type *elem = type_substitute(tt, t->array_info.element, param_count, concrete);
        if (elem == t->array_info.element) return t;
        return type_new_array(tt, elem);
    }
    if (t->kind == TYPE_OPTION) {
        Type *inner = type_substitute(tt, t->option_info.inner_type, param_count, concrete);
        if (inner == t->option_info.inner_type) return t;
        return type_new_option(tt, inner);
    }
    if (t->kind == TYPE_REF) {
        Type *inner = type_substitute(tt, t->ref_info.inner, param_count, concrete);
        if (inner == t->ref_info.inner) return t;
        return type_new_ref(tt, inner, t->ref_info.is_mut);
    }
    if (t->kind == TYPE_FN) {
        Type **params = malloc(sizeof(Type *) * t->fn_info.param_count);
        for (int i = 0; i < t->fn_info.param_count; i++)
            params[i] = type_substitute(tt, t->fn_info.params[i], param_count, concrete);
        Type *ret = type_substitute(tt, t->fn_info.ret, param_count, concrete);
        return type_new_fn(tt, params, t->fn_info.param_count, ret);
    }
    return t; // concrete types pass through unchanged
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
