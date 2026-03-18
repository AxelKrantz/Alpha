# Alpha Language

Alpha is a compiled, statically-typed language that transpiles to C. It is designed to be easy for AI to write in.

## Build & Run

```bash
cd bootstrap && make            # build the compiler
./alphac file.alpha -o output   # compile
./alphac test file.alpha        # run tests
./alphac test file.alpha --json # test with structured output
./alphac watch file.alpha       # recompile on file change
./alphac --emit-c file.alpha    # show generated C
```

## Syntax Reference

### Variables
```alpha
let x = 42                    # immutable, type inferred
let name: str = "Alpha"       # explicit type
let mut counter: i64 = 0      # mutable
counter += 1
```

### Types
`i64` `f64` `bool` `str` `[T]` (dynamic array) `Map<T>` (string-keyed hash map) `&T` `&mut T`

### Functions
```alpha
fn add(a: i64, b: i64) -> i64 {
    return a + b
}

# With contracts
fn divide(a: i64, b: i64) -> i64
    example divide(10, 2) == 5
    requires b != 0
    ensures result >= 0
{
    return a / b
}
```

### Control Flow
```alpha
if x > 0 {
    println("positive")
} else if x == 0 {
    println("zero")
} else {
    println("negative")
}

while i < 10 {
    i += 1
}

for i in 0..10 {       # range loop (0 to 9)
    println(i)
}

for item in my_array { # iterate array elements
    println(item)
}

match n {
    0 => { println("zero") }
    1 => { println("one") }
    _ => { println("other") }
}

break    # inside loops
continue # inside loops
```

### Structs & Methods
```alpha
struct Point {
    x: f64
    y: f64
}

impl Point {
    fn length(&self) -> f64 {
        return sqrt(self.x * self.x + self.y * self.y)
    }
}

let p = Point { x: 3.0, y: 4.0 }
println(p.length())
```

### Dynamic Arrays
```alpha
let mut nums: [i64] = []
nums.push(10)
nums.push(20)
nums[0]              # index access
nums.len             # length
nums.pop()           # remove last
nums.clear()         # empty
nums.clone()         # copy
nums.free()          # release memory

let names = ["Alice", "Bob", "Charlie"]  # array literal
```

### Strings
```alpha
let s = "hello world"
s.len                     # length
s.contains("world")       # bool
s.starts_with("hello")    # bool
s.substr(0, 5)            # "hello"
s.char_at(0)              # 104 (ASCII code)
s.split(" ")              # ["hello", "world"]
s.replace("world", "!")   # "hello !"
s.trim()                  # strip whitespace
"a" + "b"                 # concatenation
s == "hello"              # comparison (uses strcmp)

let parts = ["a", "b", "c"]
parts.join(", ")          # "a, b, c"
```

### Hash Maps
```alpha
let mut m: Map<str> = map_new()   # string keys, string values
let mut counts: Map<i64> = map_new() # string keys, i64 values

m.set("key", "value")
m.get("key", "default")  # returns value or default
m.has("key")              # bool
m.delete("key")
m.len                     # number of entries
m.keys()                  # returns [str]

# Iterate
for key in m.keys() {
    println(format("{} = {}", key, m.get(key, "")))
}
```

### Option Type (Error Handling)
```alpha
fn find(arr: [str], target: str) -> Option<str> {
    for item in arr {
        if item == target { return some(item) }
    }
    return none
}

let result = find(names, "Bob")
if result.is_some() {
    println(result.unwrap())       # crashes if none
}
let safe = result.unwrap_or("nobody") # returns default if none
println(result.is_none())            # check if empty
```
Supports `Option<i64>`, `Option<f64>`, `Option<str>`, `Option<bool>`.

### Lambdas & Functional Methods
```alpha
let doubled = nums.map(fn(x: i64) -> i64 { return x * 2 })
let evens = nums.filter(fn(x: i64) -> bool { return x % 2 == 0 })
let sum = nums.reduce(0, fn(a: i64, b: i64) -> i64 { return a + b })
let has_big = nums.any(fn(x: i64) -> bool { return x > 100 })
let all_pos = nums.all(fn(x: i64) -> bool { return x > 0 })
let n = nums.count(fn(x: i64) -> bool { return x > 5 })
```
Lambdas are `fn(params) -> ret { body }`. Works with `[i64]`, `[f64]`, `[str]`, `[bool]`.
Note: chaining across lines (`.filter().map()`) requires each call on the same line or intermediate variables.

### Format Strings
```alpha
let msg = format("hello {}, age {}", name, age)
println(format("{} items at {} each", count, price))
```
`{}` auto-detects type (i64, f64, str, bool).

