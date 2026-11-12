# Bottom-up multipass parsing — the one-pager

**Claim tested:** a parser that reduces an expression *bottom-up* — deepest
parentheses first, then `^`/unary, then `* /`, then `+ -`, one sweep per
precedence level, no split search — is a competitive way to evaluate
`-2 ^ 2 + 3 * (4 - 1)`, not just a curiosity.

## Verdict

- **It is correct.** It agrees with the other fourteen strategies, value or
  rejection, on every spec case and on 6 000 fuzzed inputs per run, in all
  four languages.
- **Against its own top-down family it is ahead on every tested input** — all
  24 cells of the family scoreboard below (6 inputs × 4 languages) are
  *best*. It needs no worst-case machinery: its worst case *is* its average
  case. On the family's own C++ worst-case shape the top-down variants trail
  the fused form by 1.9–4.7× (and the buffered version of it, itself
  1.3–1.4× slower than the fused form, by 1.4–3.5×).
- **Against the classic parsers it is a peer in C++, Rust and Python** —
  not a winner — **and behind in Haskell.** These numbers are after a
  [same-rules tuning pass](../bench/TUNING.md) that offered every
  implementation the same hand optimisations (the rivals gained 5–10 % in
  Python and Rust, ours almost nothing), and after a second pass aimed only
  at the fold, which kept two Python changes and nothing in Rust or Haskell.
  - C++: the fastest tree builder on the random corpus (narrowly, +3…+5 %)
    and on every structured shape; its no-tree form ties
    `direct-shunting-yard` on the corpus and trails it on nestchain by
    16–21 %.
  - Rust: the tree form is level with the arena classic on the random
    corpus and powchain (−1…+2 %; 4–5 % behind on the EPYC 7763), narrowly
    ahead on towerchain and sumchain and clearly ahead on nestchain
    (+29…+34 %). The no-tree form loses to the tuned `direct-rd` on the
    random corpus by 5–7 %, is narrowly ahead on powchain and clearly ahead
    on the other three shapes.
  - Python: the tree form ties `ast-pratt` on the random corpus at both
    sizes and on nestchain (0…+2 %) and loses the other three shapes by
    1–13 %. In Python it now builds the same nested-tuple tree as the
    classics (an arena of tuples bought nothing in CPython; see
    [TUNING.md](../bench/TUNING.md)); in the other three languages it is
    an arena. The no-tree form is narrowly ahead of `direct-rd` on the
    random corpus (+1…+3 %) and on sumchain, clearly ahead on towerchain
    and nestchain, and loses powchain by 3–5 %.
- **In Haskell both forms lose.** The tree form is 1.1–1.3× slower than the
  pointer-AST classics on every input. Its edge elsewhere appears to come
  from contiguous memory, and a runtime that boxes every node leaves little
  of that to exploit. The no-tree form ties recursive descent on powchain
  and sumchain and is 1–12 % behind it everywhere else.

## Scoreboard

### Tree-building form `multipass-reverse-fold` vs the best classic tree builder

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | ahead, narrowly (+3…+5 % vs `ast-arena`) | tie (-1…+1 % vs `ast-arena`) | tie (0…+1 % vs `ast-pratt`) | loses (1.1–1.2× slower than `ast-rd`) |
| random corpus (n=10000) | ahead, narrowly (+3…+4 % vs `ast-arena`) | tie (0 % vs `ast-arena`) | tie (0…+2 % vs `ast-pratt`) | loses (1.3× slower than `ast-rd`) |
| powchain | **best** (+9…+13 % vs `ast-arena`) | tie (-1…+2 % vs `ast-arena`) | loses (-13…-10 % vs `ast-pratt`) | loses (1.2× slower than `ast-rd`) |
| towerchain | ahead, narrowly (+3…+14 % vs `ast-arena`) | ahead, narrowly † (+6…+7 % vs `ast-arena`) | loses (-5…-1 % vs `ast-pratt`) | loses (1.1× slower than `ast-pratt`) |
| sumchain | **best** (+17…+18 % vs `ast-arena`) | ahead, narrowly (+2 % vs `ast-arena`) | loses (-5…-4 % vs `ast-pratt`) | loses (1.2–1.3× slower than `ast-rd`) |
| nestchain | **best** (+23…+32 % vs `ast-arena`) | **best** (+29…+34 % vs `ast-arena`) | tie (0…+2 % vs `ast-pratt`) | loses (1.2–1.3× slower than `ast-pratt`) |

