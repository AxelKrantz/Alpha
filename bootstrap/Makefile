CC = cc
CFLAGS = -Wall -Wextra -O2 -std=c11
SRC = src/main.c src/lexer.c src/parser.c src/ast.c src/codegen.c src/types.c src/checker.c src/error.c
OUT = alphac

.PHONY: all clean test

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) -o $(OUT) $(SRC)

debug: CFLAGS = -Wall -Wextra -g -O0 -std=c11 -DDEBUG
debug: $(OUT)

clean:
	rm -f $(OUT)
	rm -f /tmp/alpha_*.c

test: $(OUT)
	@echo "=== Compiling examples ==="
	./$(OUT) ../examples/hello.alpha -o /tmp/alpha_hello && /tmp/alpha_hello
	./$(OUT) ../examples/fibonacci.alpha -o /tmp/alpha_fib && /tmp/alpha_fib
	./$(OUT) ../examples/structs.alpha -o /tmp/alpha_structs && /tmp/alpha_structs
	./$(OUT) ../examples/match.alpha -o /tmp/alpha_match && /tmp/alpha_match
	./$(OUT) ../examples/methods.alpha -o /tmp/alpha_methods && /tmp/alpha_methods
	./$(OUT) ../examples/selfhost_test.alpha -o /tmp/alpha_selfhost && /tmp/alpha_selfhost
	./$(OUT) ../examples/mini_lexer.alpha -o /tmp/alpha_minilex && /tmp/alpha_minilex
	./$(OUT) test ../examples/tests.alpha
	./$(OUT) test ../examples/contracts.alpha
	./$(OUT) ../examples/arrays.alpha -o /tmp/alpha_arrays && /tmp/alpha_arrays
	./$(OUT) ../examples/imports.alpha -o /tmp/alpha_imports && /tmp/alpha_imports
	./$(OUT) ../examples/real_program.alpha -o /tmp/alpha_real && /tmp/alpha_real
	./$(OUT) test ../examples/json.alpha
	./$(OUT) ../examples/json.alpha -o /tmp/alpha_json && /tmp/alpha_json > /dev/null
	@echo "=== All tests passed ==="
