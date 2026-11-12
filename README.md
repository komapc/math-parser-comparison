<div align="center">

# 🧮 Math-Expression Parser & Evaluator

**Fifteen strategies for turning `-2 ^ 2 + 3 * (4 - 1)` into `5` — nine tree builders, five direct evaluators and one lexer-free control — in C++, Rust, Haskell, and Python.**

</div>

This repo tests one algorithm — **`multipass-reverse`**, a bottom-up
expression parser — against the classic parsers and against its own top-down
family. Full tables and analysis: **[FINDINGS.md](FINDINGS.md)**.
Just the verdict, one page: **[docs/one-pager.md](docs/one-pager.md)**.

**In short:**
- **It is correct.** All 15 strategies share one lexer, one grammar and one
  spec suite, and must agree on 6 000 fuzzed inputs per run, in every
  language and across languages.
- **Against its own top-down family, it is ahead on every tested input.** Its
  worst case is its average case. The top-down variants needed scan budgets,
  buckets and SIMD just to stay O(n log n), and they still trail by
  1.5–4.9× on their own worst-case shape.
- **Against the classic parsers, it is a peer, not a winner.** It is the
  fastest tree builder in C++ by 3–4 %. It ties in Rust and in Python at
  n=1000. It loses in Haskell. On the random corpus its no-tree form wins
  outright only in Python.
- **How to read the numbers.** Every number is a range over three CI runs on
  one CPU model; a margin that flips sign across runs — or between batches
  weeks apart — is reported as a tie.

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

## Result 1 — vs the classics: a sliver in C++, ties in Rust and Python, behind in Haskell

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

**Tree form (`multipass-reverse-fold`):**
- **C++:** the fastest of nine tree builders on the random corpus, but only
  3–4 % ahead of `ast-arena` — positive in all 12 size×run measurements and
  in both earlier 9V74 batches: a consistent sliver, not a margin.
- **Rust:** a tie with `ast-arena` from n=100 up (−1…+2 %). LLVM does not
  reproduce the C++ sliver.
- **Python:** a tie at n=1000 with `ast-pratt` and `ast-rd`, narrowly ahead
  (+2…+6 %) at the other sizes.
- **Haskell:** it loses — ~1.1× behind `ast-rd` up to n=1000, ~1.2–1.3× at
  n=10000. Every arena form trails the pointer classics there: contiguous
  memory is the likely edge in C++ and Rust, and a runtime that boxes every
  node leaves little of it.
- **Structured shapes:** ties or leads in C++ and Rust everywhere; in Python
  it ties towerchain and nestchain and trails powchain (6–9 %, `ast-rd`)
  and sumchain (6–7 %, `ast-pratt`).
- The buffered `multipass-reverse` sits ~1.35–1.4× behind the fold.

**No-tree form (`direct-reverse`):**
- **C++:** a hair behind `direct-shunting-yard` on the random corpus
  (0.2–1.5 %, both at ~40 ns/leaf); 15–24 % behind it on nestchain.
- **Rust:** level with `direct-rd` on the random corpus (−0.2…+1.8 %); ahead
  on every structured shape.
- **Python:** wins outright — ahead of every classic at every size, in every
  run (+6…+26 % vs `direct-rd`).
- **Haskell:** 2–5 % behind `direct-rd` on the random corpus; loses sumchain
  and nestchain.

The lexer-free control `direct-scannerless` sits ~17–38 % below the fastest
real strategy in C++, Rust and Python and ~6 % below it in Haskell; that gap
is the shared lexer, measured — see "Same rules" below. Every input ×
language, labelled: [one-pager](docs/one-pager.md); full prose:
[FINDINGS.md](FINDINGS.md).

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
precedence buckets and AVX2 to get down to O(n log n), and they still trail
the buffered form by 1.5–3.6× and the fused form by 2.0–4.9×. Bottom-up's
own worst case, deep parenthesis nesting, is flat too, and the fused form is
the fastest tree builder there (27 vs `ast-arena` 36 ns/leaf at m=8192).

"Before" is the last pre-fix CI run; "after" is the 2026-11-10 runs on EPYC
9V74, after the top-down members stopped paying per-call allocations the
fold never paid. The two columns ran on different runners, so compare within
a column. † `multipass-bfs`'s O(1) splits dodge the powchain, but a
`^`-tower catches it the same way.
[Details.](FINDINGS.md#result-2--vs-its-family-ahead-on-every-tested-input)

Correctness: curated spec suites and differential fuzzing in all four
languages — all fifteen strategies must agree, value or
rejection, on 6 000 random and mutated inputs per run — and CI checks that
the four languages agree with each other on the same inputs, which is what
catches a bug in a language's shared lexer.

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
the conclusions are the author's. Every number above comes from the CI
runs cited in [FINDINGS.md](FINDINGS.md), regenerated by
[`bench/tables.py`](bench/tables.py) from the raw outputs archived in
[`bench/results/`](bench/results/) (GitHub deletes CI artifacts after 90
days), and every claim can be rechecked from the code in this repo.

Small margins depend on the machine. GitHub hands out different CPU models
per run, and between models the small margins move by a few per cent; even
on one model, batches run weeks apart moved single cells by up to 7 points
(runner image and kernel changed in between). Treat any cell within ~7 % of
a label boundary as environment-dependent (see the
[one-pager](docs/one-pager.md)).
