<div align="center">

# 🧮 Math-Expression Parser & Evaluator — A Comparison

**Fifteen ways to turn `"-2 ^ 2 + 3 * (4 - 1)"` into `5` — benchmarked head-to-head.**

![C++26](https://img.shields.io/badge/C%2B%2B-26-00599C?logo=cplusplus&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-3.20%2B-064F8C?logo=cmake&logoColor=white)
![tests](https://img.shields.io/badge/tests-926%20checks%20%2B%20fuzz-brightgreen)
![warnings](https://img.shields.io/badge/-Wall%20-Wextra%20-Wpedantic-clean-brightgreen)
![deps](https://img.shields.io/badge/dependencies-none-blue)

</div>

---

> Part of a [three-language comparison](../README.md) (C++ · Haskell · Python). This is the C++ implementation — and the in-depth analysis the other two are measured against.

A dependency-free C++26 project implementing classic (and not-so-classic) algorithms for parsing and evaluating arithmetic expressions. Every strategy shares one lexer and one grammar — the benchmarks measure the *algorithm*, not incidental differences. The lexer has two modes over one set of rules (streaming `Lexer::next()` for strategies that read left to right, `tokenize()` for those whose algorithm indexes the token array); the one strategy that bypasses it, `direct-scannerless`, exists precisely to measure what it costs ([why](../FINDINGS.md#lexing-rules--applied-to-every-parser)).

**The recurring punchline: performance tracks memory allocation, not algorithmic cleverness.**

## ⚡ Results at a glance

> One-shot, ns per leaf at n=1000 — **shorter is faster**.
> Neutral 4-vCPU GitHub runner, median of three [CI bench](../.github/workflows/bench.yml) runs; `×` is relative to the fastest strategy that uses the shared lexer. **Trust the tiers, not the digits.**

```
direct-scannerless        ██                           37 ns   ×0.75  ← lexer-free control (not a contender)
direct-shunting-yard      ██                           50 ns   ×1.0
direct-reverse            ██                           50 ns   ×1.0
direct-recursive-descent  ██                           50 ns   ×1.0   ← three-way tie at the top
bytecode-vm               ██                           59 ns   ×1.2
──────────────────────────────── tier break: builds a tree ────────────────
multipass-reverse-fold    ███                          68 ns   ×1.4   ← fastest tree builder (bottom-up, fused)
ast-arena                 ███                          72 ns   ×1.4   ← fastest classic tree builder
──────────────────────────────── tier break: token array / N allocations ──
multipass-reverse         ████                         98 ns   ×2.0   ← bottom-up, buffered
direct-mp                 ████                        101 ns   ×2.0   ← D&C, no AST
ast-recursive-descent     █████                       130 ns   ×2.6
multipass-arena           █████                       130 ns   ×2.6
ast-shunting-yard         █████                       135 ns   ×2.7
ast-pratt                 █████                       136 ns   ×2.7
multipass-bfs             ██████                      142 ns   ×2.9
──────────────────────────────── tier break: super-linear ─────────────────
multipass                 █████████                   232 ns   ×4.7   ← O(n log n) + N allocs
```

- **Control (×0.75):** `direct-scannerless` is `direct-rd` with the lexer fused into the grammar — no token stream at all. It exists to measure the shared lexer's cost (a quarter of `direct-rd`'s time), not to compete ([why](../FINDINGS.md#lexing-rules--applied-to-every-parser)).
- **Tier 1 (×1.0–1.2):** no tree, O(n), streaming tokens, ≤1 allocation. `direct-sy`, `direct-rd` and `direct-reverse` are a **three-way tie** at ~50 ns/leaf (two of three runs within ±1.5 %); `bytecode-vm` sits ~20 % behind.
- **Tier 2 (×1.4):** the two contiguous tree builders. `multipass-reverse-fold` (bottom-up, fused — see [docs/multipass-reverse.md](../docs/multipass-reverse.md)) is the **fastest tree builder**, ~3–5 % ahead of `ast-arena` (positive in 11 of 12 size×run measurements); both stream their tokens into one node vector.
- **Tier 3 (×2.0–2.9):** everything that either indexes a token array (`multipass-reverse`, `direct-mp`, `multipass-arena`, `multipass-bfs`) or pays one `make_unique` per node (`ast-rd`, `ast-sy`, `ast-pratt`). The array-bound family must build the array — their algorithms need random access — and the pointer classics' algorithm barely matters, the allocator dominates. The top-down D&C forms are the only way to build a tree whose sub-ranges are split-independent; their two former Θ(n²) worst cases (mixed-precedence and `^`-tower chains) are capped at O(n log n) by bounded scans, per-precedence position buckets, iterator passing and an AVX2 window in `multipass` / `multipass-arena` (runtime-dispatched).
- **Tier 4 (×4.7):** O(n log n) *plus* N allocations *plus* the token array.

## 📐 Grammar

Numbers (incl. `1e5`, `.5`; overflow → `inf`, underflow → `0`, same as Python/Haskell), single-letter variables `a`–`z`, `+ - * / ^`, unary `+/-`, parentheses.
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
| [`multipass-reverse-fold`](src/multipass_reverse_fold.cpp) | Bottom-up, fused: two accumulators per paren frame, no recursion | arena AST |
| [`direct-recursive-descent`](src/direct_recursive_descent.cpp) | Recursive descent | none — returns `double` |
| [`direct-shunting-yard`](src/direct_shunting_yard.cpp) | Shunting-yard | none — returns `double` |
| [`direct-reverse`](src/multipass_reverse_fold.cpp) | Bottom-up, fused | none — returns `double` |
| [`direct-scannerless`](src/direct_scannerless.cpp) | Recursive descent, lexer fused in (no token stream) — the control for the lexer's cost | none — returns `double` |
| [`direct-mp`](src/multipass_lean.cpp) | D&C | none — returns `double` |
| [`bytecode-vm`](src/bytecode.cpp) | Shunting-yard → [bytecode](https://en.wikipedia.org/wiki/Bytecode) + VM | flat opcode stream |

The pointer-AST strategies produce structurally identical trees; the three left-to-right variants (`ast-rd`, `ast-sy`, `ast-pratt`) cluster tightly — algorithm barely matters, only allocation. The arena layout gives ~2× speedup over pointer nodes.

## 📊 Benchmarks

### One-shot — string → value

ns/leaf, 1 000-leaf expressions, neutral runner (median of three CI runs); `×` relative to the fastest strategy that uses the shared lexer:

| Strategy | ns/leaf | × | allocations / expr |
|---|--:|--:|---|
| [`direct-scannerless`](src/direct_scannerless.cpp) | 37 | 0.75 | ~0 (call stack) — *control* |
| [`direct-shunting-yard`](src/direct_shunting_yard.cpp) | 50 | 1.00 | member vectors, reused |
| [`direct-recursive-descent`](src/direct_recursive_descent.cpp) | 50 | 1.01 | ~0 (call stack) |
| [`direct-reverse`](src/multipass_reverse_fold.cpp) | 50 | 1.01 | member buffers, reused |
| [`bytecode-vm`](src/bytecode.cpp) | 59 | 1.20 | member vectors, reused |
| [`multipass-reverse-fold`](src/multipass_reverse_fold.cpp) | 68 | 1.38 | **one** (node vector); buffers reused |
| [`ast-arena`](src/arena_ast.cpp) | 72 | 1.45 | **one** (node vector) |
| [`multipass-reverse`](src/multipass_reverse.cpp) | 98 | 1.97 | one (node vector) + token array; item stack reused |
| [`direct-mp`](src/multipass_lean.cpp) | 101 | 2.04 | token array + pre-scan vectors (no AST) |
| [`multipass-arena`](src/multipass_arena.cpp) | 130 | 2.62 | one (node vector) + token array + pre-scan |
| [`ast-recursive-descent`](src/recursive_descent.cpp) | 130 | 2.62 | **one per node** |
| [`ast-shunting-yard`](src/shunting_yard.cpp) | 135 | 2.71 | **one per node** |
| [`ast-pratt`](src/pratt.cpp) | 136 | 2.74 | **one per node** |
| [`multipass-bfs`](src/multipass_opt.cpp) | 142 | 2.85 | one + token array + sparse table + pre-index |
| [`multipass`](src/multipass.cpp) | 232 | 4.69 | one per node + token array + pre-scan |

### Re-eval — compile once, evaluate many

Variables `a`–`d`, 1 000-leaf expressions:

| Strategy | compile ns/expr | per-eval ns/expr | per-eval × |
|---|--:|--:|--:|
| `rpn` | **~42k** | **18k** | 1.0 |
| `bytecode` | **~42k** | **18k** | 1.0 |
| `ast-arena` | ~44k | 26k | 1.4 |
| `multipass-arena` | ~126k | 26k | 1.4 |
| `ast-ptr` | ~106k | 27k | 1.5 |
| `reparse-rd` | ~0 | **134k** | 7.4 |

Neutral runner, median of three CI runs. Flat forms (`rpn`, `bytecode`) win per-eval. The three streaming compilers (`rpn`, `bytecode`, `ast-arena`) compile in one pass at the same cost; `multipass-arena` compiles at ~×2.9 that but is the only compiled form that supports incremental re-parsing and parallel evaluation.

### 🏁 Verdict

> - **Once, fastest?** `direct-rd` / `direct-sy` / `direct-reverse` — a three-way tie (ordering inside this group flips run-to-run), `bytecode-vm` ~20 % behind. Fusing the lexer into the grammar (`direct-scannerless`) buys another 25 %, at the price of having no lexer to share.
> - **Many times?** `rpn` / `bytecode` — allocation-free eval loop.
> - **Need a tree?** `ast-arena` — one allocation, never per-node `unique_ptr`.
> - **Parallel or incremental re-parse?** `multipass-arena` — the only strategy where sub-ranges are independent.

## 🧵 Parallelism

Multipass finds the split point *first*, making both halves independent [fork-join](https://en.wikipedia.org/wiki/Fork%E2%80%93join_model) tasks. Every other strategy is a left-to-right stream and cannot parallelize within a single expression.

**Batch scaling** (shared corpus split across W worker threads, each with its own evaluator; ns/leaf, neutral 4-vCPU runner, median of three CI runs — `scaling_bench`):

| Strategy | W=1 | W=2 | W=4 | speedup@4 |
|---|--:|--:|--:|--:|
| `direct-recursive-descent` | 50 | 25 | 18 | ×2.8 |
| `direct-reverse` | 50 | 25 | 19 | ×2.7 |
| `ast-arena` | 70 | 35 | 25 | ×2.9 |
| `multipass-reverse-fold` | 70 | 35 | 24 | ×2.9 |
| `multipass-arena` | 134 | 66 | 47 | ×2.9 |

Every strategy scales the same (~2.7–2.9× on 4 vCPUs); the ranking is core-count-invariant, so adding threads never rescues a slower strategy.

**Single-expression fork-join** — three generations, all in the build
(`single_par_bench`, `pool_bench`, `floor_bench`):

| variant | file | mechanism | vs `par1` (100k leaves, 4-core laptop) |
|---|---|---|--:|
| `par2/4/8` | [`multipass_parallel.cpp`](src/multipass_parallel.cpp) | middle-split + `std::async` per fork | ≤ ×1.2, often *slower* than `par1` (a fresh OS thread per ~1 µs subtree) |
| `pool2/4/8` | [`multipass_pool.cpp`](src/multipass_pool.cpp) | persistent help-while-waiting pool; node stored at its owner-token index (no shared atomic) | ~×1.4–1.5 |
| `dfork2/4/8` | [`multipass_pool.cpp`](src/multipass_pool.cpp) | same, but evaluation fused into the parallel recursion (no tree) | ~×1.5–1.7 |

Fork threshold ~128 tokens (`MP_FORK_THRESHOLD` overrides). Ordering
dfork > pool > par is robust run-to-run; none of them beats single-threaded
`ast-arena` on this 4-physical-core machine (controlled, interleaved runs put
`dfork8` at ~1.3× arena's time at 100k leaves). The un-parallelisable prologue
(tokenize + candidate index, `mp-setup-only`) is ~0.65–0.8× arena's whole
time, so there is structural headroom on wider hardware — untested, not
claimed. All ten variants run the full spec suite and the differential fuzz
(`parallel_evaluators()`), including malformed inputs long enough to fork;
exceptions raised inside a pool task are joined and rethrown on the caller.

## 🛠️ Build & run

From the repo root (`-S cpp`); drop the `cpp/` prefix if you're already in this directory.

```sh
cmake -S cpp -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14
cmake --build build -j

ctest --test-dir build --output-on-failure   # 926 checks + 6300-input differential fuzz (25 strategies)
./build/corpus_bench                          # one-shot, shared corpus (15 strategies; cross-language comparable)
./build/corpus_reeval                         # compile-once / eval-many, shared corpus
./build/scaling_bench                         # batch-throughput scaling at 1/2/4 threads
./build/single_par_bench                      # single-expression fork-join scaling (par*)
./build/pool_bench                            # A/B: par* vs pool* vs dfork*
./build/floor_bench                           # serial-floor probe (tokenize + candidate index only)
./build/adversarial_bench                     # 4 structured shapes: top-down worst cases (now O(n log n)), nestchain (bottom-up's), vs reverse Θ(n)
```

Requires GCC 14 + CMake ≥ 3.20. No external dependencies. The corpus, scaling
and adversarial harnesses read `bench/corpus/` — run `python3 bench/gen_corpus.py` first.

## 🗂️ Layout

```
include/parser/   interfaces (evaluator, arena_ast, reeval, …)
src/              one file per strategy + shared lexer/ast
bench/            corpus_bench.cpp  corpus_reeval.cpp  scaling_bench.cpp  adversarial_bench.cpp  (shared corpus, what CI reports)
                  single_par_bench.cpp  pool_bench.cpp  floor_bench.cpp  (single-expression parallelism probes)
tests/            test_parsers.cpp (926 checks incl. the 10 parallel variants) + fuzz_differential.cpp (6300-input cross-strategy fuzz), run via CTest
```

> One-shot, re-eval and batch-scaling numbers above are from the neutral CI runner. The single-expression fork-join table is from a throttling i7-10610U — **absolute ns there vary ±40 %; the ordering is the result**.
