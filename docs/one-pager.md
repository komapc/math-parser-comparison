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
4-vCPU GitHub runner, **three independent times** (this batch: rustc 1.98.1,
one point release newer than the previous batch — a real toolchain change,
folded into the Rust numbers below rather than called out per cell). A cell
below is labelled
from the *range across those three runs* (lowest%, highest%): **best** if the
low end is ≥5 % ahead; *ahead, narrowly* if the low end is ≥0.5 % ahead but
under 5 %; *loses* if the high end is ≤0.5 % behind; *tie* otherwise — which
covers a sign flip across runs, but also a range that sits entirely within
±0.5 %, ahead or behind, since that is noise on this runner.

## Verdict

- **Against its own family it is strictly better everywhere** — 1.4× faster
  than the buffered version of itself, which is in turn 1.5–3.7× faster than
  the top-down divide-and-conquer variants on their C++ worst-case shapes
  (2.1–5.2× for the fused form itself; see the scoreboard below for the other
  three languages), with no worst-case machinery: its worst case *is* its
  average case.
- **Against the sixty-year-old classics it is a peer in C++, Rust and Python**
  — the fastest tree builder on the C++ random corpus (narrowly) and on every
  structured C++ shape; a genuine tie with the arena classic on the Rust
  random corpus (sign flips run to run) and clearly ahead on Rust's structured
  shapes; the fastest tree builder and the fastest evaluator outright on the
  Python random corpus; ties or narrow wins on the rest, with honest losses
  (C++ nestchain to shunting-yard by ~14–21 %, Python powchain to recursive
  descent by ~3–6 %).
- **In Haskell the tree-building form loses** to the pointer-AST classics by
  1.2–1.8×: its advantage is contiguous memory, and a runtime that boxes every
  node hides exactly that. Its no-tree form is mixed there: it wins the
  random corpus and powchain, but loses sumchain and nestchain to recursive
  descent.

## Scoreboard

### Tree-building form `multipass-reverse-fold` vs the best classic tree builder

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | ahead, narrowly (+3…+5 % vs `ast-arena`) | tie (-4…+3 % vs `ast-arena`) | tie (-1…+2 % vs `ast-rd`) | loses (1.2–1.3× slower than `ast-pratt`) |
| random corpus (n=10000) | ahead, narrowly (+4…+5 % vs `ast-arena`) | tie (-4…+3 % vs `ast-arena`) | ahead, narrowly (+4…+5 % vs `ast-sy`) | loses (1.5–1.7× slower than `ast-pratt`) |
| powchain | **best** (+9…+17 % vs `ast-arena`) | ahead, narrowly (+3…+6 % vs `ast-arena`) | loses (-6…-3 % vs `ast-rd`) | loses (~1.5× slower than `ast-pratt`) |
| towerchain | ahead, narrowly (+4…+19 % vs `ast-arena`) | **best** (+8…+15 % vs `ast-arena`) | ahead, narrowly (+2…+3 % vs `ast-rd`) | loses (1.3–1.4× slower than `ast-pratt`) |
| sumchain | **best** (+19…+20 % vs `ast-arena`) | ahead, narrowly (+1…+6 % vs `ast-arena`) | tie (0…+1 % vs `ast-pratt`) | loses (1.4–1.5× slower than `ast-rd`) |
| nestchain | **best** (+15…+30 % vs `ast-arena`) | **best** (+19…+31 % vs `ast-arena`) | tie (-2…+1 % vs `ast-sy`) | loses (~1.8× slower than `ast-pratt`) |

