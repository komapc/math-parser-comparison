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
  that flips sign across runs — or between batches run months apart — is
  reported as a tie.
- **Against its own top-down family, it is ahead on every tested input.** Its
  worst case is its average case. The top-down variants needed scan budgets,
  buckets and SIMD just to stay O(n log n), and they still trail by
  1.5–4.9× on their own worst-case shape.
- **Against the classic parsers, it is a peer, not a winner.** It is the
  fastest tree builder in C++ by 3–4 %. It ties in Rust and in Python at
  n=1000. It loses in Haskell. On the random corpus its no-tree form wins
  outright only in Python.

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
independent runs of one commit on one CPU model (2026-11-10, AMD EPYC 9V74),
normalised to the fastest tree builder per language (**bold** = fastest, to
two decimals):

| tree builder | representation | C++ | Rust | Python | Haskell |
|---|---|--:|--:|--:|--:|
| `ast-recursive-descent` | pointer AST | 1.92 | 1.70 | 1.01 | **1.00** |
| `ast-shunting-yard` | pointer AST | 1.92 | 1.75 | 1.02 | 1.17 |
| `ast-pratt` | pointer AST | 1.90 | 1.68 | **1.00** | 1.02 |
| `ast-arena` | arena AST | 1.04 | 1.01 | 1.14 | 1.10 |
| `multipass` | pointer AST | 2.92 | 3.39 | 1.74 | 1.63 |
| `multipass-arena` | arena AST | 1.87 | 2.64 | 1.88 | 1.79 |
| `multipass-bfs` | arena AST | 1.99 | 2.86 | 2.31 | 2.12 |
| `multipass-reverse` | arena AST | 1.42 | 1.34 | 1.37 | 1.44 |
| `multipass-reverse-fold` | arena AST | **1.00** | **1.00** | 1.01 | 1.13 |

**On the random corpus the fused form is the fastest tree builder of nine in
C++** — 3–4 % ahead of `ast-arena` at every size (+2.9…+4.2 %), positive
in all 12 size×run measurements here and in both earlier 9V74 batches, so read it as a
consistent sliver, not a margin. **In Rust it is a tie** with `ast-arena`
from n=100 up (−1…+2 % across runs) and ahead only at n=10 (+3…+4 %): the
same algorithm under LLVM does not reproduce the C++ sliver, a hint that it
is partly a GCC story. **In Python it is a tie at n=1000** with `ast-pratt`
(−0.7…+0.4 %) and `ast-rd` (−0.1…+0.9 %), and narrowly ahead at the other
sizes (+2…+6 % over the best classic). On the structured shapes it ties or
leads in C++ and Rust everywhere; in Python it ties towerchain and nestchain
and trails on powchain (6–9 %, `ast-rd`) and sumchain (6–7 %, `ast-pratt`).
The buffered `multipass-reverse` sits ~1.35–1.4× behind the fold in all
three. Haskell is the exception: the pointer classics lead every arena form,
from ~1.1× at the tight end (`ast-arena`) to ~2.1× at the wide end
(`multipass-bfs`); the fold itself trails `ast-rd` by ~1.1× up to n=1000
and ~1.2–1.3× at n=10000. (Those gaps were ~1.2× before the shared
Haskell arena carrier was made strict — one change, applied to every arena
strategy alike; see the [Haskell README](haskell/README.md).) The pattern
fits memory layout being the main factor: contiguous forms win in C++ and
Rust, and the advantage does not carry over to a runtime that boxes every
node.

Its no-tree twin `direct-reverse` **ties `direct-shunting-yard` in C++**,
both at ~40 ns/leaf (−0.2 % at n=1000, −1.5…−0.2 % across sizes, so it
reads as narrowly behind at n=10000), and is 1–3 % ahead of
`direct-recursive-descent`. In Rust it is level with `direct-rd` (−0.2…+1.8 %
across all sizes) and 6–9 % ahead of `direct-sy`. In Python it wins
outright: ahead of both `direct-shunting-yard` and `direct-rd` at every
size, in every run (+6…+26 % vs `direct-rd`, +10…+14 % vs `direct-sy`). On
the structured shapes it leads every one in Rust and three of four in Python
(powchain: a tie with `direct-rd`, −2…0 %); in C++ it narrowly leads
powchain, ties towerchain and sumchain, and trails on nestchain (15–24 %)
against `direct-sy` — towerchain and sumchain read +4…+7 % and +3 % in
this batch, but −1…0 % and −4 % in the previous one, so they count as
ties. In Haskell it trails `direct-rd` by 2–5 % on the random corpus,
narrowly leads powchain, ties towerchain and loses sumchain and nestchain.
The lexer-free control `direct-scannerless` sits ~17–38 % below the fastest
real strategy in C++, Rust and Python and ~6 % below it in Haskell; that gap is the shared
lexer, measured — see "Same rules" below.

## Result 2 — vs its family: ahead on every tested input

The top-down `multipass` variants build the same tree by scanning for the
loosest operator and splitting. That scan is attackable: a flat
mixed-precedence chain (`3^2 * 2^2 / 2^2 * …` — a factored monomial) makes it
**Θ(n²)**. C++ powchain, m=8192, ns/leaf, neutral runner:

| strategy | before the rescue patch | after | worst-case machinery |
|---|--:|--:|---|
| `multipass` | 3 791 | 128 | scan budget + buckets |
| `multipass-arena` | 4 864 | 65 | budget + buckets + AVX2 |
| `direct-mp` | 3 236 | 53 | budget + buckets + AVX2 |
| `multipass-bfs` | 84 † | 66 | O(n log n) sparse-table RMQ |
| **`multipass-reverse`** | **43** | **36** | **none** |
| **`multipass-reverse-fold`** | — (new) | **26** | **none** |

`multipass-reverse` and its fused form are **the only members of the family
whose worst case is their average case**. The others needed bounded scans,
precedence buckets and AVX2 to get down to O(n log n) (flat in practice on
these shapes) — and still trail the buffered form
by 1.5–3.6× and the fused form by 2.0–4.9× ("after" column: 2026-11-10
runs on EPYC 9V74 — absolute ns/leaf there are not comparable with the
"before" column's older runner; measured after the top-down members stopped
paying per-call allocations the fold never paid — none of the arena or
no-tree forms allocates per evaluation now, in C++ or Rust). Bottom-up's own worst case, deep parenthesis nesting, is
benchmarked too: flat, and the fused form is the fastest tree builder there
(27 vs `ast-arena` 36 ns/leaf at m=8192).
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
   in C++ and Python, over a third in Rust, and ~8 % in Haskell. It is
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
[`bench/tables.py`](bench/tables.py) from the raw outputs archived in
[`bench/results/`](bench/results/) (GitHub deletes CI artifacts after 90
days), and every claim can be rechecked from
the code and commands in this repo. GitHub hands out different CPU models
per run, and between models the small margins move by a few per cent; even
on one model, batches run weeks apart moved single cells by up to 7 points
(runner image and kernel changed in between). Treat any cell within ~7 % of
a label boundary as environment-dependent (see the
[one-pager](docs/one-pager.md)).
