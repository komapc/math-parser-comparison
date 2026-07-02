<div align="center">

# 🧮 Math-Expression Parser & Evaluator

**Twelve ways to turn `-2 ^ 2 + 3 * (4 - 1)` into `5`, in C++, Haskell, and Python.**

</div>

This README focuses on one new algorithm and how the **tree-building** strategies
compare. For everything else — all twelve strategies (including the allocation-free
`direct-*` and `bytecode-vm`), the grammar, build instructions, the full
cross-language analysis, and multi-core scaling — see **[FINDINGS.md](FINDINGS.md)**.

## The new algorithm: `multipass-reverse`

Classic `multipass` is **top-down** divide-and-conquer: find the lowest-precedence
operator (the root, evaluated last), split there, recurse on each half.

`multipass-reverse` inverts that — it works **bottom-up**, reducing the
*tightest-binding* things first: deepest parentheses, then `^`, then `*` `/`, then
`+` `-`, agglomerating outward until one node remains. It's "multipass" in the
original sense — **one reduction pass per precedence level** — and produces a
structurally identical tree, just built in the opposite order. ([C++](cpp/src/multipass_reverse.cpp) ·
[Python](python/mathparser/evaluators.py) · [Haskell](haskell/src/MathParser/Strategies.hs))

**→ Full algorithm walk-through, with diagrams and complexity analysis:
[docs/multipass-reverse.md](docs/multipass-reverse.md)**

## How the AST builders compare

The eight strategies that build a syntax tree, ns/leaf at n=1000 on a neutral
4-vCPU GitHub runner, **normalised to the fastest tree-builder in each language**
(lower = faster; **bold** = fastest):

| tree builder | representation | C++ | Python | Haskell |
|---|---|--:|--:|--:|
| `ast-recursive-descent` | pointer AST | 2.13 | 1.00 | **1.00** |
| `ast-shunting-yard` | pointer AST | 2.23 | **1.00** | 1.11 |
| `ast-pratt` | pointer AST | 2.10 | 1.04 | 1.03 |
| `ast-arena` | arena AST | **1.00** | 1.15 | 1.26 |
| `multipass` | pointer AST | 2.81 | 2.28 | 1.28 |
| `multipass-arena` | arena AST | 1.58 | 2.44 | 1.53 |
| `multipass-bfs` | arena AST | 1.72 | 2.75 | 1.76 |
| `multipass-reverse` | arena AST | 1.18 | 1.37 | 1.36 |

Reading it:

- **C++: the arena tree-builders win decisively** (`ast-arena` ×1.0,
  `multipass-reverse` ×1.18; every pointer-AST form is ~2.1× on per-node
  `make_unique`). Contiguous memory layout is the whole game.
- **Python & Haskell: a pointer-AST builder wins, and `ast-arena` is *slower*.**
  Which pointer form is nominally fastest (shunting-yard vs recursive descent
  in Python this run is a 0.4% coin flip; Pratt trails by ~3% in Haskell) is
  within run-to-run noise — they cluster inside ~5%. The robust fact is that
  the arena's advantage was memory *layout*, which a boxed/GC'd runtime hides,
  so the trick evaporates.
- **The top-down divide-and-conquer variants lose everywhere** — they do more
  work (repeated split-scans) to build the same tree. The bottom-up
  `multipass-reverse` is the exception: best of the family in C++ and Python and
  the **second-fastest tree builder in C++** — it never scans for a split, and
  after its hot-path rewrite it parses allocation-free into an arena.
