# Haskell implementation

Idiomatic Haskell port of all fifteen strategies (GHC, `base`/`array`/`containers`/`time`).

```sh
python3 bench/gen_corpus.py        # from repo root: generate shared corpora (once)
cd haskell
cabal test                         # correctness (540 checks)
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
| ast-recursive-descent | **398** | **401** | 432 | **424** |
| ast-shunting-yard | 489 | 490 | 601 | 798 |
| ast-pratt | 424 | 474 | 433 | 486 |
| ast-arena | 489 | 516 | 549 | 885 |
| multipass | 759 | 773 | 891 | 1515 |
| multipass-arena | 885 | 886 | 1112 | 1944 |
| direct-mp | 764 | 807 | 898 | 1528 |
| multipass-bfs | 1124 | 1123 | 1455 | 2945 |
| multipass-reverse | 676 | 696 | 764 | 1495 |
| multipass-reverse-fold | 521 | 535 | 566 | 783 |
| direct-recursive-descent | 414 | 449 | **405** | 592 |
| direct-shunting-yard | 494 | 527 | 516 | 888 |
| direct-reverse | 469 | 446 | 474 | 579 |
| bytecode-vm | 514 | 573 | 646 | 834 |
| *direct-scannerless* (control) | *388* | *385* | *444* | *619* |

Median of three CI runs (36163486136 / 36163570591 / 36163577544). Correctness:
all corpus expressions agree across all 15 strategies. The pointer classic
`ast-recursive-descent` is the fastest strategy at n=10, 100 and 10000, with
`direct-recursive-descent` fastest at n=1000 and `direct-reverse` close
behind both (noisy across runs, see the one-pager). Before 2026-09-25 the
`direct-*` forms led here. The registry helpers that build the tree
strategies were then marked `INLINE`, and GHC Core shows their
dictionary-passing workers gone (`$wmkAst`/`$wmkArena`: 11 → 0); in the
next CI batch the pointer classics moved from behind `direct-rd` to level
with or ahead of it. The Core change is certain, but with this runner's
noise the size of the effect is not. The spread is ~3.1×
fastest-to-slowest on the median (vs C++'s ~4.7×), about Python's ~3.0×;
the pointer classics lead every arena form, from ~1.3× at the tight end
(`ast-arena`) to ~3.1× at the wide end (`multipass-bfs`), and "no tree"
buys nothing here. `multipass-reverse` beats `multipass-arena`/`-bfs`
at every size; the `multipass-bfs` blow-up at n=10000 is the sparse-table
build cost. The lexer-free `direct-scannerless` is a control, not a
contender: it lands within a few percent of `direct-rd`/`direct-reverse`,
noisily — the lazy token list already fuses with its consumer.

## What changes versus C++

- **The arena trick disappears** — `ast-arena` (549 @ n=1000) is *slower* than
  the pointer-AST `Expr` builders (`ast-pratt` 433, `ast-recursive-descent`
  432). A flat `Array` of boxed, GC'd nodes is no cheaper than the tree; the C++
  win was about contiguous memory *layout*, which a managed runtime hides.
- **"No tree" does not win here.** In C++ the `direct-*` forms are fastest;
  in Haskell `ast-recursive-descent` builds a tree and still leads at three
  of four sizes, with `direct-rd` and `direct-reverse` ~5–15 % behind it on
  the median over sizes (and ~40 % behind at n=10000, where GC dominates).
  With every node boxed and GC'd, *not* allocating the tree buys little of
  the layout advantage it has in the unmanaged languages, and the ranking
  among the leaders is close enough on this runner to be sensitive to which
  run you read.
- **The sparse-table `multipass-bfs` is the slowest at scale** — building the
  `Array`-based RMQ costs more than the linear split scan it replaces, exactly as
  in C++. The precompute loses to a plain linear scan in every runtime.
- **The spread is ~3.1× on the median** (vs C++'s ~4.7×), about
  Python's ~3.0×. It read ~1.7× until 2026-09-02, when the lexer's
  `reads`-based number parsing was replaced: a shared constant cost had been
  compressing every gap.

See the top-level [README](../README.md) for the cross-language table and the
[one-pager](../docs/one-pager.md) for the cross-language verdict and
scoreboard.
