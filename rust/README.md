# Rust port

The fourth language: all fifteen strategies, same grammar, same shared lexer,
same 540-check spec suite and 6 000-expression differential fuzz, same corpora
and adversarial shapes as the C++, Python and Haskell trees. Safe Rust
throughout — no `unsafe` anywhere in the crate. No dependencies.

```sh
cd rust
cargo test --release                        # 540 spec checks + differential fuzz
cargo run --release --bin bench             # ns/leaf on ../bench/corpus (python3 ../bench/gen_corpus.py first)
cargo run --release --bin bench -- ../bench/corpus direct-reverse ast-arena   # subset
cargo run --release --bin adversarial       # powchain / towerchain / sumchain / nestchain
```

## Layout

| file | what |
|---|---|
| `src/lexer.rs` | shared lexer: streaming `Lexer::next()` for left-to-right strategies, `tokenize()` for the array-bound ones; 16-byte `Token` |
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

## Results (neutral 4-vCPU runner, median of three runs, ns/leaf)

| tier | C++ | Rust |
|---|--:|--:|
| no tree (`direct-rd` / `direct-sy` / `direct-reverse`) | 53 / 52 / 52 | 61 / 65 / 61 |
| contiguous tree (`ast-arena` / `multipass-reverse-fold`) | 75 / 72 | 84 / 84 |
| pointer tree (`ast-rd` / `ast-pratt`) | 140 / 139 | 141 / 138 |
| buffered bottom-up (`multipass-reverse`) | 104 | 115 |
| bytecode-vm | 67 | 80 |
| lexer-free control (`direct-scannerless`) | 41 | 38 |

Same tiers to within a few percent (n=1000; runs 36204789075, 36204787013,
36204784693, both languages from commit 183c3d7, all three on one CPU
model, AMD EPYC 9V74; rustc 1.98.1). Two differences worth naming: the
fold's small edge over `ast-arena` in C++ does not reproduce here — Rust is
a tie from n=100 up (within ±1 % in every run) and ahead only at n=10 — and
the top-down family that lost its AVX2 candidate scan in the port is slower
in Rust: 1.5–1.9× behind C++, pointer `multipass` closest, `direct-mp`
furthest. On the structured shapes the fold beats `ast-arena` on nestchain
by 24–34 % and on towerchain by 6–11 % in every run, is ahead, narrowly, on
sumchain (+4…+5 %) and ties on powchain; `direct-reverse` is level with
`direct-rd` on the random corpus (−2…+2 % across sizes and runs) and ahead
of it on every structured shape. Full cross-language tables:
[FINDINGS.md](../FINDINGS.md), [docs/one-pager.md](../docs/one-pager.md).

## What the port was for

Two questions the C++ numbers left open:

1. **Is the fold's edge a GCC artefact?** LLVM is a second compiler, and the
   answer is mixed. The overall tiers are not a GCC story: the `direct-*`
   tier matches within a few percent of C++ in instructions, cycles and
   branch misses on the same laptop (`perf stat`, n=1000 corpus). But the
   fold's specific 1–5 % edge over `ast-arena` does not reproduce — LLVM
   ties the two at n=1000 and puts the fold slightly behind at n=10000 — so that one sliver looks compiler-sensitive even
   though the tier ordering around it is real. The CI tables in the
   top-level README carry the neutral-runner numbers.
2. **Does the fold survive safe Rust?** The C++ fold's last 10 % came from
   pre-sized raw buffers indexed by locals instead of `std::vector`
   push/pop. Measured here by instruction count (load-independent):
   `Vec` push/pop/truncate cost the fold about 3 % of instructions and
   `direct-reverse` about 7 %, with no measurable change in cycles or branch
   misses on the laptop. So `fold.rs` now uses the safe `Vec` form, like
   the buffered `reverse.rs`. On the CI runner the switch is visible after
   all: the fold went from 1.02× to 1.05× `ast-arena` at n=1000, and on
   powchain from narrowly ahead to a tie. The random-corpus verdict does not
   move — a tie either way — so the safe form stays: a ~3 % sliver is not
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
