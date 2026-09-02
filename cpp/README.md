<div align="center">

# 🧮 Math-Expression Parser & Evaluator — A Comparison

**Fifteen ways to turn `"-2 ^ 2 + 3 * (4 - 1)"` into `5` — benchmarked head-to-head.**

![C++26](https://img.shields.io/badge/C%2B%2B-26-00599C?logo=cplusplus&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-3.20%2B-064F8C?logo=cmake&logoColor=white)
![tests](https://img.shields.io/badge/tests-480%20checks%20%2B%20fuzz-brightgreen)
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

- **Control (×0.75):** `direct-scannerless` is `direct-rd` with the lexer fused into the grammar — no token stream at all. It exists to measure the shared lexer's cost (a quarter of `direct-rd`'s time), not to compete.
- **Tier 1 (×1.0–1.2):** no tree, O(n), streaming tokens, ≤1 allocation. `direct-sy`, `direct-rd` and `direct-reverse` are a **three-way tie** at ~50 ns/leaf (two of three runs within ±1.5 %); `bytecode-vm` sits ~20 % behind.
- **Tier 2 (×1.4):** the two contiguous tree builders. `multipass-reverse-fold` (bottom-up, fused — see [docs/multipass-reverse.md](../docs/multipass-reverse.md)) is the **fastest tree builder**, ~3–5 % ahead of `ast-arena` (positive in 11 of 12 size×run measurements); both stream their tokens into one node vector.
- **Tier 3 (×2.0–2.9):** everything that either indexes a token array (`multipass-reverse`, `direct-mp`, `multipass-arena`, `multipass-bfs`) or pays one `make_unique` per node (`ast-rd`, `ast-sy`, `ast-pratt`). For the pointer classics the algorithm barely matters, the allocator dominates. The top-down D&C forms' two former Θ(n²) worst cases (mixed-precedence and `^`-tower chains) are capped at O(n log n) by bounded scans, per-precedence position buckets, iterator passing and an AVX2 window (runtime-dispatched).
- **Tier 4 (×4.7):** O(n log n) *plus* N allocations *plus* the token array.

## 📐 Grammar

Numbers (incl. `1e5`, `.5`; overflow → `inf`, underflow → `0`, same as Python/Haskell), single-letter variables `a`–`z`, `+ - * / ^`, unary `+/-`, parentheses.
Precedence: `+ -` < `* /` < unary < `^` (right-associative). So `-2^2 = -4`, `2^3^2 = 512`.

## 🧩 Strategies

| Strategy | Approach | Representation | allocations / expr |
|---|---|---|---|
| [`ast-recursive-descent`](src/recursive_descent.cpp) | [Recursive descent](https://en.wikipedia.org/wiki/Recursive_descent_parser) | pointer AST | **one per node** |
| [`ast-shunting-yard`](src/shunting_yard.cpp) | [Shunting-yard](https://en.wikipedia.org/wiki/Shunting_yard_algorithm) | pointer AST | **one per node** |
| [`ast-pratt`](src/pratt.cpp) | [Pratt / precedence climbing](https://en.wikipedia.org/wiki/Operator-precedence_parser) | pointer AST | **one per node** |
| [`ast-arena`](src/arena_ast.cpp) | Recursive descent | arena AST | **one** (node vector) |
| [`multipass`](src/multipass.cpp) | D&C [Cartesian tree](https://en.wikipedia.org/wiki/Cartesian_tree) | pointer AST | one per node + token array + pre-scan |
| [`multipass-arena`](src/multipass_arena.cpp) | D&C + iterator passing | arena AST | one + token array + pre-scan |
| [`multipass-bfs`](src/multipass_opt.cpp) | D&C + [sparse-table RMQ](https://en.wikipedia.org/wiki/Range_minimum_query) | arena AST | one + token array + sparse table |
| [`multipass-reverse`](src/multipass_reverse.cpp) | Bottom-up reduction (innermost/highest first) | arena AST | one + token array; item stack reused |
| [`multipass-reverse-fold`](src/multipass_reverse_fold.cpp) | Bottom-up, fused: two accumulators per paren frame, no recursion | arena AST | **one** (node vector); buffers reused |
| [`direct-recursive-descent`](src/direct_recursive_descent.cpp) | Recursive descent | none — returns `double` | ~0 (call stack) |
| [`direct-shunting-yard`](src/direct_shunting_yard.cpp) | Shunting-yard | none — returns `double` | member vectors, reused |
| [`direct-reverse`](src/multipass_reverse_fold.cpp) | Bottom-up, fused | none — returns `double` | member buffers, reused |
| [`direct-scannerless`](src/direct_scannerless.cpp) | Recursive descent, lexer fused in — the control for the lexer's cost | none — returns `double` | ~0 (call stack) |
| [`direct-mp`](src/multipass_lean.cpp) | D&C | none — returns `double` | token array + pre-scan vectors |
| [`bytecode-vm`](src/bytecode.cpp) | Shunting-yard → [bytecode](https://en.wikipedia.org/wiki/Bytecode) + VM | flat opcode stream | member vectors, reused |

The pointer-AST strategies produce structurally identical trees; the three left-to-right variants (`ast-rd`, `ast-sy`, `ast-pratt`) cluster tightly — algorithm barely matters, only allocation. The arena layout gives ~2× speedup over pointer nodes.

## 🏁 Verdict

> - **Fastest?** `direct-rd` / `direct-sy` / `direct-reverse` — a three-way tie (ordering inside this group flips run-to-run), `bytecode-vm` ~20 % behind. Fusing the lexer into the grammar (`direct-scannerless`) buys another 25 %, at the price of having no lexer to share.
> - **Need a tree?** `multipass-reverse-fold` or `ast-arena` — one allocation, never per-node `unique_ptr`; the fused reducer is ~3–5 % ahead and has no worst case.
> - **Structured input?** Bottom-up only. Every top-down D&C form needed rescue machinery to stay O(n log n) on mixed-precedence chains and still trails `multipass-reverse-fold` by 2–5× there ([FINDINGS](../FINDINGS.md#result-2--vs-its-family-strictly-better)).

## 🛠️ Build & run

From the repo root (`-S cpp`); drop the `cpp/` prefix if you're already in this directory.

```sh
cmake -S cpp -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14
cmake --build build -j

ctest --test-dir build --output-on-failure   # 480 checks + 6300-input differential fuzz (15 strategies)
./build/corpus_bench                          # one-shot, shared corpus (cross-language comparable)
./build/adversarial_bench                     # 4 structured shapes: top-down worst cases (now O(n log n)), nestchain (bottom-up's), vs reverse Θ(n)
```

Requires GCC 14 + CMake ≥ 3.20. No external dependencies. Both harnesses
read `bench/corpus/` — run `python3 bench/gen_corpus.py` first.

## 🗂️ Layout

```
include/parser/   interfaces (evaluator, ast, arena_ast, lexer, token, parser)
src/              one file per strategy + shared lexer/ast
bench/            corpus_bench.cpp  adversarial_bench.cpp  (shared corpus, what CI reports)
tests/            test_parsers.cpp (480 checks) + fuzz_differential.cpp (6300-input cross-strategy fuzz), run via CTest
```

> All numbers above are from the neutral CI runner (median of three runs).
