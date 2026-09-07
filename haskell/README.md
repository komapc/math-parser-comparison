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
| ast-recursive-descent | 536 | 552 | 583 | 744 |
| ast-shunting-yard | 627 | 606 | 679 | 949 |
| ast-pratt | 564 | 540 | 648 | 623 |
| ast-arena | 667 | 612 | 754 | 998 |
| multipass | 906 | 871 | 972 | 1534 |
| multipass-arena | 1036 | 982 | 1184 | 2019 |
| direct-mp | 948 | 896 | 993 | 1584 |
| multipass-bfs | 1298 | 1207 | 1393 | 3081 |
| multipass-reverse | 826 | 790 | 852 | 1583 |
| multipass-reverse-fold | 618 | 600 | 699 | 933 |
| direct-recursive-descent | 507 | 493 | 464 | 631 |
| direct-shunting-yard | 540 | 545 | 617 | 892 |
| direct-reverse | **491** | **466** | **454** | **594** |
| bytecode-vm | 564 | 555 | 634 | 888 |
| *direct-scannerless* (control) | *450* | *387* | *448* | *602* |

Median of three CI runs (34136843367 / 34137622680 / 34138407724). Correctness:
all corpus expressions agree across all 15 strategies. `direct-reverse` is now
the fastest strategy at every size, edging out `direct-recursive-descent`
(both stay within ~2–8 % of each other; noisy across runs, see the one-pager).
The spread is ~2.8× fastest-to-slowest on the median (vs C++'s ~4.7×), tighter
than Python's ~3.0×; the pointer classics lead every arena form, from ~1.4× at
the tight end (`ast-arena`) to ~2.6× at the wide end (`multipass-bfs`), and
"no tree" buys little here. `multipass-reverse` beats `multipass-arena`/`-bfs`
at every size; the `multipass-bfs` blow-up at n=10000 is the sparse-table
build cost. The lexer-free `direct-scannerless` is a control, not a
contender: it lands within a few percent of `direct-rd`/`direct-reverse`,
noisily — the lazy token list already fuses with its consumer.

## What changes versus C++

- **The arena trick disappears** — `ast-arena` (754 @ n=1000) is *slower* than
  the pointer-AST `Expr` builders (`ast-pratt` 648, `ast-recursive-descent`
  583). A flat `Array` of boxed, GC'd nodes is no cheaper than the tree; the C++
  win was about contiguous memory *layout*, which a managed runtime hides.
- **"No tree" wins here now.** In C++ the `direct-*` forms are fastest; in
  Haskell `direct-reverse` is fastest at every size this batch, edging out
  `direct-rd` by a few percent (noisy across runs — see the one-pager),
  with `ast-recursive-descent` the closest pointer classic (`bytecode-vm`
  further back). With every node boxed and GC'd, *not* allocating the tree
  buys less of a layout advantage than in the unmanaged languages, and the
  ranking among the leaders is close enough on this runner to be sensitive
  to which run you read.
- **The sparse-table `multipass-bfs` is the slowest at scale** — building the
  `Array`-based RMQ costs more than the linear split scan it replaces, exactly as
  in C++. The precompute loses to a plain linear scan in every runtime.
- **The spread is ~2.8× on the median** (vs C++'s ~4.7×), tighter than
  Python's ~3.0×. It read ~1.7× until 2026-09-02, when the lexer's
  `reads`-based number parsing was replaced: a shared constant cost had been
  compressing every gap.

See the top-level [README](../README.md) for the cross-language table and the
[one-pager](../docs/one-pager.md) for the cross-language verdict and
scoreboard.
