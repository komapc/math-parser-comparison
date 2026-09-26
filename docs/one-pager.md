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
the same commit in the same runs (rustc 1.98.1).
A cell below is labelled
from the *range across those three runs* (lowest%, highest%): **best** if the
low end is ≥5 % ahead; *ahead, narrowly* if the low end is ≥0.5 % ahead but
under 5 %; *loses* if the high end is ≤0.5 % behind; *tie* otherwise — which
covers a sign flip across runs, but also a range that sits entirely within
±0.5 %, ahead or behind, since that is noise on this runner.

## Verdict

- **Against its own family it wins on every tested input** — 1.4× faster
  than the buffered version of itself, which is in turn 1.6–3.7× faster than
  the top-down divide-and-conquer variants on the family's own C++
  worst-case shape (2.2–5.2× for the fused form itself; see the scoreboard
  below for the other three languages), with no worst-case machinery: its
  worst case *is* its average case. (`multipass-arena`'s own worst-case
  shape, sumchain, is a separate story — see the note below the scoreboard.)
- **Against the classic parsers it is a peer in C++, Rust and Python**
  — the fastest tree builder on the C++ random corpus (narrowly) and on every
  structured C++ shape; in Rust a tie with the arena classic at n=1000 but
  3–5 % behind it at n=10000, ahead on two of Rust's four structured shapes
  (clearly only on nestchain) and a tie on the other two; in Python a tie at
  n=1000 and narrowly ahead at n=10000, and the fastest evaluator outright
  there in its no-tree form. Ties or narrow wins on the rest, with honest
  losses: C++ nestchain to shunting-yard by ~11–30 %, Python powchain to
  recursive descent by ~5–7 %, and Rust's no-tree form to `direct-rd` by
  5–10 % on the random corpus.
- **In Haskell the tree-building form loses** to the pointer-AST classics by
  1.2–1.9× (except a noisy tie at n=1000). Its edge elsewhere appears to come
  from contiguous memory, and a runtime that boxes every node leaves little
  of that to exploit. Its no-tree form is mixed there: it wins powchain and
  towerchain and edges ahead at n=10000, but loses the n=1000 random corpus,
  sumchain and nestchain to recursive descent.

## Scoreboard

### Tree-building form `multipass-reverse-fold` vs the best classic tree builder

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | ahead, narrowly (+1…+3 % vs `ast-arena`) | tie (-4…+2 % vs `ast-arena`) | tie (-2…+1 % vs `ast-rd`) | tie (-31…+5 % vs `ast-rd`) |
| random corpus (n=10000) | ahead, narrowly (+3…+5 % vs `ast-arena`) | loses (-5…-3 % vs `ast-arena`) | ahead, narrowly (+1…+6 % vs `ast-sy`) | loses (1.6–1.9× slower than `ast-rd`) |
| powchain | ahead, narrowly (+2…+16 % vs `ast-arena`) | tie (-16…+3 % vs `ast-arena`) | loses (-7…-5 % vs `ast-rd`) | loses (1.4–1.5× slower than `ast-rd`) |
| towerchain | **best** (+11…+17 % vs `ast-arena`) | ahead, narrowly (+3…+4 % vs `ast-arena`) | tie (-1…+1 % vs `ast-rd`) | loses (1.2–1.4× slower than `ast-pratt`) |
| sumchain | **best** (+11…+19 % vs `ast-arena`) | tie (0…+9 % vs `ast-arena`) | tie (-7…+1 % vs `ast-pratt`) | loses (1.2–1.4× slower than `ast-rd`) |
| nestchain | **best** (+7…+20 % vs `ast-arena`) | **best** (+18…+21 % vs `ast-arena`) | tie (-1…+2 % vs `ast-sy`) | loses (1.4–1.5× slower than `ast-rd`) |