- **And on structured inputs the family gap becomes a blow-out:** on flat
  mixed-precedence chains (`3^2 * 2^2 / …` — factored monomials, plus a
  `^`-tower variant that even catches the RMQ-equipped `multipass-bfs`) the
  top-down splitters degenerate to **Θ(n²)** while bottom-up stays **Θ(n)** —
  measured pre-fix at **~8× (Haskell, m=4096), ~17× (Python, m=1024) and
  ~75–115× (C++, m=8192)** in `multipass-reverse`'s favour. The gap is
  quadratic-vs-linear, so it grows with chain length — those per-language
  ratios reflect the measured size as much as the runtime. The top-down family
  has since been patched in all three languages (bounded scans +
  per-precedence position buckets cap it at **O(n log n)**, shrinking the C++
  gap to the ~2–4× the current binaries reproduce) — bottom-up needed no patch.
  See [FINDINGS.md](FINDINGS.md#where-bottom-up-provably-wins-mixed-precedence-chains).

### …and the same comparison at 4 cores

Does splitting the work across cores reorder the builders? The same eight, on the
[scaling harness](.github/workflows/bench.yml) at **W=1 vs W=4 workers** (process
pool for Python — threads are GIL-flat), each column normalised to the fastest
builder *in that column* (**bold**):

| tree builder | C++ W1 | C++ W4 | Python W1 | Python W4 | Haskell W1 | Haskell W4 |
|---|--:|--:|--:|--:|--:|--:|
| `ast-recursive-descent` | 2.36 | 2.51 | 1.03 | 1.03 | **1.00** | 1.01 |
| `ast-shunting-yard` | 2.40 | 2.63 | **1.00** | **1.00** | 1.08 | 1.06 |
| `ast-pratt` | 2.34 | 2.47 | 1.07 | 1.12 | 1.03 | **1.00** |
| `ast-arena` | **1.00** | **1.00** | 1.14 | 1.16 | 1.21 | 1.15 |
| `multipass` | 2.99 | 3.26 | 2.00 | 1.87 | 1.30 | 1.27 |
| `multipass-arena` | 1.64 | 1.70 | 2.00 | 1.92 | 1.49 | 1.41 |
| `multipass-bfs` | 1.81 | 1.91 | 2.13 | 2.15 | 1.64 | 1.58 |
| `multipass-reverse` | 1.28 | 1.29 | 1.33 | 1.30 | 1.24 | 1.21 |

Compare the **W1 and W4 columns within each language** (not against the one-shot
table above — that's a different harness). The takeaway: **adding cores does not
reorder the builders.** C++ is identical W1→W4 (`ast-arena` leads,
`multipass-reverse` second); Python and Haskell preserve the tiers, with only
adjacent *near-ties* trading places (Haskell's whole spread is ~1.0–1.6×, so
neighbours are statistical ties). The absolute gaps wobble between runs —
parallel-efficiency noise on a shared VM (an earlier run had reverse closing to
a ~1% near-tie with arena at W=4; this one doesn't), not a core-count effect.
**The AST-builder ranking is core-count-invariant.**

## When you build the tree and re-evaluate

The comparison above is **one-shot** (parse + evaluate once). If instead you build
the tree once and evaluate it many times with **different variable values**, the
verdict flips — building a reusable form is worth it after just **≲ 4 evaluations**,
because re-parsing every time is **~7–26× slower per eval**:

| per-eval, n=1000 | C++ | Python | Haskell |
|---|--:|--:|--:|
| best compiled form | 18k (rpn) | 280k (ast-ptr) | 35k (ast-ptr) |
| re-parse every time | 186k | 2.90M | 936k |

And *which* form is fastest to re-evaluate is itself runtime-specific: flat
rpn/bytecode in C++, a statistical tie between pointer-AST and bytecode in
Python (they trade places run to run), an AST in Haskell (pointer and arena
trade places run to run; flat bytecode is the one form that *loses* there).
Details and the full table in [FINDINGS.md](FINDINGS.md).

## Multi-core scaling

Splitting the corpus across W workers (neutral 4-vCPU GitHub runner). The
per-expression **ranking is core-count-invariant** — only the runtime's
parallelism model decides whether more cores help:

| runtime | throughput scaling @ 4 workers |
|---|--:|
| C++ (`std::thread`) | ~2.1–2.9× across runs (this run 2.7–2.9×) |
| Haskell (`-threaded`) | ~2.0–2.8× across runs (this run 2.4–2.5×) |
| Python — **processes** | ~1.9–2.5× across runs (this run 2.2–2.4×) |
| Python — **threads** | ~0.9–1.0× (flat — the GIL) |

Per-strategy (and per-run aggregate) efficiency differences are run-to-run noise
on a shared VM; real 8-core scaling needs dedicated hardware. (Regenerated by the
[bench workflow](.github/workflows/bench.yml).)

The headline across all twelve strategies — *in C++ memory layout dominates (flat
representations beat pointer-chasing ~2.7×), but in managed runtimes that signal
washes out and everything lands within ~1.5× — except the top-down multipass
variants, which do more work and lose everywhere (their bottom-up sibling
holds ×1.3–1.6 in all three languages)* — is in **[FINDINGS.md](FINDINGS.md)**.
