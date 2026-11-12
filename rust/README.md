# Rust port

The fourth language: all fifteen strategies, same grammar, same shared lexer,
same 615-check spec suite and 6 000-expression differential fuzz, same corpora
and adversarial shapes as the C++, Python and Haskell trees. Safe Rust
throughout — no `unsafe` anywhere in the crate. No dependencies.

```sh
cd rust
cargo test --release                        # 615 spec checks + differential fuzz
cargo run --release --bin bench             # ns/leaf on ../bench/corpus (python3 ../bench/gen_corpus.py first)
cargo run --release --bin bench -- ../bench/corpus direct-reverse ast-arena   # subset
cargo run --release --bin adversarial       # powchain / towerchain / sumchain / nestchain
```

## Layout

| file | what |
|---|---|
| `src/lexer.rs` | shared lexer: streaming `Lexer::next()` for left-to-right strategies, `tokenize_into()` (into a reused per-evaluator `Vec`) for the array-bound ones; 16-byte `Token` |
| `src/builder.rs` | the representation axis: `Builder` trait with `PtrAst` (boxed tree), `Arena` (index tree in one `Vec`), `Direct` (no tree) |
| `src/classics.rs` | recursive descent, Pratt, shunting-yard — one generic driver each |
| `src/multipass.rs` | top-down divide-and-conquer (multipass, -arena, direct-mp, -bfs), bounded scans + buckets, sparse-table RMQ |
| `src/reverse.rs` | multipass-reverse: buffered bottom-up, one pass per precedence level over a shared item stack |
| `src/fold.rs` | multipass-reverse-fold / direct-reverse: the fused single-sweep form |
| `src/bytecode.rs` | bytecode-vm |
| `src/scannerless.rs` | direct-scannerless (control row: lexer fused into the grammar) |
| `src/lib.rs` | `Evaluator` trait, driver × builder instantiations, `all_evaluators()` in the shared registry order |
| `tests/spec.rs`, `tests/fuzz.rs` | the shared suites |
| `src/bin/bench.rs`, `src/bin/adversarial.rs` | benchmarks, printing the C++ layout so the doc tables can be regenerated from `bench-rust.txt` / `adversarial-rust.txt` |

Drivers are generic functions over `Builder`, monomorphised per carrier the
way the C++ policy templates are; the builder calls inline away.

Every buffer (token array, node arena, stacks, buckets, sparse table) lives
in the evaluator and is reused, so after the first expression every arena
and no-tree strategy makes **zero** heap allocations per evaluation, as in
C++ (counted with a counting `GlobalAlloc`, on the corpus and on all four
structured shapes at m=8192); the boxed-tree forms, `multipass` included,
make one per node.

## Results (neutral 4-vCPU runner, median of three runs, ns/leaf)

| tier | C++ | Rust |
|---|--:|--:|
| no tree (`direct-rd` / `direct-sy` / `direct-reverse`) | 41 / 40 / 40 | 45 / 46 / 47 |
| contiguous tree (`ast-arena` / `multipass-reverse-fold`) | 58 / 56 | 66 / 66 |
| pointer tree (`ast-rd` / `ast-pratt`) | 108 / 108 | 109 / 109 |
| buffered bottom-up (`multipass-reverse`) | 80 | 87 |
| bytecode-vm | 53 | 61 |
| lexer-free control (`direct-scannerless`) | 32 | 29 |

