# Bottom-up multipass parsing — the one-pager

**Claim tested:** a parser that reduces an expression *bottom-up* — deepest
parentheses first, then `^`/unary, then `* /`, then `+ -`, one sweep per
precedence level, no split search — is a competitive way to evaluate
`-2 ^ 2 + 3 * (4 - 1)`, not just a curiosity.

**How it was tested.** The same algorithm was implemented in C++, Rust, Python
and Haskell next to fourteen other strategies (recursive descent, shunting-yard,
Pratt, arena AST, bytecode VM, four top-down divide-and-conquer variants and a
lexer-free control), all sharing one lexer, one grammar, one spec suite and
differential fuzzing. Every strategy ran on a shared random corpus at 10, 100,
1 000 and 10 000 leaves and on four adversarial shapes (mixed-precedence
chains, `^`-towers, a single-precedence chain, deep nesting), on a neutral
4-vCPU GitHub runner, **three independent times**, all four languages from
the same commit in the same runs. All three runs landed on one CPU model
(AMD EPYC 9V74, read from each run's `env.txt`), toolchains pinned (GCC
14.2, rustc 1.98.1, GHC 9.6.7, the image's Python 3.12), and within each run
the strategies are timed round-robin, so drift on the runner falls on all of
them alike. (In an earlier batch one run ran ~1.3× slower in absolute terms
on the same model, which is why only within-run ratios are compared.)
A cell below is labelled
from the *range across those three runs* (lowest%, highest%): **best** if the
low end is ≥5 % ahead; *ahead, narrowly* if the low end is ≥0.5 % ahead but
under 5 %; *loses* if the high end is ≤0.5 % behind; *tie* otherwise — which
covers a sign flip across runs, but also a range that sits entirely within
±0.5 %, ahead or behind, since that is noise on this runner. For the
tree and no-tree scoreboards, whose code did not change, the label is taken
over the three runs here *and* the six runs of the two earlier batches on
the same model (2026-09-26 and 2026-11-10, nine runs in all): seven labels
moved between the 2026-09-26 batch and this one without a code change
(the runner image and kernel had changed), so a margin that changes sign
between batches is a tie too. Cells marked † carry a label that this batch's
three runs alone would not give; the range shown is still this batch's. The
range measures run-to-run spread on *one* CPU model. GitHub hands out several
models, and across them the ratios move by more than that. Three runs of
this same commit on AMD EPYC 7763 change 13 of the 72 labels. Against the
fused form (9): Rust's tree form *loses* to `ast-arena` on the random corpus
at both sizes (−5…−3 %) and on powchain (−5…−2 %); C++'s tree form only
ties on sumchain (−1…+23 %) and is only narrowly ahead on nestchain
(+3…+20 %); Rust's no-tree form loses to `direct-rd` on the random corpus at
both sizes (−10…−6 %) and on powchain (−5…−3 %); Python's no-tree form loses
powchain (−3…−2 %). For it (4): Haskell's no-tree form ties on the random
corpus at n=1000 and on sumchain and is narrowly ahead on towerchain; C++'s
ties at n=10000. The tables here use EPYC 9V74 because the
previously published batch ran on it, and they keep that model for
continuity rather than mixing models; the 7763 results are reported here
instead of being averaged in. Treat any cell within ~7 % of the line as
environment-dependent.

## Verdict

- **Against its own family it is ahead on every tested input** — *best* on
  all 24 cells, though only just on C++ sumchain (+5…+39 %, see the note
  below the scoreboard) — 1.3–1.4× faster than the buffered version of
  itself, which is in turn 1.5–3.6× faster than
  the top-down divide-and-conquer variants on the family's own C++
  worst-case shape (2.0–4.9× for the fused form itself; see the scoreboard
  below for the other three languages), with no worst-case machinery: its
  worst case *is* its average case. (`multipass-arena`'s own worst-case
  shape, sumchain, is a separate story — see the note below the scoreboard.)
- **Against the classic parsers it is a peer in C++, Rust and Python**
  — the fastest tree builder on the C++ random corpus (narrowly, +3…+4 %)
  and on every structured C++ shape; in Rust a tie with the arena classic on
  the random corpus, clearly ahead on towerchain and nestchain and a tie on
  powchain and sumchain; in Python a tie with `ast-pratt` at n=1000,
  narrowly ahead at n=10000, and the fastest evaluator outright there in its
  no-tree form. Honest losses: C++'s no-tree form trails
  `direct-shunting-yard` on nestchain by 15–24 % and by 1–2 % at n=10000;
  Python's tree form trails recursive descent on powchain by 6–9 % and
  `ast-pratt` on sumchain by 6–7 %.
- **In Haskell the tree-building form loses** to the pointer-AST classics by
  1.0–1.3× on every input. Its edge elsewhere appears to come from
  contiguous memory, and a runtime that boxes every node leaves little of
  that to exploit. Its no-tree form is mostly behind there too: narrowly
  ahead on powchain, a tie on towerchain, and 1–17 % behind recursive
  descent everywhere else.

## Scoreboard

### Tree-building form `multipass-reverse-fold` vs the best classic tree builder

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | ahead, narrowly (+3…+4 % vs `ast-arena`) | tie (0…+2 % vs `ast-arena`) | tie (-1…0 % vs `ast-pratt`) | loses (1.1× slower than `ast-rd`) |
| random corpus (n=10000) | ahead, narrowly (+3…+4 % vs `ast-arena`) | tie (0…+1 % vs `ast-arena`) | ahead, narrowly (+2…+5 % vs `ast-rd`) | loses (1.2–1.3× slower than `ast-rd`) |
| powchain | **best** (+10…+15 % vs `ast-arena`) | tie † (+1 % vs `ast-arena`) | loses (-9…-6 % vs `ast-rd`) | loses (1.1–1.2× slower than `ast-rd`) |
| towerchain | **best** (+11…+15 % vs `ast-arena`) | **best** (+5…+12 % vs `ast-arena`) | tie (0…+1 % vs `ast-rd`) | loses (1.0–1.1× slower than `ast-pratt`) |
| sumchain | **best** (+18…+19 % vs `ast-arena`) | tie † (+4…+5 % vs `ast-arena`) | loses (-7…-6 % vs `ast-pratt`) | loses (1.1–1.2× slower than `ast-rd`) |
| nestchain | **best** (+24…+30 % vs `ast-arena`) | **best** (+30…+33 % vs `ast-arena`) | tie † (-2 % vs `ast-sy`) | loses (1.2× slower than `ast-rd`) |

### No-tree form `direct-reverse` vs the best classic no-tree evaluator

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | tie (0 % vs `direct-sy`) | tie (0…+1 % vs `direct-rd`) | **best** (+7…+8 % vs `direct-rd`) | loses (-3 % vs `direct-rd`) |
| random corpus (n=10000) | loses (-2…-1 % vs `direct-sy`) | tie (0…+1 % vs `direct-rd`) | **best** (+6…+7 % vs `direct-rd`) | loses (-3…-2 % vs `direct-rd`) |
| powchain | ahead, narrowly † (+8 % vs `direct-sy`) | ahead, narrowly (+2 % vs `direct-rd`) | tie (-2…0 % vs `direct-rd`) | ahead, narrowly (+3…+5 % vs `direct-rd`) |
| towerchain | tie † (+5…+7 % vs `direct-sy`) | **best** (+11…+16 % vs `direct-rd`) | **best** (+15…+17 % vs `direct-rd`) | tie † (+2…+3 % vs `direct-rd`) |
| sumchain | tie † (+3 % vs `direct-sy`) | **best** (+15 % vs `direct-rd`) | **best** (+9…+10 % vs `direct-rd`) | loses (-6…-1 % vs `direct-rd`) |
| nestchain | loses (-24…-15 % vs `direct-sy`) | **best** (+16 % vs `direct-sy`) | **best** (+9 % vs `direct-sy`) | loses (-17…-12 % vs `direct-rd`) |

† Label over all nine runs of the three 9V74 batches. This batch's three
runs alone would say: tree form — Rust powchain and sumchain *ahead,
narrowly*, Python nestchain *loses*; no-tree form — C++ powchain and
towerchain *best*, C++ sumchain and Haskell towerchain *ahead, narrowly*.

### `multipass-reverse-fold` vs the best of its own top-down family

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | **best** (+32…+33 % vs `direct-mp`) | **best** (+58 % vs `direct-mp`) | **best** (+40 % vs `direct-mp`) | **best** (+30…+31 % vs `multipass`) |
| random corpus (n=10000) | **best** (+33…+34 % vs `direct-mp`) | **best** (+64…+65 % vs `direct-mp`) | **best** (+41…+43 % vs `direct-mp`) | **best** (+39…+44 % vs `multipass`) |
| powchain | **best** (+50…+51 % vs `direct-mp`) | **best** (+73 % vs `direct-mp`) | **best** (+20…+22 % vs `direct-mp`) | **best** (+48…+51 % vs `multipass`) |
| towerchain | **best** (+50…+51 % vs `direct-mp`) | **best** (+56…+58 % vs `direct-mp`) | **best** (+13…+14 % vs `direct-mp`) | **best** (+59…+60 % vs `multipass`) |
| sumchain | **best** (+5…+39 % vs `direct-mp`) | **best** (+42…+44 % vs `direct-mp`) | **best** (+11…+12 % vs `direct-mp`) | **best** (+25…+27 % vs `multipass`) |
| nestchain | **best** (+48…+52 % vs `direct-mp`) | **best** (+24…+26 % vs `direct-mp`) | **best** (+37…+38 % vs `direct-mp`) | **best** (+44…+46 % vs `multipass`) |

Sumchain is where the top-down members come closest to the fused form —
not a rescued-family win: its single precedence level gives the split-scan
machinery nothing to search, so the "budget + buckets" rescue and the plain
buffer end up doing the same trivial pass. The fused form stays ahead of the
whole family there regardless: +5…+39 % over `direct-mp`. That range is
wide because C++ `direct-mp` on sumchain is bimodal — ~30 or ~46 ns/leaf,
switching between sizes and runs with no pattern, in this batch and in
every earlier one — so its low end is `direct-mp`'s fast mode. Since
2026-09-26 the top-down members no longer pay per-call allocations the fold
never paid (and Python's no longer pays a `key=lambda` per bisect step); as
of this batch no arena or no-tree strategy allocates anything per
evaluation in C++ or Rust (the last right-fold buffers, token arrays and
scratch stacks went in [#20](https://github.com/komapc/math-parser-comparison/pull/20)),
and the family gaps barely moved.

Numbers behind the cells (ns/leaf, medians of the three runs) are in
[FINDINGS.md](../FINDINGS.md) and the per-language READMEs; the
walk-through of the algorithm is in [multipass-reverse.md](multipass-reverse.md).

## What it does *not* claim

- It is not faster than a lexer-free evaluator: `direct-scannerless`, recursive
  descent with the lexer fused into the grammar, beats every strategy in C++,
  Rust and Python by roughly 17–38 % (Python 17 %, C++ 20 %, Rust 38 %),
  and Haskell's by ~6 %. That is the cost of having a shared
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

*Data: CI bench runs 42805297064, 42806389267, 42806442518 (2026-11-10,
commit 7b83ad4, all on AMD EPYC 9V74 — the first three of 12 dispatched that
landed on it); tree and no-tree labels also over the earlier 9V74 batches
36226353574, 36226831278, 36227295521 (2026-09-26, 921ec7f) and
42791921436, 42791964111, 42793955904 (2026-11-10, d2474e2). All archived in
[`bench/results/`](../bench/results/) and regenerated with
[`bench/tables.py`](../bench/tables.py), command in its README.*
