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
them alike. (One of the three still ran ~1.3× slower in absolute terms on
the same model, which is why only within-run ratios are compared.)
A cell below is labelled
from the *range across those three runs* (lowest%, highest%): **best** if the
low end is ≥5 % ahead; *ahead, narrowly* if the low end is ≥0.5 % ahead but
under 5 %; *loses* if the high end is ≤0.5 % behind; *tie* otherwise — which
covers a sign flip across runs, but also a range that sits entirely within
±0.5 %, ahead or behind, since that is noise on this runner. The range
measures run-to-run spread on *one* CPU model. GitHub hands out several
models, and across them the ratios move by more than that. Three runs of
this same commit on AMD EPYC 7763 flip several cells: there Rust's tree form
*loses* to `ast-arena` on the random corpus (−7…−3 %) and on powchain
(−4…−1 %), and its no-tree form loses to `direct-rd` on the random corpus
(−7…−5 %); C++'s no-tree form turns narrowly ahead on towerchain and
sumchain and trails by less on nestchain (−15…−14 %); C++ sumchain in the
family table becomes *best* (+20…+28 %). The 9V74 is the model this repo
publishes, fixed before these runs were looked at. Treat any cell within ~5 %
of the line as hardware-dependent.

## Verdict

- **Against its own family it is ahead on every tested input** — clearly
  on 23 of the 24 cells, narrowly (+4…+8 %) on C++ sumchain — 1.4× faster
  than the buffered version of itself, which is in turn 1.5–3.5× faster than
  the top-down divide-and-conquer variants on the family's own C++
  worst-case shape (2.0–4.9× for the fused form itself; see the scoreboard
  below for the other three languages), with no worst-case machinery: its
  worst case *is* its average case. (`multipass-arena`'s own worst-case
  shape, sumchain, is a separate story — see the note below the scoreboard.)
- **Against the classic parsers it is a peer in C++, Rust and Python**
  — the fastest tree builder on the C++ random corpus (narrowly, +2…+4 %)
  and on every structured C++ shape; in Rust a tie with the arena classic on
  the random corpus, ahead on three of the four structured shapes (clearly on
  towerchain and nestchain) and a tie on powchain; in Python a tie with
  `ast-pratt` at n=1000, narrowly ahead at n=10000, and the fastest
  evaluator outright there in its no-tree form. Honest losses: C++'s no-tree
  form trails the no-tree classics on nestchain by ~20–22 % and by 2–4 % on
  n=10000 and sumchain; Python's tree form trails recursive descent on
  powchain by ~7–8 % and `ast-pratt` on sumchain by 5–7 %, and its no-tree
  form trails `direct-rd` on powchain by 1–2 %.
- **In Haskell the tree-building form loses** to the pointer-AST classics by
  1.1–1.3× on every input. Its edge elsewhere appears to come from
  contiguous memory, and a runtime that boxes every node leaves little of
  that to exploit. Its no-tree form is mostly behind there too: narrowly
  ahead on powchain, a tie on towerchain, and 1–15 % behind recursive
  descent everywhere else.

## Scoreboard

### Tree-building form `multipass-reverse-fold` vs the best classic tree builder

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | ahead, narrowly (+3…+4 % vs `ast-arena`) | tie (-1…0 % vs `ast-arena`) | tie (0…+1 % vs `ast-pratt`) | loses (1.1× slower than `ast-rd`) |
| random corpus (n=10000) | ahead, narrowly (+2…+4 % vs `ast-arena`) | tie (0 % vs `ast-arena`) | ahead, narrowly (+4…+5 % vs `ast-sy`) | loses (1.2–1.3× slower than `ast-rd`) |
| powchain | **best** (+8…+16 % vs `ast-arena`) | tie (0…+1 % vs `ast-arena`) | loses (-8…-7 % vs `ast-rd`) | loses (1.1–1.2× slower than `ast-rd`) |
| towerchain | **best** (+6…+8 % vs `ast-arena`) | **best** (+9 % vs `ast-arena`) | ahead, narrowly (+1…+2 % vs `ast-rd`) | loses (1.1× slower than `ast-pratt`) |
| sumchain | **best** (+18…+20 % vs `ast-arena`) | ahead, narrowly (+2…+4 % vs `ast-arena`) | loses (-7…-5 % vs `ast-pratt`) | loses (1.2× slower than `ast-rd`) |
| nestchain | **best** (+24…+30 % vs `ast-arena`) | **best** (+27…+29 % vs `ast-arena`) | tie (-1…0 % vs `ast-sy`) | loses (1.2–1.3× slower than `ast-rd`) |

