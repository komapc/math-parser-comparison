# Haskell implementation

Idiomatic Haskell port of all twelve strategies (GHC, `base`/`array`/`containers`/`time`).

```sh
python3 bench/gen_corpus.py        # from repo root: generate shared corpora (once)
cd haskell
cabal test                         # correctness (252 checks)
cabal run bench                    # cross-check + timing on shared corpora
cabal run adversarial              # structured chains: multipass top-down Θ(n²) vs reverse Θ(n)
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
innermost/highest precedence first → `multipass-reverse`; algorithm explained in
[docs/multipass-reverse.md](../docs/multipass-reverse.md)). `bytecode-vm`
compiles to an instruction list and runs it on a value stack. Arithmetic matches
C++ `double` semantics (`x/0 = inf`, integral `^` keeps a negative base's sign via `^^`).

## Benchmark (measured)

ns/leaf on the shared corpora, neutral GitHub runner (4 vCPU) via the
[CI bench](../.github/workflows/bench.yml) — **noisy (wall-clock); trust the
tiers, not the digits.** Reproduce locally with `cabal run bench`.

| strategy | n=10 | n=100 | n=1000 | n=10000 |
|---|--:|--:|--:|--:|
| **ast-recursive-descent** | **981** | **977** | 1144 | **1085** |
| ast-shunting-yard | 1081 | 1104 | 1182 | 1506 |
| **ast-pratt** | 1012 | 1029 | **1139** | 1169 |
| ast-arena | 1160 | 1174 | 1370 | 1639 |
| multipass | 1348 | 1365 | 1566 | 2137 |
| multipass-arena | 1560 | 1547 | 1799 | 2987 |
| direct-mp | 1398 | 1391 | 1518 | 2155 |
| multipass-bfs | 1806 | 1722 | 1959 | 3999 |
| multipass-reverse | 1360 | 1396 | 1635 | 2389 |
| direct-recursive-descent | 994 | 1015 | 1230 | 1176 |
| direct-shunting-yard | 1069 | 1109 | 1299 | 1602 |
| bytecode-vm | 1094 | 1120 | 1304 | 1485 |

Correctness: all corpus expressions agree across all 12 strategies. The spread is
much tighter than C++ (~1.8× fastest-to-slowest on the median vs ~4.9×) — GC and
laziness overhead dominate. `multipass-reverse` is among the better mp variants
here (beats `multipass-arena`/`-bfs` at scale, ties `multipass`/`direct-mp`); the
`multipass-bfs` blow-up at n=10000 is the sparse-table build cost showing through.

## What changes versus C++

- **The arena trick disappears** — `ast-arena` (1370 @ n=1000) is *slower* than
  the pointer-AST `Expr` builders (`ast-recursive-descent` 1144, `ast-pratt`
  1139). A flat `Array` of boxed, GC'd nodes is no cheaper than the tree; the C++
  win was about contiguous memory *layout*, which a managed runtime hides.
- **"No tree" stops winning, too.** In C++ the `direct-*` forms are fastest; in
  Haskell a pointer-AST builder is nominally fastest and `direct-rd`/`bytecode-vm`
  sit just behind. With every node boxed and GC'd, *not* allocating the tree no
  longer buys a layout advantage — the top five strategies are a ~15% near-tie.
- **The sparse-table `multipass-bfs` is the slowest at scale** — building the
  `Array`-based RMQ costs more than the linear split scan it replaces, exactly as
  in C++. The precompute loses to a plain linear scan in every runtime.
- **The spread compresses** to ~1.8× on the median (vs C++'s ~4.9×): GC and
  laziness overhead dominate, shrinking the gaps between algorithms.

See the top-level [README](../README.md) for the cross-language table.