Same tiers to within a few percent (n=1000; runs 43227113612, 43227138258,
43231342538, both languages from one commit, after the
[same-rules tuning pass](../bench/TUNING.md), all three on one CPU model,
AMD EPYC 9V74; rustc 1.98.1). Three differences worth naming. The fold's
edge over `ast-arena` does not survive here: −1.4…+2.0 % over the four
sizes (a tie), against +3…+5 % in C++. Rust's
`direct-rd` builds through the level-aware builder forms the C++ one
already wrote inline (added in the tuning pass, +7…+10 %), and it is now
the fastest real strategy here, 5–6 % ahead of `direct-reverse` on the
random corpus from n=100 up. And the top-down family that lost its AVX2
candidate scan in the port is slower in Rust: 1.3–1.9× behind C++ at
n=1000, pointer `multipass` closest, `direct-mp` furthest. On the
structured shapes the fold beats `ast-arena` on nestchain by 29–34 %, is
narrowly ahead on towerchain (+6…+7 %) and sumchain (+2 %) and level on
powchain (−1…+2 %); `direct-reverse` is ahead of `direct-rd` on every
structured shape (+2 % to +17 %). The random-corpus tree margin is
hardware-dependent: on three EPYC 7763 runs of the same commit the fold
trails `ast-arena` by 4–5 %. A fold-only tuning pass tried pre-sized stacks
indexed by locals, as in C++ (safe, bounds-checked): the corpus gain was
matched by untouched strategies in the same binary and `direct-reverse`
lost 1.5–2 % on powchain, so it was not kept
([TUNING.md](../bench/TUNING.md)). Full cross-language tables:
[FINDINGS.md](../FINDINGS.md), [docs/one-pager.md](../docs/one-pager.md).

## What the port was for

Two questions the C++ numbers left open:

1. **Is the fold's edge a GCC artefact?** LLVM is a second compiler, and the
   answer is mixed. The overall tiers are not a GCC story: the `direct-*`
   tier matches within a few percent of C++ in instructions, cycles and
   branch misses on the same laptop (`perf stat`, n=1000 corpus). But the
   fold's specific ~3–5 % edge over `ast-arena` does not reproduce —
   on the CI runner LLVM leaves the fold level with it on one CPU model
   (−1.4…+2.0 %) and 4–5 % behind on another — so that sliver looks
   compiler- and hardware-sensitive even though the tier ordering around
   it is real.
   The CI tables in the top-level README carry the neutral-runner numbers.
2. **Does the fold survive safe Rust?** The C++ fold's last 10 % came from
   pre-sized raw buffers indexed by locals instead of `std::vector`
   push/pop. Measured here by instruction count (load-independent):
   `Vec` push/pop/truncate cost the fold about 3 % of instructions and
   `direct-reverse` about 7 %, with no measurable change in cycles or branch
   misses on the laptop. So `fold.rs` now uses the safe `Vec` form, like
   the buffered `reverse.rs`. On the CI runner the switch is visible after
   all: in that batch the fold went from 1.02× to 1.05× `ast-arena` at n=1000, and on
   powchain from narrowly ahead to a tie. The random-corpus verdict did not
   move — a tie either way in that batch — so the safe form stays: a ~3 % sliver is not
   worth `unsafe`. It survives, at the cost of that sliver.

What actually mattered, in order:

- Box the error message. `Token` is 16 bytes and `Error(Box<str>)` is 16
  bytes (pointer + length, vs 24 for an inline `String`). `Result<f64,
  Error>` stays 16 bytes — `Box`'s never-null pointer supplies a spare
  niche for the discriminant — but `Result<Token, Error>` is 24 bytes:
  `Token` already fills all 16 of its own bytes, so there is no niche left
  and the `Result` needs a real discriminant. Either way, boxing keeps
  `Error` small and the cold path cheap; this is an every-strategy cost,
  not a fold cost.
- Level-aware builder calls. The C++ fold's policy has `mulDiv` / `addSub`
  / `pow` / `unary`, each one compare; a first port routed everything
  through the generic five-way `binop`, which is two extra data-dependent
  branches per operator on a random corpus (+8 % instructions, +27 % branch
  misses on the fold). The `Builder` trait now has the same level-aware
  methods with `binop` defaults; only the two bottom-up drivers use them,
  because only they know which level they are reducing.
- One shared item stack for the buffered reverse. Allocating a fresh `Vec`
  per paren group (the Python shape) was +30 % instructions; the C++
  stack-discipline layout brings it 5 % under C++.
- A `match` on the token kind and a frequency-ordered if-chain compile to
  the same code under LLVM; it made no difference.

Not ported: the AVX2 candidate scan in the C++ top-down family. The Rust
top-down variants are the bounded-scan + bucket form, like Python.

Deep inputs (nestchain at 8 192 groups) recurse once per level in the
recursive strategies, so both binaries run on a 512 MB-stack thread.
