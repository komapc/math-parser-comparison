# Python implementation

Idiomatic Python port of all fifteen strategies. Run from the repo root.

```sh
python3 python/test_parsers.py     # correctness (540 checks)
python3 python/test_fuzz.py        # differential fuzz: 15 strategies must agree on 6000 inputs
python3 bench/gen_corpus.py        # generate shared corpora (once)
python3 python/bench.py            # cross-check + timing on shared corpora
python3 python/adversarial.py      # 4 structured shapes: top-down worst cases (now O(n log n)), nestchain, vs reverse Θ(n)
```

Requires Python 3.10+.

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
| ast-recursive-descent | 3 368 | 3 203 | 3 179 | 3 796 |
| ast-shunting-yard | 3 014 | 3 042 | 3 232 | 3 790 |
| ast-pratt | 3 262 | 3 200 | 3 171 | 3 845 |
| ast-arena | 3 722 | 3 541 | 3 596 | 4 001 |
| multipass | 4 629 | 4 787 | 5 518 | 6 873 |
| multipass-arena | 4 804 | 4 943 | 5 967 | 6 980 |
| direct-mp | 4 397 | 4 560 | 5 321 | 6 206 |
| multipass-bfs | 5 367 | 5 650 | 7 348 | 9 349 |
| multipass-reverse | 4 333 | 4 227 | 4 311 | 4 799 |
| multipass-reverse-fold | 2 897 | 2 880 | 3 174 | 3 599 |
| direct-recursive-descent | 3 157 | 2 990 | 2 924 | 2 985 |
| direct-shunting-yard | 2 814 | 2 834 | 3 008 | 3 092 |
| direct-reverse | **2 519** | **2 497** | **2 704** | **2 779** |
| bytecode-vm | 2 902 | 2 948 | 3 135 | 3 346 |
| *direct-scannerless* (control) | *2 339* | *2 191* | *2 191* | *2 184* |

`multipass-reverse` is the only buffered multipass variant that stays flat as n
grows (the others recurse and rescan per split). Its fused form
`multipass-reverse-fold` is the fastest tree builder at n=10, 100 and 10000
and a three-way tie with `ast-recursive-descent` and `ast-pratt` at n=1000
(−0.1…+0.5 % across runs), and
`direct-reverse` is the fastest strategy overall at every size: recursive
descent pays a Python call per grammar level per leaf, the fold pays none.
Median of three CI runs on one CPU model, AMD EPYC 9V74 (36204789075 /
36204787013 / 36204784693, 2026-09-26).
`direct-scannerless` (recursive descent with the lexer fused in — no `Token`
objects at all) is included as a control, not ranked: ~25 % less time than
`direct-rd`, and that gap is what the shared token list costs here (a
generator-based token stream was measured too and is a wash, 0.93–1.05×).
Correctness: a capped subset (500 per size) of the shared corpus agrees
across all 15 strategies — see `bench.py`'s `correctness()`.

## What changes versus C++

- **The arena trick disappears.** In C++ a flat node vector beats per-node
  allocation ~2×. In Python every node is a boxed object regardless, so
  `ast-arena` is no faster — in fact slightly *slower* than the pointer-AST builders
  (3 596 vs 3 171–3 232 @ n=1000) — the win was about memory *layout*, which Python
  doesn't expose.
- **"No allocation" still leads, but by less than it looks.** `direct-*` and
  `bytecode-vm` (which never build a tree) are the fastest tier, roughly
  1–16 % ahead of the pointer-AST builders at n=1000 — when every operation is
  already boxed, skipping the tree saves some, not most. What clearly loses
  is the **top-down multipass family** (~1.9–2.5×): the repeated split-scans
  are real extra work no runtime hides. Bottom-up `multipass-reverse`
  (~1.7×) escapes most of that by never scanning for a split.

See the top-level [README](../README.md) for the cross-language table and the
[one-pager](../docs/one-pager.md) for the cross-language verdict and
scoreboard.
