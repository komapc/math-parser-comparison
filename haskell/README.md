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
| ast-recursive-descent | 612 | 578 | **708** | **634** |
| ast-shunting-yard | 703 | 680 | 836 | 867 |
| ast-pratt | **604** | 588 | 724 | 644 |
| ast-arena | 700 | 681 | 858 | 840 |
| multipass | 945 | 955 | 1 152 | 1 272 |
| multipass-arena | 1 103 | 1 106 | 1 329 | 1 545 |
| direct-mp | 1 014 | 1 014 | 1 193 | 1 328 |
| multipass-bfs | 1 298 | 1 325 | 1 572 | 1 936 |
| multipass-reverse | 934 | 900 | 1 090 | 1 226 |
| multipass-reverse-fold | 740 | 698 | 858 | 885 |
| direct-recursive-descent | 616 | **569** | 712 | 654 |
| direct-shunting-yard | 710 | 687 | 829 | 871 |
| direct-reverse | 642 | 594 | 729 | 664 |
| bytecode-vm | 726 | 694 | 852 | 888 |
| *direct-scannerless* (control) | *546* | *512* | *646* | *570* |

Median of three CI runs on one CPU model, AMD EPYC 9V74 (36204789075 /
36204787013 / 36204784693, 2026-09-26). Correctness: all corpus
expressions agree across all 15 strategies. The leaders are a cluster: the
pointer classics `ast-recursive-descent` and `ast-pratt` and the no-tree
`direct-recursive-descent` take the four sizes between them, within ~3 % of
each other, with `direct-reverse` 2–6 % behind `direct-rd` in every run.
Before 2026-09-25 the `direct-*` forms led here. The registry helpers that
build the tree strategies were then marked `INLINE`, and GHC Core shows
their dictionary-passing workers gone (`$wmkAst`/`$wmkArena`: 11 → 0); in
the next CI batch the pointer classics moved from behind `direct-rd` to
level with or ahead of it, and this batch agrees.

The absolute numbers are ~1.5× the 2026-09-25 batch's. Part of that is the
CPU model (the other languages' fastest strategies moved ~1.05–1.2× the same
way); the rest arrived with the 64 MB nursery (`-with-rtsopts=-A64m`) and
round-robin timing, which landed together and were not measured apart. In
exchange the run-to-run spread fell 5–10×: the tiers hold in every run (the
fold behind `ast-rd`, `direct-reverse` behind `direct-rd`), though the order
inside the leading cluster is too close to call. The spread is ~2.3× fastest-to-slowest
on the median (vs C++'s ~4.0×, Python's ~2.5×); the pointer classics lead
every arena form, from ~1.2× at the tight end (`ast-arena`) to ~2.2–3.1× at
the wide end (`multipass-bfs`), and "no tree" buys nothing here.
`multipass-reverse` beats `multipass-arena`/`-bfs` at every size; the
`multipass-bfs` growth at n=10000 is the sparse-table build cost. The
lexer-free `direct-scannerless` is a control, not a contender: it sits
~9–13 % below `direct-rd` at every size — the lazy token list already fuses
with its consumer, so the lexer is a smaller share here than in the other
three languages.

## What changes versus C++

- **The arena trick disappears** — `ast-arena` (858 @ n=1000) is *slower* than
  the pointer-AST `Expr` builders (`ast-pratt` 724, `ast-recursive-descent`
  708). A flat `Array` of boxed, GC'd nodes is no cheaper than the tree; the C++
  win was about contiguous memory *layout*, which a managed runtime hides.
- **"No tree" does not win here.** In C++ the `direct-*` forms are fastest;
  in Haskell `ast-recursive-descent` builds a tree and still leads at two
  of four sizes (`ast-pratt` and `direct-rd` take one each), with `direct-rd`
  ~2 % and `direct-reverse` ~5 % behind it on the median over sizes. With
  every node boxed and GC'd, *not* allocating the tree buys little of the
  layout advantage it has in the unmanaged languages.
- **The sparse-table `multipass-bfs` is the slowest at scale** — building the
  `Array`-based RMQ costs more than the linear split scan it replaces, exactly as
  in C++. The precompute loses to a plain linear scan in every runtime.
- **The spread is ~2.3× on the median** (vs C++'s ~4.0×), about
  Python's ~2.5×. It read ~1.7× until 2026-09-02, when the lexer's
  `reads`-based number parsing was replaced: a shared constant cost had been
  compressing every gap.

See the top-level [README](../README.md) for the cross-language table and the
[one-pager](../docs/one-pager.md) for the cross-language verdict and
scoreboard.
