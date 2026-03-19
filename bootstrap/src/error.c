#include "error.h"
#include "ast.h"
#include <string.h>

DiagContext *g_diag = NULL;

void diag_init(DiagContext *ctx, const char *source, const char *filename) {
    ctx->source = source;
    ctx->filename = filename;
    ctx->json_mode = false;
    ctx->items = NULL;
    ctx->count = 0;
    ctx->capacity = 0;
    ctx->error_count = 0;
    ctx->warning_count = 0;
    ctx->fatal_on_error = true; // default: exit on first error
}

// Get a specific line from source (returns pointer, sets length)
static const char *get_source_line(const char *source, int target_line, int *out_len) {
    if (!source) { *out_len = 0; return ""; }
    const char *p = source;
    int current_line = 1;
    while (*p && current_line < target_line) {
        if (*p == '\n') current_line++;
        p++;
    }
    const char *start = p;
    while (*p && *p != '\n') p++;
    *out_len = (int)(p - start);
    return start;
}

// Pretty-print a single diagnostic to stderr
static void diag_print_pretty(DiagContext *ctx, Diagnostic *d) {
    // Level label with color
    const char *label, *color, *reset;
    bool use_color = true; // could check isatty

    if (use_color) {
        reset = "\033[0m";
        switch (d->level) {
            case DIAG_ERROR:   label = "error";   color = "\033[1;31m"; break;
            case DIAG_WARNING: label = "warning"; color = "\033[1;33m"; break;
            case DIAG_NOTE:    label = "note";    color = "\033[1;36m"; break;
        }
    } else {
        reset = "";
        color = "";
        switch (d->level) {
            case DIAG_ERROR:   label = "error";   break;
            case DIAG_WARNING: label = "warning"; break;
            case DIAG_NOTE:    label = "note";    break;
        }
    }

    // Header: "error: message"
    fprintf(stderr, "%s%s%s: %s\n", color, label, reset, d->message);

    // Location: "  --> file:line:col"
    fprintf(stderr, "  %s-->%s %s:%d:%d\n", use_color ? "\033[1;34m" : "", reset,
            d->file, d->line, d->col);

    // Source line with underline
    if (ctx->source) {
        int line_len;
        const char *line = get_source_line(ctx->source, d->line, &line_len);

        // Line number gutter
        char line_num[16];
        snprintf(line_num, sizeof(line_num), "%d", d->line);
        int gutter_width = (int)strlen(line_num);

        // Empty gutter line
        fprintf(stderr, " %*s %s|%s\n", gutter_width, "", use_color ? "\033[1;34m" : "", reset);

        // Source line
        fprintf(stderr, " %s%s%s %s|%s %.*s\n",
                use_color ? "\033[1;34m" : "", line_num, reset,
                use_color ? "\033[1;34m" : "", reset,
                line_len, line);

        // Caret/underline
        int span_len = d->length > 0 ? d->length : 1;
        fprintf(stderr, " %*s %s|%s ", gutter_width, "", use_color ? "\033[1;34m" : "", reset);
        for (int i = 1; i < d->col; i++) fprintf(stderr, " ");
        fprintf(stderr, "%s", color);
        for (int i = 0; i < span_len; i++) fprintf(stderr, "^");
        // Suggestion on same line
        if (d->suggestion[0]) {
            fprintf(stderr, " %s", d->suggestion);
        }
        fprintf(stderr, "%s\n", reset);
    }

    fprintf(stderr, "\n");
}

void diag_emit(DiagContext *ctx, DiagLevel level, int line, int col, int length,
               const char *suggestion, const char *fmt, ...) {
    if (!ctx) {
        // Fallback: no context, use old-style error
        fprintf(stderr, "error: ");
        va_list args;
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
        fprintf(stderr, "\n");
        if (level == DIAG_ERROR) exit(1);
        return;
    }

    // Build diagnostic
    Diagnostic d;
    d.level = level;
    d.file = ctx->filename;
    d.line = line;
    d.col = col;
    d.length = length;
    d.suggestion[0] = '\0';

    va_list args;
    va_start(args, fmt);
    vsnprintf(d.message, sizeof(d.message), fmt, args);
    va_end(args);

    if (suggestion) {
        snprintf(d.suggestion, sizeof(d.suggestion), "%s", suggestion);
    }

    // Track counts
    if (level == DIAG_ERROR) ctx->error_count++;
    if (level == DIAG_WARNING) ctx->warning_count++;

    // Store for JSON mode
    if (ctx->json_mode) {
        GROW_ARRAY(ctx->items, ctx->count, ctx->capacity, Diagnostic);
        ctx->items[ctx->count++] = d;
    } else {
        // Pretty print immediately
        diag_print_pretty(ctx, &d);
    }

    // Fatal on error unless collecting
    if (level == DIAG_ERROR && ctx->fatal_on_error) {
        exit(1);
    }
}

void diag_print_json(DiagContext *ctx) {
    printf("{\"file\": \"%s\", \"diagnostics\": [", ctx->filename);
    for (int i = 0; i < ctx->count; i++) {
        Diagnostic *d = &ctx->items[i];
        if (i > 0) printf(", ");
        printf("{\"level\": \"%s\", \"message\": \"",
               d->level == DIAG_ERROR ? "error" :
               d->level == DIAG_WARNING ? "warning" : "note");
        // Escape message for JSON
        for (const char *p = d->message; *p; p++) {
            if (*p == '"') printf("\\\"");
            else if (*p == '\\') printf("\\\\");
            else if (*p == '\n') printf("\\n");
            else putchar(*p);
        }
        printf("\", \"line\": %d, \"col\": %d", d->line, d->col);
        if (d->length > 0) printf(", \"length\": %d", d->length);
        if (d->suggestion[0]) {
            printf(", \"suggestion\": \"");
            for (const char *p = d->suggestion; *p; p++) {
                if (*p == '"') printf("\\\"");
                else putchar(*p);
            }
            printf("\"");
        }
        printf("}");
    }
    printf("], \"errors\": %d, \"warnings\": %d}\n", ctx->error_count, ctx->warning_count);
}

bool diag_has_errors(DiagContext *ctx) {
    return ctx->error_count > 0;
}

// Legacy API — still used by parser/checker for now
void error_at(const char *filename, int line, int col, const char *fmt, ...) {
    if (g_diag) {
        char msg[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);
        diag_emit(g_diag, DIAG_ERROR, line, col, 0, NULL, "%s", msg);
        return;
    }
    fprintf(stderr, "%s:%d:%d: error: ", filename, line, col);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    exit(1);
}

void error_fatal(const char *fmt, ...) {
    fprintf(stderr, "fatal: ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    exit(1);
}
