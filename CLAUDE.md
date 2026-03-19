# Alpha Language

Alpha is a compiled, statically-typed language that transpiles to C. Designed for AI.

## Build & Run

```bash
cd bootstrap && make            # build the compiler
./alphac file.alpha -o output   # compile to binary
./alphac test file.alpha        # run tests
./alphac test file.alpha --json # structured test output
./alphac watch file.alpha       # recompile on file change
./alphac --emit-c file.alpha    # show generated C
```

## Types

`i64` `f64` `bool` `str` `[T]` `Map<T>` `Option<T>` `Result<T>` structs enums `&T` `&mut T`

## Variables

```alpha
let x = 42                    # immutable, type inferred
let name: str = "Alpha"       # explicit type
let mut counter: i64 = 0      # mutable
counter += 1
```

## Functions

```alpha
fn add(a: i64, b: i64) -> i64 {
    return a + b
}
```

## Control Flow

```alpha
if x > 0 { } else if x == 0 { } else { }

while condition { }

for i in 0..10 { }            # range (0 to 9)
for item in array { }          # iterate elements

match n {
    0 => { println("zero") }
    1 => { println("one") }
    _ => { println("other") }
}
```

## Structs & Methods

```alpha
struct Point { x: f64, y: f64 }

impl Point {
    fn length(&self) -> f64 {
        return sqrt(self.x * self.x + self.y * self.y)
    }
}

let p = Point { x: 3.0, y: 4.0 }
println(p.length())
```

## Traits

```alpha
trait Display {
    fn to_string(&self) -> str
}

impl Display for Point {
    fn to_string(&self) -> str {
        return format("({}, {})", self.x, self.y)
    }
}

println(p.to_string())
```

## Enums with Data

```alpha
enum Shape {
    Circle(f64)
    Rectangle(f64, f64)
    Point
}

let s = Shape::Circle(5.0)
match s {
    Shape::Circle(r) => { println(format("radius: {}", r)) }
    Shape::Rectangle(w, h) => { println(format("{}x{}", w, h)) }
    Shape::Point => { println("point") }
}
```

## Generics

```alpha
fn first<T>(arr: [T]) -> T {
    return arr[0]
}

fn contains<T>(arr: [T], target: T) -> bool {
    for item in arr {
        if item == target { return true }
    }
    return false
}

let x = first([1, 2, 3])      # T inferred as i64
let s = first(["a", "b"])     # T inferred as str
```

Monomorphization — generates specialized C functions at compile time. Zero runtime overhead.

## Dynamic Arrays

```alpha
let mut nums: [i64] = []
nums.push(10)
nums[0]                        # index
nums.len                       # length
nums.pop()                     # remove last
nums.clear()                   # empty
nums.clone()                   # copy

let names = ["Alice", "Bob"]   # array literal

# Functional methods
nums.map(fn(x: i64) -> i64 { return x * 2 })
nums.filter(fn(x: i64) -> bool { return x > 5 })
nums.reduce(0, fn(a: i64, b: i64) -> i64 { return a + b })
nums.any(fn(x: i64) -> bool { return x > 100 })
nums.all(fn(x: i64) -> bool { return x > 0 })
nums.count(fn(x: i64) -> bool { return x > 5 })
```

## Closures (Lambdas with Captures)

```alpha
let threshold = 5
let big = nums.filter(fn(x: i64) -> bool { return x > threshold })
```

Lambdas capture variables from the enclosing scope.

## Strings

```alpha
s.len                          # length
s.contains("x")               # bool
s.starts_with("x")            # bool
s.substr(0, 5)                 # substring
s.char_at(0)                   # ASCII code (i64)
s.split(",")                   # [str]
s.trim()                       # strip whitespace
s.replace("old", "new")       # replace all
"a" + "b"                      # concatenation
s == "hello"                   # comparison
["a", "b"].join(", ")         # "a, b"

# Multi-line strings
let html = """<html>
  <body>Hello</body>
</html>"""

# String escapes: \n \t \r \\ \" \0
```

## Hash Maps

```alpha
let mut m: Map<str> = map_new()
m.set("key", "value")
m.get("key", "default")       # value or default
m.has("key")                   # bool
m.delete("key")
m.len                          # entry count
m.keys()                       # [str]

for key in m.keys() {
    println(format("{} = {}", key, m.get(key, "")))
}
```

