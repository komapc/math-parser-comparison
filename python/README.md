# Python implementation

Idiomatic Python port of all twelve strategies. Run from the repo root.

```sh
python3 python/test_parsers.py     # correctness (252 checks)
python3 bench/gen_corpus.py        # generate shared corpora (once)
python3 python/bench.py            # cross-check + timing on shared corpora
python3 python/adversarial.py      # structured chains: multipass top-down Θ(n²) vs reverse Θ(n)
```

Requires Python 3.10+ (uses `bisect(..., key=...)`).

## Design

The twelve strategies are combinations of a **representation** and a **parse order**,
so the shared logic lives in two places instead of being copy-pasted:

- **Builders** (`evaluators.py`) — `TupleBuilder` (pointer-AST analog: nested
  tuples), `ArenaBuilder` (flat list, integer-indexed children), `DirectBuilder`
  (no tree — every node collapses to a float as it is built).
- **Drivers** — `rd_parse` (recursive descent), `sy_parse` (shunting-yard),
  `pratt_parse`, `_MP` (top-down divide & conquer, with an optional sparse-table
  RMQ for the `multipass-bfs` variant), and `reverse_mp_parse` (bottom-up
  reduction, innermost/highest precedence first → `multipass-reverse`).
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
| ast-recursive-descent | 3234 | 3070 | 3021 | 3793 |
| ast-shunting-yard | 2869 | 2881 | 3062 | 3609 |
| ast-pratt | 3244 | 3158 | 3132 | 3865 |
| ast-arena | 3588 | 3378 | 3523 | 4165 |
| multipass | 5540 | 6096 | 7442 | 12696 |
| multipass-arena | 5714 | 6281 | 7973 | 13360 |
| direct-mp | 5290 | 5891 | 7194 | 12138 |
| multipass-bfs | 6336 | 7028 | 9394 | 15906 |
| multipass-reverse | 6460 | 6603 | 6959 | **7397** |
| **direct-recursive-descent** | 2985 | 2804 | **2754** | **2855** |
| direct-shunting-yard | **2654** | **2709** | 2843 | 2974 |
| bytecode-vm | 2728 | 2787 | 2941 | 3208 |

`multipass-reverse` is the only multipass variant that stays flat as n grows
(others recurse + bisect per call) — at n=10000 it's the fastest of the mp
family. It still doesn't beat the no-tree winners. Correctness: 1110/1110 corpus
expressions agree across all 12 strategies.

## What changes versus C++

- **The arena trick disappears.** In C++ a flat node vector beats per-node
  allocation ~2×. In Python every node is a boxed object regardless, so
  `ast-arena` is no faster — in fact slightly *slower* than `ast-recursive-descent`
  (3523 vs 3021 @ n=1000) — the win was about memory *layout*, which Python
  doesn't expose.
- **"No allocation" still leads, but only just.** `direct-*` and `bytecode-vm`
  (which never build a tree) are the fastest tier, but only ~10% ahead of the
  pointer-AST builders — when every operation is already boxed, skipping the tree
  saves little. What clearly loses is the **multipass family** (~2.4–3×): the
  repeated split-scans are real extra work no runtime hides.

See the top-level [README](../README.md) for the cross-language table.
