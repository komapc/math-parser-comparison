<div align="center">

# 🧮 Math-Expression Parser & Evaluator

**Twelve ways to turn `-2 ^ 2 + 3 * (4 - 1)` into `14`, in C++, Haskell, and Python.**

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
bit-identical tree, just built in the opposite order. ([C++](cpp/src/multipass_reverse.cpp) ·
[Python](python/mathparser/evaluators.py) · [Haskell](haskell/src/MathParser/Strategies.hs))

## How the AST builders compare

The eight strategies that build a syntax tree, ns/leaf at n=1000 on a neutral
4-vCPU GitHub runner, **normalised to the fastest tree-builder in each language**
(lower = faster; **bold** = fastest):

| tree builder | representation | C++ | Python | Haskell |
|---|---|--:|--:|--:|
| `ast-recursive-descent` | pointer AST | 2.08 | **1.00** | 1.11 |
| `ast-shunting-yard` | pointer AST | 2.15 | 1.01 | 1.20 |
| `ast-pratt` | pointer AST | 2.09 | 1.04 | **1.00** |
| `ast-arena` | arena AST | **1.00** | 1.17 | 1.36 |
| `multipass` | pointer AST | 3.85 | 2.46 | 1.40 |
| `multipass-arena` | arena AST | 1.54 | 2.64 | 1.62 |
| `multipass-bfs` | arena AST | 1.63 | 3.11 | 1.86 |
| `multipass-reverse` | arena AST | 2.08 | 2.30 | 1.43 |

Reading it:

- **C++: the arena tree-builder wins decisively** (`ast-arena` ×1.0; the pointer-AST
  forms are ~2.1× on per-node `make_unique`). Contiguous memory layout is the whole game.
- **Python & Haskell: a pointer-AST builder wins, and `ast-arena` is *slower*.**
  Which pointer form is nominally fastest (recursive descent in Python, Pratt in
  Haskell) is within run-to-run noise — they cluster inside ~10%. The robust fact
  is that the arena's advantage was memory *layout*, which a boxed/GC'd runtime
  hides, so the trick evaporates.
- **The divide-and-conquer family (`multipass*`) loses everywhere** — it does more
  work to build the same tree. Among them, `multipass-reverse` is the best in Python
  (bottom-up iteration avoids the recursion+bisect overhead that punishes the
  top-down variants in a slow runtime) and mid-pack in C++/Haskell.

### …and the same comparison at 4 cores

Does splitting the work across cores reorder the builders? The same eight, on the
[scaling harness](.github/workflows/bench.yml) at **W=1 vs W=4 workers** (process
pool for Python — threads are GIL-flat), each column normalised to the fastest
builder *in that column* (**bold**):

| tree builder | C++ W1 | C++ W4 | Python W1 | Python W4 | Haskell W1 | Haskell W4 |
|---|--:|--:|--:|--:|--:|--:|
| `ast-recursive-descent` | 2.29 | 1.72 | 1.06 | 1.04 | **1.00** | **1.00** |
| `ast-shunting-yard` | 2.31 | 1.78 | **1.00** | **1.00** | 1.11 | 1.17 |
| `ast-pratt` | 2.28 | 1.69 | 1.10 | 1.10 | 1.11 | 1.06 |
| `ast-arena` | **1.00** | **1.00** | 1.18 | 1.20 | 1.30 | 1.44 |
| `multipass` | 4.01 | 2.98 | 2.06 | 2.11 | 1.37 | 1.45 |
| `multipass-arena` | 1.66 | 1.19 | 2.06 | 2.17 | 1.78 | 1.45 |
| `multipass-bfs` | 1.76 | 1.26 | 2.36 | 2.44 | 1.78 | 1.63 |
| `multipass-reverse` | 2.09 | 1.61 | 2.20 | 2.11 | 1.49 | 1.41 |

Compare the **W1 and W4 columns within each language** (not against the one-shot
table above — that's a different harness). The takeaway: **adding cores does not
reorder the builders.** C++ is identical W1→W4; Python and Haskell preserve the
tiers, with only adjacent *near-ties* trading places (Haskell's whole spread is
~1.0–1.6×, so neighbours are statistical ties). The absolute gaps wobble — C++'s
look a touch tighter at W4 — but that's run-to-run parallel-efficiency noise
(arena's efficiency alone swings 0.53–0.80 between runs), not a core-count effect.
**The AST-builder ranking is core-count-invariant.**

## When you build the tree and re-evaluate

The comparison above is **one-shot** (parse + evaluate once). If instead you build
the tree once and evaluate it many times with **different variable values**, the
verdict flips — building a reusable form is worth it after just **≲ 4 evaluations**,
because re-parsing every time is **~9–21× slower per eval**:

| per-eval, n=1000 | C++ | Python | Haskell |
|---|--:|--:|--:|
| best compiled form | 21k (bytecode) | 307k (bytecode) | 44k (ast-ptr) |
| re-parse every time | 208k | 2.86M | 892k |

And *which* form is fastest to re-evaluate is itself runtime-specific: flat
bytecode/arena in C++, bytecode in Python, the pointer-AST `Expr` in Haskell.
Details and the full table in [FINDINGS.md](FINDINGS.md).

## Multi-core scaling

Splitting the corpus across W workers (neutral 4-vCPU GitHub runner). The
per-expression **ranking is core-count-invariant** — only the runtime's
parallelism model decides whether more cores help:

| runtime | throughput scaling @ 4 workers |
|---|--:|
| C++ (`std::thread`) | ~2.1–3× |
| Haskell (`-threaded`) | ~2.3–2.8× |
| Python — **processes** | ~2.3–2.5× |
| Python — **threads** | ~0.95–1.0× (flat — the GIL) |

Per-strategy efficiency differences are run-to-run noise on a shared VM; real
8-core scaling needs dedicated hardware. (Regenerated by the
[bench workflow](.github/workflows/bench.yml).)

The headline across all twelve strategies — *in C++ memory layout dominates (flat
representations beat pointer-chasing ~2.7×), but in managed runtimes that signal
washes out and everything lands within ~1.5× — except the multipass family, which
does more work and loses everywhere* — is in **[FINDINGS.md](FINDINGS.md)**.
