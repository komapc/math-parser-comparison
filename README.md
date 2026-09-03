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
| `ast-recursive-descent` | pointer AST | 1.87 | 1.96 | **1.00** | 1.11 |
| `ast-shunting-yard` | pointer AST | 2.02 | 2.03 | 1.02 | 1.21 |
| `ast-pratt` | pointer AST | 1.99 | 1.94 | 1.04 | **1.00** |
| `ast-arena` | arena AST | 1.03 | **1.00** | 1.21 | 1.42 |
| `multipass` | pointer AST | 3.55 | 4.11 | 2.25 | 1.82 |
| `multipass-arena` | arena AST | 2.00 | 2.81 | 2.37 | 2.28 |
| `multipass-bfs` | arena AST | 2.10 | 3.13 | 2.73 | 2.89 |
| `multipass-reverse` | arena AST | 1.47 | 1.41 | 1.41 | 1.99 |
| `multipass-reverse-fold` | arena AST | **1.00** | **1.00** | **1.00** | 1.59 |

**The fused form is the fastest tree builder of nine in C++ and in Python** —
~2–5 % ahead of `ast-arena` in C++, ~1–8 % ahead of the best pointer classic
in Python. Positive in 11 of 12 size×run measurements in each, so read it as
a consistent sliver, not a margin. **In Rust the sliver is gone**: the same
code under LLVM lands 0–2 % *behind* `ast-arena` in all 12 measurements — a
tie, and a hint that the C++ edge is partly a GCC story. The buffered
`multipass-reverse` sits ~1.4–1.5× behind in all three. Haskell is the
exception: the pointer classics lead every arena form by ~1.5–1.7×.
Contiguous memory is the whole game in C++ and Rust, and a boxed, GC'd
runtime hides it.

Its no-tree twin `direct-reverse` is a **three-way tie** with
`direct-recursive-descent` and `direct-shunting-yard` in C++, all at ~50
ns/leaf (+0…+3 % vs `direct-rd`, −1…+5 % vs `direct-sy` across runs). Rust
repeats the C++ tier at ~52 ns/leaf: a tie with `direct-rd` (−5…+2 %) and
7–10 % ahead of `direct-sy`. In Python it wins outright: ~10–13 % over
`direct-shunting-yard` and ~7–21 % over `direct-rd` at every size, in every
run. On the structured shapes it ties or beats both, except C++ nestchain
against `direct-sy` (~10–16 % behind). The lexer-free control
`direct-scannerless` sits 20–35 % below all three in C++, Rust and Python;
that gap is the shared lexer, measured — see "Same rules" below.

## Result 2 — vs its family: strictly better

The top-down `multipass` variants build the same tree by scanning for the
loosest operator and splitting. That scan is attackable: a flat
mixed-precedence chain (`3^2 * 2^2 / 2^2 * …` — a factored monomial) makes it
**Θ(n²)**. C++ powchain, m=8192, ns/leaf, neutral runner:

| strategy | before the rescue patch | after | worst-case machinery |
|---|--:|--:|---|
| `multipass` | 3 791 | 151 | scan budget + buckets |
| `multipass-arena` | 4 864 | 86 | budget + buckets + AVX2 |
| `direct-mp` | 3 236 | 70 | budget + buckets + AVX2 |
| `multipass-bfs` | 84 † | 87 | O(n log n) sparse-table RMQ |
| **`multipass-reverse`** | **43** | **40** | **none** |
| **`multipass-reverse-fold`** | — (new) | **30** | **none** |

`multipass-reverse` and its fused form are **the only members of the family
whose worst case is their average case**. The others needed bounded scans,
precedence buckets and AVX2 to go linear — and still trail the buffered form
by 1.7–3.8× and the fused form by 2.3–5×. Bottom-up's own worst case, deep
parenthesis nesting, is benchmarked too: flat, and the fused form is the
fastest tree builder there (33 vs `ast-arena` 50 ns/leaf at m=8192).
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
   gap between the two *is* the lexer's cost: ~25 % in C++ and Python,
   ~5–15 % and noisy in Haskell. It is the control row, not a contender.

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
