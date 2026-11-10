# Python implementation

Idiomatic Python port of all fifteen strategies. Run from the repo root.

```sh
python3 python/test_parsers.py     # correctness (615 checks)
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
| ast-recursive-descent | 2 632 | 2 487 | 2 478 | 2 969 |
| ast-shunting-yard | 2 341 | 2 362 | 2 521 | 2 969 |
| ast-pratt | 2 524 | 2 478 | 2 465 | 3 016 |
| ast-arena | 2 885 | 2 743 | 2 801 | 3 188 |
| multipass | 3 595 | 3 706 | 4 293 | 5 358 |
| multipass-arena | 3 722 | 3 845 | 4 634 | 5 463 |
| direct-mp | 3 416 | 3 530 | 4 118 | 4 862 |
| multipass-bfs | 4 185 | 4 389 | 5 690 | 7 387 |
| multipass-reverse | 3 374 | 3 287 | 3 371 | 3 826 |
| multipass-reverse-fold | 2 252 | 2 248 | 2 480 | 2 836 |
| direct-recursive-descent | 2 446 | 2 320 | 2 269 | 2 329 |
| direct-shunting-yard | 2 168 | 2 193 | 2 338 | 2 403 |
| direct-reverse | **1 952** | **1 949** | **2 098** | **2 164** |
| bytecode-vm | 2 262 | 2 292 | 2 434 | 2 610 |
| *direct-scannerless* (control) | *1 790* | *1 686* | *1 684* | *1 665* |

`multipass-reverse` is the only buffered multipass variant that stays flat as n
grows (the others recurse and rescan per split). Its fused form
`multipass-reverse-fold` is the fastest tree builder at n=10, 100 and 10000
and a tie at n=1000 (−0.7…+0.4 % vs `ast-pratt` and −0.1…+0.9 % vs
`ast-recursive-descent` across runs), and
`direct-reverse` is the fastest strategy overall at every size: recursive
descent pays a Python call per grammar level per leaf, the fold pays none.
Median of three CI runs on one CPU model, AMD EPYC 9V74 (42805297064 /
42806389267 / 42806442518, 2026-11-10).
`direct-scannerless` (recursive descent with the lexer fused in — no `Token`
objects at all) is included as a control, not ranked: ~25–29 % less time than
`direct-rd`, and that gap is what the shared token list costs here (a
generator-based token stream was measured too and is a wash, 0.93–1.05×).
Correctness: a capped subset (500 per size) of the shared corpus agrees
across all 15 strategies — see `bench.py`'s `correctness()`.

## What changes versus C++

- **The arena trick disappears.** In C++ a flat node vector beats per-node
  allocation ~2×. In Python every node is a boxed object regardless, so
  `ast-arena` is no faster — in fact slightly *slower* than the pointer-AST builders
  (2 801 vs 2 465–2 521 @ n=1000) — the win was about memory *layout*, which Python
  doesn't expose.
- **"No allocation" still leads, but by less than it looks.** `direct-*` and
  `bytecode-vm` (which never build a tree) are the fastest tier, roughly
  1–20 % ahead of the pointer-AST builders at n=1000 — when every operation is
  already boxed, skipping the tree saves some, not most. What clearly loses
  is the **top-down multipass family** (~2.0–2.7×): the repeated split-scans
  are real extra work no runtime hides. Bottom-up `multipass-reverse`
  (~1.6×) escapes most of that by never scanning for a split.

See the top-level [README](../README.md) for the cross-language table and the
[one-pager](../docs/one-pager.md) for the cross-language verdict and
scoreboard.
