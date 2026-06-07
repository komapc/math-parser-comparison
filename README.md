# Math-expression evaluator comparison

A small C++20 project that implements and benchmarks many approaches to
parsing/evaluating arithmetic expressions. All share one tokenizer
(`src/lexer.cpp`) and grammar, so the comparison measures the strategy rather
than incidental differences. There are two benchmarks:

- **`bench`** — *one-shot*: source text → value, in a single call (9 strategies).
- **`reeval`** — *compile once, evaluate many*: how the strategies behave under
  repeated evaluation with changing variable bindings (5 strategies).

## Grammar

Numbers (decimals + exponents), single-letter variables `a`–`z`, `+ - * / ^`,
unary `+/-`, and parentheses. Precedence, low → high:
`+ -` < `* /` < unary `+ -` < `^` (right-associative).

```
expr    := term (('+' | '-') term)*
term    := unary (('*' | '/') unary)*
unary   := ('+' | '-') unary | power
power   := primary ('^' unary)?              // right-associative; 2^-3 works
primary := NUMBER | IDENT | '(' expr ')'
```

So `-2^2 == -4`, `2^3^2 == 512`, `2^-3 == 0.125`. Variables resolve through an
environment indexed by letter (`a`→0 … `z`→25).

## Strategies

**One-shot** strategies implement `mp::IEvaluator` (`eval(src) -> double`); see
`include/parser/evaluator.hpp` / `all_evaluators()`.

Build an AST, then walk it:

| Strategy | File | Notes |
|---|---|---|
| `ast-recursive-descent` | `src/recursive_descent.cpp` | One function per grammar rule. |
| `ast-shunting-yard` | `src/shunting_yard.cpp` | Two-stack; folds operands into AST nodes. |
| `ast-pratt` | `src/pratt.cpp` | Binding powers + `nud`/infix. |
| `ast-arena` | `src/arena_ast.cpp` | **Arena AST**: all nodes in one contiguous vector, children by index — one allocation instead of N. |
| `multipass` | `src/multipass.cpp` | **Divide-and-conquer**: recursively split on the lowest-precedence operator (the naive, re-scanning sibling of recursive descent). Super-linear. |

Evaluate inline while parsing (no intermediate form):

| Strategy | File | Notes |
|---|---|---|
| `direct-recursive-descent` | `src/direct_recursive_descent.cpp` | Each rule returns a `double`; recursion on the call stack. |
| `direct-shunting-yard` | `src/direct_shunting_yard.cpp` | Dijkstra's original: the stack holds values. |

Compile to a flat form, then run it:

| Strategy | File | Notes |
|---|---|---|
| `rpn-stack` | `src/rpn.cpp` | Postfix (RPN) tagged tokens + value stack. |
| `bytecode-vm` | `src/bytecode.cpp` | Compact `uint8_t` opcodes + constant pool + stack VM. |

**Re-eval** strategies implement `mp::ICompiler` → `ICompiledExpr::eval(vars)`
(`include/parser/reeval.hpp` / `all_compilers()`): `ast-ptr` (pointer AST),
`ast-arena`, `rpn`, `bytecode`, and `reparse-rd` (the baseline: re-lex + re-parse
every call). The compiled forms' `eval()` is **allocation-free** — value stacks
are reserved at compile time and reused.

## Build & run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

ctest --test-dir build --output-on-failure   # correctness (all strategies, incl. variables)
./build/bench                                 # one-shot comparison
./build/reeval                                # compile-once / eval-many comparison
```

## Findings (4-core i7-10610U laptop, ~10000-leaf expressions)

The whole project's through-line: **performance tracks allocation, not algorithm
cleverness.** The numbers below are from one clean run; this is a throttling
laptop part, so **absolute ns swing ±40% run-to-run — the ratios are the
result, not the absolutes.** Run `./build/bench` and `./build/reeval` yourself.

### One-shot (string → value once)

ns/leaf at 10000 leaves (lower is better):

| Strategy | ~ns/leaf | allocations/expr |
|---|---|---|
| `direct-recursive-descent` | **135–185** | ~0 (call stack) |
| `direct-shunting-yard` / `rpn` / `bytecode` | 160–210 | a few buffers |
| `ast-arena` | 245 | **one** (the node vector) |
| `ast-*` (pointer) | 500–550 | **one per node** |
| `multipass` | **1950** (and rising) | one per node + re-scans |

- Avoiding per-node allocation is a ~2× win: `ast-arena` (~245) vs pointer ASTs
  (~520) — same tree walk, only the allocation strategy differs.
- For a single evaluation, inline `direct-recursive-descent` wins (nothing to
  allocate or build). RPN/bytecode pay compile cost they can't amortize here —
  their advantage shows up only under re-evaluation, below.
- **`multipass` is the only strategy that is *not* linear.** Its per-leaf cost
  *grows* with expression size (≈629 → 925 → 1148 → 1946 ns/leaf at
  10 / 100 / 1000 / 10000 leaves) because it re-scans every sub-range to find its
  split operator — average ≈ O(n log n) on random inputs, worst case O(n²) on
  skewed chains. Every other strategy has ~constant ns/leaf (linear). The upside:
  its independent sub-ranges make it the most naturally **fork-join parallel**
  strategy (see below), unlike the inherently sequential single-pass parsers.

### Re-eval (compile once, evaluate many)

The ranking inverts — compiled forms win, and the reparse baseline collapses
(numbers at 1000 leaves):

| Strategy | compile ns/expr | per-eval ns/expr |
|---|---|---|
| `bytecode` / `rpn` | ~169k | **41k–43k** |
| `ast-arena` | ~159k | ~61k |
| `ast-ptr` | ~431k | ~64k |
| `reparse-rd` | ~0 (stores source) | **~476k** |

- Flat forms (rpn/bytecode) evaluate fastest — cache-friendly linear walk.
- `ast-arena`'s clear win over `ast-ptr` is **compile** (~2.7×: one allocation
  vs N) and one-shot (~2×). Per eval it is only modestly faster (a few percent,
  noisy — cache locality, not a big margin).
- `reparse-rd` (which re-parses *to an AST* every call) is ~8–11× slower per
  eval, so compiling beats **re-parsing** after barely one evaluation. (Against
  the one-shot champion, inline `direct-recursive-descent`, break-even is still
  only ~1–2 evals.)

**No universal winner:** the right strategy is a function of how many times you
evaluate. One evaluation → inline direct. Many evaluations → compile to a flat
form (or at least an arena AST).

## Disabled: single-expression parallelism

`src/parallel.cpp` (`parse_parallel()`) splits one large expression at top-level
`+`/`-`, parses the terms across threads, and folds them. It works and is correct
but only reached ~1.7× on 4 threads because AST construction was allocation-bound
(the arena AST above is the better lever). It is **excluded from the build** (see
`CMakeLists.txt`); the file is kept for reference.

The `multipass` strategy would be the better parallelization target: every split
yields two independent sub-ranges, so it maps directly onto **fork-join**
parallelism (parse left/right concurrently down to a size cutoff) — finer-grained
than `parse_parallel`'s single top-level split. It would still be allocation-bound
(per-thread arena regions would be the fix), but it is the most parallel-friendly
of the strategies here.

## Extending to functions

Variables already exist. Adding `sin(x)`, `max(a,b)`, etc. is localized: the lexer
already emits `Ident` tokens (currently restricted to single letters); add a
`Call` AST node / opcode and extend each strategy's `primary`/`nud` entry point
(an identifier followed by `(` is a call). Operator precedence and the harnesses
are unaffected.
