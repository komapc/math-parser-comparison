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
| ast-recursive-descent | 2 410 | 2 353 | 2 307 | 2 878 |
| ast-shunting-yard | 2 214 | 2 241 | 2 392 | 2 834 |
| ast-pratt | 2 319 | 2 296 | 2 257 | 2 782 |
| ast-arena | 2 672 | 2 590 | 2 656 | 3 020 |
| multipass | 3 636 | 3 731 | 4 306 | 5 480 |
| multipass-arena | 3 757 | 3 896 | 4 643 | 5 526 |
| direct-mp | 3 466 | 3 581 | 4 139 | 4 898 |
| multipass-bfs | 4 215 | 4 426 | 5 720 | 7 584 |
| multipass-reverse | 3 396 | 3 332 | 3 408 | 3 866 |
| multipass-reverse-fold | 2 086 | 2 090 | 2 256 | 2 732 |
| direct-recursive-descent | 2 236 | 2 161 | 2 116 | 2 158 |
| direct-shunting-yard | 2 039 | 2 076 | 2 222 | 2 281 |
| direct-reverse | **1 905** | **1 916** | **2 059** | **2 117** |
| bytecode-vm | 2 124 | 2 166 | 2 313 | 2 511 |
| *direct-scannerless* (control) | *1 789* | *1 686* | *1 690* | *1 698* |

`multipass-reverse` is the only buffered multipass variant that stays flat as n
grows (the others recurse and rescan per split). Its fused form
`multipass-reverse-fold` builds the same nested-tuple tree as the classics
here (an arena of tuples is only slower in CPython; see below). After the
[same-rules tuning pass](../bench/TUNING.md) hand-inlined the classics
(cached token kind, helpers folded into the caller, a precedence table;
+5…+10 % on the corpus), the fold had fallen 8–10 % behind `ast-pratt` at
n=1000; a second pass aimed only at the fold switched it from
`ArenaBuilder` to `TupleBuilder` (+5…+9 %) and skipped a Python call per
barrier when there is no segment to fold (+1…+3 %). It is now the fastest
tree builder at n=10 and 100 (5–8 % ahead of `ast-shunting-yard`) and
level with `ast-pratt` at n=1000 and 10000 (−0.3…+2.6 %). `direct-reverse`
is the fastest strategy at every size: +13…+17 % over `direct-rd` at n=10
and 100, +1…+3 % at n=1000 and 10000, and 6–9 % over `direct-sy`. Median of
three CI runs on one CPU model, AMD EPYC 9V74 (43227113612 / 43227138258 /
43231342538, 2026-11-12).
`direct-scannerless` (recursive descent with the lexer fused in — no `Token`
objects at all) is included as a control, not ranked: ~20–22 % less time than
`direct-rd`, and that gap is what the shared token list costs here (a
generator-based token stream was measured too and is a wash, 0.93–1.05×).
Correctness: a capped subset (500 per size) of the shared corpus agrees
across all 15 strategies — see `bench.py`'s `correctness()`.

## What changes versus C++

- **The arena trick disappears.** In C++ a flat node vector beats per-node
  allocation ~2×. In Python every node is a boxed object regardless, so
  `ast-arena` is no faster — in fact slightly *slower* than the pointer-AST builders
  (2 656 vs 2 257–2 392 @ n=1000) — the win was about memory *layout*, which Python
  doesn't expose. That is why the fused fold builds tuples here, unlike in
  the other three languages.
- **"No allocation" still leads, but by less than it looks.** The `direct-*`
  forms (which never build a tree) are the fastest tier, up to 9 % ahead of
  the best tree builder at n=1000, and `bytecode-vm` sits level with it —
  when every operation is already boxed, skipping the tree saves some, not
  most. What clearly loses is the **top-down multipass family**
  (~1.8–2.5×): the repeated split-scans are real extra work no runtime
  hides. Bottom-up `multipass-reverse` (~1.5×) escapes most of that by
  never scanning for a split.

See the top-level [README](../README.md) for the cross-language table and the
[one-pager](../docs/one-pager.md) for the cross-language verdict and
scoreboard.
