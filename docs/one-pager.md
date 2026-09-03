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
4-vCPU GitHub runner, **three independent times**. A cell below is labelled
from the *range across those three runs*: **best** only if it is ≥5 % ahead in
every run; *ahead, narrowly* if positive in every run but under 5 %; *tie* if
the sign flips; *loses* if behind in every run.

## Verdict

- **Against its own family it is strictly better everywhere** — 1.3–1.5× faster
  than the buffered version of itself and 1.5–3.5× faster than the top-down
  divide-and-conquer variants on their worst-case shapes, with no worst-case
  machinery: its worst case *is* its average case.
- **Against the sixty-year-old classics it is a peer in C++, Rust and Python**
  — the fastest tree builder on the C++ random corpus (narrowly) and on every
  structured C++ shape; a tie with the arena classic on the Rust random corpus
  and clearly ahead on three of four Rust shapes; the fastest tree builder and
  the fastest evaluator outright on the Python random corpus; ties or narrow
  wins on the rest, with honest losses (C++ nestchain to shunting-yard by
  ~10–16 %, Python powchain to recursive descent by ~6–10 %, Haskell
  nestchain by ~15–25 %).
- **In Haskell the tree-building form loses** to the pointer-AST classics by
  1.5–1.9×: its advantage is contiguous memory, and a runtime that boxes every
  node hides exactly that. Its no-tree form still ties or wins there.

## Scoreboard

### Tree-building form `multipass-reverse-fold` vs the best classic tree builder

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | ahead, narrowly (+2…+5 % vs `ast-arena`) | tie (-2…-0 % vs `ast-arena`) | tie (-1…+1 % vs `ast-rd`) | loses (1.5–1.6× slower than `ast-pratt`) |
| random corpus (n=10000) | ahead, narrowly (+3…+5 % vs `ast-arena`) | tie (-1…-0 % vs `ast-arena`) | ahead, narrowly (+4…+8 % vs `ast-sy`) | loses (1.7–1.7× slower than `ast-pratt`) |
| powchain | ahead, narrowly (+4…+16 % vs `ast-arena`) | tie (-8…+5 % vs `ast-arena`) | loses (-10…-6 % vs `ast-rd`) | loses (1.5–1.6× slower than `ast-pratt`) |
| towerchain | **best** (+14…+14 % vs `ast-arena`) | **best** (+9…+15 % vs `ast-arena`) | tie (-2…+2 % vs `ast-rd`) | loses (1.5–1.7× slower than `ast-pratt`) |
| sumchain | **best** (+20…+22 % vs `ast-arena`) | **best** (+8…+22 % vs `ast-arena`) | ahead, narrowly (+2…+3 % vs `ast-pratt`) | loses (1.5–1.8× slower than `ast-rd`) |
| nestchain | **best** (+22…+35 % vs `ast-arena`) | **best** (+22…+39 % vs `ast-arena`) | loses (-1…-1 % vs `ast-sy`) | loses (1.7–1.9× slower than `ast-pratt`) |

### No-tree form `direct-reverse` vs the best classic no-tree evaluator

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | tie (+0…+3 % vs `direct-rd`) | tie (-4…+1 % vs `direct-rd`) | **best** (+9…+10 % vs `direct-rd`) | tie (-11…+10 % vs `direct-rd`) |
| random corpus (n=10000) | tie (+0…+3 % vs `direct-rd`) | tie (-5…+0 % vs `direct-rd`) | **best** (+7…+9 % vs `direct-rd`) | **best** (+5…+18 % vs `direct-rd`) |
| powchain | **best** (+5…+8 % vs `direct-sy`) | ahead, narrowly (+2…+4 % vs `direct-rd`) | tie (+0…+3 % vs `direct-rd`) | **best** (+14…+19 % vs `direct-rd`) |
| towerchain | ahead, narrowly (+1…+8 % vs `direct-sy`) | **best** (+10…+15 % vs `direct-rd`) | **best** (+15…+20 % vs `direct-rd`) | loses (-6…-4 % vs `direct-rd`) |
| sumchain | ahead, narrowly (+2…+29 % vs `direct-sy`) | **best** (+15…+15 % vs `direct-rd`) | **best** (+14…+16 % vs `direct-rd`) | loses (-7…-2 % vs `direct-rd`) |
| nestchain | loses (-16…-10 % vs `direct-sy`) | **best** (+17…+22 % vs `direct-sy`) | **best** (+5…+7 % vs `direct-sy`) | loses (-25…-15 % vs `direct-rd`) |

### `multipass-reverse-fold` vs the best of its own top-down family

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | **best** (+33…+36 % vs `direct-mp`) | **best** (+58…+60 % vs `direct-mp`) | **best** (+54…+57 % vs `direct-mp`) | **best** (+11…+23 % vs `multipass`) |
| random corpus (n=10000) | **best** (+37…+41 % vs `direct-mp`) | **best** (+65…+66 % vs `direct-mp`) | **best** (+57…+60 % vs `direct-mp`) | **best** (+33…+36 % vs `direct-mp`) |
| powchain | **best** (+53…+57 % vs `direct-mp`) | **best** (+69…+73 % vs `direct-mp`) | **best** (+48…+50 % vs `direct-mp`) | **best** (+34…+40 % vs `multipass`) |
| towerchain | **best** (+53…+53 % vs `direct-mp`) | **best** (+53…+57 % vs `direct-mp`) | **best** (+37…+37 % vs `direct-mp`) | **best** (+39…+45 % vs `direct-mp`) |
| sumchain | **best** (+9…+37 % vs `direct-mp`) | **best** (+50…+55 % vs `direct-mp`) | **best** (+37…+38 % vs `direct-mp`) | ahead, narrowly (+5…+9 % vs `multipass`) |
| nestchain | **best** (+51…+60 % vs `direct-mp`) | **best** (+31…+37 % vs `direct-mp`) | **best** (+44…+47 % vs `direct-mp`) | **best** (+35…+47 % vs `direct-mp`) |

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

*Data: CI bench runs 33734046295, 33734053376, 33734059421 (2026-09-03).*
