# Python implementation

Idiomatic Python port of all twelve strategies. Run from the repo root.

```sh
python3 python/test_parsers.py     # correctness (252 checks)
python3 bench/gen_corpus.py        # generate shared corpora (once)
python3 python/bench.py            # cross-check + timing on shared corpora
python3 python/adversarial.py      # structured chains: top-down multipass worst cases (now O(n log n)) vs reverse Θ(n)
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
  reduction, innermost/highest precedence first → `multipass-reverse`; algorithm
  explained in [docs/multipass-reverse.md](../docs/multipass-reverse.md)).
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
| ast-recursive-descent | 2511 | 2394 | 2356 | 2966 |
| ast-shunting-yard | 2226 | 2233 | 2413 | 2857 |
| ast-pratt | 2528 | 2455 | 2453 | 3042 |
| ast-arena | 2839 | 2673 | 2730 | 3182 |
| multipass | 4054 | 4448 | 5413 | 7001 |
| multipass-arena | 4192 | 4585 | 5796 | 7249 |
| direct-mp | 3862 | 4258 | 5252 | 6523 |
| multipass-bfs | 4660 | 5143 | 6819 | 9138 |
| multipass-reverse | 3193 | 3146 | 3318 | **3711** |
| **direct-recursive-descent** | 2298 | 2150 | 2122 | **2179** |
| direct-shunting-yard | **1997** | **2038** | **2184** | 2294 |
| bytecode-vm | 2103 | 2136 | 2256 | 2527 |

`multipass-reverse` is the only multipass variant that stays flat as n grows
(others recurse + rescan per split) — it's the fastest of the mp family at every
size, ~2.0× ahead of the next at n=10000, and within ~1.5× of the no-tree
winners (which it still doesn't beat). Correctness: 1110/1110 corpus expressions
agree across all 12 strategies.

## What changes versus C++

- **The arena trick disappears.** In C++ a flat node vector beats per-node
  allocation ~2×. In Python every node is a boxed object regardless, so
  `ast-arena` is no faster — in fact slightly *slower* than `ast-recursive-descent`
  (2730 vs 2356 @ n=1000) — the win was about memory *layout*, which Python
  doesn't expose.
- **"No allocation" still leads, but only just.** `direct-*` and `bytecode-vm`
  (which never build a tree) are the fastest tier, but only ~10% ahead of the
  pointer-AST builders — when every operation is already boxed, skipping the tree
  saves little. What clearly loses is the **top-down multipass family** (~2.4–3.1×):
  the repeated split-scans are real extra work no runtime hides. Bottom-up
  `multipass-reverse` (~1.5×) escapes most of that by never scanning for a split.

See the top-level [README](../README.md) for the cross-language table.
