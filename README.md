# Alpha

A compiled, statically-typed programming language designed to be the easiest language for AI to write in.

Alpha compiles to C, then to native binaries. It's fast, safe, and self-hosting.

## Install

**One-liner:**
```bash
curl -fsSL https://raw.githubusercontent.com/AxelKrantz/Alpha/main/install.sh | sh
```

**Or from source:**
```bash
git clone https://github.com/AxelKrantz/Alpha.git
cd Alpha/bootstrap
make && sudo make install
```

**Or download a binary** from [GitHub Releases](https://github.com/AxelKrantz/Alpha/releases).

## Quick Start

```bash
alphac examples/hello.alpha -o hello && ./hello
```

## What Makes Alpha Different

**Built for AI agents.** Every design decision optimizes for the AI coding workflow: write code, get structured feedback, fix, repeat.

### The spec is the function

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

Contracts live on the function — no separate test files. `alphac test` verifies examples, `panics` anti-examples prove bad inputs are rejected.

### Errors that help you fix things

```
error: unknown variable 'reuslt'
  --> math.alpha:3:12
   |
 3 |     return reuslt
   |            ^^^^^^ did you mean 'result'?
```

Multiple errors per pass. Shows source lines, carets, suggestions.

### Error propagation with `?`

```alpha
fn load_config(path: str) -> Result<str> {
    if path.len == 0 { return err("empty path") }
    return ok(file_read(path))
}

fn process() -> Result<i64> {
    let config = load_config("app.conf")?  // propagates error automatically
    return ok(config.len)
}
```

### Automatic memory management

Arrays and maps are freed when they leave scope. No manual free, no garbage collector.

```alpha
fn compute() -> [i64] {
    let mut temp: [i64] = []   // auto-freed on return
    let mut result: [i64] = []
    for i in 0..100 { result.push(i * i) }
    return result              // moved to caller, not freed
}
```

### Recover from crashes

```alpha
fn safe_parse(s: str) -> Option<i64>
    recover { return none }
{
    return some(str_to_i64(s))  // if this panics, recover catches it
}
```

## Language Features

| Category | Features |
|----------|----------|
| **Types** | `i64` `f64` `bool` `str` `[T]` `Map<T>` `Option<T>` `Result<T>` structs enums |
| **Generics** | `fn first<T>(arr: [T]) -> T` with monomorphization |
| **Traits** | `trait Display { fn to_string(&self) -> str }` |
| **Enums** | Data variants: `enum Shape { Circle(f64), Rectangle(f64, f64) }` |
| **Pattern matching** | Destructuring: `Shape::Circle(r) => { use r }` |
| **Closures** | Variable capture: `nums.filter(fn(x: i64) -> bool { return x > threshold })` |
| **Arrays** | push, pop, map, filter, reduce, any, all, count, join, clone |
| **Strings** | split, join, trim, replace, contains, starts_with, multi-line `"""..."""` |
| **Maps** | `Map<T>` with set, get, has, delete, keys |
| **Errors** | `Option<T>`, `Result<T>` with `?` operator, panic, recover |
| **Contracts** | example, panics, requires, ensures on functions |
| **Memory** | Automatic scope-based cleanup for arrays and maps |
| **Testing** | Smart `assert()` with expression decomposition, JSON output |
| **Tooling** | Watch mode, colored diagnostics, typo suggestions, imports |
| **Pipe** | `x \|> f \|> g` chains function calls left-to-right |
| **Format** | `format("hello {}, age {}", name, age)` with type auto-detection |

## Examples

### Functional Programming

```alpha
fn main() {
    let nums = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]

    let threshold = 5
    let result = nums
        .filter(fn(x: i64) -> bool { return x > threshold })
        .map(fn(x: i64) -> i64 { return x * x })
        .reduce(0, fn(a: i64, b: i64) -> i64 { return a + b })

    println(format("Sum of squares above {}: {}", threshold, result))
}
```

### Structs, Traits, Generics

```alpha
trait Display {
    fn to_string(&self) -> str
}

struct Point { x: f64, y: f64 }

impl Display for Point {
    fn to_string(&self) -> str {
        return format("({}, {})", self.x, self.y)
    }
}

fn print_all<T>(items: [T]) {
    for item in items {
        println(item.to_string())
    }
}

fn main() {
    print_all([Point { x: 1.0, y: 2.0 }, Point { x: 3.0, y: 4.0 }])
}
```

### Enums with Pattern Matching

```alpha
enum Shape {
    Circle(f64)
    Rectangle(f64, f64)
    Point
}

fn area(s: Shape) -> f64 {
    match s {
        Shape::Circle(r) => { return 3.14159 * r * r }
        Shape::Rectangle(w, h) => { return w * h }
        Shape::Point => { return 0.0 }
    }
    return 0.0
}

fn main() {
    let shapes = [Shape::Circle(5.0), Shape::Rectangle(3.0, 4.0)]
    for s in shapes {
        println(format("area: {}", area(s)))
    }
}
```

### Error Handling

```alpha
fn parse_number(s: str) -> Result<i64> {
    for i in 0..s.len {
        let c = s.char_at(i)
        if c < 48 or c > 57 { return err("not a number: " + s) }
    }
    return ok(str_to_i64(s))
}

fn sum_all(inputs: [str]) -> Result<i64> {
    let mut total: i64 = 0
    for input in inputs {
        let n = parse_number(input)?   // ? propagates errors
        total += n
    }
    return ok(total)
}

fn main() {
    let r = sum_all(["10", "20", "30"])
    println(format("sum: {}", r.unwrap()))

    let bad = sum_all(["10", "abc", "30"])
    println(format("error: {}", bad.error()))
}
```

### JSON Parser

Alpha includes a full JSON parser in `examples/json.alpha` — parses nested objects/arrays, pretty-prints, with inline tests.

## CLI

```bash
alphac file.alpha -o output     # compile to binary
alphac test file.alpha          # run tests
alphac test file.alpha --json   # structured test output
alphac watch file.alpha         # recompile on save
alphac --emit-c file.alpha      # show generated C
```

## Self-Hosting

Alpha is self-hosting — the compiler can compile itself:

```bash
./alphac ../alpha/alpha.alpha -o alphac-self
./alphac-self ../examples/hello.alpha -o hello && ./hello
```

## Project Structure

```
bootstrap/      Stage 0 compiler (C) — lexer, parser, type checker, codegen
alpha/          Stage 1 compiler (Alpha) + standard library
examples/       Example programs, tests, and JSON parser
CLAUDE.md       Complete language reference for AI assistants
```

## License

MIT — if you make a boatload of money off this, cut me in would ya?
