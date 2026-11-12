# Bottom-up multipass parsing — the one-pager

**Claim tested:** a parser that reduces an expression *bottom-up* — deepest
parentheses first, then `^`/unary, then `* /`, then `+ -`, one sweep per
precedence level, no split search — is a competitive way to evaluate
`-2 ^ 2 + 3 * (4 - 1)`, not just a curiosity.

## Verdict

- **It is correct.** It agrees with the other fourteen strategies, value or
  rejection, on every spec case and on 6 000 fuzzed inputs per run, in all
  four languages.
- **Against its own top-down family it is ahead on every tested input** — all
  24 cells of the family scoreboard below (6 inputs × 4 languages): *best*
  on 23, *ahead, narrowly* on C++ sumchain (+6…+24 % here, +4 % in one
  earlier run; see the note below that scoreboard). It needs no
  worst-case machinery: its worst case *is* its average case. On the
  family's own C++ worst-case shape the top-down variants trail the fused
  form by 1.9–4.9× (and the buffered version of it, itself 1.3–1.4× slower
  than the fused form, by 1.5–3.6×).
- **Against the classic parsers it is a peer in C++ and Rust** — not a
  winner — **and behind in Python and Haskell.** These numbers are after a
  [same-rules tuning pass](../bench/TUNING.md) that offered every
  implementation, ours included, the same hand optimisations: the rivals
  gained 5–10 % (Python, Rust's recursive descent), ours almost nothing.
  - C++: the fastest tree builder on the random corpus (narrowly, +3…+5 %)
    and on every structured shape; its no-tree form ties
    `direct-shunting-yard` on the corpus and trails it on nestchain by
    15–17 %.
  - Rust: the tree form is narrowly ahead of the arena classic everywhere
    but nestchain, where it is clearly ahead. The no-tree form loses to
    the tuned `direct-rd` on the random corpus by 6–7 %, is narrowly ahead
    on powchain and clearly ahead on the other three shapes.
  - Python: after the classics were hand-inlined, the tree form loses to
    `ast-pratt` on every input (1–2 % at n=10000, 8–15 % elsewhere). The
    no-tree form ties `direct-rd` at n=1000 and on sumchain, loses by 1 %
    at n=10000 and by 5–6 % on powchain, and is clearly ahead on towerchain
    and nestchain.
- **In Haskell both forms lose.** The tree form is 1.1–1.4× slower than the
  pointer-AST classics on every input. Its edge elsewhere appears to come
  from contiguous memory, and a runtime that boxes every node leaves little
  of that to exploit. The no-tree form ties recursive descent on sumchain
  and is 3–15 % behind it everywhere else.

## Scoreboard

### Tree-building form `multipass-reverse-fold` vs the best classic tree builder

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | ahead, narrowly (+3…+4 % vs `ast-arena`) | ahead, narrowly (+1 % vs `ast-arena`) | loses (-10…-8 % vs `ast-pratt`) | loses (1.2× slower than `ast-rd`) |
| random corpus (n=10000) | ahead, narrowly (+4…+5 % vs `ast-arena`) | ahead, narrowly (+1…+2 % vs `ast-arena`) | loses (-2…-1 % vs `ast-pratt`) | loses (1.3–1.4× slower than `ast-rd`) |
| powchain | **best** (+11…+15 % vs `ast-arena`) | ahead, narrowly (+1 % vs `ast-arena`) | loses (-15…-13 % vs `ast-pratt`) | loses (1.2× slower than `ast-rd`) |
| towerchain | **best** (+6…+7 % vs `ast-arena`) | ahead, narrowly (+4…+8 % vs `ast-arena`) | loses (-11…-10 % vs `ast-pratt`) | loses (1.1–1.2× slower than `ast-rd`) |
| sumchain | **best** (+19…+22 % vs `ast-arena`) | ahead, narrowly (+2…+4 % vs `ast-arena`) | loses (-15…-13 % vs `ast-pratt`) | loses (1.1–1.2× slower than `ast-rd`) |
| nestchain | **best** (+27…+32 % vs `ast-arena`) | **best** (+31…+36 % vs `ast-arena`) | loses (-8 % vs `ast-pratt`) | loses (1.2–1.3× slower than `ast-pratt`) |

### No-tree form `direct-reverse` vs the best classic no-tree evaluator

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | tie (0 % vs `direct-sy`) | loses (-7…-6 % vs `direct-rd`) | tie (0…+1 % vs `direct-rd`) | loses (-6…-5 % vs `direct-rd`) |
| random corpus (n=10000) | tie (-1…0 % vs `direct-sy`) | loses (-7 % vs `direct-rd`) | loses (-1 % vs `direct-rd`) | loses (-13…-12 % vs `direct-rd`) |
| powchain | ahead, narrowly † (+8…+9 % vs `direct-sy`) | ahead, narrowly (+2 % vs `direct-rd`) | loses (-6…-5 % vs `direct-rd`) | loses (-10…-3 % vs `direct-rd`) |
| towerchain | tie † (+5…+8 % vs `direct-sy`) | **best** (+21 % vs `direct-rd`) | **best** (+5…+7 % vs `direct-rd`) | loses (-5…-3 % vs `direct-rd`) |
| sumchain | tie † (+3…+39 % vs `direct-sy`) | **best** (+14 % vs `direct-rd`) | tie (0…+2 % vs `direct-rd`) | tie (-3…+2 % vs `direct-rd`) |
| nestchain | loses (-17…-15 % vs `direct-sy`) | **best** (+7 % vs `direct-rd`) | **best** (+6…+8 % vs `direct-sy`) | loses (-15…-12 % vs `direct-rd`) |

† Label over all twelve runs of the four 9V74 batches; only the C++
columns are pooled this way (their code did not change). This batch's three
runs alone would say: C++ powchain and towerchain *best*, C++ sumchain
*ahead, narrowly*. The C++ sumchain range is wide because `direct-sy` ran
in a slow mode on sumchain in one run (+39 %); in the other two the margin
was +3 % and +5 %.

### `multipass-reverse-fold` vs the best of its own top-down family

| input | C++ | Rust | Python | Haskell |
|---|---|---|---|---|
| random corpus (n=1000) | **best** (+32 % vs `direct-mp`) | **best** (+58 % vs `direct-mp`) | **best** (+40…+41 % vs `direct-mp`) | **best** (+36 % vs `multipass`) |
| random corpus (n=10000) | **best** (+34 % vs `direct-mp`) | **best** (+64 % vs `direct-mp`) | **best** (+42 % vs `direct-mp`) | **best** (+44…+49 % vs `multipass`) |
| powchain | **best** (+48…+51 % vs `direct-mp`) | **best** (+73 % vs `direct-mp`) | **best** (+22…+23 % vs `direct-mp`) | **best** (+47…+50 % vs `multipass`) |
| towerchain | **best** (+48 % vs `direct-mp`) | **best** (+56…+57 % vs `direct-mp`) | **best** (+12…+14 % vs `direct-mp`) | **best** (+57…+60 % vs `multipass`) |
| sumchain | ahead, narrowly † (+6…+24 % vs the faster of `direct-mp` and `multipass-arena`) | **best** (+48…+49 % vs `direct-mp`) | **best** (+10…+12 % vs `direct-mp`) | **best** (+26…+27 % vs `multipass`) |
| nestchain | **best** (+48…+50 % vs `direct-mp`) | **best** (+23…+25 % vs `direct-mp`) | **best** (+37…+38 % vs `direct-mp`) | **best** (+46…+47 % vs `multipass`) |

Sumchain is where the top-down members come closest to the fused form —
not a rescued-family win: its single precedence level gives the split-scan
machinery nothing to search, so the "budget + buckets" rescue and the plain
buffer end up doing the same trivial pass. The fused form stays ahead of the
whole family there regardless: +6…+24 % over the faster of `direct-mp`
and `multipass-arena` in each run. C++ `direct-mp` on sumchain is bimodal —
~30 or ~46 ns/leaf, switching between sizes and runs with no pattern, in
this batch and in every earlier one — and in its fast mode it comes within
+6 % here and +4 % in one earlier run, hence *ahead, narrowly* (the C++
family column is labelled over all twelve runs of the four 9V74 batches;
on the medians of this batch alone `multipass-arena` is the closest member
at +23…+24 %). Since
2026-09-26 the top-down members no longer pay per-call allocations the fold
never paid (and Python's no longer pays a `key=lambda` per bisect step); as
of this batch no arena or no-tree strategy allocates anything per
evaluation in C++ or Rust (the last right-fold buffers, token arrays and
scratch stacks went in [#20](https://github.com/komapc/math-parser-comparison/pull/20)),
and the family gaps barely moved.

Numbers behind the cells (ns/leaf, medians of the three runs) are in
[FINDINGS.md](../FINDINGS.md) and the per-language READMEs; the
walk-through of the algorithm is in [multipass-reverse.md](multipass-reverse.md).

## How it was tested

**Setup.** The same algorithm was implemented in C++, Rust, Python and
Haskell next to fourteen other strategies (recursive descent, shunting-yard,
Pratt, arena AST, bytecode VM, four top-down divide-and-conquer variants and
a lexer-free control), all sharing one lexer, one grammar, one spec suite
and differential fuzzing. Every strategy ran on a shared random corpus at
10, 100, 1 000 and 10 000 leaves and on four adversarial shapes
(mixed-precedence chains, `^`-towers, a single-precedence chain, deep
nesting), on a neutral 4-vCPU GitHub runner, **three independent times**,
all four languages from the same commit in the same runs. All three runs
landed on one CPU model (AMD EPYC 9V74, read from each run's `env.txt`),
toolchains pinned (GCC 14.2, rustc 1.98.1, GHC 9.6.7, the image's Python
3.12). Within each run the strategies are timed round-robin, so drift on
the runner falls on all of them alike. In an earlier batch one run ran
~1.3× slower in absolute terms on the same model, which is why only
within-run ratios are compared.

**Labels.** A cell is labelled from the *range across the three runs*
(lowest%, highest%):
- **best** if the low end is ≥5 % ahead;
- *ahead, narrowly* if the low end is ≥0.5 % ahead but under 5 %;
- *loses* if the high end is ≤0.5 % behind;
- *tie* otherwise — which covers a sign flip across runs, but also a range
  that sits entirely within ±0.5 %, ahead or behind, since that is noise on
  this runner.

**Earlier batches (†).** Only columns whose code did not change since the
earlier 9V74 batches are labelled over more than this batch: the C++
columns of all three tables, over all twelve runs of the four batches (2026-09-26, two on
2026-11-10, and this one), and the Haskell family column. Every other
column had code change on at least one side in between (the Rust family in
#20, the rest in the 2026-11 tuning pass), so those are labelled from this
batch's three runs alone. Eight labels moved
between the 2026-09-26 batch and the 2026-11-10 one without a code change
(the runner image and kernel had changed in between), so a margin that
clears a label boundary in only some batches does not get that label.
Cells marked † carry a label that this batch's three runs alone would not
give; the range shown is still this batch's.

**Other CPU models.** The range measures run-to-run spread on *one* CPU
model. GitHub hands out several models, and across them the ratios move by
more than that. Of the same commit, three runs landed on AMD EPYC 7763 and
three on Intel Xeon Platinum 8573C.

On the **EPYC 7763**, 14 of the 72 labels change.
- Against the fused form (8):
  - Rust's tree form *loses* to `ast-arena` on the random corpus at both
    sizes (−5…−4 %) and only ties on powchain (0…+2 %);
  - Rust's no-tree form loses powchain to `direct-rd` (−6 %) and is only
    narrowly ahead on nestchain (+3…+4 %);
  - C++'s no-tree form loses to `direct-sy` on the random corpus at both
    sizes (−2…−1 %);
  - Python's no-tree form is only narrowly ahead on towerchain (+4 %).
- For it (6):
  - Rust's tree form is *best* on towerchain (+7 %);
  - C++'s no-tree form is narrowly ahead on towerchain (+4…+6 %) and
    *best* on sumchain (+8…+11 %);
  - Python's no-tree form ties at n=10000; Haskell's ties on towerchain;
  - C++ sumchain in the family table is *best* (+20…+25 %).

On the **Xeon 8573C**, 14 of the 72 labels change.
- Against the fused form (11):
  - Rust's tree form *loses* to `ast-arena` on the random corpus at both
    sizes (−6…−5 %) and ties on powchain (−1…0 %);
  - C++'s tree form only ties at n=10000 (0…+1 %) and is only narrowly
    ahead on powchain (+4…+9 %);
  - Rust's no-tree form only ties on powchain (−2…+1 %) and is only
    narrowly ahead on towerchain (+4…+5 %) and nestchain (+3…+7 %);
  - C++'s no-tree form loses at n=1000 (−1 %); Python's is only narrowly
    ahead on towerchain (+1…+4 %);
  - C++ sumchain in the family table is a tie (−2…+12 % vs `direct-mp`).
- For it (3): Python's tree form ties at n=10000, its no-tree form ties at
  n=10000, and C++'s no-tree form is *best* on powchain (+9…+11 %).

The tables here use EPYC 9V74 because the previously published batches ran
on it, and they keep that model for continuity rather than mixing models;
the other models' results are reported here instead of being averaged in.
Treat any cell within ~7 % of the line as environment-dependent.

## What it does *not* claim

- It is not faster than a lexer-free evaluator: `direct-scannerless`, recursive
  descent with the lexer fused into the grammar, beats every strategy in C++,
  Rust and Python by roughly 16–35 % (Python 16 %, C++ 20 %, Rust 35 %),
  and Haskell's by ~9 %. That is the cost of having a shared
  lexer at all and every real strategy in the table pays it; it is the
  control, not a rival.
- The design targets this one four-level grammar. "One sweep per precedence
  level" does not carry over for free to function calls of arbitrary arity,
  mixed associativity, statements, or a dozen-plus precedence levels; for
  those, recursive descent and Pratt parsing remain the general-purpose
  default.
- Margins under ~5 % are reported as ranges and labelled *narrowly* or *tie*
  on purpose: single-run numbers at that scale flip sign on the same runner,
  and batches run weeks apart moved single cells by up to 7 points.

*Data: CI bench runs 43186179671, 43186221304, 43201629859 (2026-11-12,
commit a7f2a16 — source-identical to `main` after the tuning PRs #25–#28 —
all on AMD EPYC 9V74, the first three of 12 dispatched that landed on it);
C++ and Haskell-family labels also over the earlier 9V74 batches
36226353574, 36226831278, 36227295521 (2026-09-26, 921ec7f),
42791921436, 42791964111, 42793955904 (2026-11-10, d2474e2) and
42805297064, 42806389267, 42806442518 (2026-11-10, 7b83ad4). Other models:
43186189949, 43186200491, 43201661232 (EPYC 7763) and 43186170714,
43186210593, 43201619944 (Xeon 8573C). All archived in
[`bench/results/`](../bench/results/) and regenerated with
[`bench/tables.py`](../bench/tables.py), command in its README.*
