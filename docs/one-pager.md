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
4-vCPU GitHub runner, **three independent times** (rustc 1.98.1 throughout).
A cell below is labelled
from the *range across those three runs* (lowest%, highest%): **best** if the
low end is ≥5 % ahead; *ahead, narrowly* if the low end is ≥0.5 % ahead but
under 5 %; *loses* if the high end is ≤0.5 % behind; *tie* otherwise — which
covers a sign flip across runs, but also a range that sits entirely within
±0.5 %, ahead or behind, since that is noise on this runner.

## Verdict

- **Against its own family it is strictly better everywhere** — 1.4× faster
  than the buffered version of itself, which is in turn 1.5–3.6× faster than
  the top-down divide-and-conquer variants on the family's own C++
  worst-case shape (2.1–5.2× for the fused form itself; see the scoreboard
  below for the other three languages), with no worst-case machinery: its
  worst case *is* its average case. (`multipass-arena`'s own worst-case
  shape, sumchain, is a separate story — see the note below the scoreboard.)
- **Against the sixty-year-old classics it is a peer in C++, Rust and Python**
  — the fastest tree builder on the C++ random corpus (narrowly) and on most
  structured C++ shapes (towerchain is a tie by the range rule this batch); a
  genuine tie with the arena classic on the Rust random corpus (sign flips
  run to run) and clearly ahead on three of Rust's four structured shapes
  (sumchain is a tie this batch); the fastest tree builder and the fastest
  evaluator outright on the Python random corpus; ties or narrow wins on the
  rest, with honest losses (C++ nestchain to shunting-yard by ~14–24 %,
  Python powchain to recursive descent by ~4 %).
- **In Haskell the tree-building form loses** to the pointer-AST classics by
  1.2–1.8×: its advantage is contiguous memory, and a runtime that boxes every
  node hides exactly that. Its no-tree form is mixed there: it wins the
  random corpus, powchain and towerchain, but loses sumchain and nestchain to
  recursive descent.

## Scoreboard

### Tree-building form `multipass-reverse-fold` vs the best classic tree builder

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | ahead, narrowly (+3…+4 % vs `ast-arena`) | tie (-3…+4 % vs `ast-arena`) | tie (0…+2 % vs `ast-rd`) | loses (1.2–1.3× slower than `ast-rd`) |
| random corpus (n=10000) | ahead, narrowly (+1…+4 % vs `ast-arena`) | tie (-2…+3 % vs `ast-arena`) | **best** (+5…+6 % vs `ast-sy`) | loses (1.5–1.6× slower than `ast-pratt`) |
| powchain | **best** (+10…+16 % vs `ast-arena`) | ahead, narrowly (+3…+7 % vs `ast-arena`) | loses (-4…-4 % vs `ast-rd`) | loses (1.4–1.5× slower than `ast-pratt`) |
| towerchain | tie (-1…+17 % vs `ast-arena`) | **best** (+12…+13 % vs `ast-arena`) | tie (0…+2 % vs `ast-rd`) | loses (1.3–1.4× slower than `ast-pratt`) |
| sumchain | **best** (+19…+21 % vs `ast-arena`) | tie (-11…+6 % vs `ast-arena`) | ahead, narrowly (+1…+2 % vs `ast-rd`) | loses (1.4–1.6× slower than `ast-rd`) |
| nestchain | **best** (+16…+29 % vs `ast-arena`) | **best** (+11…+33 % vs `ast-arena`) | tie (0…0 % vs `ast-sy`) | loses (1.7–1.8× slower than `ast-pratt`) |

### No-tree form `direct-reverse` vs the best classic no-tree evaluator

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | tie (0…+1 % vs `direct-rd`) | tie (-5…+4 % vs `direct-rd`) | **best** (+8…+9 % vs `direct-rd`) | tie (-6…+10 % vs `direct-rd`) |
| random corpus (n=10000) | tie (0…+1 % vs `direct-rd`) | tie (-5…+3 % vs `direct-rd`) | **best** (+6…+8 % vs `direct-rd`) | ahead, narrowly (+4…+9 % vs `direct-rd`) |
| powchain | ahead, narrowly (+2…+9 % vs `direct-sy`) | ahead, narrowly (+2…+10 % vs `direct-rd`) | ahead, narrowly (+3…+3 % vs `direct-rd`) | **best** (+21…+26 % vs `direct-rd`) |
| towerchain | tie (-1…+1 % vs `direct-sy`) | **best** (+10…+14 % vs `direct-rd`) | **best** (+17…+19 % vs `direct-rd`) | ahead, narrowly (+5…+7 % vs `direct-rd`) |
| sumchain | tie (-4…+32 % vs `direct-sy` — this shape is volatile on this runner across batches, see [FINDINGS.md](../FINDINGS.md)) | **best** (+15…+18 % vs `direct-rd`) | **best** (+11…+13 % vs `direct-rd`) | loses (-3…-1 % vs `direct-rd`) |
| nestchain | loses (-24…-14 % vs `direct-sy`) | **best** (+19…+22 % vs `direct-sy`) | ahead, narrowly (+4…+4 % vs `direct-sy`) | loses (-20…-19 % vs `direct-rd`) |

### `multipass-reverse-fold` vs the best of its own top-down family

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | **best** (+31…+34 % vs `direct-mp`) | **best** (+58…+60 % vs `direct-mp`) | **best** (+55…+56 % vs `direct-mp`) | **best** (+28…+32 % vs `multipass`) |
| random corpus (n=10000) | **best** (+37…+38 % vs `direct-mp`) | **best** (+65…+66 % vs `direct-mp`) | **best** (+59 % vs `direct-mp`) | **best** (+40…+42 % vs `direct-mp`) |
| powchain | **best** (+51…+54 % vs `direct-mp`) | **best** (+74 % vs `direct-mp`) | **best** (+47…+49 % vs `direct-mp`) | **best** (+35…+42 % vs `multipass`) |
| towerchain | **best** (+50…+54 % vs `direct-mp`) | **best** (+59 % vs `direct-mp`) | **best** (+33…+34 % vs `direct-mp`) | **best** (+49…+54 % vs `direct-mp`) |
| sumchain | **best** (+37…+45 % vs `direct-mp` — its closest family rival here, `multipass-arena`, has a bimodal runtime on this shape; see the note below) | **best** (+49…+63 % vs `direct-mp`) | **best** (+33…+34 % vs `direct-mp`) | **best** (+10…+12 % vs `multipass`) |
| nestchain | **best** (+49…+52 % vs `direct-mp`) | **best** (+26…+32 % vs `direct-mp`) | **best** (+45…+46 % vs `direct-mp`) | **best** (+31…+36 % vs `multipass`) |

`multipass-arena`'s C++ sumchain runtime is bimodal on this runner — roughly
44 ns/leaf or 75 ns/leaf, not a single stable value (see
[FINDINGS.md](../FINDINGS.md)) — and in its fast mode it lands within noise
of the buffered `multipass-reverse` form. That is not a rescued-family win:
sumchain's single precedence level gives the split-scan machinery nothing to
search, so the "budget + buckets" rescue and the plain buffer end up doing
the same trivial pass. The fused form stays ahead of the whole family there
regardless, by at least 1.35× at the median.

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

*Data: CI bench runs 34136843367, 34137622680, 34138407724 (2026-09-07).*