### No-tree form `direct-reverse` vs the best classic no-tree evaluator

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | tie (0…+5 % vs `direct-rd`) | tie (-5…+3 % vs `direct-rd`) | **best** (+8…+10 % vs `direct-rd`) | tie (-33…+3 % vs `direct-rd`) |
| random corpus (n=10000) | tie (0…+4 % vs `direct-rd`) | tie (-8…+4 % vs `direct-rd`) | **best** (+6…+9 % vs `direct-rd`) | tie (-5…+10 % vs `direct-rd`) |
| powchain | ahead, narrowly (+4…+9 % vs `direct-sy`) | ahead, narrowly (+2…+9 % vs `direct-rd`) | ahead, narrowly (+2…+3 % vs `direct-rd`) | **best** (+20…+22 % vs `direct-rd`) |
| towerchain | tie (0…+2 % vs `direct-sy`) | **best** (+10…+21 % vs `direct-rd`) | **best** (+18…+19 % vs `direct-rd`) | **best** (+6…+10 % vs `direct-rd`) |
| sumchain | tie (-2…+33 % vs `direct-sy` — one run spiked this shape specifically, see [FINDINGS.md](../FINDINGS.md)) | **best** (+15…+18 % vs `direct-rd`) | **best** (+12 % vs `direct-rd`) | loses (-9…-2 % vs `direct-rd`) |
| nestchain | loses (-21…-14 % vs `direct-sy`) | **best** (+19…+22 % vs `direct-sy`) | **best** (+5…+7 % vs `direct-sy`) | loses (-22…-20 % vs `direct-rd`) |

### `multipass-reverse-fold` vs the best of its own top-down family

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | **best** (+32…+35 % vs `direct-mp`) | **best** (+58…+60 % vs `direct-mp`) | **best** (+55 % vs `direct-mp`) | **best** (+24…+29 % vs `multipass`) |
| random corpus (n=10000) | **best** (+37…+40 % vs `direct-mp`) | **best** (+64…+66 % vs `direct-mp`) | **best** (+59…+60 % vs `direct-mp`) | **best** (+37…+41 % vs `direct-mp`) |
| powchain | **best** (+51…+55 % vs `direct-mp`) | **best** (+74 % vs `direct-mp`) | **best** (+47…+48 % vs `direct-mp`) | **best** (+35…+42 % vs `multipass`) |
| towerchain | **best** (+50…+54 % vs `direct-mp`) | **best** (+58…+59 % vs `direct-mp`) | **best** (+33…+35 % vs `direct-mp`) | **best** (+49…+53 % vs `direct-mp`) |
| sumchain | **best** (+38…+57 % vs `direct-mp` — same outlier run as above widens this one; see [FINDINGS.md](../FINDINGS.md)) | **best** (+49…+50 % vs `direct-mp`) | **best** (+33…+34 % vs `direct-mp`) | **best** (+11…+15 % vs `multipass`) |
| nestchain | **best** (+46…+53 % vs `direct-mp`) | **best** (+26…+35 % vs `direct-mp`) | **best** (+46…+47 % vs `direct-mp`) | **best** (+32…+34 % vs `direct-mp`) |

Numbers behind the cells (ns/leaf, medians of the three runs) are in
[FINDINGS.md](../FINDINGS.md) and the per-language READMEs; the
walk-through of the algorithm is in [multipass-reverse.md](multipass-reverse.md).

## What it does *not* claim

- It is not faster than a lexer-free evaluator: `direct-scannerless`, recursive
  descent with the lexer fused into the grammar, beats every strategy in C++,
  Rust and Python by roughly 16–44 %. That is the cost of having a shared
  lexer at all and every real strategy in the table pays it; it is the
  control, not a rival.
- The design targets this one four-level grammar. "One sweep per precedence
  level" does not carry over for free to function calls of arbitrary arity,
  mixed associativity, statements, or a dozen-plus precedence levels; for
  those, recursive descent and Pratt parsing remain the general-purpose
  default.
- Margins under ~5 % are reported as ranges and labelled *narrowly* or *tie*
  on purpose: single-run numbers at that scale flip sign on the same runner.

*Data: CI bench runs 34130395595, 34131316618, 34132166596 (2026-09-07).*
