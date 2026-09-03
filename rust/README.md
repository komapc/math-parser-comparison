# Rust port

The fourth language: all fifteen strategies, same grammar, same shared lexer,
same 480-check spec suite and 6 000-expression differential fuzz, same corpora
and adversarial shapes as the C++, Python and Haskell trees. Safe Rust except
for one documented block in the fused fold (below). No dependencies.

```sh
cd rust
cargo test --release                        # 480 spec checks + differential fuzz
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
| no tree (`direct-rd` / `direct-sy` / `direct-reverse`) | 50 / 52 / 49 | 52 / 56 / 52 |
| contiguous tree (`ast-arena` / `multipass-reverse-fold`) | 69 / 67 | 71 / 72 |
| pointer tree (`ast-rd` / `ast-pratt`) | 125 / 133 | 139 / 138 |
| buffered bottom-up (`multipass-reverse`) | 98 | 100 |
| bytecode-vm | 62 | 67 |
| lexer-free control (`direct-scannerless`) | 39 | 36 |

Same tiers to within a few percent (n=1000; runs 33734046295, 33734053376,
33734059421). Two differences worth naming: the fold's 2–5 % edge over
`ast-arena` in C++ does not reproduce here — Rust puts it 0–2 % *behind*
in all 12 size×run measurements, a tie — and the top-down family is
1.5–1.9× slower than in C++, which is the AVX2 candidate scan the port
does not have. On the structured shapes the fold beats `ast-arena` by
9–39 % on towerchain, sumchain and nestchain in every run and ties it on
powchain (−8…+5 %); `direct-reverse` ties `direct-rd` on the random corpus
and is 10–22 % ahead on the chains. Full cross-language tables:
[FINDINGS.md](../FINDINGS.md), [docs/one-pager.md](../docs/one-pager.md).

## What the port was for

Two questions the C++ numbers left open:

1. **Is the fold's edge a GCC artefact?** LLVM is a second compiler. The
   Rust `direct-*` tier and the C++ one are within a few percent of each
   other in instructions, cycles and branch misses on the same laptop
   (`perf stat`, n=1000 corpus), so the C++ tables are not a GCC story.
   The CI tables in the top-level README carry the neutral-runner numbers.
2. **Does the fold survive safe Rust?** The C++ fold's last 10 % came from
   pre-sized raw buffers indexed by locals instead of `std::vector`
   push/pop. Measured here by instruction count (load-independent):
   `Vec` push/pop/truncate cost the fold about 3 % of instructions and
   `direct-reverse` about 7 %, with no measurable change in cycles or branch
   misses. `fold.rs` keeps the raw-buffer form (one `unsafe` block, bound
   argument in the comment); switching it back to `Vec` is a mechanical
   edit and the safe version is what the buffered `reverse.rs` uses.

What actually mattered, in order:

- `Result<Token, Error>` must stay 16 bytes. With an inline `String` error
  it is 24 bytes and every token is returned through memory; boxing the
  message (`Error(Box<str>)`) lets the `Kind` niche carry the discriminant
  and the `Ok` path goes back to two registers. This is an every-strategy
  cost, not a fold cost.
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
