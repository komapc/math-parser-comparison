<div align="center">

# 🧮 Math-Expression Parser & Evaluator — A Comparison

**Eleven ways to turn `"-2 ^ 2 + 3 * (4 - 1)"` into a number — benchmarked head-to-head**
**(plus a five-way compile-once / evaluate-many shoot-out).**

![C++26](https://img.shields.io/badge/C%2B%2B-26-00599C?logo=cplusplus&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-3.20%2B-064F8C?logo=cmake&logoColor=white)
![tests](https://img.shields.io/badge/tests-272%20passing-brightgreen)
![warnings](https://img.shields.io/badge/-Wall%20-Wextra%20-Wpedantic-clean-brightgreen)
![deps](https://img.shields.io/badge/dependencies-none-blue)

</div>

---

A dependency-free C++26 project implementing classic (and not-so-classic) algorithms
for parsing and evaluating arithmetic expressions. **Every strategy shares one tokenizer
and one grammar**, so the benchmarks measure the *algorithm*, not incidental differences.

The recurring punchline: **performance tracks memory allocation, not algorithmic cleverness.**

## 💡 TL;DR

Instead of parsing left-to-right, find the **weakest operator** first — it's the root.
Split there. Recurse on each half. Repeat until atoms.

```
2 + 3 * 4 - 1   →   find weakest: −   →   (2 + 3 * 4)  −  (1)
                                               find weakest: +
                                            (2)  +  (3 * 4)
```

The result is a [Cartesian tree](https://en.wikipedia.org/wiki/Cartesian_tree) built top-down. Each split is independent → naturally parallel.
The cost: **O(n log n)** vs O(n) for left-to-right, so single-threaded **left-to-right wins by ~2×**.
Worth it for parallelism, incremental re-parsing, or sub-expression reuse.

## ⚡ Results at a glance

> One-shot, ns per leaf — **shorter is faster**.
> Throttling laptop: **absolute ns drift ±40% run-to-run — trust the ratios**.

```
direct-recursive-descent  ███                                137 ns   ×1.0   ← fastest
direct-shunting-yard      ███                                150 ns   ×1.1
bytecode-vm               ███                                158 ns   ×1.2
rpn-stack                 ███                                163 ns   ×1.2
ast-arena                 ████                               174 ns   ×1.3
──────────────────────────────── tier break: D&C pre-scan + direct eval ──
direct-mp                 █████                              251 ns   ×1.8   ← D&C, no AST
──────────────────────────────── tier break: AST build + eval walk ────────
multipass-arena           ██████                             297 ns   ×2.2   ← D&C + arena AST
multipass-bfs             ██████                             296 ns   ×2.2   ← D&C + sparse-table RMQ
──────────────────────────────── tier break: N heap allocations ───────────
ast-pratt                 ████████                           412 ns   ×3.0
ast-recursive-descent     ████████                           414 ns   ×3.0
ast-shunting-yard         █████████                          423 ns   ×3.1
──────────────────────────────── tier break: super-linear ─────────────────
multipass                 ██████████████                     754 ns   ×5.5   ← O(n log n) + N allocs
```

- **Tier 1 (×1.0–1.3):** All O(n) strategies with ≤1 allocation per call. Algorithm barely matters — allocation pattern dominates.
- **Tier 1.5 (×1.8):** D&C without an AST — O(n log n) work, recursion returns `double` directly.
- **Tier 2 (×2.2):** D&C with an arena AST. Both variants are tied — the sparse table's overhead exactly cancels its per-split savings at typical expression sizes.
- **Tier 3 (×3.0–3.1):** One `make_unique` per node. All three algorithms land within 3% — *allocation is the cost, not the algorithm*.
- **Tier 4 (×5.5):** O(n log n) *plus* N allocations. The arena sibling is ×2.2.

## 📐 The grammar

Numbers, single-letter variables `a`–`z`, `+ - * / ^`, unary `+/-`, parentheses.
Precedence: `+ -` < `* /` < unary < `^` (right-associative).
So `-2^2 == -4`, `2^3^2 == 512`, `2^-3 == 0.125`.

## 🧩 The strategies

### Build an [AST](https://en.wikipedia.org/wiki/Abstract_syntax_tree), then walk it

| Strategy | In one sentence |
|---|---|
| [`ast-recursive-descent`](src/recursive_descent.cpp) | [Recursive descent](https://en.wikipedia.org/wiki/Recursive_descent_parser): a function per grammar rule calls down the precedence ladder, building a pointer AST as the recursion returns. |
| [`ast-shunting-yard`](src/shunting_yard.cpp) | [Shunting-yard](https://en.wikipedia.org/wiki/Shunting_yard_algorithm): scan left-to-right, popping higher-precedence operators off a stack to fold operands into pointer AST nodes. |
| [`ast-pratt`](src/pratt.cpp) | [Pratt / precedence climbing](https://en.wikipedia.org/wiki/Operator-precedence_parser): parsing driven by each operator's binding power, looping while the next operator binds tighter than the caller's minimum. |
| [`ast-arena`](src/arena_ast.cpp) | Recursive descent, but every node is appended to one contiguous [arena](https://en.wikipedia.org/wiki/Region-based_memory_management) vector; children referenced by index, not pointer. |
| [`multipass`](src/multipass.cpp) | Recursively find the lowest-precedence operator, make it the root of a [Cartesian tree](https://en.wikipedia.org/wiki/Cartesian_tree), recurse on both halves (pointer AST). |
| [`multipass-arena`](src/multipass_arena.cpp) | Same D&C, but arena AST + O(1) paren matching + iterator passing to eliminate redundant binary searches. |
| [`multipass-bfs`](src/multipass_opt.cpp) | Arena D&C + [sparse-table RMQ](https://en.wikipedia.org/wiki/Range_minimum_query) for O(1) split-finding + pre-indexed paren ranges. Theoretical ceiling of the AST-building D&C variants. |

### Evaluate inline (no intermediate form)

| Strategy | In one sentence |
|---|---|
| [`direct-recursive-descent`](src/direct_recursive_descent.cpp) | [Recursive descent](https://en.wikipedia.org/wiki/Recursive_descent_parser) whose rules return `double` directly — no AST built. |
| [`direct-shunting-yard`](src/direct_shunting_yard.cpp) | [Shunting-yard](https://en.wikipedia.org/wiki/Shunting_yard_algorithm) with a value stack instead of an AST node stack — evaluates during the scan. |
| [`direct-mp`](src/multipass_lean.cpp) | D&C split-find + inline eval: O(n) pre-scan builds candidate lists, recursion returns `double` directly — no AST. |

### Compile to a flat form, then run

| Strategy | In one sentence |
|---|---|
| [`bytecode-vm`](src/bytecode.cpp) | Shunting-yard compiles to a [bytecode](https://en.wikipedia.org/wiki/Bytecode) `uint8_t` opcode stream + constant pool; a switch-dispatch VM runs it. |

### Compile once, evaluate many

[`reeval.cpp`](src/reeval.cpp) implements `ICompiler` → `ICompiledExpr::eval(vars)` in five forms:
`ast-ptr`, `ast-arena`, `rpn`, `bytecode`, `reparse-rd`. Compiled forms are allocation-free on eval.

Shared infrastructure: [`lexer.cpp`](src/lexer.cpp) and [`ast.cpp`](src/ast.cpp).

## 🌳 Representation taxonomy

| Group | Representation | `2 + 3 * 4` |
|---|---|---|
| `ast-rd/sy/pratt`, `multipass` | [Pointer AST](https://en.wikipedia.org/wiki/Abstract_syntax_tree) — `unique_ptr<Expr>` nodes | `Add(Num2, Mul(Num3, Num4))` |
| `ast-arena`, `multipass-arena` | [Arena](https://en.wikipedia.org/wiki/Region-based_memory_management) AST — flat vector, index children | `[2, 3, 4, Mul(1,2), Add(0,3)]` |
| `direct-*` (all three) | None — value computed on the fly | *(yields `14`)* |
| `rpn-stack` | Flat [postfix (RPN)](https://en.wikipedia.org/wiki/Reverse_Polish_notation) | `2 3 4 * +` |
| `bytecode-vm` | [Bytecode](https://en.wikipedia.org/wiki/Bytecode) opcodes + const pool | `PUSH PUSH PUSH MUL ADD` |

The four pointer-AST strategies produce bit-identical trees and land within 3% of each other — they differ only in *how* they find the tree. The arena layout change alone gives ~2× speedup over pointer nodes.

## 📊 Benchmarks

```sh
./build/bench     # one-shot: string -> value
./build/reeval    # compile once, evaluate many
```

### One-shot — string → value, once

ns/leaf, 1 000-leaf expressions (100 reps); `×` relative to fastest:

| Strategy | ns/leaf | × | allocations / expr |
|---|--:|--:|---|
| [`direct-recursive-descent`](src/direct_recursive_descent.cpp) | 137 | **1.0** | ~0 (call stack) |
| [`direct-shunting-yard`](src/direct_shunting_yard.cpp) | 150 | 1.1 | member vectors, reused |
| [`bytecode-vm`](src/bytecode.cpp) | 158 | 1.2 | member vectors, reused |
| [`ast-arena`](src/arena_ast.cpp) | 174 | 1.3 | **one** (node vector) |
| [`direct-mp`](src/multipass_lean.cpp) | 251 | **1.8** | pre-scan vectors (no AST) |
| [`multipass-bfs`](src/multipass_opt.cpp) | 296 | **2.2** | one + sparse table + pre-index |
| [`multipass-arena`](src/multipass_arena.cpp) | 297 | **2.2** | one (node vector) + pre-scan |
| [`ast-pratt`](src/pratt.cpp) | 412 | 3.0 | **one per node** |
| [`ast-recursive-descent`](src/recursive_descent.cpp) | 414 | 3.0 | **one per node** |
| [`ast-shunting-yard`](src/shunting_yard.cpp) | 423 | 3.1 | **one per node** |
| [`multipass`](src/multipass.cpp) | 754 | 5.5 | one per node + pre-scan |

#### ⚖️ Tier 1: leveling the playing field

The original `shunting_yard`, `direct_shunting_yard`, `rpn`, and `bytecode` allocated
every working vector fresh each call. The recursive-descent family kept `tokens_` as a
member, reusing capacity automatically. Promoting all working vectors to class members
and `.clear()`-ing at entry eliminated this incidental handicap — after the first call,
all tier 1 strategies are allocation-free. The residual gaps are algorithmic:

| | ×rd | cause |
|---|--:|---|
| `direct-rd` | 1.0 | single left-to-right pass, call stack only |
| `direct-sy` | 1.1 | two explicit stacks (operand + operator) |
| `rpn` / `bytecode` | 1.2 | two passes: build intermediate form, then evaluate |
| `ast-arena` | 1.3 | one allocation (node vector), then tree-walk |

#### 🔬 `multipass` → `multipass-arena` → `multipass-bfs`

D&C parsers build a [Cartesian tree](https://en.wikipedia.org/wiki/Cartesian_tree) top-down — inherently O(n log n). The journey:

| Version | ns/leaf | ×rd | what changed |
|---|--:|--:|---|
| Naive (re-scan every range) | 1932 | ×30 | — |
| + pre-scan + flat-chain fold | 879 | ×14 | O(n²) → O(n log n) |
| + arena AST | 568 | ×8.7 | per-node allocation gone |
| + O(1) paren matching | ~555 | ~×8.5 | precomputed `parenMatch[]` |
| + **iterator passing** (`multipass-arena`) | **~297** | **~×2.2** | 7× fewer binary searches |
| + **[sparse-table RMQ](https://en.wikipedia.org/wiki/Range_minimum_query) + paren pre-index** (`multipass-bfs`) | **~296** | **~×2.2** | O(1) findSplit + O(1) paren strips |

Iterator passing was the decisive step. `multipass-bfs` eliminates the last binary search
(paren-depth transitions) via a sparse table, but the table's larger cache footprint can
cancel that gain. Both variants trade places run-to-run. This is the **theoretical ceiling**
of D&C: every per-call operation is O(1), but the O(n log n) total work is irreducible.

#### ⚡ Collapsing the eval pass: `direct-mp`

Returning `double` directly from the recursion eliminates the AST build + eval-walk passes:

| Version | ns/leaf | ×rd | what changed |
|---|--:|--:|---|
| `multipass-bfs` | ~296 | ×2.2 | baseline — O(1) split, arena AST, eval walk |
| `direct-mp` | ~251 | **×1.8** | drop AST, linear RTL scan, return `double` |

Direct evaluation cuts ~15% from `multipass-bfs` by eliminating the O(n) node-construction
and eval-walk passes, while keeping the same O(n log n) D&C structure.

### Re-eval — compile once, evaluate many

Variables `a`–`d`, 1000-leaf expressions:

| Strategy | compile ns/expr | per-eval ns/expr | per-eval × |
|---|--:|--:|--:|
| `bytecode` | ~138k | **38k** | 1.0 |
| `rpn` | ~141k | 38k | 1.0 |
| `ast-arena` | **~130k** | 52k | 1.4 |
| `ast-ptr` | ~405k | 66k | 1.7 |
| `reparse-rd` | ~0 | **435k** | 11.5 |

Flat forms win per-eval: cache-friendly linear walk, zero per-call allocation.
`ast-arena` compiles fastest (130k ns) but evaluates slower than bytecode/rpn (52k vs 38k).
Re-parsing beats compiling only if you evaluate fewer than ~1–2 times.

### 🏁 The verdict

> - **Once, fastest?** `direct-recursive-descent` — nothing to allocate or build.
> - **Many times?** `bytecode` / `rpn` — allocation-free eval loop.
> - **Need a tree?** Arena — never per-node `unique_ptr`.
> - **Parallelism or incremental re-parse?** `multipass-arena` — the only strategy
>   where sub-ranges are independent and a token change requires only local re-parsing.

## 🧵 Parallelism & divide-and-conquer

Every strategy except multipass is a left-to-right stream — sub-expressions can't be
parsed concurrently because the split point is unknown until the full scan completes.
Multipass finds the split *first*, making both halves fully independent [fork-join](https://en.wikipedia.org/wiki/Fork%E2%80%93join_model) tasks.

| Property | Why only multipass |
|---|---|
| **[Fork-join](https://en.wikipedia.org/wiki/Fork%E2%80%93join_model) parallelism** | Every split is an independent fork; O(log n) parallel depth with per-thread arena regions. |
| **Incremental re-parsing** | Only the sub-tree containing the changed token needs re-parsing. |
| **Range-based sub-evaluation** | Evaluate any `[lo, hi)` token sub-range directly. |
| **[BFS](https://en.wikipedia.org/wiki/Breadth-first_search) / level-by-level** | All nodes at depth d are independent — natural for [SIMD](https://en.wikipedia.org/wiki/Single_instruction,_multiple_data) or GPU batch parsing. |

**Measured batch scaling (400 × 1000-leaf expressions, i7-10610U, 4 physical cores):**

| Strategy | 1T ns/expr | 2T ns/expr | 4T ns/expr | 2T ×speedup | 4T ×speedup |
|---|--:|--:|--:|--:|--:|
| `ast-arena` | 117k | 61k | 46k | ×1.9 | ×2.5 |
| `multipass-arena` | 466k | 222k | 140k | ×2.1 | **×3.3** |

`multipass-arena` scales better per core (83% efficiency vs 63% for `ast-arena`), but
starts from a ×4× higher single-thread baseline. On this 4-core machine **batch
throughput still favours `ast-arena` at every thread count**. The break-even core count
was not reached in this measurement.

The structural advantage is elsewhere: `multipass-arena` is the only strategy that can
parallelize a **single large expression** (fork at the root split, join the results) and
the only one that supports **incremental re-parsing** (change one token → re-parse one
sub-tree). Neither capability is reflected in the batch benchmark above.

## 🛠️ Build & run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

ctest --test-dir build --output-on-failure   # 272 checks
./build/bench                                 # one-shot (11 strategies)
./build/reeval                                # compile-once / eval-many
```

Requires GCC 14 + CMake ≥ 3.20. Falls back to C++23 on older compilers (CMake sets the standard automatically). No external dependencies.

## 🗂️ Layout

```
include/parser/   interfaces (token, lexer, ast, evaluator, arena_ast, reeval)
src/              one file per strategy + shared lexer/ast
bench/            benchmark.cpp, reeval.cpp
tests/            test_parsers.cpp (dependency-free, run via CTest)
```

## 📎 Notes

- Numbers from a throttling i7-10610U; **absolute ns vary ±40% — ratios are the result**.
- `src/parallel.cpp` kept for reference, excluded from the build.