### Imports
```alpha
import "math_lib.alpha"   # includes all fn/struct/enum from file
```
Imported file's `main()` and `test` blocks are excluded.

### Testing
```alpha
test "math works" {
    assert(2 + 2 == 4)
    assert(fibonacci(10) == 55)
    assert("hello".len == 5)
}
```
`assert()` decomposes comparisons automatically and shows both values on failure.

### Contracts
```alpha
fn clamp(val: i64, lo: i64, hi: i64) -> i64
    example clamp(5, 0, 10) == 5
    example clamp(-1, 0, 10) == 0
    requires lo <= hi
    ensures result >= lo
    ensures result <= hi
{
    if val < lo { return lo }
    if val > hi { return hi }
    return val
}
```
- `example` — verified by `alphac test`, stripped in normal builds
- `panics` — anti-example, verifies that bad inputs cause a crash
- `requires` — checked at function entry, prints args on violation
- `ensures` — checked at every return, `result` refers to return value
- `recover { return none }` — catch panics, convert to return value

### Recover (Crash Handler)
```alpha
fn safe_parse(input: str) -> Option<i64>
    recover { return none }        # catches any panic in the body
{
    // if anything panics here, recover block runs instead
    return some(str_to_i64(input))
}
```
Uses setjmp/longjmp. Catches `requires` violations, `panic()` calls, and `unwrap()` on none.

### Panics (Anti-examples)
```alpha
fn divide(a: i64, b: i64) -> i64
    requires b != 0
    example divide(10, 2) == 5     # must succeed
    panics divide(10, 0)           # must crash
{
    return a / b
}
```
`panics` clauses verify that bad inputs trigger contract violations. Tested by `alphac test`.

### Built-in Functions
```alpha
# I/O
print(x)              println(x)           eprintln(x)
file_read("path")     file_write("path", data)

# String ops
len(s)                str_concat(a, b)     str_substr(s, start, len)
str_char_at(s, i)     str_contains(s, p)   str_starts_with(s, p)
char_to_str(65)       i64_to_str(42)       str_to_i64("42")
format("hi {}", x)

# Math
sqrt(x)

# System
args_count()          args_get(i)          run_command("cmd")
exit(code)            assert(expr)          panic("message")
free(ptr)             env_get("NAME")

# Casting
as_i64(x)            as_f64(x)            as_u8(x)             as_bool(x)

# Memory (automatic — arrays/maps freed when out of scope)
panic("message")      # explicit crash, catchable by recover
```

### Type Casting
```alpha
let f = as_f64(42)        # i64 -> f64
let i = as_i64(3.14)      # f64 -> i64 (truncates)
let b = as_u8(256)         # wraps to 0
```

### Automatic Memory Management
Arrays and maps are automatically freed when they go out of scope. No manual free, no annotations needed.
```alpha
fn process() -> [i64] {
    let mut result: [i64] = []
    let mut temp: [i64] = []    # auto-freed when function returns
    for i in 0..10 {
        temp.push(i)
        result.push(i * i)
    }
    return result               # result is moved to caller, temp is freed
}
```

### Defer (Non-memory cleanup)
```alpha
fn read_data() {
    let f = file_open("data.txt")
    defer file_close(f)         # runs when function exits
    // ...
}
```

### Operators
```
+  -  *  /  %              # arithmetic (+ also concatenates strings)
== != < > <= >=            # comparison (== on strings uses strcmp)
and or not                 # logical
= += -= *= /=             # assignment
& &mut *                  # reference/dereference
..                         # range (0..10)
```

### Standard Library
```alpha
import "/path/to/alpha/std.alpha"

# Math: abs_val, min, max, clamp, min_f64, max_f64, abs_f64
# Strings: str_repeat, str_pad_left, str_pad_right, str_to_upper, str_to_lower
#           str_is_digit, str_is_alpha, str_is_space
# Arrays: range(start, end), sum(arr), sum_f64(arr)
# System: env_get(name), free(ptr)
```

### String Escapes
`\n` (newline), `\t` (tab), `\r` (carriage return), `\\` (backslash), `\"` (quote), `\0` (null)

### Key behaviors
- Newlines terminate statements (no semicolons)
- Lines ending with an operator continue to the next line
- Newlines suppressed inside `()` and `[]`
- `//` for comments
- Variables prefixed with `_` suppress unused warnings

## Project Structure
```
bootstrap/src/    # C compiler: lexer, parser, checker, codegen
alpha/            # Self-hosted compiler (alpha.alpha)
examples/         # Example programs and tests
```

## Compiler Diagnostics
The compiler catches and reports with source context:
- Unknown variables/functions (with "did you mean?" suggestions)
- Wrong argument counts
- Assignment to immutable variables
- break/continue outside loops
- Unused variables (warning)
- Missing returns (warning)
