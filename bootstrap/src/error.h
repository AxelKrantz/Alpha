#ifndef ALPHA_ERROR_H
#define ALPHA_ERROR_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>

typedef enum {
    DIAG_ERROR,
    DIAG_WARNING,
    DIAG_NOTE,
} DiagLevel;

typedef struct {
    DiagLevel level;
    const char *file;
    int line;
    int col;
    int length;          // length of the underlined span (0 = use default)
    char message[512];
    char suggestion[256]; // optional "did you mean X?"
} Diagnostic;

typedef struct {
    const char *source;   // full source text (for showing context lines)
    const char *filename;
    bool json_mode;       // output JSON instead of pretty text

    Diagnostic *items;
    int count;
    int capacity;
    int error_count;
    int warning_count;
    bool fatal_on_error; // if true, exit(1) on first error; if false, collect
} DiagContext;

// Initialize diagnostic context
void diag_init(DiagContext *ctx, const char *source, const char *filename);

// Emit diagnostics
void diag_emit(DiagContext *ctx, DiagLevel level, int line, int col, int length,
               const char *suggestion, const char *fmt, ...);

// Convenience macros
#define diag_error(ctx, line, col, ...) \
    diag_emit(ctx, DIAG_ERROR, line, col, 0, NULL, __VA_ARGS__)
#define diag_error_span(ctx, line, col, len, ...) \
    diag_emit(ctx, DIAG_ERROR, line, col, len, NULL, __VA_ARGS__)
#define diag_warning(ctx, line, col, ...) \
    diag_emit(ctx, DIAG_WARNING, line, col, 0, NULL, __VA_ARGS__)
#define diag_note(ctx, line, col, ...) \
    diag_emit(ctx, DIAG_NOTE, line, col, 0, NULL, __VA_ARGS__)

// Print all collected diagnostics (for JSON mode — batch output)
void diag_print_json(DiagContext *ctx);

// Check if any errors occurred
bool diag_has_errors(DiagContext *ctx);

// Old API — still works, exits immediately
void error_at(const char *filename, int line, int col, const char *fmt, ...);
void error_fatal(const char *fmt, ...);

// Global diagnostic context (set by main before parsing)
extern DiagContext *g_diag;

#endif
