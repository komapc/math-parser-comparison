# Python implementation

Idiomatic Python port of all fifteen strategies. Run from the repo root.

```sh
python3 python/test_parsers.py     # correctness (540 checks)
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

Most strategies are a driver feeding a builder, e.g. `multipass-arena` = `_MP`
+ `ArenaBuilder`. Two are not: `bytecode-vm` is a separate compile-then-run
pass (`compile_bytecode` then `run_bytecode`, no builder), and
`scannerless_eval` (→ `direct-scannerless`) fuses the lexer into a
character-level recursive-descent evaluator with no token list, tree, or
separate builder at all — every grammar rule collapses straight to a float
as it matches.

Arithmetic is kept IEEE-faithful to the C++ `double` semantics (`_div`/`_pow`
return `nan`/`inf` instead of raising), so results agree across languages.

## Benchmark (measured)

ns/leaf on the shared corpora, neutral GitHub runner (4 vCPU) via the
[CI bench](../.github/workflows/bench.yml) — **trust the tiers, not the digits.**
Reproduce locally with `python3 python/bench.py`.

| strategy | n=10 | n=100 | n=1000 | n=10000 |
|---|--:|--:|--:|--:|
| ast-recursive-descent | 3464 | 3288 | 3244 | 3882 |
| ast-shunting-yard | 3179 | 3158 | 3343 | 3803 |
| ast-pratt | 3490 | 3391 | 3371 | 4004 |
| ast-arena | 3817 | 3624 | 3771 | 4131 |
| multipass | 5880 | 6159 | 7395 | 9248 |
| multipass-arena | 6158 | 6358 | 7848 | 9516 |
| direct-mp | 5714 | 5987 | 7211 | 8671 |
| multipass-bfs | 6949 | 7058 | 9143 | 11844 |
| multipass-reverse | 4647 | 4370 | 4505 | 4828 |
| multipass-reverse-fold | 3047 | 2988 | 3232 | 3566 |
| direct-recursive-descent | 3194 | 3021 | 2990 | 3033 |
| direct-shunting-yard | 2883 | 2922 | 3067 | 3145 |
| direct-reverse | **2640** | **2587** | **2732** | **2828** |
| bytecode-vm | 3000 | 3019 | 3165 | 3427 |
| *direct-scannerless* (control) | *2370* | *2264* | *2291* | *2275* |

`multipass-reverse` is the only buffered multipass variant that stays flat as n
grows (the others recurse and rescan per split). Its fused form
`multipass-reverse-fold` is the fastest tree builder at every size (a clear
lead at n=10/100/10000, a tie with `ast-recursive-descent` at n=1000) and
`direct-reverse` the fastest strategy overall: recursive descent pays a Python
call per grammar level per leaf, the fold pays none. Median of three CI runs.
`direct-scannerless` (recursive descent with the lexer fused in — no `Token`
objects at all) is a control, not a contender: ~23–26 % faster than
`direct-rd`, and that gap is what the shared token list costs here (a
generator-based token stream was measured too and is a wash, 0.93–1.05×).
Correctness: a capped subset (500 per size) of the shared corpus agrees
across all 15 strategies — see `bench.py`'s `correctness()`.

## What changes versus C++

- **The arena trick disappears.** In C++ a flat node vector beats per-node
  allocation ~2×. In Python every node is a boxed object regardless, so
  `ast-arena` is no faster — in fact slightly *slower* than the pointer-AST builders
  (3771 vs 3244–3371 @ n=1000) — the win was about memory *layout*, which Python
  doesn't expose.
- **"No allocation" still leads, but by less than it looks.** `direct-*` and
  `bytecode-vm` (which never build a tree) are the fastest tier, roughly
  16–26 % ahead of the pointer-AST builders — when every operation is
  already boxed, skipping the tree saves some, not most. What clearly loses
  is the **top-down multipass family** (~2.5–3.0×): the repeated split-scans
  are real extra work no runtime hides. Bottom-up `multipass-reverse`
  (~1.7×) escapes most of that by never scanning for a split.

See the top-level [README](../README.md) for the cross-language table and the
[one-pager](../docs/one-pager.md) for the cross-language verdict and
scoreboard.
