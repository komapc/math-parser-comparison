# Haskell implementation

Idiomatic Haskell port of all twelve strategies (GHC, `base`/`array`/`containers`/`time`).

```sh
python3 bench/gen_corpus.py        # from repo root: generate shared corpora (once)
cd haskell
cabal test                         # correctness (252 checks)
cabal run bench                    # cross-check + timing on shared corpora
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
| `Arena` | a state-threaded flat node array, children by `Int` index | `ast-arena`, `multipass-arena`, `multipass-bfs`, `multipass-reverse` |
| `Direct` | an `Env -> Double` closure — no tree | `direct-*` |

Drivers: `rdParse` (recursive descent), `prattParse`, `syParse` (shunting-yard),
`mpRun` (top-down divide & conquer, with an optional `Data.Array` sparse-table
RMQ for `multipass-bfs`), and `reverseMpParse` (bottom-up reduction,
innermost/highest precedence first → `multipass-reverse`). `bytecode-vm`
compiles to an instruction list and runs it on a value stack. Arithmetic matches
C++ `double` semantics (`x/0 = inf`, integral `^` keeps a negative base's sign via `^^`).

## Benchmark (measured)

ns/leaf on the shared corpora, neutral GitHub runner (4 vCPU) via the
[CI bench](../.github/workflows/bench.yml) — **noisy (wall-clock); trust the
tiers, not the digits.** Reproduce locally with `cabal run bench`.

| strategy | n=10 | n=100 | n=1000 | n=10000 |
|---|--:|--:|--:|--:|
| **ast-recursive-descent** | **1229** | **1212** | 1408 | 1350 |
| ast-shunting-yard | 1391 | 1342 | 1527 | 1645 |
| **ast-pratt** | 1288 | 1252 | **1274** | **1296** |
| ast-arena | 1445 | 1416 | 1734 | 1840 |
| multipass | 1601 | 1522 | 1781 | 2374 |
| multipass-arena | 1759 | 1728 | 2062 | 3481 |
| direct-mp | 1623 | 1547 | 1834 | 2343 |
| multipass-bfs | 1949 | 1920 | 2369 | 4718 |
| multipass-reverse | 1655 | 1599 | 1821 | 2554 |
| direct-recursive-descent | 1318 | 1291 | 1486 | 1346 |
| direct-shunting-yard | 1336 | 1328 | 1532 | 2021 |
| bytecode-vm | 1371 | 1354 | 1542 | 1650 |

Correctness: all corpus expressions agree across all 12 strategies. The spread is
much tighter than C++ (~1.7× fastest-to-slowest on the median vs ~4.7×) — GC and
laziness overhead dominate. `multipass-reverse` is among the better mp variants
here (beats `multipass-arena`/`-bfs` at scale); the `multipass-bfs` blow-up at
n=10000 is the sparse-table build cost showing through.

## What changes versus C++

- **The arena trick disappears** — `ast-arena` (1734 @ n=1000) is *slower* than
  the pointer-AST `Expr` builders (`ast-recursive-descent` 1408, `ast-pratt`
  1274). A flat `Array` of boxed, GC'd nodes is no cheaper than the tree; the C++
  win was about contiguous memory *layout*, which a managed runtime hides.
- **"No tree" stops winning, too.** In C++ the `direct-*` forms are fastest; in
  Haskell a pointer-AST builder is nominally fastest and `direct-rd`/`bytecode-vm`
  sit just behind. With every node boxed and GC'd, *not* allocating the tree no
  longer buys a layout advantage — the top five strategies are a ~15% near-tie.
- **The sparse-table `multipass-bfs` is the slowest at scale** — building the
  `Array`-based RMQ costs more than the linear split scan it replaces, exactly as
  in C++. The precompute loses to a plain linear scan in every runtime.
- **The spread compresses** to ~1.7× on the median (vs C++'s ~4.7×): GC and
  laziness overhead dominate, shrinking the gaps between algorithms.

See the top-level [README](../README.md) for the cross-language table.
