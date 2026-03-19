#include "lexer.h"
#include "parser.h"
#include "checker.h"
#include "codegen.h"
#include "error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        error_fatal("could not open file '%s'", path);
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *buffer = malloc(size + 1);
    if (!buffer) {
        error_fatal("out of memory reading '%s'", path);
    }

    size_t read = fread(buffer, 1, size, file);
    buffer[read] = '\0';
    fclose(file);

    return buffer;
}

static void print_usage(void) {
    fprintf(stderr, "Usage: alphac <file.alpha> [-o output]\n");
    fprintf(stderr, "       alphac test <file.alpha> [--json]  (run tests)\n");
    fprintf(stderr, "       alphac watch <file.alpha>          (recompile on change)\n");
    fprintf(stderr, "       alphac --emit-c <file.alpha>       (print generated C to stdout)\n");
}

#include <sys/stat.h>

static long get_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return st.st_mtime;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char *input_file = NULL;
    const char *output_file = NULL;
    bool emit_c = false;
    bool test_mode = false;
    bool test_json = false;
    bool watch_mode = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "watch") == 0) {
            watch_mode = true;
        } else if (strcmp(argv[i], "test") == 0) {
            test_mode = true;
        } else if (strcmp(argv[i], "--json") == 0) {
            test_json = true;
        } else if (strcmp(argv[i], "--emit-c") == 0) {
            emit_c = true;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                error_fatal("-o requires an argument");
            }
            output_file = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        } else if (argv[i][0] != '-') {
            input_file = argv[i];
        } else {
            error_fatal("unknown option '%s'", argv[i]);
        }
    }

    if (!input_file) {
        error_fatal("no input file specified");
    }

    // Default output name: replace .alpha with nothing, or append .out
    char default_output[256];
    if (!output_file) {
        const char *dot = strrchr(input_file, '.');
        const char *slash = strrchr(input_file, '/');
        const char *basename = slash ? slash + 1 : input_file;
        if (dot && dot > basename) {
            int len = (int)(dot - basename);
            snprintf(default_output, sizeof(default_output), "%.*s", len, basename);
        } else {
            snprintf(default_output, sizeof(default_output), "%s.out", basename);
        }
        output_file = default_output;
    }

    // Watch mode: recompile on change
    if (watch_mode) {
        fprintf(stderr, "\033[1;36m[watch]\033[0m watching %s (Ctrl+C to stop)\n\n", input_file);
        long last_mtime = 0;
        while (1) {
            long mtime = get_mtime(input_file);
            if (mtime != last_mtime) {
                last_mtime = mtime;
                // Clear screen
                fprintf(stderr, "\033[2J\033[H");
                fprintf(stderr, "\033[1;36m[watch]\033[0m compiling %s...\n", input_file);

                // Build command and run
                char cmd[1024];
                if (test_mode) {
                    snprintf(cmd, sizeof(cmd), "%s test %s 2>&1", argv[0], input_file);
                } else {
                    char bin_path[512];
                    snprintf(bin_path, sizeof(bin_path), "/tmp/alpha_watch_%d", getpid());
                    snprintf(cmd, sizeof(cmd), "%s %s -o %s 2>&1 && %s 2>&1", argv[0], input_file, bin_path, bin_path);
                }
                int result = system(cmd);
                if (result != 0) {
                    fprintf(stderr, "\n\033[1;31m[watch]\033[0m build failed\n");
                } else {
                    fprintf(stderr, "\n\033[1;32m[watch]\033[0m OK\n");
                }
            }
            usleep(500000); // 500ms
        }
        return 0; // unreachable
    }

    // Read source
    char *source = read_file(input_file);

    // Set up diagnostics
    DiagContext diag;
    diag_init(&diag, source, input_file);
    g_diag = &diag;

    // Lex & Parse
    Lexer lexer;
    lexer_init(&lexer, source, input_file);

    Parser parser;
    parser_init(&parser, &lexer, input_file);

    ASTNode *program = parser_parse(&parser);

    // Process imports: parse imported files and merge declarations
    for (int i = 0; i < program->program.decls.count; i++) {
        ASTNode *decl = program->program.decls.items[i];
        if (decl->type != NODE_IMPORT) continue;

        // Resolve path: absolute paths used as-is, relative resolved from input file
        char import_path[512];
        if (decl->import_decl.path[0] == '/') {
            snprintf(import_path, sizeof(import_path), "%s", decl->import_decl.path);
        } else {
            const char *last_slash = strrchr(input_file, '/');
            if (last_slash) {
                int dir_len = (int)(last_slash - input_file + 1);
                snprintf(import_path, sizeof(import_path), "%.*s%s", dir_len, input_file, decl->import_decl.path);
            } else {
                snprintf(import_path, sizeof(import_path), "%s", decl->import_decl.path);
            }
        }

        char *imp_source = read_file(import_path);
        Lexer imp_lexer;
        lexer_init(&imp_lexer, imp_source, import_path);
        Parser imp_parser;
        parser_init(&imp_parser, &imp_lexer, import_path);
        ASTNode *imp_program = parser_parse(&imp_parser);

        // Merge declarations (skip main, test blocks, and imports)
        for (int j = 0; j < imp_program->program.decls.count; j++) {
            ASTNode *imp_decl = imp_program->program.decls.items[j];
            if (imp_decl->type == NODE_FN_DECL && strcmp(imp_decl->fn_decl.name, "main") == 0) continue;
            if (imp_decl->type == NODE_TEST_DECL) continue;
            if (imp_decl->type == NODE_IMPORT) continue;
            node_list_push(&program->program.decls, imp_decl);
        }
    }

    // Type check (non-fatal: collect all errors before exiting)
    diag.fatal_on_error = false;
    Checker checker;
    checker_init(&checker, input_file);
    checker_check(&checker, program);
    diag.fatal_on_error = true;

    if (diag_has_errors(&diag)) {
        fprintf(stderr, "\n%d error%s found\n", diag.error_count,
                diag.error_count == 1 ? "" : "s");
        free(source);
        return 1;
    }

    if (emit_c) {
        CodeGen gen;
        codegen_init(&gen, stdout);
        gen.test_mode = test_mode;
        gen.type_table = &checker.types;
        codegen_emit(&gen, program);
        free(source);
        return 0;
    }

    if (test_mode) {
        // Compile and run tests
        char c_path[512];
        snprintf(c_path, sizeof(c_path), "/tmp/alpha_test_%d.c", getpid());
        char bin_path[512];
        snprintf(bin_path, sizeof(bin_path), "/tmp/alpha_test_%d", getpid());

        FILE *c_file = fopen(c_path, "w");
        if (!c_file) error_fatal("could not create temporary C file");

        CodeGen gen;
        codegen_init(&gen, c_file);
        gen.test_mode = true;
        gen.test_filename = input_file;
        gen.type_table = &checker.types;
        codegen_emit(&gen, program);
        fclose(c_file);

        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "cc -O2 -w -o %s %s -lm", bin_path, c_path);
        int result = system(cmd);
        if (result != 0) error_fatal("test compilation failed");

        // Run tests, forwarding --json if present
        if (test_json) {
            snprintf(cmd, sizeof(cmd), "%s --json", bin_path);
        } else {
            snprintf(cmd, sizeof(cmd), "%s", bin_path);
        }
        result = system(cmd);

        remove(c_path);
        remove(bin_path);

        free(source);
        return WEXITSTATUS(result);
    }

    // Generate C to temp file
    char c_path[512];
    snprintf(c_path, sizeof(c_path), "/tmp/alpha_%d.c", getpid());

    FILE *c_file = fopen(c_path, "w");
    if (!c_file) {
        error_fatal("could not create temporary C file");
    }

    CodeGen gen;
    codegen_init(&gen, c_file);
    gen.type_table = &checker.types;
    codegen_emit(&gen, program);
    fclose(c_file);

    // Compile C to binary
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cc -O2 -w -o %s %s -lm", output_file, c_path);

    int result = system(cmd);
    if (result != 0) {
        error_fatal("C compilation failed (exit code %d)", result);
    }

    // Clean up temp file
    remove(c_path);

    printf("compiled: %s -> %s\n", input_file, output_file);

    free(source);
    return 0;
}
