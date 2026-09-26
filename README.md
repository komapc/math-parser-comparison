<div align="center">

# 🧮 Math-Expression Parser & Evaluator

**Fifteen strategies for turning `-2 ^ 2 + 3 * (4 - 1)` into `5` — nine tree builders, five direct evaluators and one lexer-free control — in C++, Rust, Haskell, and Python.**

</div>

This repo tests one algorithm — **`multipass-reverse`**, a bottom-up
expression parser — against the classic parsers and against its own top-down
family. Full tables and analysis: **[FINDINGS.md](FINDINGS.md)**.
Just the verdict, one page: **[docs/one-pager.md](docs/one-pager.md)**.

**In short:**
- **Same rules for everyone.** All 15 strategies share one lexer, one
  grammar and one spec suite, and must agree on 6 000 fuzzed inputs per run.
  Every number is a range over three CI runs on one CPU model, and a margin
  that flips sign across runs is reported as a tie.
- **Against its own top-down family, it is ahead on every tested input.** Its
  worst case is its average case. The top-down variants needed scan budgets,
  buckets and SIMD just to stay O(n log n), and they still trail by
  1.5–4.9× on their own worst-case shape.
- **Against the classic parsers, it is a peer, not a winner.** It is the
  fastest tree builder in C++ by 2–4 %. It ties in Rust and in Python at
  n=1000. It loses in Haskell. Its no-tree form wins outright only in Python.

## The algorithm

`multipass-reverse` reduces the *tightest-binding* constructs first — deepest
parentheses, then `^`, then `*` `/`, then `+` `-` — one in-place sweep per
precedence level, until a single node remains. It builds the tree in the
opposite order, and that tree evaluates identically to every other parser's on
every spec and fuzz input (checked by value, not by shape). For this fixed
four-level grammar it never searches for a split point, so it is
**Θ(n) on every input, with no fallback machinery**. A fused form,
`multipass-reverse-fold` (and its no-tree twin `direct-reverse`), performs the
same level reductions on the fly with two accumulators per parenthesis frame —
no recursion, no prepass, every token touched once.
([C++](cpp/src/multipass_reverse.cpp) ·
[Rust](rust/src/fold.rs) ·
[Python](python/mathparser/evaluators.py) ·
[Haskell](haskell/src/MathParser/Strategies.hs) ·
**[full walk-through](docs/multipass-reverse.md)**)

## Result 1 — vs the classics: narrowly ahead in C++, a tie in Rust and Python

Tree builders and direct evaluators are compared separately: the table below
builds a tree, the paragraph after it covers the no-tree forms.

Random corpus, ns/leaf at n=1000, neutral 4-vCPU CI runner, median of three
independent runs of one commit on one CPU model (2026-09-26, AMD EPYC 9V74),
normalised to the fastest tree builder per language (**bold** = fastest, to
two decimals):

| tree builder | representation | C++ | Rust | Python | Haskell |
|---|---|--:|--:|--:|--:|
| `ast-recursive-descent` | pointer AST | 1.93 | 1.69 | 1.01 | **1.00** |
| `ast-shunting-yard` | pointer AST | 1.91 | 1.76 | 1.03 | 1.17 |
| `ast-pratt` | pointer AST | 1.91 | 1.67 | **1.00** | 1.01 |
| `ast-arena` | arena AST | 1.04 | **1.00** | 1.14 | 1.10 |
| `multipass` | pointer AST | 2.99 | 3.93 | 1.75 | 1.64 |
| `multipass-arena` | arena AST | 1.93 | 2.70 | 1.89 | 1.79 |
| `multipass-bfs` | arena AST | 2.05 | 2.99 | 2.33 | 2.15 |
| `multipass-reverse` | arena AST | 1.44 | 1.38 | 1.36 | 1.47 |
| `multipass-reverse-fold` | arena AST | **1.00** | **1.00** | **1.00** | 1.13 |