### No-tree form `direct-reverse` vs the best classic no-tree evaluator

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | tie (-7…+1 % vs `direct-sy`) | loses (-9…-5 % vs `direct-rd`) | **best** (+7…+9 % vs `direct-rd`) | loses (-17…-6 % vs `direct-rd`) |
| random corpus (n=10000) | tie (-2…+1 % vs `direct-sy`) | loses (-10…-5 % vs `direct-rd`) | **best** (+7 % vs `direct-rd`) | ahead, narrowly (+2…+4 % vs `direct-rd`) |
| powchain | ahead, narrowly (+4…+7 % vs `direct-sy`) | ahead, narrowly (+1…+6 % vs `direct-rd`) | ahead, narrowly (+2…+3 % vs `direct-rd`) | **best** (+20…+21 % vs `direct-rd`) |
| towerchain | tie (-4…+2 % vs `direct-sy`) | ahead, narrowly (+4…+12 % vs `direct-rd`) | **best** (+17…+19 % vs `direct-rd`) | ahead, narrowly (+3…+7 % vs `direct-rd`) |
| sumchain | tie (-13…+33 % vs `direct-sy` — this shape is volatile on this runner across batches, see [FINDINGS.md](../FINDINGS.md)) | **best** (+12…+13 % vs `direct-rd`) | **best** (+12…+15 % vs `direct-rd`) | loses (-7…-1 % vs `direct-rd`) |
| nestchain | loses (-30…-11 % vs `direct-sy`) | **best** (+17…+20 % vs `direct-rd`) | ahead, narrowly (+4…+5 % vs `direct-sy`) | loses (-20…-18 % vs `direct-rd`) |

### `multipass-reverse-fold` vs the best of its own top-down family

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | **best** (+31…+33 % vs `direct-mp`) | **best** (+58…+59 % vs `direct-mp`) | **best** (+54…+60 % vs `direct-mp`) | **best** (+34…+53 % vs `multipass`) |
| random corpus (n=10000) | **best** (+36…+39 % vs `direct-mp`) | **best** (+64…+65 % vs `direct-mp`) | **best** (+57…+61 % vs `direct-mp`) | **best** (+48…+61 % vs `multipass`) |
| powchain | **best** (+55…+57 % vs `direct-mp`) | **best** (+68…+72 % vs `direct-mp`) | **best** (+48…+49 % vs `direct-mp`) | **best** (+43…+46 % vs `multipass`) |
| towerchain | **best** (+52…+61 % vs `direct-mp`) | **best** (+55…+59 % vs `direct-mp`) | **best** (+33…+37 % vs `direct-mp`) | **best** (+50…+52 % vs `direct-mp`) |
| sumchain | **best** (+9…+37 % vs `direct-mp` — wide because sumchain is noisy on this runner, see the note below) | **best** (+49…+61 % vs `direct-mp`) | **best** (+34…+37 % vs `direct-mp`) | **best** (+17…+21 % vs `multipass`) |
| nestchain | **best** (+50…+61 % vs `direct-mp`) | **best** (+29…+33 % vs `direct-mp`) | **best** (+45…+47 % vs `direct-mp`) | **best** (+46…+56 % vs `multipass`) |

C++ sumchain is the noisiest measurement point on this runner: across two
batches, different strategies spiked on different runs (in this batch the
middle run was slow for nearly everything, the fused form included; see
[FINDINGS.md](../FINDINGS.md)). The top-down members also come closest to
the buffered `multipass-reverse` form here — not a rescued-family win:
sumchain's single precedence level gives the split-scan machinery nothing to
search, so the "budget + buckets" rescue and the plain buffer end up doing
the same trivial pass. The fused form stays ahead of the whole family there
regardless, by ~1.1× over `direct-mp` at the median.

Numbers behind the cells (ns/leaf, medians of the three runs) are in
[FINDINGS.md](../FINDINGS.md) and the per-language READMEs; the
walk-through of the algorithm is in [multipass-reverse.md](multipass-reverse.md).

## What it does *not* claim

- It is not faster than a lexer-free evaluator: `direct-scannerless`, recursive
  descent with the lexer fused into the grammar, beats every strategy in C++,
  Rust and Python by roughly 25–36 % or more (and is level with the fastest
  strategy in Haskell). That is the cost of having a shared
  lexer at all and every real strategy in the table pays it; it is the
  control, not a rival.
- The design targets this one four-level grammar. "One sweep per precedence
  level" does not carry over for free to function calls of arbitrary arity,
  mixed associativity, statements, or a dozen-plus precedence levels; for
  those, recursive descent and Pratt parsing remain the general-purpose
  default.
- Margins under ~5 % are reported as ranges and labelled *narrowly* or *tie*
  on purpose: single-run numbers at that scale flip sign on the same runner.

*Data: CI bench runs 36163486136, 36163570591, 36163577544 (2026-09-25,
commit 3064d0f).*
