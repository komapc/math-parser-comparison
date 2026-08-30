# Python implementation

Idiomatic Python port of all fourteen strategies. Run from the repo root.

```sh
python3 python/test_parsers.py     # correctness (448 checks)
python3 python/test_fuzz.py        # differential fuzz: 14 strategies must agree on 6000 inputs
python3 bench/gen_corpus.py        # generate shared corpora (once)
python3 python/bench.py            # cross-check + timing on shared corpora
python3 python/adversarial.py      # 4 structured shapes: top-down worst cases (now O(n log n)), nestchain, vs reverse Θ(n)
```

Requires Python 3.10+ (uses `bisect(..., key=...)`).

## Design

The fourteen strategies are combinations of a **representation** and a **parse order**,
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
| ast-recursive-descent | 3303 | 3108 | 3078 | 3836 |
| ast-shunting-yard | 2936 | 2946 | 3095 | 3673 |
| ast-pratt | 3294 | 3180 | 3163 | 3871 |
| ast-arena | 3622 | 3430 | 3549 | 3967 |
| multipass | 5541 | 6022 | 7344 | 9448 |
| multipass-arena | 5712 | 6191 | 7830 | 9699 |
| direct-mp | 5351 | 5822 | 7130 | 8821 |
| multipass-bfs | 6322 | 6933 | 9188 | 12188 |
| multipass-reverse | 4116 | 4020 | 4194 | 4704 |
| multipass-reverse-fold | 2737 | 2702 | 2978 | 3396 |
| direct-recursive-descent | 2991 | 2796 | 2737 | 2809 |
| direct-shunting-yard | 2636 | 2662 | 2814 | 2901 |
| direct-reverse | **2371** | **2350** | **2526** | **2611** |
| bytecode-vm | 2753 | 2790 | 2945 | 3192 |

`multipass-reverse` is the only buffered multipass variant that stays flat as n
grows (others recurse + rescan per split) — the fastest of the top-down family
at every size. Its fused form `multipass-reverse-fold` is the fastest tree
builder at every size and `direct-reverse` the fastest strategy overall:
recursive descent pays a Python call per grammar level per leaf, the fold pays
none. Correctness: 1110/1110 corpus expressions
agree across all 14 strategies.

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
