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
| ast-recursive-descent | 584 | 583 | 688 | 806 |
| ast-shunting-yard | 696 | 677 | 749 | 958 |
| ast-pratt | 609 | 578 | 678 | 681 |
| ast-arena | 709 | 685 | 837 | 1033 |
| multipass | 1007 | 944 | 1104 | 1572 |
| multipass-arena | 1152 | 1057 | 1286 | 2132 |
| direct-mp | 1059 | 972 | 1131 | 1631 |
| multipass-bfs | 1410 | 1282 | 1471 | 3347 |
| multipass-reverse | 925 | 879 | 893 | 1643 |
| multipass-reverse-fold | 734 | 681 | 815 | 1006 |
| direct-recursive-descent | **562** | **549** | 504 | 636 |
| direct-shunting-yard | 658 | 639 | 725 | 932 |
| direct-reverse | 585 | 560 | **489** | **610** |
| bytecode-vm | 673 | 642 | 770 | 854 |
| *direct-scannerless* (control) | *501* | *464* | *574* | *615* |

Median of three CI runs (34130395595 / 34131316618 / 34132166596). Correctness:
all corpus expressions agree across all 15 strategies. These numbers are
~1.6–2.9× lower than the ones this table carried before 2026-09-02: the shared
lexer parsed numbers with `reads`, which goes through `Rational` and was about
two-thirds of the fastest strategies' time; it now takes Clinger's fast path
(bit-identical, `reads` remains the fallback). With that constant gone the
spread is ~2.6× fastest-to-slowest on the median (vs C++'s ~6×), tighter than
Python's ~3.5×; the pointer classics lead every arena form, from ~1.2× at the
tight end (`ast-arena`) to ~2.2× at the wide end (`multipass-bfs`), and "no
tree" buys little here. `multipass-reverse` beats `multipass-arena`/`-bfs` at
every size; the `multipass-bfs` blow-up at n=10000 is the sparse-table build
cost. The lexer-free `direct-scannerless` is a control, not a contender: it
lands within 2–8 % of `direct-rd`, noisily — the lazy token list already
fuses with its consumer.

## What changes versus C++

- **The arena trick disappears** — `ast-arena` (837 @ n=1000) is *slower* than
  the pointer-AST `Expr` builders (`ast-pratt` 678, `ast-recursive-descent`
  688). A flat `Array` of boxed, GC'd nodes is no cheaper than the tree; the C++
  win was about contiguous memory *layout*, which a managed runtime hides.
- **"No tree" is closer to winning here now.** In C++ the `direct-*` forms
  are fastest; in Haskell `direct-rd` and `direct-reverse` are fastest at
  three of the four sizes too, though `ast-pratt` stays close at the small
  and largest sizes (`bytecode-vm` further back). With every node boxed and
  GC'd, *not* allocating the tree buys less of a layout advantage than in the
  unmanaged languages, and the ranking among the leaders is noisy enough on
  this runner to flip size to size.
- **The sparse-table `multipass-bfs` is the slowest at scale** — building the
  `Array`-based RMQ costs more than the linear split scan it replaces, exactly as
  in C++. The precompute loses to a plain linear scan in every runtime.
- **The spread is ~2.6× on the median** (vs C++'s ~6×), tighter than Python's
  ~3.5×. It read ~1.7× until 2026-09-02, when the lexer's `reads`-based number
  parsing was replaced: a shared constant cost had been compressing every gap.

See the top-level [README](../README.md) for the cross-language table and the
[one-pager](../docs/one-pager.md) for the cross-language verdict and
scoreboard.
