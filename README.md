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

A dependency-free C++20 project implementing classic (and not-so-classic) algorithms
for parsing and evaluating arithmetic expressions. **Every strategy shares one tokenizer
and one grammar**, so the benchmarks measure the *algorithm*, not incidental differences.

The recurring punchline: **performance tracks memory allocation, not algorithmic cleverness.**

## ⚡ Results at a glance

> One-shot, ns per leaf — **shorter is faster**.
> Throttling laptop: **absolute ns drift ±40% — trust the ratios**.

```
direct-recursive-descent  ███                                162 ns   ×1.0   ← fastest
direct-shunting-yard      ███                                179 ns   ×1.1
bytecode-vm               ███                                192 ns   ×1.2
rpn-stack                 ███                                193 ns   ×1.2
ast-arena                 ███                                195 ns   ×1.2
──────────────────────────────── tier break: O(n) pre-scan overhead ──────
direct-mp-simd            █████                              273 ns   ×1.7   ← D&C, no AST, SIMD scan
direct-mp                 █████                              293 ns   ×1.8   ←   linear RTL scan
direct-mp-full            █████                              304 ns   ×1.9   ←   + operator-only filter
──────────────────────────────── tier break: AST build + eval walk ────────
multipass-arena           ██████                             378 ns   ×2.3   ← D&C + arena AST
multipass-bfs             ██████                             384 ns   ×2.4   ← D&C + sparse-table RMQ
──────────────────────────────── tier break: N heap allocations ───────────
ast-pratt                 ████████                           456 ns   ×2.8
ast-recursive-descent     ████████                           488 ns   ×3.0
ast-shunting-yard         ████████                           500 ns   ×3.1
──────────────────────────────── tier break: super-linear ─────────────────
multipass                 ██████████████                     839 ns   ×5.2   ← O(n log n) + N allocs
```

- **Tier 1 (×1.0–1.2):** All O(n) single-pass strategies with ≤1 allocation per call, compressed to 20% of each other. Algorithm irrelevant inside this tier.
- **Tier 1.5 (×1.7–1.9):** Divide-and-conquer without building an AST — O(n log n) work, but the recursion returns `double` directly.
- **Tier 2 (×2.3–2.4):** D&C with an arena AST. Both variants trade places run-to-run; the sparse table's cache footprint can cancel its per-split savings.
- **Tier 3 (×2.8–3.1):** One `make_unique` per AST node. All three algorithms land within 11% — *algorithm is irrelevant*, allocation is the cost.
- **Tier 4 (×5.2):** O(n log n) *plus* N allocations. The arena sibling (`multipass-arena`) is ×2.3.

## 📐 The grammar

Numbers, single-letter variables `a`–`z`, `+ - * / ^`, unary `+/-`, parentheses.
Precedence: `+ -` < `* /` < unary < `^` (right-associative).
So `-2^2 == -4`, `2^3^2 == 512`, `2^-3 == 0.125`.

## 🧩 The strategies

### Build an AST, then walk it

| Strategy | Source | In one sentence |
|---|---|---|
| `ast-recursive-descent` | [`recursive_descent.cpp`](src/recursive_descent.cpp) | A function per grammar rule calls down the precedence ladder, building a pointer AST as the recursion returns. |
| `ast-shunting-yard` | [`shunting_yard.cpp`](src/shunting_yard.cpp) | Scan left-to-right, popping higher-precedence operators off a stack to fold operands into pointer AST nodes. |
| `ast-pratt` | [`pratt.cpp`](src/pratt.cpp) | Parsing driven by each operator's binding power, looping while the next operator binds tighter than the caller's minimum. |
| `ast-arena` | [`arena_ast.cpp`](src/arena_ast.cpp) | Recursive descent, but every node is appended to one contiguous vector; children referenced by index, not pointer. |
| `multipass` | [`multipass.cpp`](src/multipass.cpp) | Recursively find the lowest-precedence operator, make it the root, recurse on both halves (pointer AST). |
| `multipass-arena` | [`multipass_arena.cpp`](src/multipass_arena.cpp) | Same D&C, but arena AST + O(1) paren matching + iterator passing to eliminate redundant binary searches. |
| `multipass-bfs` | [`multipass_opt.cpp`](src/multipass_opt.cpp) | Arena D&C + sparse-table RMQ for O(1) split-finding + pre-indexed paren ranges. Theoretical ceiling of this family. |

### Evaluate inline (no intermediate form)