## Format Strings

```alpha
format("hello {}, age {}", name, age)
```

`{}` auto-detects type (i64, f64, str, bool).

## Pipe Operator

```alpha
let result = 5 |> double |> add_one |> to_string
```

`x |> f` becomes `f(x)`. Chains left-to-right.

## Option Type

```alpha
fn find(arr: [str], target: str) -> Option<str> {
    for item in arr {
        if item == target { return some(item) }
    }
    return none
}

result.is_some()               # bool
result.is_none()               # bool
result.unwrap()                # value or crash
result.unwrap_or("default")   # value or default
```

## Result Type & ? Operator

```alpha
fn parse(s: str) -> Result<i64> {
    if s.len == 0 { return err("empty") }
    return ok(str_to_i64(s))
}

fn process(s: str) -> Result<i64> {
    let n = parse(s)?          # unwraps Ok or early-returns Err
    return ok(n * 2)
}

result.is_ok()                 # bool
result.is_err()                # bool
result.unwrap()                # value or crash
result.unwrap_or(0)            # value or default
result.error()                 # error message
```

## Contracts

```alpha
fn clamp(val: i64, lo: i64, hi: i64) -> i64
    example clamp(5, 0, 10) == 5
    example clamp(-1, 0, 10) == 0
    panics clamp(5, 10, 0)
    requires lo <= hi
    ensures result >= lo
    ensures result <= hi
{
    if val < lo { return lo }
    if val > hi { return hi }
    return val
}
```

- `example` — verified by `alphac test`, stripped in production
- `panics` — anti-example, verifies bad inputs crash
- `requires` — precondition, prints args on violation
- `ensures` — postcondition, `result` = return value

## Recover (Crash Handler)

```alpha
fn safe_parse(s: str) -> Option<i64>
    recover { return none }
{
    return some(str_to_i64(s))
}
```

Catches `requires` violations, `panic()`, and `unwrap()` on none/err.

## Testing

```alpha
test "math" {
    assert(2 + 2 == 4)
    assert(fibonacci(10) == 55)
}
```

Smart `assert()` — decomposes comparisons, shows both values on failure:
```
FAIL  test.alpha:5: assert(fibonacci(10) == 55)
       left:  54
       right: 55
```

## Automatic Memory Management

Arrays and maps freed automatically when they leave scope. Return values are moved, not freed.

```alpha
fn process() -> [i64] {
    let mut temp: [i64] = []   # auto-freed on return
    let mut result: [i64] = []
    // ...
    return result              # moved to caller
}
```

## Imports

```alpha
import "math_lib.alpha"        # relative path
import "/absolute/path.alpha"  # absolute path
```

Excludes `main()` and `test` blocks from imported files.

## Type Casting

```alpha
as_f64(42)       # i64 -> f64
as_i64(3.14)     # f64 -> i64 (truncates)
as_u8(256)       # wraps to 0
as_bool(1)       # -> true
```

## Built-in Functions

```alpha
# I/O
print(x)   println(x)   eprintln(x)
file_read(path)   file_write(path, data)

# Strings
len(s)   format("hi {}", x)   char_to_str(65)
i64_to_str(42)   str_to_i64("42")

# Math
sqrt(x)

# System
args_count()   args_get(i)   run_command(cmd)
exit(code)   panic(msg)   env_get(name)

# Type casting
as_i64(x)   as_f64(x)   as_u8(x)   as_bool(x)
```

## Operators

```
+  -  *  /  %         # arithmetic (+ concatenates strings)
== != < > <= >=       # comparison (== on strings uses strcmp)
and  or  not          # logical
= += -= *= /=        # assignment
..                    # range (0..10)
|>                    # pipe (x |> f becomes f(x))
?                     # try (unwrap Result/Option or early-return)
```

## Compiler Diagnostics

Colored errors with source context, typo suggestions, multiple errors per pass:

```
error: unknown variable 'reuslt'
  --> math.alpha:3:12
   |
 3 |     return reuslt
   |            ^^^^^^ did you mean 'result'?
```

Catches: unknown variables/functions, wrong argument counts, immutable assignment, break outside loops. Warns: unused variables, missing returns.

## Project Structure

```
bootstrap/src/    # C compiler (lexer, parser, checker, codegen, types, error)
alpha/            # Self-hosted compiler + standard library
examples/         # Example programs and tests
```
