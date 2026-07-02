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
| `ast-recursive-descent` | pointer AST | 2.12 | **1.00** | **1.00** |
| `ast-shunting-yard` | pointer AST | 2.16 | 1.02 | 1.11 |
| `ast-pratt` | pointer AST | 2.09 | 1.04 | 1.03 |
| `ast-arena` | arena AST | **1.00** | 1.16 | 1.26 |
| `multipass` | pointer AST | 2.78 | 2.30 | 1.36 |
| `multipass-arena` | arena AST | 1.57 | 2.46 | 1.52 |
| `multipass-bfs` | arena AST | 1.67 | 2.89 | 1.62 |
| `multipass-reverse` | arena AST | 1.19 | 1.41 | 1.41 |

Reading it:

- **C++: the arena tree-builders win decisively** (`ast-arena` ×1.0,
  `multipass-reverse` ×1.19; every pointer-AST form is ~2.1× on per-node
  `make_unique`). Contiguous memory layout is the whole game.
- **Python & Haskell: a pointer-AST builder wins, and `ast-arena` is *slower*.**
  Which pointer form is nominally fastest (recursive descent in both this run;
  Pratt trails by ~3% in Haskell) is within run-to-run noise — they cluster inside
  ~5%. The robust fact is that the arena's advantage was memory *layout*, which a
  boxed/GC'd runtime hides, so the trick evaporates.
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
| `ast-recursive-descent` | 2.29 | 1.84 | 1.10 | 1.05 | **1.00** | **1.00** |
| `ast-shunting-yard` | 2.31 | 1.90 | **1.00** | **1.00** | 1.08 | 1.08 |
| `ast-pratt` | 2.27 | 1.84 | 1.10 | 1.10 | 1.08 | 1.02 |
| `ast-arena` | **1.00** | **1.00** | 1.16 | 1.12 | 1.23 | 1.16 |
| `multipass` | 2.91 | 2.38 | 2.00 | 1.94 | 1.39 | 1.30 |
| `multipass-arena` | 1.57 | 1.25 | 2.10 | 1.98 | 1.58 | 1.45 |
| `multipass-bfs` | 1.73 | 1.39 | 2.32 | 2.24 | 1.73 | 1.61 |
| `multipass-reverse` | 1.25 | 1.01 | 1.42 | 1.35 | 1.39 | 1.29 |

Compare the **W1 and W4 columns within each language** (not against the one-shot
table above — that's a different harness). The takeaway: **adding cores does not
reorder the builders.** C++ is identical W1→W4 (`ast-arena` leads; `multipass-reverse`
closes to a ~1% near-tie at W=4); Python and Haskell preserve the tiers, with only
adjacent *near-ties* trading places (Haskell's whole spread is ~1.0–1.7×, so
neighbours are statistical ties). The absolute gaps wobble between runs —
parallel-efficiency noise on a shared VM (arena's efficiency alone has spanned
0.53–0.80 across runs), not a core-count effect.
**The AST-builder ranking is core-count-invariant.**

## When you build the tree and re-evaluate

The comparison above is **one-shot** (parse + evaluate once). If instead you build
the tree once and evaluate it many times with **different variable values**, the
verdict flips — building a reusable form is worth it after just **≲ 4 evaluations**,
because re-parsing every time is **~9–21× slower per eval**:

| per-eval, n=1000 | C++ | Python | Haskell |
|---|--:|--:|--:|
| best compiled form | 18k (bytecode) | 264k (bytecode) | 43k (ast-arena) |
| re-parse every time | 192k | 2.94M | 772k |

And *which* form is fastest to re-evaluate is itself runtime-specific: flat
bytecode/arena in C++, bytecode in Python, an AST in Haskell (pointer and arena
trade places run to run; flat bytecode is the one form that *loses* there).
Details and the full table in [FINDINGS.md](FINDINGS.md).

## Multi-core scaling

Splitting the corpus across W workers (neutral 4-vCPU GitHub runner). The
per-expression **ranking is core-count-invariant** — only the runtime's
parallelism model decides whether more cores help:

| runtime | throughput scaling @ 4 workers |
|---|--:|
| C++ (`std::thread`) | ~2.1–2.9× across runs |
| Haskell (`-threaded`) | ~2.0–2.8× across runs |
| Python — **processes** | ~1.9–2.5× across runs |
| Python — **threads** | ~0.9–1.0× (flat — the GIL) |

Per-strategy (and per-run aggregate) efficiency differences are run-to-run noise
on a shared VM; real 8-core scaling needs dedicated hardware. (Regenerated by the
[bench workflow](.github/workflows/bench.yml).)

The headline across all twelve strategies — *in C++ memory layout dominates (flat
representations beat pointer-chasing ~2.7×), but in managed runtimes that signal
washes out and everything lands within ~1.5× — except the top-down multipass
variants, which do more work and lose everywhere (their bottom-up sibling now
holds ×1.5 in all three languages)* — is in **[FINDINGS.md](FINDINGS.md)**.
