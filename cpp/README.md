<div align="center">

# 🧮 Math-Expression Parser & Evaluator — A Comparison

**Twelve ways to turn `"-2 ^ 2 + 3 * (4 - 1)"` into `5` — benchmarked head-to-head.**

![C++26](https://img.shields.io/badge/C%2B%2B-26-00599C?logo=cplusplus&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-3.20%2B-064F8C?logo=cmake&logoColor=white)
![tests](https://img.shields.io/badge/tests-288%20passing-brightgreen)
![warnings](https://img.shields.io/badge/-Wall%20-Wextra%20-Wpedantic-clean-brightgreen)
![deps](https://img.shields.io/badge/dependencies-none-blue)

</div>

---

> Part of a [three-language comparison](../README.md) (C++ · Haskell · Python). This is the C++ implementation — and the in-depth analysis the other two are measured against.

A dependency-free C++26 project implementing classic (and not-so-classic) algorithms for parsing and evaluating arithmetic expressions. Every strategy shares one tokenizer and one grammar — the benchmarks measure the *algorithm*, not incidental differences.

**The recurring punchline: performance tracks memory allocation, not algorithmic cleverness.**

## ⚡ Results at a glance

> One-shot, ns per leaf — **shorter is faster**.
> Throttling laptop: **absolute ns drift ±40% run-to-run — trust the ratios**.

```
direct-recursive-descent  ███                                199 ns   ×1.0   ← fastest
bytecode-vm               ████                               247 ns   ×1.2
ast-arena                 ████                               264 ns   ×1.3   ← fastest AST builder
direct-shunting-yard      ████                               276 ns   ×1.4
──────────────────────────────── tier break: D&C pre-scan + direct eval ──
direct-mp                 █████                              294 ns   ×1.5   ← D&C, no AST
──────────────────────────────── tier break: multipass AST build + walk ───
multipass-reverse         █████                              310 ns   ×1.6   ← bottom-up + arena AST
multipass-arena           ██████                             367 ns   ×1.8   ← D&C + arena AST
multipass-bfs             ███████                            463 ns   ×2.3   ← D&C + sparse-table RMQ
──────────────────────────────── tier break: N heap allocations ───────────
ast-pratt                 █████████                          564 ns   ×2.8
ast-shunting-yard         █████████                          578 ns   ×2.9
ast-recursive-descent     █████████                          584 ns   ×2.9
──────────────────────────────── tier break: super-linear ─────────────────
multipass                 ████████████████                  1012 ns   ×5.1   ← O(n log n) + N allocs
```

- **Tier 1 (×1.0–1.4):** O(n), ≤1 allocation. `direct-rd` is fastest; `bytecode-vm` and the other direct forms sit within ~40% and trade places run-to-run. `ast-arena` is the **fastest AST builder** — recursive-descent + one arena vector beats every other AST approach.
- **Tier 1.5 (×1.5):** D&C without an AST — O(n log n) pre-scan, result returned inline.
- **Tier 2 (×1.6–2.3):** the rest of the multipass family, arena AST. `multipass-reverse` (bottom-up, allocation-free item stack — see [docs/multipass-reverse.md](../docs/multipass-reverse.md)) leads it and is the **second-fastest tree builder**; the top-down D&C forms are the only way to build a tree whose sub-ranges are split-independent.
- **Tier 3 (×2.8–2.9):** One `make_unique` per node. Algorithm barely matters — allocator dominates.
- **Tier 4 (×5.1):** O(n log n) *plus* N allocations.

## 📐 Grammar

Numbers, single-letter variables `a`–`z`, `+ - * / ^`, unary `+/-`, parentheses.
Precedence: `+ -` < `* /` < unary < `^` (right-associative). So `-2^2 = -4`, `2^3^2 = 512`.

## 🧩 Strategies

| Strategy | Approach | Representation |
|---|---|---|
| [`ast-recursive-descent`](src/recursive_descent.cpp) | [Recursive descent](https://en.wikipedia.org/wiki/Recursive_descent_parser) | pointer AST |
| [`ast-shunting-yard`](src/shunting_yard.cpp) | [Shunting-yard](https://en.wikipedia.org/wiki/Shunting_yard_algorithm) | pointer AST |
| [`ast-pratt`](src/pratt.cpp) | [Pratt / precedence climbing](https://en.wikipedia.org/wiki/Operator-precedence_parser) | pointer AST |
| [`ast-arena`](src/arena_ast.cpp) | Recursive descent | arena AST |
| [`multipass`](src/multipass.cpp) | D&C [Cartesian tree](https://en.wikipedia.org/wiki/Cartesian_tree) | pointer AST |
| [`multipass-arena`](src/multipass_arena.cpp) | D&C + iterator passing | arena AST |
| [`multipass-bfs`](src/multipass_opt.cpp) | D&C + [sparse-table RMQ](https://en.wikipedia.org/wiki/Range_minimum_query) | arena AST |
| [`multipass-reverse`](src/multipass_reverse.cpp) | Bottom-up reduction (innermost/highest first) | arena AST |
| [`direct-recursive-descent`](src/direct_recursive_descent.cpp) | Recursive descent | none — returns `double` |
| [`direct-shunting-yard`](src/direct_shunting_yard.cpp) | Shunting-yard | none — returns `double` |
| [`direct-mp`](src/multipass_lean.cpp) | D&C | none — returns `double` |
| [`bytecode-vm`](src/bytecode.cpp) | Shunting-yard → [bytecode](https://en.wikipedia.org/wiki/Bytecode) + VM | flat opcode stream |

The pointer-AST strategies produce bit-identical trees; the three left-to-right variants (`ast-rd`, `ast-sy`, `ast-pratt`) cluster tightly — algorithm barely matters, only allocation. The arena layout gives ~2× speedup over pointer nodes.

## 📊 Benchmarks

### One-shot — string → value

ns/leaf, 1 000-leaf expressions; `×` relative to fastest:

| Strategy | ns/leaf | × | allocations / expr |
|---|--:|--:|---|
| [`direct-recursive-descent`](src/direct_recursive_descent.cpp) | 199 | **1.0** | ~0 (call stack) |
| [`bytecode-vm`](src/bytecode.cpp) | 247 | **1.2** | member vectors, reused |
| [`ast-arena`](src/arena_ast.cpp) | 264 | 1.3 | **one** (node vector) |
| [`direct-shunting-yard`](src/direct_shunting_yard.cpp) | 276 | 1.4 | member vectors, reused |
| [`direct-mp`](src/multipass_lean.cpp) | 294 | **1.5** | pre-scan vectors (no AST) |
| [`multipass-reverse`](src/multipass_reverse.cpp) | 310 | **1.6** | one (node vector); item stack reused |
| [`multipass-arena`](src/multipass_arena.cpp) | 367 | **1.8** | one (node vector) + pre-scan |
| [`multipass-bfs`](src/multipass_opt.cpp) | 463 | **2.3** | one + sparse table + pre-index |
| [`ast-pratt`](src/pratt.cpp) | 564 | 2.8 | **one per node** |
| [`ast-shunting-yard`](src/shunting_yard.cpp) | 578 | 2.9 | **one per node** |
| [`ast-recursive-descent`](src/recursive_descent.cpp) | 584 | 2.9 | **one per node** |
| [`multipass`](src/multipass.cpp) | 1012 | 5.1 | one per node + pre-scan |

### Re-eval — compile once, evaluate many

Variables `a`–`d`, 1 000-leaf expressions:

| Strategy | compile ns/expr | per-eval ns/expr | per-eval × |
|---|--:|--:|--:|
| `rpn` | ~90k | **25k** | 1.0 |
| `ast-arena` | **~76k** | 28k | 1.1 |
| `bytecode` | ~120k | 31k | 1.2 |
| `multipass-arena` | ~215k | 40k | 1.6 |
| `ast-ptr` | ~270k | 50k | 2.0 |
| `reparse-rd` | ~0 | **380k** | 15.2 |

Flat forms (`rpn`, `bytecode`) win per-eval. `ast-arena` compiles fastest — one recursive-descent pass. `multipass-arena` compiles at ~×2.8 the cost but is the only compiled form that supports incremental re-parsing and parallel evaluation.

### 🏁 Verdict

> - **Once, fastest?** `direct-rd` — with `bytecode-vm` and the other direct forms within ~30% (ordering inside this group flips run-to-run).
> - **Many times?** `rpn` / `bytecode` — allocation-free eval loop.
> - **Need a tree?** `ast-arena` — one allocation, never per-node `unique_ptr`.
> - **Parallel or incremental re-parse?** `multipass-arena` — the only strategy where sub-ranges are independent.

## 🧵 Parallelism

Multipass finds the split point *first*, making both halves independent [fork-join](https://en.wikipedia.org/wiki/Fork%E2%80%93join_model) tasks. Every other strategy is a left-to-right stream and cannot parallelize within a single expression.

**Batch scaling** (400 × 1 000-leaf expressions, i7-10610U, 4 physical / 8 logical cores):

| Strategy | 1T ns/expr | 2T ns/expr | 4T ns/expr | 8T ns/expr | 4T × | 8T × |
|---|--:|--:|--:|--:|--:|--:|
| `ast-arena` | 222k | 110k | 68k | 58k | ×3.3 | ×3.8 |
| `multipass-arena` | 248k | 154k | 86k | 76k | ×2.9 | ×3.3 |

Per-core efficiency fluctuates run-to-run on this throttling laptop. `ast-arena` leads in absolute throughput at every thread count.

**Single-expression fork-join** (`multipass-parallel`, middle-split + `std::async`):

| n (leaves) | par1 ns/leaf | par4 ns/leaf | par8 ns/leaf | best speedup | vs `ast-arena` |
|---|--:|--:|--:|--:|--:|
| 10 000 | 315 | **258** | 355 | par4 ×1.2 | still slower |
| 100 000 | 372 | 378 | **315** | par8 ×1.2 | still slower |

Fork-join delivers ~×1.2 over sequential multipass. Beating `ast-arena` requires T > log₂ n physical cores (≈13 for n=10k) — the O(n log n) gap is fundamental.

## 🛠️ Build & run

From the repo root (`-S cpp`); drop the `cpp/` prefix if you're already in this directory.

```sh
cmake -S cpp -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14
cmake --build build -j

ctest --test-dir build --output-on-failure   # 288 checks
./build/bench                                 # one-shot (12 strategies)
./build/corpus_bench                          # shared corpus (cross-language comparable)
./build/reeval                                # compile-once / eval-many
./build/parallel_bench                        # batch scaling 1–8 threads
./build/single_par_bench                      # single-expression fork-join scaling
./build/adversarial_bench                     # structured chains: multipass top-down Θ(n²) vs reverse Θ(n)
```

Requires GCC 14 + CMake ≥ 3.20. No external dependencies. `corpus_bench` reads
`bench/corpus/` — run `python3 bench/gen_corpus.py` first.

## 🗂️ Layout

```
include/parser/   interfaces (evaluator, arena_ast, reeval, …)
src/              one file per strategy + shared lexer/ast
bench/            benchmark.cpp  corpus_bench.cpp  reeval.cpp  parallel_bench.cpp  single_par_bench.cpp  adversarial_bench.cpp
tests/            test_parsers.cpp (288 checks, run via CTest)
```

> Numbers from a throttling i7-10610U — **absolute ns vary ±40%; ratios are the result**.
> `src/parallel.cpp` kept for reference, excluded from the build.
