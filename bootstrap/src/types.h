#ifndef ALPHA_TYPES_H
#define ALPHA_TYPES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TYPE_VOID,
    TYPE_BOOL,
    TYPE_I8,
    TYPE_I16,
    TYPE_I32,
    TYPE_I64,
    TYPE_U8,
    TYPE_U16,
    TYPE_U32,
    TYPE_U64,
    TYPE_F32,
    TYPE_F64,
    TYPE_STR,
    TYPE_STRUCT,
    TYPE_ENUM,
    TYPE_ARRAY,
    TYPE_REF,
    TYPE_FN,
    TYPE_MAP,
    TYPE_OPTION,
    TYPE_UNKNOWN,
} TypeKind;

typedef struct Type Type;

// Field info for structs (resolved version)
typedef struct {
    char *name;
    Type *type;
} TypeField;

struct Type {
    TypeKind kind;
    char *name; // for struct/enum: type name

    union {
        // TYPE_STRUCT
        struct {
            TypeField *fields;
            int field_count;
        } struct_info;

        // TYPE_ENUM
        struct {
            char **variants;
            int variant_count;
        } enum_info;

        // TYPE_ARRAY
        struct {
            Type *element;
        } array_info;

        // TYPE_REF
        struct {
            Type *inner;
            bool is_mut;
        } ref_info;

        // TYPE_FN
        struct {
            Type **params;
            int param_count;
            Type *ret;
        } fn_info;

        // TYPE_MAP (string keys, typed values)
        struct {
            Type *value_type;
        } map_info;

        // TYPE_OPTION
        struct {
            Type *inner_type;
        } option_info;
    };
};

// Symbol in a scope
typedef struct {
    char *name;
    Type *type;
    bool is_mut;
    bool used;
    int line;
    int col;
} Symbol;

// Lexical scope
typedef struct Scope {
    Symbol *symbols;
    int count;
    int capacity;
    struct Scope *parent;
} Scope;

// Type table: owns all types and scopes
typedef struct {
    // Built-in type singletons
    Type *t_void;
    Type *t_bool;
    Type *t_i8;
    Type *t_i16;
    Type *t_i32;
    Type *t_i64;
    Type *t_u8;
    Type *t_u16;
    Type *t_u32;
    Type *t_u64;
    Type *t_f32;
    Type *t_f64;
    Type *t_str;
    Type *t_unknown;

    // User-defined types
    Type **user_types;
    int user_type_count;
    int user_type_cap;

    // Scope stack
    Scope *current_scope;
} TypeTable;

// Type table operations
void type_table_init(TypeTable *tt);
Type *type_new(TypeTable *tt, TypeKind kind, const char *name);
Type *type_resolve_name(TypeTable *tt, const char *name);
Type *type_new_ref(TypeTable *tt, Type *inner, bool is_mut);
Type *type_new_array(TypeTable *tt, Type *element);
Type *type_new_fn(TypeTable *tt, Type **params, int param_count, Type *ret);
Type *type_new_map(TypeTable *tt, Type *value_type);
Type *type_new_option(TypeTable *tt, Type *inner_type);

// Scope operations
void scope_push(TypeTable *tt);
void scope_pop(TypeTable *tt);
void scope_define(TypeTable *tt, const char *name, Type *type, bool is_mut);
void scope_define_at(TypeTable *tt, const char *name, Type *type, bool is_mut, int line, int col);
Symbol *scope_lookup(TypeTable *tt, const char *name);

// Type utilities
bool type_is_numeric(Type *t);
bool type_is_integer(Type *t);
bool type_is_float(Type *t);
bool type_equals(Type *a, Type *b);
const char *type_kind_name(TypeKind kind);
const char *type_to_c(Type *t);
const char *type_array_suffix(Type *elem);

#endif
