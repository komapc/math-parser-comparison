<div align="center">

# 🧮 Math-Expression Parser & Evaluator

**Fourteen ways to turn `-2 ^ 2 + 3 * (4 - 1)` into `5`, in C++, Haskell, and Python.**

</div>

This repo tests one new algorithm — **`multipass-reverse`**, a bottom-up
expression parser — against the classic parsers and against its own top-down
family. Grammar, builds, full tables and analysis: **[FINDINGS.md](FINDINGS.md)**.

## The algorithm

`multipass-reverse` reduces the *tightest-binding* constructs first — deepest
parentheses, then `^`, then `*` `/`, then `+` `-` — one in-place sweep per
precedence level, until a single node remains. Same tree as every other parser,
built in the opposite order. It never searches for a split point, so it is
**Θ(n) on every input, with no fallback machinery**. A fused form,
`multipass-reverse-fold` (and its no-tree twin `direct-reverse`), performs the
same level reductions on the fly with two accumulators per parenthesis frame —
no recursion, no prepass, every token touched once
([design and numbers](docs/multipass-reverse.md#the-fused-variant)).
([C++](cpp/src/multipass_reverse.cpp) ·
[Python](python/mathparser/evaluators.py) ·
[Haskell](haskell/src/MathParser/Strategies.hs) ·
**[full walk-through](docs/multipass-reverse.md)**)

## Result 1 — vs the classics: competitive — narrowly ahead in C++ and Python

Sixty-year-old, maximally-tuned algorithms on their home turf: random corpus,
ns/leaf at n=1000, neutral 4-vCPU CI runner, normalised to the fastest
tree-builder per language (**bold** = fastest):

| tree builder | representation | C++ | Python | Haskell |
|---|---|--:|--:|--:|
| `ast-recursive-descent` | pointer AST | 2.15 | 1.03 | 1.06 |
| `ast-shunting-yard` | pointer AST | 2.23 | 1.04 | 1.18 |
| `ast-pratt` | pointer AST | 2.20 | 1.06 | **1.00** |
| `ast-arena` | arena AST | 1.03 | 1.19 | 1.17 |
| `multipass` | pointer AST | 2.91 | 2.47 | 1.44 |
| `multipass-arena` | arena AST | 1.63 | 2.63 | 1.62 |
| `multipass-bfs` | arena AST | 1.76 | 3.09 | 1.70 |
| `multipass-reverse` | arena AST | 1.23 | 1.41 | 1.40 |
| `multipass-reverse-fold` | arena AST | **1.00** | **1.00** | 1.29 |

**The fused form, `multipass-reverse-fold`, is the fastest tree builder of
nine in C++ and in Python** — ahead of `ast-arena` by ~2–4 % and of
`ast-shunting-yard` by ~3–8 %, a small but real lead confirmed across three
independent CI runs at n=100/1,000/10,000 (always positive, never a coin
flip). At n=10 the margin is noise — sign flips run to run — so read the win
as holding from n=100 up, not "at every size." The buffered `multipass-reverse`
sits ~1.2× behind in C++. Layout is still the central cross-language finding:
contiguous memory is the whole game in C++ (~2× tier gap), and boxed/GC'd
runtimes hide it — in Haskell the pointer classics lead every arena form,
fold included, by ~1.3×. Adding cores reorders nothing — the ranking is
identical at W=1 and W=4 in all three languages.

Its no-tree twin `direct-reverse` is, honestly, a **tie** with
`direct-recursive-descent` on the C++ corpus — the same three-run check that
confirmed the tree-tier win shows this one flipping sign at every size (e.g.
n=1,000: +2.2 %, +0.3 %, +0.0 %), so there is no real edge to claim, in either
direction. On the Python corpus the win is real and repeats across all three
runs, ~9–12 % over `direct-shunting-yard` at every size. On the structured
shapes below, the picture is mixed and shape-dependent rather than a clean
"wins two, loses two": powchain and towerchain are robust wins (double digits
over `direct-shunting-yard`, low-to-mid single digits or better over
`direct-recursive-descent`), sumchain is a robust loss to `direct-rd`
(~5–9 %, consistent across runs), and nestchain vs. `direct-sy` is itself a
coin flip (−5 % to +11 % across the three runs) — not a dependable loss.

## Result 2 — vs its family: strictly better

The top-down `multipass` variants build the same tree by scanning for the
loosest operator and splitting. That scan is attackable: a flat
mixed-precedence chain (`3^2 * 2^2 / 2^2 * …` — a factored monomial) makes it
**Θ(n²)**. C++ powchain, m=8192, ns/leaf, neutral runner:

| strategy | before the rescue patch | after | worst-case machinery |
|---|--:|--:|---|
| `multipass` | 3 791 | 166 | scan budget + buckets |
| `multipass-arena` | 4 864 | 87 | budget + buckets + AVX2 |
| `direct-mp` | 3 236 | 70 | budget + buckets + AVX2 |
| `multipass-bfs` | 84 † | 90 | O(n log n) sparse-table RMQ |
| **`multipass-reverse`** | **43** | **42** | **none** |
| **`multipass-reverse-fold`** | — (new) | **36** | **none** |

`multipass-reverse` (and its fused form) are **the only members of the family
whose worst case is their average case**. The others needed bounded scans,
precedence buckets and AVX2 to go linear — and still trail by 1.7–4×. Its own worst-case shape (deep
parenthesis nesting, the one construct where it recurses) is also benchmarked:
flat there too — and `multipass-reverse-fold` is faster than every other
tree builder there: 38 vs `ast-arena` 62 ns/leaf at m=8192, ~0.6× the
time, since a nesting level costs one frame push instead of ~5 call frames.
(† `multipass-bfs`'s O(1) splits dodge the powchain but a
`^`-tower shape catches it the same way; the "before" column is the last
pre-fix CI run — the shipped code reproduces the "after" column.
[Details.](FINDINGS.md#result-2--vs-its-family-strictly-better))

Correctness is enforced by curated spec suites in all three languages plus
differential fuzzing in C++ and Python: every strategy must agree — value or
rejection — on 6 000 random and mutated inputs per run (C++ also fuzzes its
ten intra-expression parallel variants on inputs long enough to fork).

## Scope

This is a *specialized* parser for one fixed grammar — numbers, five binary
operators, unary ±. The one-sweep-per-precedence-level structure is tuned to
exactly that and is **not** a general-purpose technique: richer grammars
(function calls, statements, many precedence levels) stay recursive descent's
and Pratt's home turf. The results above are claims about that narrow job —
whether the approach generalizes is an open question, not a claim.

## Also in [FINDINGS.md](FINDINGS.md)

- **Compile once, evaluate many:** any reusable form beats re-parsing ~7–26×
  (break-even ≲ 5 evals); the best form is runtime-specific.
- **Multi-core:** C++/Haskell threads ~2–2.9×@4, Python processes ~2×,
  Python threads flat (the GIL, live).
- Grammar, build instructions, per-language deep dives, full tables.