**On the random corpus the fused form is the fastest tree builder of nine in
C++** — ~2–4 % ahead of `ast-arena` from n=100 up (+3…+9 % at n=10),
positive in all 12 size×run measurements, so read it as a consistent sliver,
not a margin. **In Rust it is a tie** with `ast-arena` from n=100 up (within
±1 % in every run) and ahead only at n=10 (+3 %): the same algorithm under
LLVM does not reproduce the C++ sliver, a hint that it is partly a GCC
story. **In Python it is a tie at n=1000** with `ast-pratt` (+0.3…+0.5 %),
~1 % ahead of `ast-rd`, and narrowly ahead at the other sizes (+4…+6 % over
the best classic, `ast-shunting-yard` there). On the structured shapes it
ties or leads in C++ and Rust everywhere; in Python it narrowly leads
towerchain, ties nestchain and trails on powchain (~7–8 %, `ast-rd`) and
sumchain (5–7 %, `ast-pratt`). The buffered `multipass-reverse` sits ~1.4×
behind the fold in all three. Haskell is the exception: the pointer classics
lead every arena form, from ~1.1× at the tight end (`ast-arena`) to ~2.2× at
the wide end (`multipass-bfs`); the fold itself trails `ast-rd` by ~1.1× up
to n=1000 and ~1.25× at n=10000. (Those gaps were ~1.2× before the shared
Haskell arena carrier was made strict — one change, applied to every arena
strategy alike; see the [Haskell README](haskell/README.md).) The pattern
fits memory layout being the main factor: contiguous forms win in C++ and
Rust, and the advantage does not carry over to a runtime that boxes every
node.

Its no-tree twin `direct-reverse` is a **three-way tie** with
`direct-recursive-descent` and `direct-shunting-yard` in C++, all at ~41
ns/leaf (+0.5…+1.7 % vs `direct-rd`, −1…0 % vs `direct-sy` across runs at
n=1000). In Rust it is level with `direct-rd` (−2…+2 % across all sizes,
0–2 % behind at n=1000) and 6–7 % ahead of `direct-sy`. In Python it wins
outright: ahead of both `direct-shunting-yard` and `direct-rd` at every
size, in every run (+7…+26 % vs `direct-rd`, +11…+13 % vs
`direct-sy`). On the structured shapes it wins every one in Rust and three
of four in Python (powchain: 1–2 % behind `direct-rd`); in C++ it wins
powchain, ties towerchain and trails on sumchain (4 %) and nestchain
(~20–22 %) against `direct-sy`. In Haskell it trails `direct-rd` by 1–9 % on
the random corpus, narrowly leads powchain, ties towerchain and loses
sumchain and nestchain. The lexer-free control `direct-scannerless` sits
~16–38 % below the fastest real strategy in C++, Rust and Python and ~9 %
below it in Haskell; that gap is the shared
lexer, measured — see "Same rules" below.

## Result 2 — vs its family: ahead on every tested input

The top-down `multipass` variants build the same tree by scanning for the
loosest operator and splitting. That scan is attackable: a flat
mixed-precedence chain (`3^2 * 2^2 / 2^2 * …` — a factored monomial) makes it
**Θ(n²)**. C++ powchain, m=8192, ns/leaf, neutral runner:

| strategy | before the rescue patch | after | worst-case machinery |
|---|--:|--:|---|
| `multipass` | 3 791 | 132 | scan budget + buckets |
| `multipass-arena` | 4 864 | 71 | budget + buckets + AVX2 |
| `direct-mp` | 3 236 | 55 | budget + buckets + AVX2 |
| `multipass-bfs` | 84 † | 76 | O(n log n) sparse-table RMQ |
| **`multipass-reverse`** | **43** | **37** | **none** |
| **`multipass-reverse-fold`** | — (new) | **27** | **none** |

