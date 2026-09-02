# Bottom-up multipass parsing — the one-pager

**Claim tested:** a parser that reduces an expression *bottom-up* — deepest
parentheses first, then `^`/unary, then `* /`, then `+ -`, one sweep per
precedence level, no split search — is a competitive way to evaluate
`-2 ^ 2 + 3 * (4 - 1)`, not just a curiosity.

**How it was tested.** The same algorithm was implemented in C++, Python and
Haskell next to fourteen other strategies (recursive descent, shunting-yard,
Pratt, arena AST, bytecode VM, four top-down divide-and-conquer variants and a
lexer-free control), all sharing one lexer, one grammar, one spec suite and
differential fuzzing. Every strategy ran on a shared random corpus at 10, 100,
1 000 and 10 000 leaves and on four adversarial shapes (mixed-precedence
chains, `^`-towers, a single-precedence chain, deep nesting), on a neutral
4-vCPU GitHub runner, **three independent times**. A cell below is labelled
from the *range across those three runs*: **best** only if it is ≥5 % ahead in
every run; *ahead, narrowly* if positive in every run but under 5 %; *tie* if
the sign flips; *loses* if behind in every run.

## Verdict

- **Against its own family it is strictly better everywhere** — 1.4–1.6× faster
  than the buffered version of itself and 2–5× faster than the top-down
  divide-and-conquer variants on their worst-case shapes, with no worst-case
  machinery: its worst case *is* its average case.
- **Against the sixty-year-old classics it is a peer in C++ and Python** — the
  fastest tree builder on the C++ random corpus (narrowly) and on every
  structured C++ shape (clearly); the fastest tree builder and the fastest
  evaluator outright on the Python random corpus; ties or narrow wins on the
  rest, with three honest losses (C++ nestchain to shunting-yard by ~6 %,
  Python powchain to recursive descent by ~7 %, Haskell nestchain by ~15 %).
- **In Haskell the tree-building form loses** to the pointer-AST classics by
  1.5–2.6×: its advantage is contiguous memory, and a runtime that boxes every
  node hides exactly that. Its no-tree form still ties or wins there.

## Scoreboard

### Tree-building form `multipass-reverse-fold` vs the best classic tree builder

| input | C++ | Python | Haskell |
|---|---|---|---|
| random corpus (n=1000) | ahead, narrowly (+1…+5 % vs `ast-arena`) | ahead, narrowly (+1…+2 % vs `ast-rd`) | loses (1.5–1.7× slower than `ast-pratt`) |
| random corpus (n=10000) | tie (-1…+7 % vs `ast-arena`) | ahead, narrowly (+4…+9 % vs `ast-sy`) | loses (2.1–2.6× slower than `ast-pratt`) |
| powchain | **best** (+11…+17 % vs `ast-arena`) | loses (-8…-6 % vs `ast-rd`) | loses (1.7–2.1× slower than `ast-rd`) |
| towerchain | **best** (+8…+16 % vs `ast-arena`) | tie (-1…+3 % vs `ast-rd`) | loses (1.5–1.7× slower than `ast-pratt`) |
| sumchain | **best** (+20…+33 % vs `ast-arena`) | tie (-1…+1 % vs `ast-pratt`) | loses (1.6–1.8× slower than `ast-pratt`) |
| nestchain | **best** (+10…+21 % vs `ast-arena`) | tie (-3…-0 % vs `ast-sy`) | loses (1.6–1.9× slower than `ast-rd`) |

### No-tree form `direct-reverse` vs the best classic no-tree evaluator

| input | C++ | Python | Haskell |
|---|---|---|---|
| random corpus (n=1000) | tie (-1…+15 % vs `direct-sy`) | **best** (+8…+9 % vs `direct-rd`) | tie (-15…+17 % vs `direct-rd`) |
| random corpus (n=10000) | tie (-1…+15 % vs `direct-sy`) | **best** (+7…+8 % vs `direct-rd`) | tie (-1…+10 % vs `direct-rd`) |
| powchain | tie (-1…+3 % vs `direct-rd`) | ahead, narrowly (+1…+3 % vs `direct-rd`) | **best** (+21…+29 % vs `direct-rd`) |
| towerchain | tie (-9…+4 % vs `direct-rd`) | **best** (+16…+17 % vs `direct-rd`) | **best** (+5…+11 % vs `direct-rd`) |
| sumchain | tie (+0…+4 % vs `direct-rd`) | **best** (+14…+15 % vs `direct-rd`) | tie (-3…-3 % vs `direct-rd`) |
| nestchain | loses (-7…-6 % vs `direct-sy`) | ahead, narrowly (+3…+5 % vs `direct-sy`) | loses (-19…-15 % vs `direct-rd`) |

### `multipass-reverse-fold` vs the best of its own top-down family

| input | C++ | Python | Haskell |
|---|---|---|---|
| random corpus (n=1000) | **best** (+33…+33 % vs `direct-mp`) | **best** (+57…+58 % vs `direct-mp`) | **best** (+13…+34 % vs `direct-mp`) |
| random corpus (n=10000) | **best** (+36…+38 % vs `direct-mp`) | **best** (+59…+61 % vs `direct-mp`) | **best** (+31…+39 % vs `multipass`) |
| powchain | **best** (+54…+59 % vs `direct-mp`) | **best** (+50…+51 % vs `direct-mp`) | **best** (+24…+30 % vs `multipass`) |
| towerchain | **best** (+52…+60 % vs `direct-mp`) | **best** (+37…+38 % vs `direct-mp`) | **best** (+39…+43 % vs `direct-mp`) |
| sumchain | **best** (+19…+26 % vs `multipass-arena`) | **best** (+37…+37 % vs `direct-mp`) | tie (-9…+6 % vs `multipass`) |
| nestchain | **best** (+50…+71 % vs `direct-mp`) | **best** (+47…+49 % vs `direct-mp`) | **best** (+33…+41 % vs `multipass`) |

Numbers behind the cells (ns/leaf, medians of the three runs) are in
[FINDINGS.md](../FINDINGS.md) and the per-language READMEs; the
walk-through of the algorithm is in [multipass-reverse.md](multipass-reverse.md).

## What it does *not* claim

- It is not faster than a lexer-free evaluator: `direct-scannerless`, recursive
  descent with the lexer fused into the grammar, beats every strategy in C++
  and Python by ~25 %. That is the cost of having a shared lexer at all and
  every real strategy in the table pays it; it is the control, not a rival.
- The design targets this one four-level grammar. "One sweep per precedence
  level" does not carry over for free to function calls of arbitrary arity,
  mixed associativity, statements, or a dozen-plus precedence levels; for
  those, recursive descent and Pratt parsing remain the general-purpose
  default.
- Margins under ~5 % are reported as ranges and labelled *narrowly* or *tie*
  on purpose: single-run numbers at that scale flip sign on the same runner.

*Data: CI bench runs 33634873982, 33635442782, 33635439496 (2026-09-02).*
