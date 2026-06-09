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

ns/leaf on the shared corpora, one run on a throttling i7-10610U — **noisy
(wall-clock, single run); trust the tiers, not the digits.** The
[CI bench](../.github/workflows/bench.yml) regenerates these on a neutral
runner. Reproduce locally with `cabal run bench`.

| strategy | n=10 | n=100 | n=1000 | n=10000 |
|---|--:|--:|--:|--:|
| ast-recursive-descent | 4133 | 3751 | 5788 | 7906 |
| ast-shunting-yard | 6161 | 6298 | 7657 | 10823 |
| ast-pratt | 6088 | 6559 | 6248 | 7852 |
| ast-arena | 5529 | 5221 | 6327 | 8412 |
| multipass | 6306 | 6299 | 7023 | 11095 |
| multipass-arena | 7534 | 6961 | 8595 | 14863 |
| direct-mp | 7546 | 7352 | 8182 | 10684 |
| multipass-bfs | 9211 | 8262 | 10165 | 20316 |
| multipass-reverse | 6897 | 6200 | 6747 | 11164 |
| direct-recursive-descent | 5559 | 5414 | 5213 | **5151** |
| direct-shunting-yard | 4695 | 4679 | 4878 | 8448 |
| **bytecode-vm** | 4866 | 5174 | **4663** | 6831 |

Correctness: all corpus expressions agree across all 12 strategies. The spread is
much tighter than C++ (~2× fastest-to-slowest vs ~5×) — GC and laziness
overhead dominate. `multipass-reverse` is among the better mp variants here
(beats `multipass-arena`/`-bfs` at scale); the `multipass-bfs` blow-up at
n=10000 is the sparse-table build cost showing through.

## What changes versus C++

- **The arena trick disappears** — `ast-arena` ≈ `ast-recursive-descent`. A flat
  `Array` of boxed, GC'd nodes is no cheaper than the `Expr` tree; the C++ win
  was about contiguous memory *layout*, which a managed runtime hides.
- **The sparse-table `multipass-bfs` is the slowest at scale** — building the
  `Array`-based RMQ costs more than the linear split scan it replaces, exactly as
  in C++. Cleverness loses to allocation in every runtime.
- **The spread compresses** to ~2-3× (vs C++'s ~5×): GC and laziness overhead
  dominate, shrinking the gaps between algorithms.

See the top-level [README](../README.md) for the cross-language table.
