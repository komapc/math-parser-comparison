# Python implementation

Idiomatic Python port of all fifteen strategies. Run from the repo root.

```sh
python3 python/test_parsers.py     # correctness (480 checks)
python3 python/test_fuzz.py        # differential fuzz: 15 strategies must agree on 6000 inputs
python3 bench/gen_corpus.py        # generate shared corpora (once)
python3 python/bench.py            # cross-check + timing on shared corpora
python3 python/adversarial.py      # 4 structured shapes: top-down worst cases (now O(n log n)), nestchain, vs reverse Θ(n)
```

Requires Python 3.10+ (uses `bisect(..., key=...)`).

## Design

The fifteen strategies are combinations of a **representation** and a **parse order**,
so the shared logic lives in two places instead of being copy-pasted:

- **Builders** (`evaluators.py`) — `TupleBuilder` (pointer-AST analog: nested
  tuples), `ArenaBuilder` (flat list, integer-indexed children), `DirectBuilder`
  (no tree — every node collapses to a float as it is built).
- **Drivers** — `rd_parse` (recursive descent), `sy_parse` (shunting-yard),
  `pratt_parse`, `_MP` (top-down divide & conquer, with an optional sparse-table
  RMQ for the `multipass-bfs` variant), and `reverse_mp_parse` (bottom-up
  reduction, innermost/highest precedence first → `multipass-reverse`; algorithm
  explained in [docs/multipass-reverse.md](../docs/multipass-reverse.md)) and
  `reverse_fold_parse` (the fused form: same order, two accumulators per paren
  frame, no recursion → `multipass-reverse-fold` / `direct-reverse`).
  `bytecode-vm` is a separate compile-then-run pass.

A strategy is a driver feeding a builder, e.g. `multipass-arena` = `_MP` + `ArenaBuilder`.

Arithmetic is kept IEEE-faithful to the C++ `double` semantics (`_div`/`_pow`
return `nan`/`inf` instead of raising), so results agree across languages.

## Benchmark (measured)

ns/leaf on the shared corpora, neutral GitHub runner (4 vCPU) via the
[CI bench](../.github/workflows/bench.yml) — **trust the tiers, not the digits.**
Reproduce locally with `python3 python/bench.py`.

| strategy | n=10 | n=100 | n=1000 | n=10000 |
|---|--:|--:|--:|--:|
| ast-recursive-descent | 3328 | 3166 | 3150 | 3848 |
| ast-shunting-yard | 3024 | 3011 | 3206 | 3726 |
| ast-pratt | 3367 | 3286 | 3288 | 3963 |
| ast-arena | 3666 | 3494 | 3712 | 4103 |
| multipass | 5892 | 6169 | 7373 | 9429 |
| multipass-arena | 6120 | 6427 | 7840 | 9925 |
| direct-mp | 5605 | 6011 | 7258 | 8959 |
| multipass-bfs | 6812 | 7046 | 9143 | 12413 |
| multipass-reverse | 4502 | 4251 | 4424 | 4884 |
| multipass-reverse-fold | 2840 | 2836 | 3125 | 3535 |
| direct-recursive-descent | 3103 | 2963 | 2907 | 2962 |
| direct-shunting-yard | 2772 | 2777 | 2968 | 3053 |
| direct-reverse | **2500** | **2451** | **2645** | **2720** |
| bytecode-vm | 2843 | 2868 | 3063 | 3338 |
| *direct-scannerless* (control) | *2264* | *2140* | *2171* | *2171* |

`multipass-reverse` is the only buffered multipass variant that stays flat as n
grows (the others recurse and rescan per split). Its fused form
`multipass-reverse-fold` is the fastest tree builder at every size and
`direct-reverse` the fastest strategy overall: recursive descent pays a Python
call per grammar level per leaf, the fold pays none. Median of three CI runs.
`direct-scannerless` (recursive descent with the lexer fused in — no `Token`
objects at all) is a control, not a contender: ~25–35 % faster than
`direct-rd`, and that gap is what the shared token list costs here (a
generator-based token stream was measured too and is a wash, 0.93–1.05×).
Correctness: 1110/1110 corpus expressions agree across all 15 strategies.

## What changes versus C++

- **The arena trick disappears.** In C++ a flat node vector beats per-node
  allocation ~2×. In Python every node is a boxed object regardless, so
  `ast-arena` is no faster — in fact slightly *slower* than the pointer-AST builders
  (3680 vs 3204–3343 @ n=1000) — the win was about memory *layout*, which Python
  doesn't expose.
- **"No allocation" still leads, but only just.** `direct-*` and `bytecode-vm`
  (which never build a tree) are the fastest tier, but only ~10% ahead of the
  pointer-AST builders — when every operation is already boxed, skipping the tree
  saves little. What clearly loses is the **top-down multipass family** (~2.2–2.7×):
  the repeated split-scans are real extra work no runtime hides. Bottom-up
  `multipass-reverse` (~1.5×) escapes most of that by never scanning for a split.

See the top-level [README](../README.md) for the cross-language table.
