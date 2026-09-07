<div align="center">

# 🧮 Math-Expression Parser & Evaluator

**Fifteen ways to turn `-2 ^ 2 + 3 * (4 - 1)` into `5`, in C++, Rust, Haskell, and Python.**

</div>

This repo tests one algorithm — **`multipass-reverse`**, a bottom-up
expression parser — against the classic parsers and against its own top-down
family. Full tables and analysis: **[FINDINGS.md](FINDINGS.md)**.
Just the verdict, one page: **[docs/one-pager.md](docs/one-pager.md)**.

## The algorithm

`multipass-reverse` reduces the *tightest-binding* constructs first — deepest
parentheses, then `^`, then `*` `/`, then `+` `-` — one in-place sweep per
precedence level, until a single node remains. Same tree as every other parser,
built in the opposite order. It never searches for a split point, so it is
**Θ(n) on every input, with no fallback machinery**. A fused form,
`multipass-reverse-fold` (and its no-tree twin `direct-reverse`), performs the
same level reductions on the fly with two accumulators per parenthesis frame —
no recursion, no prepass, every token touched once.
([C++](cpp/src/multipass_reverse.cpp) ·
[Rust](rust/src/fold.rs) ·
[Python](python/mathparser/evaluators.py) ·
[Haskell](haskell/src/MathParser/Strategies.hs) ·
**[full walk-through](docs/multipass-reverse.md)**)

## Result 1 — vs the classics: narrowly ahead in C++ and Python, a tie in Rust

Random corpus, ns/leaf at n=1000, neutral 4-vCPU CI runner, median of three
independent runs, normalised to the fastest tree builder per language
(**bold** = fastest):

| tree builder | representation | C++ | Rust | Python | Haskell |
|---|---|--:|--:|--:|--:|
| `ast-recursive-descent` | pointer AST | 1.90 | 1.79 | 1.00 | **1.00** |
| `ast-shunting-yard` | pointer AST | 1.96 | 1.88 | 1.03 | 1.16 |
| `ast-pratt` | pointer AST | 1.99 | 1.77 | 1.04 | 1.11 |
| `ast-arena` | arena AST | 1.04 | **1.00** | 1.17 | 1.29 |
| `multipass` | pointer AST | 3.38 | 4.02 | 2.29 | 1.67 |
| `multipass-arena` | arena AST | 1.88 | 2.74 | 2.43 | 2.03 |
| `multipass-bfs` | arena AST | 2.06 | 3.00 | 2.83 | 2.39 |
| `multipass-reverse` | arena AST | 1.39 | 1.37 | 1.39 | 1.46 |
| `multipass-reverse-fold` | arena AST | **1.00** | 1.02 | **1.00** | 1.20 |

**The fused form is the fastest tree builder of nine in C++ and in Python** —
~1–5 % ahead of `ast-arena` in C++, +2…+7 % against the best pointer classic
in Python. Positive in all 12 size×run measurements in both languages, so
read it as a consistent sliver, not a margin. **In Rust it's a genuine
tie**: the same code under LLVM lands anywhere from ~5 % ahead to ~4 %
behind `ast-arena` depending on the run and size, with the sign flipping run
to run rather than settling one way — noisier than the C++/Python sliver,
and a hint that the C++ edge is partly a GCC story. The buffered
`multipass-reverse` sits ~1.4× behind in all three. Haskell is the
exception: the pointer classics lead every arena form, from ~1.3× at the
tight end (`ast-arena`) to ~2.4× at the wide end (`multipass-bfs`); the fold
itself trails by ~1.1–1.5× depending on corpus size. Contiguous memory is
the whole game in C++ and Rust, and a boxed, GC'd runtime hides it.

Its no-tree twin `direct-reverse` is a **three-way tie** with
`direct-recursive-descent` and `direct-shunting-yard` in C++, all at ~50
ns/leaf (0…+2 % vs `direct-rd`, −2…+1 % vs `direct-sy` across runs — this
shape's classics are much closer than the wider sumchain range below).
Rust repeats the C++ tier at ~55 ns/leaf: a tie with `direct-rd` (−5…+4 %)
and ahead of `direct-sy`. In Python it wins outright: ahead of both
`direct-shunting-yard` and `direct-rd` at every size, in every run. On the
structured shapes it ties or beats both, except C++ nestchain against
`direct-sy` (~14–24 % behind). Haskell is mixed: it wins the random corpus,
powchain and towerchain, but loses sumchain and nestchain to recursive
descent — the same boxed-node effect that erases the tree-building form's
edge. The lexer-free control `direct-scannerless` sits roughly 16–44 %
below the field in C++, Rust and Python; that gap is the shared lexer,
measured — see "Same rules" below.

## Result 2 — vs its family: strictly better

The top-down `multipass` variants build the same tree by scanning for the
loosest operator and splitting. That scan is attackable: a flat
mixed-precedence chain (`3^2 * 2^2 / 2^2 * …` — a factored monomial) makes it
**Θ(n²)**. C++ powchain, m=8192, ns/leaf, neutral runner:

| strategy | before the rescue patch | after | worst-case machinery |
|---|--:|--:|---|
| `multipass` | 3 791 | 168 | scan budget + buckets |
| `multipass-arena` | 4 864 | 87 | budget + buckets + AVX2 |
| `direct-mp` | 3 236 | 70 | budget + buckets + AVX2 |
| `multipass-bfs` | 84 † | 93 | O(n log n) sparse-table RMQ |
| **`multipass-reverse`** | **43** | **46** | **none** |
| **`multipass-reverse-fold`** | — (new) | **32** | **none** |

`multipass-reverse` and its fused form are **the only members of the family
whose worst case is their average case**. The others needed bounded scans,
precedence buckets and AVX2 to go linear — and still trail the buffered form
by 1.5–3.6× and the fused form by 2.1–5.2×. Bottom-up's own worst case, deep
parenthesis nesting, is benchmarked too: flat, and the fused form is the
fastest tree builder there (32 vs `ast-arena` 42 ns/leaf at m=8192).
(† `multipass-bfs`'s O(1) splits dodge the powchain but a `^`-tower catches
it the same way; the "before" column is the last pre-fix CI run.
[Details.](FINDINGS.md#result-2--vs-its-family-strictly-better))

Correctness: curated spec suites in all four languages plus differential
fuzzing in C++, Rust and Python — all fifteen strategies must agree, value or
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
   in C++ and Python, a third in Rust, and noisy — sometimes negative — in
   Haskell. It is the control row, not a contender.

Details and per-strategy effects: [FINDINGS.md](FINDINGS.md#lexing-rules--applied-to-every-parser).

## Scope

This is a *specialized* parser for one fixed grammar — numbers, five binary
operators, unary ±. The one-sweep-per-precedence-level structure is tuned to
exactly that and is **not** a general-purpose technique: richer grammars
(function calls, statements, many precedence levels) stay recursive descent's
and Pratt's home turf. The results above are claims about that narrow job —
whether the approach generalizes is an open question, not a claim.

## Where to look

- [FINDINGS.md](FINDINGS.md) — grammar, build & run, the cross-language table,
  the lexing rules, both results in full.
- [docs/multipass-reverse.md](docs/multipass-reverse.md) — the algorithm,
  step by step, and the fused variant.
- [docs/one-pager.md](docs/one-pager.md) — every input × every language,
  labelled best / narrowly ahead / tie / loses from three runs.
- [cpp/](cpp/README.md) · [rust/](rust/README.md) · [python/](python/README.md) · [haskell/](haskell/README.md)
  — per-language implementations and tables.
