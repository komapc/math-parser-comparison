# Rust port

The fourth language: all fifteen strategies, same grammar, same shared lexer,
same 480-check spec suite and 6 000-expression differential fuzz, same corpora
and adversarial shapes as the C++, Python and Haskell trees. Safe Rust except
for one unsafe mechanism, confined to the fused fold (below). No dependencies.

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
| no tree (`direct-rd` / `direct-sy` / `direct-reverse`) | 52 / 50 / 50 | 54 / 61 / 57 |
| contiguous tree (`ast-arena` / `multipass-reverse-fold`) | 72 / 69 | 74 / 77 |
| pointer tree (`ast-rd` / `ast-pratt`) | 132 / 138 | 132 / 132 |
| buffered bottom-up (`multipass-reverse`) | 97 | 102 |
| bytecode-vm | 62 | 74 |
| lexer-free control (`direct-scannerless`) | 38 | 34 |

Same tiers to within a few percent (n=1000; runs 34130395595, 34131316618,
34132166596; this batch ran rustc 1.98.1, one point release newer than the
previous batch). Two differences worth naming: the fold's small edge over
`ast-arena` in C++ does not reproduce here — Rust is a genuine tie, sign
flipping run to run rather than settling one way — and the top-down family
that lost its AVX2 candidate scan in the port is slower in Rust, unevenly:
pointer `multipass` itself is closest at ~1.3×, while the arena/direct forms
that actually used the scan (`multipass-arena`, `direct-mp`, `multipass-bfs`)
are 1.5–1.8× behind C++. On the structured shapes the fold beats
`ast-arena` on towerchain, sumchain and nestchain in every run and is
ahead, narrowly, on powchain; `direct-reverse` ties `direct-rd` on
the random corpus, is only narrowly ahead on powchain, and clearly ahead on
towerchain, sumchain and nestchain. Full cross-language tables:
[FINDINGS.md](../FINDINGS.md), [docs/one-pager.md](../docs/one-pager.md).

## What the port was for

Two questions the C++ numbers left open:

1. **Is the fold's edge a GCC artefact?** LLVM is a second compiler, and the
   answer is mixed. The overall tiers are not a GCC story: the `direct-*`
   tier matches within a few percent of C++ in instructions, cycles and
   branch misses on the same laptop (`perf stat`, n=1000 corpus). But the
   fold's specific 2–5 % edge over `ast-arena` does not reproduce — LLVM
   ties the two instead — so that one sliver looks compiler-sensitive even
   though the tier ordering around it is real. The CI tables in the
   top-level README carry the neutral-runner numbers.
2. **Does the fold survive safe Rust?** The C++ fold's last 10 % came from
   pre-sized raw buffers indexed by locals instead of `std::vector`
   push/pop. Measured here by instruction count (load-independent):
   `Vec` push/pop/truncate cost the fold about 3 % of instructions and
   `direct-reverse` about 7 %, with no measurable change in cycles or branch
   misses. `fold.rs` keeps the raw-buffer form (one unsafe mechanism, all
   sites confined to that file; bound argument in the comment); switching
   it back to `Vec` is a mechanical edit and the safe version is what the
   buffered `reverse.rs` uses.

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