### No-tree form `direct-reverse` vs the best classic no-tree evaluator

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | tie (0 % vs `direct-sy`) | loses (-7…-6 % vs `direct-rd`) | ahead, narrowly (+2…+3 % vs `direct-rd`) | loses (-7…-6 % vs `direct-rd`) |
| random corpus (n=10000) | tie † (-1 % vs `direct-sy`) | loses (-6…-5 % vs `direct-rd`) | ahead, narrowly (+1…+2 % vs `direct-rd`) | loses (-12…-10 % vs `direct-rd`) |
| powchain | ahead, narrowly † (+8…+10 % vs `direct-sy`) | ahead, narrowly (+2 % vs `direct-rd`) | loses (-5…-3 % vs `direct-rd`) | tie (-10…+1 % vs `direct-rd`) |
| towerchain | tie † (+5…+7 % vs `direct-sy`) | **best** (+12…+17 % vs `direct-rd`) | **best** (+6 % vs `direct-rd`) | loses (-4…-1 % vs `direct-rd`) |
| sumchain | tie † (+3…+4 % vs `direct-sy`) | **best** (+14 % vs `direct-rd`) | ahead, narrowly (+1…+3 % vs `direct-rd`) | tie † (-5…-1 % vs `direct-rd`) |
| nestchain | loses (-21…-16 % vs `direct-sy`) | **best** (+7 % vs `direct-rd`) | **best** (+10…+12 % vs `direct-sy`) | loses (-12…-9 % vs `direct-rd`) |

† Label over every earlier 9V74 batch whose code on both sides of the
comparison is unchanged (see "Earlier batches" below). This batch's three
runs alone would say: C++ no-tree *loses* at n=10000 (−1 %), *best* on
powchain and towerchain and *ahead, narrowly* on sumchain; Rust tree
*best* on towerchain; Haskell no-tree *loses* sumchain. Two of these six
go against the fused form.

### `multipass-reverse-fold` vs the best of its own top-down family

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | **best** (+32…+33 % vs `direct-mp`) | **best** (+57…+58 % vs `direct-mp`) | **best** (+45…+46 % vs `direct-mp`) | **best** (+36…+38 % vs `multipass`) |
| random corpus (n=10000) | **best** (+32…+34 % vs `direct-mp`) | **best** (+64 % vs `direct-mp`) | **best** (+44 % vs `direct-mp`) | **best** (+44…+46 % vs `multipass`) |
| powchain | **best** (+48…+50 % vs `direct-mp`) | **best** (+73 % vs `direct-mp`) | **best** (+23…+24 % vs `direct-mp`) | **best** (+48…+49 % vs `multipass`) |
| towerchain | **best** (+47…+50 % vs `direct-mp`) | **best** (+57…+58 % vs `direct-mp`) | **best** (+14…+21 % vs `direct-mp`) | **best** (+59…+60 % vs `direct-mp`) |
| sumchain | **best** (+7…+25 % vs the faster of `direct-mp` and `multipass-arena`) | **best** (+47…+49 % vs `direct-mp`) | **best** (+18…+19 % vs `direct-mp`) | **best** (+20…+27 % vs `multipass`) |
| nestchain | **best** (+47…+49 % vs `direct-mp`) | **best** (+24…+28 % vs `direct-mp`) | **best** (+40…+42 % vs `direct-mp`) | **best** (+44…+46 % vs `multipass`) |

