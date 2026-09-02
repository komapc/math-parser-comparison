# Haskell implementation

Idiomatic Haskell port of all fifteen strategies (GHC, `base`/`array`/`containers`/`time`).

```sh
python3 bench/gen_corpus.py        # from repo root: generate shared corpora (once)
cd haskell
cabal test                         # correctness (480 checks)
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
RMQ for `multipass-bfs`), and `reverseMpParse` (bottom-up reduction,
innermost/highest precedence first → `multipass-reverse`; algorithm explained in
[docs/multipass-reverse.md](../docs/multipass-reverse.md)). `bytecode-vm`
compiles to an instruction list and runs it on a value stack. Arithmetic matches
C++ `double` semantics exactly: `x/0 = inf`, and `^` is GHC's `**`, which is
libm `pow` — bit-identical to `std::pow`, including negative bases with
integral exponents.

## Benchmark (measured)

ns/leaf on the shared corpora, neutral GitHub runner (4 vCPU) via the
[CI bench](../.github/workflows/bench.yml) — **noisy (wall-clock); trust the
tiers, not the digits.** Reproduce locally with `cabal run bench`.

| strategy | n=10 | n=100 | n=1000 | n=10000 |
|---|--:|--:|--:|--:|
| ast-recursive-descent | **504** | 505 | 589 | 479 |
| ast-shunting-yard | 620 | 615 | 686 | 899 |
| ast-pratt | 529 | **484** | 466 | **476** |
| ast-arena | 682 | 637 | 689 | 1037 |
| multipass | 952 | 875 | 1035 | 1507 |
| multipass-arena | 1161 | 1059 | 1285 | 2137 |
| direct-mp | 1004 | 933 | 986 | 1626 |
| multipass-bfs | 1424 | 1251 | 1458 | 3223 |
| multipass-reverse | 923 | 875 | 892 | 1682 |
| multipass-reverse-fold | 733 | 676 | 804 | 1041 |
| direct-recursive-descent | 528 | 516 | **432** | 613 |
| direct-shunting-yard | 634 | 575 | 552 | 883 |
| direct-reverse | 567 | 505 | 474 | 618 |
| bytecode-vm | 618 | 565 | 576 | 871 |
| *direct-scannerless* (control) | *469* | *400* | *424* | *610* |

Median of three CI runs. Correctness: all corpus expressions agree across all
15 strategies. These numbers are ~1.6–2.9× lower than the ones this table
carried before 2026-09-02: the shared lexer parsed numbers with `reads`, which
goes through `Rational` and was about two-thirds of the fastest strategies'
time; it now takes Clinger's fast path (bit-identical, `reads` remains the
fallback). With that constant gone the spread is ~3× fastest-to-slowest, the
same as Python's; the pointer classics lead every arena form by ~1.5×, and
"no tree" buys nothing here. `multipass-reverse` beats
`multipass-arena`/`-bfs` at every size; the `multipass-bfs` blow-up at
n=10000 is the sparse-table build cost. The lexer-free `direct-scannerless`
is a control, not a contender: it lands within 5–15 % of `direct-rd` — the
lazy token list already fuses with its consumer.

## What changes versus C++

- **The arena trick disappears** — `ast-arena` (689 @ n=1000) is *slower* than
  the pointer-AST `Expr` builders (`ast-pratt` 466, `ast-recursive-descent`
  589). A flat `Array` of boxed, GC'd nodes is no cheaper than the tree; the C++
  win was about contiguous memory *layout*, which a managed runtime hides.
- **"No tree" stops winning, too.** In C++ the `direct-*` forms are fastest; in
  Haskell a pointer-AST builder is nominally fastest and `direct-rd` /
  `direct-reverse` sit just behind (`bytecode-vm` ~20 % back). With every node
  boxed and GC'd, *not* allocating the tree no longer buys a layout advantage —
  the top four strategies are a ~10 % near-tie on the median.
- **The sparse-table `multipass-bfs` is the slowest at scale** — building the
  `Array`-based RMQ costs more than the linear split scan it replaces, exactly as
  in C++. The precompute loses to a plain linear scan in every runtime.
- **The spread is ~3× on the median** (vs C++'s ~4.8×), the same as Python's.
  It read ~1.7× until 2026-09-02, when the lexer's `reads`-based number parsing
  was replaced: a shared constant cost had been compressing every gap.

See the top-level [README](../README.md) for the cross-language table.
