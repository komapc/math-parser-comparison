<div align="center">

# 🧮 Math-Expression Parser & Evaluator — A Comparison

**Fourteen ways to turn `"-2 ^ 2 + 3 * (4 - 1)"` into a number — benchmarked head-to-head**
**(plus a five-way compile-once / evaluate-many shoot-out).**

![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-3.20%2B-064F8C?logo=cmake&logoColor=white)
![tests](https://img.shields.io/badge/tests-404%20passing-brightgreen)
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

> One-shot, ns per leaf — **shorter is faster**. Averaged over several runs;
> this is a throttling laptop part so **absolute ns drift ±40% — trust the ratios**.

```
direct-recursive-descent  ███                                163 ns   ×1.0   ← fastest
direct-shunting-yard      ████                               178 ns   ×1.1
bytecode-vm               ████                               193 ns   ×1.2
rpn-stack                 █████                              200 ns   ×1.2
ast-arena                 █████                              208 ns   ×1.3
──────────────────────────────── tier break: 1 allocation vs pre-scan ────
direct-mp                 █████                              265 ns   ×1.6   ← div-and-conquer, no AST
direct-mp-full            █████                              275 ns   ×1.7   ←   + operator-only filter
direct-mp-simd            █████                              285 ns   ×1.7   ←   + AVX2 SIMD scan
──────────────────────────────── tier break: pre-scan + AST build ─────────
multipass-bfs             ████████                           330 ns   ×2.0   ← div-and-conquer + sparse-table
multipass-arena           ████████                           343 ns   ×2.1   ← div-and-conquer + arena
──────────────────────────────── tier break: N allocations ────────────────
ast-pratt                 ██████████                         400 ns   ×2.5
ast-shunting-yard         ███████████                        434 ns   ×2.7
ast-recursive-descent     ████████████                       466 ns   ×2.9
──────────────────────────────── tier break: super-linear ─────────────────
multipass                 █████████████████████              864 ns   ×5.3   ← O(n log n)
```

Five tiers fall out cleanly:

- **Tier 1 (×1.0–1.3): zero or one allocation.** Direct evaluators build nothing;
  compiled flat forms and the arena AST allocate one contiguous buffer. All land within
  30% of each other — algorithm barely matters inside this tier.
- **Tier 1.5 (×1.6–1.7): divide-and-conquer, no AST.** `direct-mp` variants apply the
  same D&C split-find as `multipass-arena`, but the recursion returns `double` directly
  — no AST is built, no eval-walk pass. The O(n) pre-scan (prec array + paren matching)
  is the main cost. A new tier between tier 1 and tier 2.
- **Tier 2 (×2.0–2.1): divide-and-conquer + arena AST.** `multipass-arena` and
  `multipass-bfs` sit here after an extensive optimization series (see below). Both
  are faster than all pointer-AST parsers despite being O(n log n).
- **Tier 3 (×2.5–2.9): N allocations.** One `make_unique` per AST node. All three
  parser algorithms (Pratt, shunting-yard, recursive descent) land within 17% of each
  other — the *algorithm is irrelevant*, per-node allocation is the cost.
- **Tier 4 (×5.3): super-linear.** `multipass` (pointer-AST) is inherently O(n log n).
  Arena + optimizations moved its arena sibling to tier 2, but the pointer-AST base
  pays N allocations on top of the log factor.

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
| `multipass` | [`multipass.cpp`](src/multipass.cpp) | 179 | Recursively find the lowest-precedence operator at depth 0, make it the root, and recurse on the two halves (pointer AST). |
| `multipass-arena` | [`multipass_arena.cpp`](src/multipass_arena.cpp) | 223 | Same divide-and-conquer, but with arena AST + O(1) paren matching + iterator passing to eliminate redundant binary searches. |
| `multipass-bfs` | [`multipass_opt.cpp`](src/multipass_opt.cpp) | — | All the arena optimisations plus a sparse-table RMQ for O(1) split-finding and pre-indexed paren ranges for O(1) paren strips. Theoretical ceiling of the divide-and-conquer family with this grammar. |

### Evaluate inline while parsing (no intermediate form)

| Strategy | Source | Code | In one sentence |
|---|---|--:|---|
| `direct-recursive-descent` | [`direct_recursive_descent.cpp`](src/direct_recursive_descent.cpp) | 81 | Recursive descent whose rules return the computed number directly, so no AST is ever built. |
| `direct-shunting-yard` | [`direct_shunting_yard.cpp`](src/direct_shunting_yard.cpp) | 109 | Dijkstra's original — the operand stack holds running values, computing the result during the single scan. |
| `direct-mp` | [`multipass_lean.cpp`](src/multipass_lean.cpp) | — | D&C split-find + inline eval: one O(n) pre-scan builds the candidate lists, then the recursion finds the split and returns `double` directly — no AST built or walked. |
| `direct-mp-simd` | [`multipass_lean.cpp`](src/multipass_lean.cpp) | — | Same, but an AVX2 `_mm256_min_epi8` pass finds the minimum precedence in 32-byte chunks, replacing the linear RTL scan. |
| `direct-mp-full` | [`multipass_lean.cpp`](src/multipass_lean.cpp) | — | SIMD split-finding plus an operator-only `buildAll` that skips `Number`/`Ident` tokens, reducing candidate-array size at the cost of a more complex token loop. |

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

## 🌳 Do they all build the same AST? No — four categories

Take `2 + 3 * 4`:

| Group | Representation | `2 + 3 * 4` becomes |
|---|---|---|
| `ast-rd`, `ast-sy`, `ast-pratt`, `multipass` | **Pointer AST** — heap nodes (`unique_ptr<Expr>`) | `Binary(+, Num 2, Binary(*, Num 3, Num 4))` |
| `ast-arena`, `multipass-arena` | **Arena AST** — one flat vector, index children | `[Num2, Num3, Num4, Mul→(1,2), Add→(0,3)]`, root=4 |
| `direct-recursive-descent`, `direct-shunting-yard`, `direct-mp`, `direct-mp-simd`, `direct-mp-full` | **None** — value computed on the fly | *(nothing; yields `14`)* |
| `rpn-stack` | **Flat postfix** | `2 3 4 * +` |
| `bytecode-vm` | **Flat opcodes + const pool** | `PUSH PUSH PUSH MUL ADD` |

Key points:

- The four **pointer-AST** strategies produce a **bit-identical** tree — that's *why*
  their speeds are nearly equal: they differ only in *how* they find the tree, not
  what they build.
- The **arena AST** is the *same logical tree, different physical layout* — and that
  layout change alone is the ~2× speedup.
- **RPN / bytecode aren't trees** — they're the AST *flattened to post-order*.
- The **direct** evaluators build *no* representation at all.
- **`multipass` vs `multipass-arena`**: identical algorithm, different node allocation
  strategy — the arena variant is in tier 2 (×2.1), the pointer variant in tier 4 (×5.3).

## 📊 Benchmarks

```sh
./build/bench     # one-shot: string -> value
./build/reeval    # compile once, evaluate many
```

### One-shot — string → value, once

Full table, ns/leaf (10 000-leaf expressions; `×` = relative to the fastest):

| Strategy | ns/leaf | ×fastest | allocations / expr |
|---|--:|--:|---|
| `direct-recursive-descent` | 163 | **1.0** | ~0 (call stack) |
| `direct-shunting-yard` | 178 | 1.1 | 2 stacks |
| `bytecode-vm` | 193 | 1.2 | a few buffers |
| `rpn-stack` | 200 | 1.2 | a few buffers |
| `ast-arena` | 208 | 1.3 | **one** (node vector) |
| `direct-mp` | 265 | **1.6** | pre-scan vectors (no AST) |
| `direct-mp-full` | 275 | **1.7** | pre-scan vectors (no AST) |
| `direct-mp-simd` | 285 | **1.7** | pre-scan vectors (no AST) |
| `multipass-bfs` | 330 | **2.0** | one + sparse table + pre-index |
| `multipass-arena` | 343 | **2.1** | one (node vector) + pre-scan |
| `ast-pratt` | 400 | 2.5 | **one per node** |
| `ast-shunting-yard` | 434 | 2.7 | **one per node** |
| `ast-recursive-descent` | 466 | 2.9 | **one per node** |
| `multipass` | 864 | 5.3 | one per node + pre-scan |

For a *single* evaluation, inline `direct-recursive-descent` wins — there's nothing to
allocate or build. RPN/bytecode pay compile cost they can't amortize here; their
moment comes under re-evaluation.

#### 🔬 `multipass` / `multipass-arena` — the optimization story

Both are divide-and-conquer parsers that build a Cartesian tree top-down (find the
root operator, recurse on both halves). This is inherently O(n log n), unlike all
other strategies which are O(n) single-pass. The journey shows how far optimization
can go without changing the algorithm:

| Version | ns/leaf | ×rd | what changed |
|---|--:|--:|---|
| Naive (re-scan every range) | 1932 | ×30 | — |
| + pre-scan + flat-chain fold | 879 | ×14 | O(n²) → O(n log n) |
| + arena AST | 568 | ×8.7 | per-node allocation gone |
| + O(1) paren matching | ~555 | ~×8.5 | precomputed `parenMatch[]` |
| + **iterator passing** | **343** | **×2.1** | 7× fewer binary searches |
| + **sparse-table RMQ + paren pre-index** (`multipass-bfs`) | **~330** | **~×2.0** | O(1) findSplit + O(1) paren strips — theoretical floor |

**Iterator passing** was the decisive step: instead of binary-searching for candidates
at every recursive call, each split passes the known sub-iterator range directly to its
children — O(1) instead of O(log n) per call. Only paren-depth changes (when stripping
`()`) still required a binary search.

**`multipass-bfs`** eliminates that last binary search too, via two structures built in
one extra O(n log n) pass during `buildAll`:

- A **sparse table** (range-minimum query) over each depth's candidate list → O(1)
  `findSplit` instead of the O(k) linear scan.
- **Pre-indexed paren ranges** (`parenCandStart_[i]` / `parenCandEnd_[i]`) → O(1)
  paren-depth transition instead of O(log n) binary search.

The result is ~×2.0 — barely ahead of `multipass-arena` at ~×2.1. The sparse table's
build cost (~O(n log n) extra work, larger working set, more cache pressure) almost
exactly cancels the per-split savings at typical expression sizes. This is the
*theoretical ceiling* of the divide-and-conquer approach: every per-call operation is
now O(1), but the O(n log n) total work of the algorithm itself cannot be improved
without changing the strategy entirely.

#### ⚡ Collapsing the eval pass: `direct-mp`, `direct-mp-simd`, `direct-mp-full`

`multipass-bfs` still builds a Cartesian-tree AST, then walks it in a second O(n)
pass. What if the recursion returned `double` directly? [`multipass_lean.cpp`](src/multipass_lean.cpp)
tests three stacked ideas — direct eval, AVX2 SIMD scan, and operator-only token filter:

| Version | ns/leaf | ×rd | what changed |
|---|--:|--:|---|
| `multipass-bfs` | ~330 | ×2.0 | baseline (O(1) split + arena AST + eval walk) |
| `direct-mp` | ~265 | **×1.6** | drop AST — recurse directly to `double` |
| `direct-mp-full` | ~275 | ×1.7 | + operator-only `buildAll` (skip Number/Ident) |
| `direct-mp-simd` | ~285 | ×1.7 | + AVX2 `_mm256_min_epi8` prec scan |

**Direct evaluation** is the decisive step — eliminating the AST build and eval-walk
cuts ~35% from `multipass-bfs`. Both the O(n) pre-scan (building prec arrays and paren
matching) and the O(n log n) D&C recursion stay, but the constant factor shrinks
significantly when there are no heap nodes to construct or traverse.

The SIMD scan processes 32 precedence bytes per instruction but must scan the whole
candidate array to find the minimum. The simple RTL linear scan exits as soon as it
finds the split, often at ~50% depth — SIMD only wins for very long expressions
(the ordering of `direct-mp` vs `direct-mp-simd` reverses around 5 000 leaves).
The operator-only filter trades smaller candidate arrays against a more complex
tokenisation loop; the break-even is similarly expression-size-dependent.

**The winning combination**: direct evaluation alone (`direct-mp`). The SIMD and
filter add complexity with mixed payoff. The three variants form a controlled experiment
rather than a strict ranking.

#### 🚀 Bracket-free expressions: multipass-arena's best case

Without parentheses, all operators are at depth 0. There are no paren-depth changes,
so **no binary searches at all** after the root call. `multipass-arena` falls into the
same performance tier as the compiled flat forms:

```
Bracket-free expressions, 10000 leaves, ns/leaf:

direct-recursive-descent   56   ×1.0
rpn / bytecode           65–67   ×1.2
ast-arena                  71   ×1.3
multipass-arena           104   ×1.9   ← same tier as compiled forms
ast-ptr parsers         173–205  ×3.1–3.7
```

This also exposes the **Cartesian tree connection**: every O(n) parser (recursive
descent, shunting-yard, Pratt) is building a Cartesian tree *bottom-up* with a stack.
Multipass builds it *top-down*. The bottom-up direction is inherently sequential; the
top-down direction is inherently parallelizable. The log-factor cost is the price of
parallelism.

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
> you evaluate and whether you need parallelism.**
>
> - **Once, fastest?** `direct-recursive-descent` (nothing to allocate or build).
> - **Many times?** Compile to `bytecode`/`rpn` (allocation-free eval loop).
> - **Want a tree, but fast?** Arena, never per-node `unique_ptr`.
> - **Parallelism or incremental re-parsing?** `multipass-arena` — see below.

## 🧵 Parallelism & the divide-and-conquer advantage

Single-expression parsing is mostly *sequential* (each token's meaning depends on what
came before). Every strategy except `multipass` is a left-to-right stream with running
state — the two sub-expressions cannot be parsed concurrently because you don't know
where the split is until you've processed everything up to it.

`multipass` is different: it **finds the split first**, then the two halves are
completely independent. This maps naturally onto fork-join parallelism at every level of
the recursion tree.

### Three parallelism shapes, in order of suitability

**1. Batch (easy, scales well)** — many independent expressions, one parser per thread.
Measured ~3.5× on a 4-core/8-thread laptop. Works with any strategy; the only limit
is the heap allocator (per-thread arena regions fix this).

**2. `parse_parallel` top-level split** *(disabled, `src/parallel.cpp`)* — split one
expression at its single top-level `+`/`-`, parse the terms across threads. Got ~1.7×
on 4 threads; capped by AST allocation. Better addressed by the arena.

**3. `multipass` fork-join *(the right fit)*** — every recursive split is an
independent fork point:
```
find_root(expr) → [left_half, right_half]
               → parse in parallel, any depth
               → fold result at the join
```
This gives O(log n) parallel depth — far finer-grained than (2)'s single split.
With per-thread arena regions, allocation becomes thread-local and the ceiling lifts.

### Other unique properties of `multipass-arena`

| Property | Why only multipass has it |
|---|---|
| **Incremental re-parsing** | If token k changes, only the sub-tree containing k needs re-parsing. The root operator and the other half are unchanged. All other parsers are single-pass with no sub-range structure to exploit. |
| **Range-based sub-evaluation** | Evaluate any `[lo, hi)` token range as a standalone expression — directly, without re-tokenizing. |
| **Bracket-free parity** | Without parens, iterator passing eliminates all binary-search overhead; performance enters the compiled-flat-forms tier while remaining fork-join parallel. |
| **BFS / level-by-level** | All nodes at the same tree depth are independent — natural for SIMD (process 4–8 splits simultaneously) or GPU batch parsing. |

## 🛠️ Build & run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

ctest --test-dir build --output-on-failure   # 404 checks: all strategies incl. variables
./build/bench                                 # one-shot comparison (14 strategies)
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

- Benchmark numbers are averaged over several runs on a throttling i7-10610U;
  **absolute ns vary ±40% run-to-run — the ratios are the result.** Run them yourself.
- `src/parallel.cpp` is kept for reference but **excluded from the build** (see
  `CMakeLists.txt`).