Sumchain is where the top-down members come closest to the fused form —
not a rescued-family win: its single precedence level gives the split-scan
machinery nothing to search, so the "budget + buckets" rescue and the plain
buffer end up doing the same trivial pass. C++ `direct-mp` on sumchain is
bimodal — ~30 or ~46 ns/leaf, switching between sizes and runs with no
pattern — so that cell shows the per-run margin over the faster of
`direct-mp` and `multipass-arena`: +7…+25 % in this batch (in a 2026-09
batch, before the top-down members lost their per-call allocations, it
came within +4 %). Since 2026-09-26 the top-down members no longer pay
per-call allocations the fold never paid (and Python's no longer pays a
`key=lambda` per bisect step); no arena or no-tree strategy allocates
anything per evaluation in C++ or Rust (the last right-fold buffers, token
arrays and scratch stacks went in
[#20](https://github.com/komapc/math-parser-comparison/pull/20)), and the
family gaps barely moved.

Numbers behind the cells (ns/leaf, medians of the three runs) are in
[FINDINGS.md](../FINDINGS.md) and the per-language READMEs; the
walk-through of the algorithm is in [multipass-reverse.md](multipass-reverse.md).

## How it was tested

**Setup.** The same algorithm was implemented in C++, Rust, Python and
Haskell next to fourteen other strategies (recursive descent, shunting-yard,
Pratt, arena AST, bytecode VM, four top-down divide-and-conquer variants and
a lexer-free control), all sharing one lexer, one grammar, one spec suite
and differential fuzzing. Every strategy ran on a shared random corpus at
10, 100, 1 000 and 10 000 leaves and on four adversarial shapes
(mixed-precedence chains, `^`-towers, a single-precedence chain, deep
nesting), on a neutral 4-vCPU GitHub runner, **three independent times**,
all four languages from the same commit in the same runs. All three runs
landed on one CPU model (AMD EPYC 9V74, read from each run's `env.txt`),
toolchains pinned (GCC 14.2, rustc 1.98.1, GHC 9.6.7, the image's Python
3.12). Within each run the strategies are timed round-robin, so drift on
the runner falls on all of them alike. In an earlier batch one run ran
~1.3× slower in absolute terms on the same model, which is why only
within-run ratios are compared.

**Labels.** A cell is labelled from the *range across the three runs*
(lowest%, highest%):
- **best** if the low end is ≥5 % ahead;
- *ahead, narrowly* if the low end is ≥0.5 % ahead but under 5 %;
- *loses* if the high end is ≤0.5 % behind;
- *tie* otherwise — which covers a sign flip across runs, but also a range
  that sits entirely within ±0.5 %, ahead or behind, since that is noise on
  this runner.

**Earlier batches (†).** A column is labelled over this batch plus every
earlier 9V74 batch whose code on both sides of the comparison has not
changed since (`POOL_DEPTH` in `tables.py`): the C++ tree and no-tree
columns and the Haskell family column over all five batches (fifteen
runs: 2026-09-26, two on 2026-11-10, 2026-11-12 and this one); the C++
family over four (its top-down members changed after 2026-09-26); the
Rust family over three; the Rust and Haskell tree and no-tree columns
over two (their classics changed in the 2026-11 tuning pass); every
Python column over this batch alone (the fold changed in this one). Eight
labels moved between the 2026-09-26 batch and the 2026-11-10 one without a
code change (the runner image and kernel had changed in between), so a
margin that clears a label boundary in only some batches does not get
that label. Cells marked † carry a label that this batch's three runs
alone would not give; the range shown is still this batch's.

**Other CPU models.** The range measures run-to-run spread on *one* CPU
model. GitHub hands out several models, and across them the ratios move by
more than that. Of the same commit, three runs landed on AMD EPYC 7763
(the rest on four other models, none three times).

On the **EPYC 7763**, 12 of the 72 labels change.
- Against the fused form (7):
  - Rust's tree form *loses* to `ast-arena` on the random corpus at both
    sizes (−5…−4 %) and only ties on towerchain (−3…+10 %);
  - Rust's no-tree form loses powchain to `direct-rd` (−6 %) and is only
    narrowly ahead on nestchain (+3 %);
  - Haskell's no-tree form loses powchain (−10…−3 %);
  - Python's no-tree form is only narrowly ahead on towerchain (+4…+5 %).
- For it (5):
  - Python's tree form is narrowly ahead at n=10000 (+1…+4 %) and ties on
    towerchain;
  - C++'s tree form is *best* on towerchain (+10…+20 %);
  - C++'s no-tree form is narrowly ahead on towerchain (+2…+6 %) and
    *best* on sumchain (+8…+10 %).

The tables here use EPYC 9V74 because the previously published batches ran
on it, and they keep that model for continuity rather than mixing models;
the other models' results are reported here instead of being averaged in.
Treat any cell within ~7 % of the line as environment-dependent.

## What it does *not* claim

- It is not faster than a lexer-free evaluator: `direct-scannerless`, recursive
  descent with the lexer fused into the grammar, beats every strategy in C++,
  Rust and Python by roughly 16–35 % (Python 16 %, C++ 20 %, Rust 35 %),
  and Haskell's by ~9 %. That is the cost of having a shared
  lexer at all and every real strategy in the table pays it; it is the
  control, not a rival.
- The design targets this one four-level grammar. "One sweep per precedence
  level" does not carry over for free to function calls of arbitrary arity,
  mixed associativity, statements, or a dozen-plus precedence levels; for
  those, recursive descent and Pratt parsing remain the general-purpose
  default.
- Margins under ~5 % are reported as ranges and labelled *narrowly* or *tie*
  on purpose: single-run numbers at that scale flip sign on the same runner,
  and batches run weeks apart moved single cells by up to 7 points.

*Data: CI bench runs 43227113612, 43227138258, 43231342538 (2026-11-12,
branch `opt/py-fold-guard` after the fold-only pass, all on AMD EPYC 9V74,
the first three of 12 dispatched that landed on it); pooled per column, as
above, with the earlier 9V74 batches 43186179671, 43186221304, 43201629859
(2026-11-12, a7f2a16), 42805297064, 42806389267, 42806442518 (2026-11-10,
7b83ad4), 42791921436, 42791964111, 42793955904 (2026-11-10, d2474e2) and
36226353574, 36226831278, 36227295521 (2026-09-26, 921ec7f). EPYC 7763:
43227145326, 43227152139, 43231354248. All archived in
[`bench/results/`](../bench/results/) and regenerated with
[`bench/tables.py`](../bench/tables.py), command in its README.*
