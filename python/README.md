# Python implementation

Idiomatic Python port of all fourteen strategies. Run from the repo root.

```sh
python3 python/test_parsers.py     # correctness (360 checks)
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
| ast-recursive-descent | 3381 | 3267 | 3216 | 3907 |
| ast-shunting-yard | 3059 | 3082 | 3204 | 3760 |
| ast-pratt | 3393 | 3350 | 3343 | 3933 |
| ast-arena | 3728 | 3568 | 3680 | 4059 |
| multipass | 5626 | 6040 | 7314 | 9175 |
| multipass-arena | 5908 | 6299 | 7804 | 9416 |
| direct-mp | 5342 | 5752 | 7007 | 8608 |
| multipass-bfs | 6661 | 6937 | 8825 | 11585 |
| multipass-reverse | 4493 | 4183 | 4374 | 4776 |
| direct-recursive-descent | 3092 | 3002 | **2965** | **3011** |
| direct-shunting-yard | **2807** | **2853** | 3004 | 3076 |
| bytecode-vm | 2872 | 2914 | 3076 | 3364 |

`multipass-reverse` is the only multipass variant that stays flat as n grows
(others recurse + rescan per split) — it's the fastest of the mp family at every
size, ~1.8× ahead of the next at n=10000, and within ~1.6× of the no-tree
winners (which it still doesn't beat). Correctness: 1110/1110 corpus expressions
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
