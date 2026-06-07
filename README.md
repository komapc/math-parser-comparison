<div align="center">

# 🧮 Math-Expression Parser & Evaluator — A Comparison

**Nine ways to turn `"-2 ^ 2 + 3 * (4 - 1)"` into a number — benchmarked head-to-head**
**(plus a five-way compile-once / evaluate-many shoot-out).**

![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-3.20%2B-064F8C?logo=cmake&logoColor=white)
![tests](https://img.shields.io/badge/tests-228%20passing-brightgreen)
![warnings](https://img.shields.io/badge/-Wall%20-Wextra%20-Wpedantic-clean-brightgreen)
![deps](https://img.shields.io/badge/dependencies-none-blue)

</div>

---

A small, dependency-free C++20 project that implements the classic (and not-so-classic)
algorithms for parsing and evaluating arithmetic expressions. **Every strategy shares
one tokenizer and one grammar**, so the benchmarks measure the *algorithm*, not
incidental differences. Two questions drive it:

1. **One-shot** — how fast can you turn a string into a value, *once*?
2. **Re-eval** — if you'll evaluate the same expression *many* times (with changing
   variables), what should you compile it to?

The recurring punchline: **performance tracks memory allocation, not algorithmic
cleverness.**

## ⚡ Results at a glance

> One-shot, ns per leaf at 10 000-leaf expressions — **shorter is faster**.
> (Throttling laptop part; absolute ns swing run-to-run, so trust the **ratios**.)

```
direct-recursive-descent  ██                                 134 ns   ×1.0   ← fastest
direct-shunting-yard      ███                                167 ns   ×1.2
bytecode-vm               ███                                172 ns   ×1.3
rpn-stack                 ███                                187 ns   ×1.4
ast-arena                 ████                               229 ns   ×1.7
ast-recursive-descent     ████████                           502 ns   ×3.7
ast-shunting-yard         █████████                          528 ns   ×3.9
ast-pratt                 █████████                          538 ns   ×4.0
multipass                 ████████████████████████████████  1932 ns   ×14    ← super-linear
```

Three things fall right out:

- **The AST allocation tax is real.** The three classic pointer-AST parsers all land
  ~3.7–4.0× behind, *regardless of algorithm* — the cost is one `make_unique` per
  node, not the parsing logic.
- **Swap pointers for an arena and that tax mostly vanishes** (`ast-arena`, ×1.7) —
  same tree, same walk, one allocation instead of N.
- **`multipass` is the only non-linear strategy** and pays for it (×14, and growing).

## 📐 The grammar

Numbers (decimals + exponents), single-letter variables `a`–`z`, `+ - * / ^`,
unary `+/-`, and parentheses. Precedence (loosest → tightest):
`+ -` < `* /` < unary `+ -` < `^` (right-associative).

```
expr    := term (('+' | '-') term)*
term    := unary (('*' | '/') unary)*
unary   := ('+' | '-') unary | power
power   := primary ('^' unary)?              // right-associative; 2^-3 works
primary := NUMBER | IDENT | '(' expr ')'
```

So `-2^2 == -4`, `2^3^2 == 512`, `2^-3 == 0.125`.

## 🧩 The strategies

All one-shot strategies implement [`mp::IEvaluator`](include/parser/evaluator.hpp)
(`eval(src) -> double`). "Code" = non-blank, non-comment lines.

### Build an AST, then walk it

| Strategy | Source | Code | In one sentence |
|---|---|--:|---|
| `ast-recursive-descent` | [`recursive_descent.cpp`](src/recursive_descent.cpp) | 83 | A function per grammar rule calls down the precedence ladder, building an AST as the recursion returns. |
| `ast-shunting-yard` | [`shunting_yard.cpp`](src/shunting_yard.cpp) | 116 | Scan left-to-right, popping higher-precedence operators off a stack to fold operands into AST nodes. |
| `ast-pratt` | [`pratt.cpp`](src/pratt.cpp) | 81 | Parsing driven by each operator's binding power, looping while the next operator binds tighter than the caller's minimum. |
| `ast-arena` | [`arena_ast.cpp`](src/arena_ast.cpp) | 114 | Recursive descent, but every node is appended to one contiguous vector and children are referenced by index, not pointer. |
| `multipass` | [`multipass.cpp`](src/multipass.cpp) | 99 | Recursively find the lowest-precedence operator at depth 0, make it the root, and recurse on the two halves. |

### Evaluate inline while parsing (no intermediate form)

| Strategy | Source | Code | In one sentence |
|---|---|--:|---|
| `direct-recursive-descent` | [`direct_recursive_descent.cpp`](src/direct_recursive_descent.cpp) | 81 | Recursive descent whose rules return the computed number directly, so no AST is ever built. |
| `direct-shunting-yard` | [`direct_shunting_yard.cpp`](src/direct_shunting_yard.cpp) | 109 | Dijkstra's original — the operand stack holds running values, computing the result during the single scan. |

### Compile to a flat form, then run it

| Strategy | Source | Code | In one sentence |
|---|---|--:|---|
| `rpn-stack` | [`rpn.cpp`](src/rpn.cpp) | 134 | Shunting-yard emits a flat postfix (RPN) token sequence, then a value stack walks it. |
| `bytecode-vm` | [`bytecode.cpp`](src/bytecode.cpp) | 138 | Shunting-yard compiles to a compact `uint8_t` opcode stream + constant pool, then a switch-dispatch stack VM executes it. |

### Compile once, evaluate many (the [re-eval](src/reeval.cpp) module)

[`reeval.cpp`](src/reeval.cpp) (329 lines, all five forms) implements
[`mp::ICompiler`](include/parser/reeval.hpp) → `ICompiledExpr::eval(vars)`:
`ast-ptr`, `ast-arena`, `rpn`, `bytecode`, and `reparse-rd` (the baseline — re-lex +
re-parse + walk a fresh AST *every call*). The compiled forms' `eval()` is
**allocation-free**: value stacks are reserved at compile time and reused.

Shared infrastructure: [`lexer.cpp`](src/lexer.cpp) (69) and [`ast.cpp`](src/ast.cpp) (33).

## 🌳 Do they all build the same AST? No — three categories

Take `2 + 3 * 4`:

| Group | Representation | `2 + 3 * 4` becomes |
|---|---|---|
| `ast-recursive-descent`, `ast-shunting-yard`, `ast-pratt`, `multipass` | **Pointer AST** — tree of heap nodes (`unique_ptr<Expr>`) | `Binary(+, Num 2, Binary(*, Num 3, Num 4))` |
| `ast-arena` | **Arena AST** — same tree, one flat vector, children by index | `[Num2, Num3, Num4, Mul→(1,2), Add→(0,3)]`, root = 4 |
| `direct-recursive-descent`, `direct-shunting-yard` | **None** — value computed on the fly | *(nothing; yields `14`)* |
| `rpn-stack` | **Flat postfix** | `2 3 4 * +` |
| `bytecode-vm` | **Flat opcodes + const pool** | `PUSH PUSH PUSH MUL ADD` |

Key points:

- The four **pointer-AST** strategies produce a **bit-identical** tree for a given
  input — that's *why* their speeds are nearly equal: they differ only in *how* they
  find the tree, not what they build.
- The **arena AST** is the *same logical tree, different physical layout* — and that
  layout change alone is the ~2× speedup.
- **RPN / bytecode aren't trees** — they're the AST *flattened to post-order*. No
  pointers, no recursion to evaluate.
- The **direct** evaluators build *no* representation at all.

## 📊 Benchmarks

```sh
./build/bench     # one-shot: string -> value
./build/reeval    # compile once, evaluate many
```

### One-shot — string → value, once

Full table, ns/leaf (10 000-leaf expressions; `×` = relative to the fastest):

| Strategy | ns/leaf | ×fastest | allocations / expr |
|---|--:|--:|---|
| `direct-recursive-descent` | 134 | **1.0** | ~0 (call stack) |
| `direct-shunting-yard` | 167 | 1.2 | 2 stacks |
| `bytecode-vm` | 172 | 1.3 | a few buffers |
| `rpn-stack` | 187 | 1.4 | a few buffers |
| `ast-arena` | 229 | 1.7 | **one** (node vector) |
| `ast-recursive-descent` | 502 | 3.7 | **one per node** |
| `ast-shunting-yard` | 528 | 3.9 | **one per node** |
| `ast-pratt` | 538 | 4.0 | **one per node** |
| `multipass` | 1932 | 14.4 | one per node + re-scans |

For a *single* evaluation, inline `direct-recursive-descent` wins — there's nothing to
allocate or build. RPN/bytecode pay a compile cost they can't amortize here; their
moment comes under re-evaluation.

#### ⚠️ `multipass` is the odd one out — it isn't linear

Every other strategy has ~constant ns/leaf. `multipass` re-scans each sub-range to
locate its split operator, so its **per-leaf cost grows with size**:

```
leaves       10    100   1000  10000
ns/leaf     531    856   1087   1932     ← rising = super-linear
```

Average ≈ **O(n log n)** on random inputs; **worst case O(n²)** on skewed chains like
`1-2-3-…-n`. It's the naive, re-scanning sibling of recursive descent — but its
independent sub-ranges make it the most **parallel-friendly** strategy (see below).

### Re-eval — compile once, evaluate many

Variables `a`–`d`, values in `[0.5, 2.0]`, 1000-leaf expressions:

| Strategy | compile ns/expr | per-eval ns/expr | per-eval ×fastest |
|---|--:|--:|--:|
| `bytecode` | ~136k | **33k** | 1.0 |
| `rpn` | ~149k | 36k | 1.1 |
| `ast-arena` | **~134k** | 56k | 1.7 |
| `ast-ptr` | ~372k | 54k | 1.6 |
| `reparse-rd` | ~0 *(stores source)* | **372k** | 11.2 |

The ranking **inverts** vs. one-shot:

- **Flat forms (bytecode/rpn) now win per eval** — a cache-friendly linear walk over
  contiguous memory, with zero per-call allocation.
- **`ast-arena` is ~2.8× cheaper to *compile* than `ast-ptr`** (one allocation vs N);
  per-eval the two are within noise.
- **`reparse-rd` is ~11× slower per eval**, so compiling beats re-parsing after barely
  one evaluation.

### 🏁 The verdict

> **There is no universal winner — the best strategy is a function of how many times
> you evaluate.**
>
> - **Once?** Inline `direct-recursive-descent`.
> - **Many times?** Compile to `bytecode`/`rpn`.
> - **Want a tree, but fast?** Use an **arena**, never per-node `unique_ptr`.

## 🧵 Multithreading ideas

Single-expression parsing is mostly *sequential* (each token's meaning depends on what
came before), so threads help in two specific shapes:

1. **Batch parallelism** *(easy, scales well)* — many independent expressions, one
   parser per thread. Measured ~3.5× on a 4-core/8-thread laptop. Embarrassingly
   parallel; the only limit is the allocator.
2. **Single-expression split** *(`src/parallel.cpp`, currently disabled)* — split one
   big expression at top-level `+`/`-`, parse the terms across threads, fold the
   results. Correct, but reached only ~1.7× on 4 threads because AST construction is
   **allocation-bound** — the real ceiling everywhere in this project.
3. **`multipass` fork-join** *(the best fit, not yet built)* — every split yields two
   genuinely independent sub-ranges, so it maps directly onto fork-join parallelism
   (parse left/right concurrently down to a size cutoff). Finer-grained than (2)'s
   single top-level split.

The thread that ties it together: more cores don't help much until you fix allocation.
A **per-thread arena/bump allocator** is the lever that would unlock real parallel
scaling for any of these — the same insight the arena AST demonstrates single-threaded.

## 🛠️ Build & run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

ctest --test-dir build --output-on-failure   # correctness: all strategies, incl. variables
./build/bench                                 # one-shot comparison
./build/reeval                                # compile-once / eval-many comparison
```

Requires a C++20 compiler and CMake ≥ 3.20. No external dependencies.

## 🗂️ Project layout

```
include/parser/   token, lexer, ast, parser, evaluator, arena_ast, reeval  (interfaces)
src/              lexer + ast (shared) and one file per strategy
bench/            benchmark.cpp (one-shot), reeval.cpp (compile-once/eval-many)
tests/            test_parsers.cpp — dependency-free, run via CTest
```

## 🔭 Extending to functions

Variables already exist. Adding `sin(x)`, `max(a, b)`, etc. is localized: the lexer
already emits `Ident` tokens (currently single letters); add a `Call` AST node / opcode
and extend each strategy's `primary`/`nud` entry point (an identifier followed by `(`
is a call). Operator precedence and the harnesses are unaffected.

## 📎 Notes

- Benchmark numbers come from one clean run on a throttling i7-10610U; **absolute ns
  vary ±40% run-to-run — the ratios are the result.** Run them yourself.
- `src/parallel.cpp` is kept for reference but **excluded from the build** (see
  `CMakeLists.txt`).