| Strategy | Source | In one sentence |
|---|---|---|
| `direct-recursive-descent` | [`direct_recursive_descent.cpp`](src/direct_recursive_descent.cpp) | Recursive descent whose rules return `double` directly — no AST built. |
| `direct-shunting-yard` | [`direct_shunting_yard.cpp`](src/direct_shunting_yard.cpp) | Shunting-yard with a value stack instead of an AST node stack — evaluates during the scan. |
| `direct-mp` | [`multipass_lean.cpp`](src/multipass_lean.cpp) | D&C split-find + inline eval: O(n) pre-scan builds candidate lists, recursion finds split and returns `double` — no AST. |
| `direct-mp-simd` | [`multipass_lean.cpp`](src/multipass_lean.cpp) | `direct-mp` with AVX2 `_mm256_min_epi8` replacing the linear RTL scan for the split-finding step. |
| `direct-mp-full` | [`multipass_lean.cpp`](src/multipass_lean.cpp) | SIMD split-finding + operator-only `buildAll` that skips `Number`/`Ident` tokens. |

### Compile to a flat form, then run

| Strategy | Source | In one sentence |
|---|---|---|
| `rpn-stack` | [`rpn.cpp`](src/rpn.cpp) | Shunting-yard emits a flat postfix token sequence; a value stack executes it. |
| `bytecode-vm` | [`bytecode.cpp`](src/bytecode.cpp) | Shunting-yard compiles to a `uint8_t` opcode stream + constant pool; a switch-dispatch VM runs it. |

### Compile once, evaluate many

[`reeval.cpp`](src/reeval.cpp) implements `ICompiler` → `ICompiledExpr::eval(vars)` in five forms:
`ast-ptr`, `ast-arena`, `rpn`, `bytecode`, `reparse-rd`. Compiled forms are allocation-free on eval.

Shared infrastructure: [`lexer.cpp`](src/lexer.cpp) and [`ast.cpp`](src/ast.cpp).

## 🌳 Representation taxonomy

| Group | Representation | `2 + 3 * 4` |
|---|---|---|
| `ast-rd/sy/pratt`, `multipass` | Pointer AST — `unique_ptr<Expr>` nodes | `Add(Num2, Mul(Num3, Num4))` |
| `ast-arena`, `multipass-arena` | Arena AST — flat vector, index children | `[2, 3, 4, Mul(1,2), Add(0,3)]` |
| `direct-*` (all five) | None — value computed on the fly | *(yields `14`)* |
| `rpn-stack` | Flat postfix | `2 3 4 * +` |
| `bytecode-vm` | Opcodes + const pool | `PUSH PUSH PUSH MUL ADD` |

The four pointer-AST strategies produce bit-identical trees and land within 11% of each other — they differ only in *how* they find the tree. The arena layout change alone gives ~2× speedup over pointer nodes.

## 📊 Benchmarks

```sh
./build/bench     # one-shot: string -> value
./build/reeval    # compile once, evaluate many
```

### One-shot — string → value, once

ns/leaf, 1 000-leaf expressions (100 reps); `×` relative to fastest:

| Strategy | ns/leaf | × | allocations / expr |
|---|--:|--:|---|
| `direct-recursive-descent` | 162 | **1.0** | ~0 (call stack) |
| `direct-shunting-yard` | 179 | 1.1 | member vectors, reused |
| `bytecode-vm` | 192 | 1.2 | member vectors, reused |
| `rpn-stack` | 193 | 1.2 | member vectors, reused |
| `ast-arena` | 195 | 1.2 | **one** (node vector) |
| `direct-mp-simd` | 273 | **1.7** | pre-scan vectors (no AST) |
| `direct-mp` | 293 | **1.8** | pre-scan vectors (no AST) |
| `direct-mp-full` | 304 | **1.9** | pre-scan vectors (no AST) |
| `multipass-arena` | 378 | **2.3** | one (node vector) + pre-scan |
| `multipass-bfs` | 384 | **2.4** | one + sparse table + pre-index |
| `ast-pratt` | 456 | 2.8 | **one per node** |
| `ast-recursive-descent` | 488 | 3.0 | **one per node** |
| `ast-shunting-yard` | 500 | 3.1 | **one per node** |
| `multipass` | 839 | 5.2 | one per node + pre-scan |

#### ⚖️ Tier 1: leveling the playing field

The original `shunting_yard`, `direct_shunting_yard`, `rpn`, and `bytecode` allocated
every working vector (token list, operator stack, output, eval stack) fresh each call.
The recursive-descent family kept `tokens_` as a member, reusing capacity automatically.
Promoting all working vectors to class members and `.clear()`-ing at entry cost eliminated
this incidental handicap. After the first warm-up call, all tier 1 strategies are
allocation-free. The residual gaps are algorithmic:

