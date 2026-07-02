<div align="center">

# 🧮 Math-Expression Parser & Evaluator

**Twelve ways to turn `-2 ^ 2 + 3 * (4 - 1)` into `5`, in C++, Haskell, and Python.**

</div>

This repo tests one new algorithm — **`multipass-reverse`**, a bottom-up
expression parser — against the classic parsers and against its own top-down
family. Grammar, builds, full tables and analysis: **[FINDINGS.md](FINDINGS.md)**.

## The algorithm

`multipass-reverse` reduces the *tightest-binding* constructs first — deepest
parentheses, then `^`, then `*` `/`, then `+` `-` — one in-place sweep per
precedence level, until a single node remains. Same tree as every other parser,
built in the opposite order. It never searches for a split point, so it is
**Θ(n) on every input, with no fallback machinery**.
([C++](cpp/src/multipass_reverse.cpp) ·
[Python](python/mathparser/evaluators.py) ·
[Haskell](haskell/src/MathParser/Strategies.hs) ·
**[full walk-through](docs/multipass-reverse.md)**)

## Result 1 — vs the classics: competitive

Sixty-year-old, maximally-tuned algorithms on their home turf: random corpus,
ns/leaf at n=1000, neutral 4-vCPU CI runner, normalised to the fastest
tree-builder per language (**bold** = fastest):

| tree builder | representation | C++ | Python | Haskell |
|---|---|--:|--:|--:|
| `ast-recursive-descent` | pointer AST | 2.13 | 1.00 | **1.00** |
| `ast-shunting-yard` | pointer AST | 2.23 | **1.00** | 1.11 |
| `ast-pratt` | pointer AST | 2.10 | 1.04 | 1.03 |
| `ast-arena` | arena AST | **1.00** | 1.15 | 1.26 |
| `multipass` | pointer AST | 2.81 | 2.28 | 1.28 |
| `multipass-arena` | arena AST | 1.58 | 2.44 | 1.53 |
| `multipass-bfs` | arena AST | 1.72 | 2.75 | 1.76 |
| `multipass-reverse` | arena AST | 1.18 | 1.37 | 1.36 |

**Second-fastest tree builder of eight in C++, ahead of every pointer-AST
classic** — behind only `ast-arena`, which shares its arena layout. The layout
is also the repo's central cross-language finding: contiguous memory is the
whole game in C++ (~2× tier gap), and boxed/GC'd runtimes hide it, so in
Python/Haskell the classics lead by ~1.3× and the pointer forms cluster within
~5% of each other. Adding cores reorders nothing — the ranking is identical at
W=1 and W=4 in all three languages.

## Result 2 — vs its family: strictly better

The top-down `multipass` variants build the same tree by scanning for the
loosest operator and splitting. That scan is attackable: a flat
mixed-precedence chain (`3^2 * 2^2 / 2^2 * …` — a factored monomial) makes it
**Θ(n²)**. C++ powchain, m=8192, ns/leaf, neutral runner:

| strategy | before the rescue patch | after | worst-case machinery |
|---|--:|--:|---|
| `multipass` | 3 791 | 146 | scan budget + buckets |
| `multipass-arena` | 4 864 | 83 | budget + buckets + AVX2 |
| `direct-mp` | 3 236 | 69 | budget + buckets + AVX2 |
| `multipass-bfs` | 84 † | 89 | O(n log n) sparse-table RMQ |
| **`multipass-reverse`** | **43** | **41** | **none** |

`multipass-reverse` is **the only member of the family whose worst case is its
average case**. The others needed bounded scans, precedence buckets and AVX2 to
go linear — and still trail by 1.7–3.6×. Its own worst-case shape (deep
parenthesis nesting, the one construct where it recurses) is also benchmarked:
flat there too. († `multipass-bfs`'s O(1) splits dodge the powchain but a
`^`-tower shape catches it the same way; the "before" column is the last
pre-fix CI run — the shipped code reproduces the "after" column.
[Details.](FINDINGS.md#result-2--vs-its-family-strictly-better))

Correctness is enforced by differential fuzzing in CI: all twelve strategies
must agree — value or rejection — on 6 000 random and mutated inputs per
language per run.

## Also in [FINDINGS.md](FINDINGS.md)

- **Compile once, evaluate many:** any reusable form beats re-parsing ~7–26×
  (break-even ≲ 5 evals); the best form is runtime-specific.
- **Multi-core:** C++/Haskell threads ~2–2.9×@4, Python processes ~2×,
  Python threads flat (the GIL, live).
- Grammar, build instructions, per-language deep dives, full tables.
