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
| ast-recursive-descent | 2 389 | 2 313 | 2 286 | 2 765 |
| ast-shunting-yard | 2 191 | 2 224 | 2 393 | 2 798 |
| ast-pratt | 2 290 | 2 264 | 2 264 | 2 758 |
| ast-arena | 2 654 | 2 542 | 2 602 | 2 944 |
| multipass | 3 608 | 3 717 | 4 292 | 5 375 |
| multipass-arena | 3 745 | 3 851 | 4 618 | 5 443 |
| direct-mp | 3 442 | 3 547 | 4 133 | 4 835 |
| multipass-bfs | 4 179 | 4 406 | 5 680 | 7 350 |
| multipass-reverse | 3 353 | 3 279 | 3 402 | 3 784 |
| multipass-reverse-fold | 2 242 | 2 233 | 2 470 | 2 796 |
| direct-recursive-descent | 2 209 | 2 129 | 2 099 | **2 129** |
| direct-shunting-yard | 2 026 | 2 055 | 2 207 | 2 266 |
| direct-reverse | **1 943** | **1 929** | **2 089** | 2 148 |
| bytecode-vm | 2 105 | 2 144 | 2 304 | 2 463 |
| *direct-scannerless* (control) | *1 807* | *1 700* | *1 691* | *1 672* |

`multipass-reverse` is the only buffered multipass variant that stays flat as n
grows (the others recurse and rescan per split). Its fused form
`multipass-reverse-fold` is no longer the fastest tree builder at any size:
after the [same-rules tuning pass](../bench/TUNING.md) hand-inlined the
classics (cached token kind, helpers folded into the caller, a precedence
table; +5…+10 % on the corpus), it trails `ast-shunting-yard` by 0–3 % at
n=10 and 100 and `ast-pratt` by 8–10 % at n=1000 and 1–2 % at n=10000.
The same treatment applied to the fold measured no gain; only a `^ leaf`
fast path (powchain +1…+5 %) was kept. `direct-reverse` is the fastest
strategy at n=10, 100 and 1000 — clearly at n=10 and 100 (+3…+6 % over
`direct-sy`, +9…+12 % over `direct-rd`), by 0.2–0.6 % at n=1000 — and 1 %
behind `direct-rd` at
n=10000. Median of three CI runs on one CPU model, AMD EPYC 9V74
(43186179671 / 43186221304 / 43201629859, 2026-11-12).
`direct-scannerless` (recursive descent with the lexer fused in — no `Token`
objects at all) is included as a control, not ranked: ~18–21 % less time than
`direct-rd`, and that gap is what the shared token list costs here (a
generator-based token stream was measured too and is a wash, 0.93–1.05×).
Correctness: a capped subset (500 per size) of the shared corpus agrees
across all 15 strategies — see `bench.py`'s `correctness()`.

## What changes versus C++

- **The arena trick disappears.** In C++ a flat node vector beats per-node
  allocation ~2×. In Python every node is a boxed object regardless, so
  `ast-arena` is no faster — in fact slightly *slower* than the pointer-AST builders
  (2 602 vs 2 264–2 393 @ n=1000) — the win was about memory *layout*, which Python
  doesn't expose.
- **"No allocation" still leads, but by less than it looks.** The `direct-*`
  forms (which never build a tree) are the fastest tier, 3–8 % ahead of the
  best pointer-AST builder at n=1000, and `bytecode-vm` sits level with it —
  when every operation is already boxed, skipping the tree saves some, not
  most. What clearly loses is the **top-down multipass family**
  (~1.8–2.5×): the repeated split-scans are real extra work no runtime
  hides. Bottom-up `multipass-reverse` (~1.5×) escapes most of that by
  never scanning for a split.

See the top-level [README](../README.md) for the cross-language table and the
[one-pager](../docs/one-pager.md) for the cross-language verdict and
scoreboard.