`multipass-reverse` and its fused form are **the only members of the family
whose worst case is their average case**. The others needed bounded scans,
precedence buckets and AVX2 to go linear — and still trail the buffered form
by 1.5–3.5× and the fused form by 2.0–4.9× ("after" column: 2026-09-26
runs on EPYC 9V74 — absolute ns/leaf there are not comparable with the
"before" column's older runner; after the top-down members stopped paying per-call allocations the
fold never paid). Bottom-up's own worst case, deep parenthesis nesting, is
benchmarked too: flat, and the fused form is the fastest tree builder there
(27 vs `ast-arena` 35 ns/leaf at m=8192).
(† `multipass-bfs`'s O(1) splits dodge the powchain but a `^`-tower catches
it the same way; the "before" column is the last pre-fix CI run.
[Details.](FINDINGS.md#result-2--vs-its-family-ahead-on-every-tested-input))

Correctness: curated spec suites and differential fuzzing in all four
languages — all fifteen strategies must agree, value or
rejection, on 6 000 random and mutated inputs per run.

## Same rules for every parser

A rule that helps one strategy is applied to all of them or to none. Three
lexing rules came out of writing the shortest correct evaluator, and each was
then applied wherever it applies:

1. **Parse numbers on the fast path.** Haskell's shared lexer used `reads`,
   roughly two-thirds of the fastest Haskell strategies' time. Clinger's fast
   path (bit-identical, verified) made every Haskell strategy 1.6–2.9×
   faster; the tiers did not move.
2. **Don't materialise what the algorithm never indexes.** The shared C++
   lexer streams tokens to every strategy that reads left to right; only the
   divide-and-conquer family, whose algorithm indexes the token array, still
   builds it. Every streaming C++ strategy got 16–33 % faster.
3. **Measure the lexer, don't guess it.** `direct-scannerless` is
   `direct-recursive-descent` with the lexer fused into the grammar, so the
   gap between the two *is* the lexer's cost: roughly a quarter of the time
   in C++ and Python, over a third in Rust, and ~10 % in Haskell. It is
   included as a control, not as a parser using the common lexer, so it is
   left out of the rankings.

Details and per-strategy effects: [FINDINGS.md](FINDINGS.md#lexing-rules--applied-to-every-parser).

## Scope

This is a *specialized* parser for one fixed grammar — numbers, five binary
operators, unary ±. The one-sweep-per-precedence-level structure is tuned to
exactly that and is **not** a general-purpose technique: richer grammars
(function calls, statements, many precedence levels) stay recursive descent's
and Pratt's home turf. The results above are claims about that narrow job —
whether the approach generalizes is an open question, not a claim.

Nor is bottom-up per-level reduction new: operator-precedence parsing and the
multi-pass expression translators of early compilers work the same way. What
this repo tests is whether a fused, single-sweep form of that old idea
competes with today's defaults under identical rules — and the answer is
measured, not assumed.

## Where to look

- [FINDINGS.md](FINDINGS.md) — grammar, build & run, the cross-language table,
  the lexing rules, both results in full.
- [docs/multipass-reverse.md](docs/multipass-reverse.md) — the algorithm,
  step by step, and the fused variant.
- [docs/one-pager.md](docs/one-pager.md) — every input × every language,
  labelled best / narrowly ahead / tie / loses from three runs.
- [cpp/](cpp/README.md) · [rust/](rust/README.md) · [python/](python/README.md) · [haskell/](haskell/README.md)
  — per-language implementations and tables.

## How this was built

Development was substantially assisted by Claude Code, which is why most
commits carry a `Co-Authored-By: Claude` line. The algorithm, the rules and
the conclusions are the author's; every number above comes from the CI runs
cited in [FINDINGS.md](FINDINGS.md), regenerated by
[`bench/tables.py`](bench/tables.py), and every claim can be rechecked from
the code and commands in this repo. GitHub hands out different CPU models
per run, and between models the small margins move by a few per cent. Treat
any cell within ~5 % of a label boundary as hardware-dependent (see the
[one-pager](docs/one-pager.md)).