### No-tree form `direct-reverse` vs the best classic no-tree evaluator

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | tie (-1…0 % vs `direct-sy`) | tie (-2…0 % vs `direct-rd`) | **best** (+7…+8 % vs `direct-rd`) | loses (-2…-1 % vs `direct-rd`) |
| random corpus (n=10000) | loses (-3…-2 % vs `direct-sy`) | tie (0 % vs `direct-rd`) | **best** (+7 % vs `direct-rd`) | loses (-8…-2 % vs `direct-rd`) |
| powchain | **best** (+9 % vs `direct-rd`) | ahead, narrowly (+1…+2 % vs `direct-rd`) | loses (-2…-1 % vs `direct-rd`) | ahead, narrowly (+3…+7 % vs `direct-rd`) |
| towerchain | tie (-1…0 % vs `direct-sy`) | **best** (+13…+21 % vs `direct-rd`) | **best** (+14…+15 % vs `direct-rd`) | tie (-2…+5 % vs `direct-rd`) |
| sumchain | loses (-4 % vs `direct-sy`) | **best** (+15…+26 % vs `direct-rd`) | **best** (+9 % vs `direct-rd`) | loses (-7…-1 % vs `direct-rd`) |
| nestchain | loses (-22…-20 % vs `direct-sy`) | **best** (+15…+16 % vs `direct-sy`) | **best** (+8…+10 % vs `direct-sy`) | loses (-15…-12 % vs `direct-rd`) |

### `multipass-reverse-fold` vs the best of its own top-down family

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | **best** (+32…+33 % vs `direct-mp`) | **best** (+59 % vs `direct-mp`) | **best** (+40 % vs `direct-mp`) | **best** (+30…+34 % vs `multipass`) |
| random corpus (n=10000) | **best** (+33…+34 % vs `direct-mp`) | **best** (+65 % vs `direct-mp`) | **best** (+42 % vs `direct-mp`) | **best** (+43…+44 % vs `multipass`) |
| powchain | **best** (+47…+51 % vs `direct-mp`) | **best** (+73…+74 % vs `direct-mp`) | **best** (+21…+22 % vs `direct-mp`) | **best** (+48…+50 % vs `multipass`) |
| towerchain | **best** (+47…+50 % vs `direct-mp`) | **best** (+58 % vs `direct-mp`) | **best** (+13…+15 % vs `direct-mp`) | **best** (+57…+58 % vs `direct-mp`) |
| sumchain | ahead, narrowly (+4…+8 % vs `direct-mp`) | **best** (+38…+40 % vs `direct-mp`) | **best** (+12 % vs `direct-mp`) | **best** (+24…+26 % vs `multipass`) |
| nestchain | **best** (+48…+50 % vs `direct-mp`) | **best** (+20…+21 % vs `direct-mp`) | **best** (+37…+38 % vs `direct-mp`) | **best** (+42…+45 % vs `multipass`) |

Sumchain is where the top-down members come closest to the fused form —
not a rescued-family win: its single precedence level gives the split-scan
machinery nothing to search, so the "budget + buckets" rescue and the plain
buffer end up doing the same trivial pass. The fused form stays ahead of the
whole family there regardless, by ~1.08× over `direct-mp` at the median
(+4…+8 % across runs, so *ahead, narrowly* — the one family cell short of
*best*). Since 2026-09-26
the top-down members no longer pay per-call allocations the fold never paid
(and Python's no longer pays a `key=lambda` per bisect step), which is why
the family gaps are smaller than in earlier batches — most visibly in
Python.

Numbers behind the cells (ns/leaf, medians of the three runs) are in
[FINDINGS.md](../FINDINGS.md) and the per-language READMEs; the
walk-through of the algorithm is in [multipass-reverse.md](multipass-reverse.md).

## What it does *not* claim

- It is not faster than a lexer-free evaluator: `direct-scannerless`, recursive
  descent with the lexer fused into the grammar, beats every strategy in C++,
  Rust and Python by roughly 16–38 % (Python 16 %, C++ 21 %, Rust 38 %),
  and Haskell's by ~9 %. That is the cost of having a shared
  lexer at all and every real strategy in the table pays it; it is the
  control, not a rival.
- The design targets this one four-level grammar. "One sweep per precedence
  level" does not carry over for free to function calls of arbitrary arity,
  mixed associativity, statements, or a dozen-plus precedence levels; for
  those, recursive descent and Pratt parsing remain the general-purpose
  default.
- Margins under ~5 % are reported as ranges and labelled *narrowly* or *tie*
  on purpose: single-run numbers at that scale flip sign on the same runner.

*Data: CI bench runs 36226353574, 36226831278, 36227295521 (2026-09-26,
commit 921ec7f, all on AMD EPYC 9V74 — the first three of 18 dispatched that
landed on it; regenerated with
[`bench/tables.py`](../bench/tables.py)).*
