# Alpha

A compiled, statically-typed programming language designed to be the easiest language for AI to write in.

Alpha compiles to C, then to native binaries. It's fast, safe, and self-hosting.

## Quick Start

```bash
# Build the compiler
cd bootstrap && make

# Compile and run a program
./alphac ../examples/hello.alpha -o hello && ./hello
```

## What makes Alpha different

**Built for AI agents.** Every design decision optimizes for the AI coding workflow: write code, get structured feedback, fix, repeat.

### Contracts are the spec

Functions carry their own specification. No separate test files.

```alpha
fn fibonacci(n: i64) -> i64
    example fibonacci(0) == 0
    example fibonacci(1) == 1
    example fibonacci(10) == 55
    requires n >= 0
    ensures result >= 0
{
    if n <= 1 { return n }
    return fibonacci(n - 1) + fibonacci(n - 2)
}
```

- `example` — executable spec, verified by `alphac test`, stripped in production
- `requires` — precondition, checked at runtime with argument values on failure
- `ensures` — postcondition, checked on every return path
- `panics` — anti-example, verifies bad inputs are rejected

### Smart diagnostics

Errors show source context with suggestions:

```
error: unknown variable 'reuslt'
  --> math.alpha:3:12
   |
 3 |     return reuslt
   |            ^^^^^^ did you mean 'result'?
```

Multiple errors in a single pass. Catches: undefined variables/functions, wrong argument counts, immutable assignment, break outside loops, unused variables.

### Smart assert

One function that handles everything:

```alpha
test "math" {
    assert(fibonacci(10) == 55)
}
```

On failure, automatically decomposes the expression and shows both values:

```
  FAIL  tests.alpha:15: assert(fibonacci(10) == 55)
         left:  54
         right: 55
```

JSON output for programmatic consumption:

```bash
alphac test file.alpha --json
```

```json
{"tests": [{"name": "math", "status": "fail", "failures": [
  {"line": 15, "expr": "fibonacci(10) == 55", "left": 54, "right": 55}
]}]}
```

### Automatic memory management

Arrays and maps are freed when they go out of scope. No manual memory management, no garbage collector pauses.

```alpha
fn process() -> [i64] {
    let mut result: [i64] = []
    let mut temp: [i64] = []    // auto-freed when function returns
    for i in 0..10 {
        temp.push(i * i)
        result.push(i)
    }
    return result               // moved to caller, not freed
}
```

### Recover from crashes

Functions can catch panics and convert them to return values:

```alpha
fn safe_parse(input: str) -> Option<i64>
    recover { return none }
{
    return some(str_to_i64(input))
}
```

## Language Features

| Category | Features |
|----------|----------|
| **Types** | `i64` `f64` `bool` `str` `[T]` `Map<T>` `Option<T>` structs enums |
| **Control** | if/else, while, for-in (arrays + ranges), match, break/continue |
| **Functions** | lambdas, contracts, methods via impl blocks |
| **Arrays** | push, pop, map, filter, reduce, any, all, count, join, clone |
| **Strings** | split, join, trim, replace, contains, starts_with, substr, format |
| **Maps** | set, get, has, delete, keys, iteration |
| **Errors** | `Option<T>` with some/none/unwrap/unwrap_or, panic, recover |
| **Memory** | Automatic scope-based cleanup, defer for non-memory resources |
| **Testing** | Inline tests, smart assert, contracts, JSON output |
| **Tooling** | Watch mode, colored errors, typo suggestions, imports |

## Examples

### Hello World

```alpha
fn main() {
    println("Hello, Alpha!")
}
```

### Functional Array Operations

```alpha
fn main() {
    let nums = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]

    let evens = nums.filter(fn(x: i64) -> bool { return x % 2 == 0 })
    let squared = evens.map(fn(x: i64) -> i64 { return x * x })
    let sum = squared.reduce(0, fn(a: i64, b: i64) -> i64 { return a + b })

    println(format("Sum of squares of evens: {}", sum))
}
```

### Structs and Methods

```alpha
struct Vec2 {
    x: f64
    y: f64
}

impl Vec2 {
    fn length(&self) -> f64 {
        return sqrt(self.x * self.x + self.y * self.y)
    }

    fn add(&self, other: Vec2) -> Vec2 {
        return Vec2 { x: self.x + other.x, y: self.y + other.y }
    }
}

fn main() {
    let a = Vec2 { x: 3.0, y: 4.0 }
    println(format("length: {}", a.length()))
}
```

### Error Handling

```alpha
fn find(names: [str], target: str) -> Option<str> {
    for name in names {
        if name == target { return some(name) }
    }
    return none
}

fn main() {
    let names = ["Alice", "Bob", "Charlie"]
    let result = find(names, "Bob")
    println(format("Found: {}", result.unwrap_or("nobody")))
}
```

### JSON Parser

Alpha includes a full JSON parser in `examples/json.alpha` — parses, accesses nested values, and pretty-prints. ~300 lines.

```alpha
let root = json_parse("{\"name\": \"Alpha\", \"version\": 1.0}")
println(json_str(json_get(root, "name")))
```

## CLI

```bash
alphac file.alpha -o output     # compile to binary
alphac test file.alpha          # run tests
alphac test file.alpha --json   # structured test output
alphac watch file.alpha         # recompile on save
alphac --emit-c file.alpha      # show generated C
```

## Building from Source

Requires a C compiler (cc/gcc/clang).

```bash
git clone https://github.com/axelkrantz/Alpha.git
cd Alpha/bootstrap
make
make test
```

## Self-Hosting

Alpha is self-hosting — the compiler can compile itself:

```bash
./alphac ../alpha/alpha.alpha -o alphac-self    # compile the compiler with itself
./alphac-self examples/hello.alpha -o hello     # use the self-compiled compiler
```

## Project Structure

```
bootstrap/     Stage 0 compiler (C) — lexer, parser, type checker, codegen
alpha/         Stage 1 compiler (Alpha) — self-hosting single-pass transpiler
alpha/std.alpha Standard library
examples/      Example programs and tests
CLAUDE.md      Language reference for AI assistants
```

## License

MIT
