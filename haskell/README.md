# Haskell implementation

Idiomatic Haskell port of all fifteen strategies (GHC, `base`/`array`/`containers`/`time`).

```sh
python3 bench/gen_corpus.py        # from repo root: generate shared corpora (once)
cd haskell
cabal test                         # correctness (615 checks) + differential fuzz
cabal run bench                    # cross-check + timing on shared corpora
cabal run adversarial              # 4 structured shapes: top-down worst cases (now O(n log n)), nestchain, vs reverse Θ(n)
```

### Toolchain

Needs GHC ≥ 9.2 + cabal (e.g. via [`ghcup`](https://www.haskell.org/ghcup/)).
GHC links against **libgmp**; if you hit `cannot find -lgmp`, install the dev
package (`sudo apt install libgmp-dev` on Debian/Ubuntu) or use a
native-bignum GHC bindist. No Hackage packages are needed — everything resolves
against GHC's boot libraries, so `cabal build` works offline.

## Design

Representation is abstracted **tagless-final** via the `Sym` class, so each parse
algorithm is written once and instantiated at three carriers:

| carrier | what it is | strategies |
|---|---|---|
| `Expr` | the algebraic data type (pointer-AST analog) | `ast-*`, `multipass` |
| `Arena` | a state-threaded flat node array, children by `Int` index | `ast-arena`, `multipass-arena`, `multipass-bfs`, `multipass-reverse`, `multipass-reverse-fold` |
| `Direct` | an `Env -> Double` closure — no tree | `direct-*` |

Drivers: `rdParse` (recursive descent), `prattParse`, `syParse` (shunting-yard),
`mpRun` (top-down divide & conquer, with an optional `Data.Array` sparse-table
RMQ for `multipass-bfs`), `reverseMpParse` (bottom-up reduction,
innermost/highest precedence first → `multipass-reverse`; algorithm explained in
[docs/multipass-reverse.md](../docs/multipass-reverse.md)), and `reverseFoldParse`
(the fused single-sweep form of `reverseMpParse` → `multipass-reverse-fold` and
`direct-reverse`). `bytecode-vm` compiles to an instruction list and runs it on
a value stack. `scanParse` fuses the lexer into the grammar for
`direct-scannerless`, the lexer-cost control row. Arithmetic matches
C++ `double` semantics exactly: `x/0 = inf`, and `^` is GHC's `**`, which is
libm `pow` — bit-identical to `std::pow`, including negative bases with
integral exponents.

## Benchmark (measured)

ns/leaf on the shared corpora, neutral GitHub runner (4 vCPU) via the
[CI bench](../.github/workflows/bench.yml) — **noisy (wall-clock); trust the
tiers, not the digits.** Reproduce locally with `cabal run bench`.

| strategy | n=10 | n=100 | n=1000 | n=10000 |
|---|--:|--:|--:|--:|
| ast-recursive-descent | 526 | 514 | **458** | **424** |
| ast-shunting-yard | 604 | 598 | 559 | 634 |
| ast-pratt | **526** | 520 | 471 | 444 |
| ast-arena | 563 | 550 | 515 | 498 |
| multipass | 790 | 808 | 830 | 1 050 |
| multipass-arena | 875 | 892 | 921 | 1 210 |
| direct-mp | 846 | 858 | 868 | 1 119 |
| multipass-bfs | 1 033 | 1 064 | 1 115 | 1 665 |
| multipass-reverse | 754 | 733 | 721 | 890 |
| multipass-reverse-fold | 615 | 584 | 532 | 565 |
| direct-recursive-descent | 531 | **510** | 460 | 425 |
| direct-shunting-yard | 611 | 599 | 568 | 662 |
| direct-reverse | 563 | 525 | 482 | 478 |
| bytecode-vm | 628 | 598 | 583 | 664 |
| *direct-scannerless* (control) | *485* | *454* | *412* | *426* |

Median of three CI runs on one CPU model, AMD EPYC 9V74 (43186179671 /
43186221304 / 43201629859, 2026-11-12, commit a7f2a16, after the
[same-rules tuning pass](../bench/TUNING.md), which made the recursive-descent
and Pratt parsers strict in their (result, rest) pairs). Correctness: all
corpus expressions agree across all 15 strategies. The leaders are a
cluster: the pointer classics `ast-recursive-descent` and `ast-pratt` and the
no-tree `direct-recursive-descent` take the four sizes between them, within
~5 % of each other, with `direct-reverse` 2–13 % behind `direct-rd` in every
run (most at n=10000). Before 2026-09-25 the `direct-*` forms led here. The registry helpers
that build the tree strategies were then marked `INLINE`, and GHC Core shows
their dictionary-passing workers gone (`$wmkAst`/`$wmkArena`: 11 → 0); since
then the pointer classics sit level with or ahead of `direct-rd`.

Absolute numbers are not comparable across batches: on the same CPU model
whole runs have differed by ~1.3× (one of the three 2026-09-26 runs), and the 64 MB
nursery (`-with-rtsopts=-A64m`) and round-robin timing (2026-09-26) moved
them again. What that switch bought is a 5–10× smaller run-to-run spread in
the ratios: the tiers hold in every run (the fold behind `ast-rd`,
`direct-reverse` behind `direct-rd`), though the order inside the leading
cluster is too close to call. The n=10000 column is the least clean — the
heap carries over from one strategy to the next within a round, and the
arena forms lose most ground there. The spread is ~2.2× fastest-to-slowest
on the median (vs C++'s ~4×, Python's ~2.5×); the pointer classics lead
every arena form, from ~1.07–1.17× at the tight end (`ast-arena`) to
~2.0–3.9× at the wide end (`multipass-bfs`), and "no tree" buys nothing
here. `multipass-reverse` beats `multipass-arena`/`-bfs` at every size; the
`multipass-bfs` growth at n=10000 is the sparse-table build cost. The
lexer-free `direct-scannerless` is a control, not a contender: it sits
~9–11 % below `direct-rd` up to n=1000 (level with it at n=10000) — the
lazy token
list already fuses with its consumer, so the lexer is a smaller share here
than in the other three languages.

### The arena carrier

All five arena strategies share one `Arena` carrier, so a change there lands
on each of them alike; the fold's own code is not touched by it. Until
2026-09-26 the carrier threaded its state as a lazy `(Int, [Node])` pair,
which left a thunk per node for `evalArena` to force, and `evalArena`
reversed the node list before indexing it. The carrier now uses strict
constructors (`ArenaSt`, `Emitted`, strict `Node` fields) and indexes the
newest-first list directly. On EPYC 7763 CI runs, at n=1000, `ast-arena`
went from 1.20× to 1.10× `ast-rd` and `multipass-reverse-fold` from 1.19× to
1.11× (2 runs before, 4 after; the strategies that do not use the carrier
moved by 3 % or less). A struct-of-arrays
version in `ST` was tried and was slower (`ast-arena` 1.22× → 1.39× in a
local A/B), so it was dropped. The gap left is structural: the carrier
builds a closure tree and then serialises it, so it cannot beat building
`Expr` directly without rewriting the drivers monadically.

## What changes versus C++

- **The arena trick does not pay** — `ast-arena` (651 @ n=1000) is still
  *slower* than the pointer-AST `Expr` builders (`ast-pratt` 603,
  `ast-recursive-descent` 594), by ~1.1× since the carrier went strict. A flat
  `Array` of boxed, GC'd nodes is no cheaper than the tree; the C++ win was
  about contiguous memory *layout*, which a managed runtime hides.
- **"No tree" does not win here.** In C++ the `direct-*` forms are fastest;
  in Haskell `ast-recursive-descent` builds a tree and still leads at two
  of four sizes (`ast-pratt` and `direct-rd` take one each); on the median
  over sizes those three are within ~2 % of each other, with `direct-reverse`
  ~5 % behind. With
  every node boxed and GC'd, *not* allocating the tree buys little of the
  layout advantage it has in the unmanaged languages.
- **The sparse-table `multipass-bfs` is the slowest at scale** — building the
  `Array`-based RMQ costs more than the linear split scan it replaces, exactly as
  in C++. The precompute loses to a plain linear scan in every runtime.
- **The spread is ~2.2× on the median** (vs C++'s ~3.9×), about
  Python's ~2.5×. It read ~1.7× until 2026-09-02, when the lexer's
  `reads`-based number parsing was replaced: a shared constant cost had been
  compressing every gap.

See the top-level [README](../README.md) for the cross-language table and the
[one-pager](../docs/one-pager.md) for the cross-language verdict and
scoreboard.