| | ×rd | cause |
|---|--:|---|
| `direct-rd` | 1.0 | single left-to-right pass, call stack only |
| `direct-sy` | 1.1 | two explicit stacks (operand + operator) |
| `rpn` / `bytecode` / `ast-arena` | 1.2 | two passes: build intermediate form, then evaluate |

#### 🔬 `multipass` → `multipass-arena` → `multipass-bfs`

D&C parsers build a Cartesian tree top-down — inherently O(n log n). The journey:

| Version | ns/leaf | ×rd | what changed |
|---|--:|--:|---|
| Naive (re-scan every range) | 1932 | ×30 | — |
| + pre-scan + flat-chain fold | 879 | ×14 | O(n²) → O(n log n) |
| + arena AST | 568 | ×8.7 | per-node allocation gone |
| + O(1) paren matching | ~555 | ~×8.5 | precomputed `parenMatch[]` |
| + **iterator passing** (`multipass-arena`) | **~343** | **~×2.1** | 7× fewer binary searches |
| + **sparse-table RMQ + paren pre-index** (`multipass-bfs`) | **~330** | **~×2.0** | O(1) findSplit + O(1) paren strips |

Iterator passing was the decisive step: sub-iterator ranges are passed directly to children
instead of binary-searching at each call. `multipass-bfs` eliminates the last binary search
(paren-depth transitions) via a sparse table and pre-indexed paren ranges — but the table's
larger cache footprint can cancel that gain on a throttling CPU. Both variants trade places
run-to-run. This is the **theoretical ceiling** of D&C: every per-call operation is O(1),
but the O(n log n) total work cannot be reduced without changing the algorithm.

#### ⚡ Collapsing the eval pass: `direct-mp` variants

The AST build + eval-walk is itself O(n) overhead on top of the O(n log n) D&C work.
Returning `double` directly from the recursion eliminates both passes:

| Version | ns/leaf | ×rd | what changed |
|---|--:|--:|---|
| `multipass-bfs` | ~384 | ×2.4 | baseline — O(1) split, arena AST, eval walk |
| `direct-mp-simd` | ~273 | **×1.7** | drop AST + AVX2 SIMD prec scan |
| `direct-mp` | ~293 | ×1.8 | drop AST, linear RTL scan |
| `direct-mp-full` | ~304 | ×1.9 | + operator-only candidate filter |

Direct evaluation cuts ~35% from `multipass-bfs`. The three variants order differently
by expression size (SIMD helps at large; filter helps at small) — treat them as a
controlled experiment rather than a strict ranking.

### Re-eval — compile once, evaluate many

Variables `a`–`d`, 1000-leaf expressions:

| Strategy | compile ns/expr | per-eval ns/expr | per-eval × |
|---|--:|--:|--:|
| `bytecode` | ~136k | **33k** | 1.0 |
| `rpn` | ~149k | 36k | 1.1 |
| `ast-arena` | **~134k** | 56k | 1.7 |
| `ast-ptr` | ~372k | 54k | 1.6 |
| `reparse-rd` | ~0 | **372k** | 11.2 |

Flat forms (bytecode/rpn) win per-eval: cache-friendly linear walk, zero per-call allocation.
`ast-arena` compiles 2.8× cheaper than `ast-ptr`; per-eval both are within noise.
Re-parsing beats compiling only if you evaluate fewer than ~2 times.

### 🏁 The verdict

> - **Once, fastest?** `direct-recursive-descent` — nothing to allocate or build.
> - **Many times?** `bytecode` / `rpn` — allocation-free eval loop.
> - **Need a tree?** Arena — never per-node `unique_ptr`.
> - **Parallelism or incremental re-parse?** `multipass-arena` — the only strategy
>   where sub-ranges are independent and a token change requires only local re-parsing.

## 🧵 Parallelism & divide-and-conquer

Every strategy except multipass is a left-to-right stream — sub-expressions can't be
parsed concurrently because the split point is unknown until the full scan completes.
Multipass finds the split *first*, making both halves fully independent fork-join tasks.

| Property | Why only multipass |
|---|---|
| **Fork-join parallelism** | Every split is an independent fork; O(log n) parallel depth with per-thread arena regions. |
| **Incremental re-parsing** | Only the sub-tree containing the changed token needs re-parsing. |
| **Range-based sub-evaluation** | Evaluate any `[lo, hi)` token sub-range directly. |
| **BFS / level-by-level** | All nodes at depth d are independent — natural for SIMD or GPU batch parsing. |

## 🛠️ Build & run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

ctest --test-dir build --output-on-failure   # 404 checks
./build/bench                                 # one-shot (14 strategies)
./build/reeval                                # compile-once / eval-many
```

Requires C++20 and CMake ≥ 3.20. No external dependencies.

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
