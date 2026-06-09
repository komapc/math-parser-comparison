# Python implementation

Idiomatic Python port of all twelve strategies. Run from the repo root.

```sh
python3 python/test_parsers.py     # correctness (252 checks)
python3 bench/gen_corpus.py        # generate shared corpora (once)
python3 python/bench.py            # cross-check + timing on shared corpora
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

ns/leaf on the shared corpora, one (warm) run on a throttling i7-10610U —
**noisy; trust the tiers, not the digits.** The [CI bench](../.github/workflows/bench.yml)
regenerates these on a neutral runner. Reproduce locally with `python3 python/bench.py`.

| strategy | n=10 | n=100 | n=1000 | n=10000 |
|---|--:|--:|--:|--:|
| ast-recursive-descent | 18634 | 18819 | 20830 | 24190 |
| ast-shunting-yard | 15025 | 16132 | 19761 | 23721 |
| ast-pratt | 18962 | 18944 | 19403 | 22491 |
| ast-arena | 24399 | 23480 | 18732 | 18293 |
| multipass | 21644 | 17469 | 29078 | 38608 |
| multipass-arena | 23900 | 24964 | 30422 | 45898 |
| direct-mp | 22057 | 30149 | 38008 | 56232 |
| multipass-bfs | 33000 | 34530 | 41229 | 56249 |
| multipass-reverse | 31954 | 31869 | 32858 | **28765** |
| **direct-recursive-descent** | **12967** | **12720** | **12983** | 14031 |
| direct-shunting-yard | 12179 | 17198 | 16956 | 17767 |
| **bytecode-vm** | 12378 | **11728** | 14244 | 16901 |

`multipass-reverse` is the only multipass variant that stays flat as n grows
(others recurse + bisect per call) — at n=10000 it's the fastest of the mp
family. It still doesn't beat the no-tree winners. Correctness: 1110/1110 corpus
expressions agree across all 12 strategies.

## What changes versus C++

- **The arena trick disappears.** In C++ a flat node vector beats per-node
  allocation ~2×. In Python every node is a boxed object regardless, so
  `ast-arena` ≈ `ast-recursive-descent` — the win was about memory *layout*,
  which Python doesn't expose.
- **"No allocation" still wins.** `direct-*` and `bytecode-vm` (which never build
  a tree) remain the fastest tier, ~2-3× over the AST builders — avoiding object
  creation matters even when you can't control layout.

See the top-level [README](../README.md) for the cross-language table.
