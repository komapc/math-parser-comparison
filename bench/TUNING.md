# Same-rules tuning audit (2026-11)

Every implementation, ours included, was offered the same kind of hand tuning,
and every idea was measured. This file records what was tried, what was kept
and why, so a reader can check that the rivals were not left untuned.

## Rules

- **Measurement.** A full-build A/B on GitHub runners (the temporary `ab`
  workflow on the tuning branch). `base` is `main`, `var`
  is the tuning branch. Both are built in the same job and run interleaved,
  with the order alternated per round, and every job is compared within
  itself. Strategies whose code did not change are the controls. Laptop runs
  were not used to decide anything: untouched strategies moved 20–40 % there.
- **Accept** a change if it gains beyond the controls' spread and **no cell
  of any strategy sharing that code gets worse** than the controls.
- **Tie-break, added during the audit:** when a *rival's* result is
  ambiguous (it gains in some jobs and is neutral in others, with no worse
  cell), keep it. That can only make our claims more conservative. Our own
  changes get no such benefit of the doubt.
- **At most 2–3 ideas per strategy.** A technique the fused fold already has
  is offered to every strategy whose shape allows it.

## Grid

Percentages are base/var − 1, medians over jobs (+ means the change made the
strategy faster). Run IDs are `ab` workflow runs.

| Lang | Technique | Applied to | Result | Kept? | Runs |
|---|---|---|---|---|---|
| C++ | two-mode operand/operator scan | shunting-yard (tree + direct) | neutral to −7 % | no | 42925215038 |
| C++ | `^ leaf` fast path (twin of the sign fast path) | fold (tree + direct) | direct powchain +11 %, but direct nestchain −5 % and tree powchain −9 % on 9V74 | no | 42985526677 |
| Rust | level-aware builder forms (`add_sub`/`mul_div`/`pow`) in RD — the C++ RD already writes these inline | recursive descent | direct-rd corpus +7…+10 %, nestchain +13…+37 %; ast-arena flat | yes | 42983981414, 43112168784 |
| Rust | two-mode scan | shunting-yard | direct-SY +3 % corpus/powchain in one sample, −1…+2 % in the other; no worse cell (tie-break) | yes | 43110431215, 43112168784 |
| Rust | two-mode scan | bytecode compile | powchain −5 % on 9V74 | no | 43110431215 |
| Rust | `^ leaf` fast path | fold (tree + direct) | direct powchain +11 %, nestchain +10 %; tree nestchain −1…−3.5 % vs control in 8/9 jobs | no | 42983981414, 43112168784 |
| Python | hand inlining (cached token kind, helpers folded into the caller, precedence table) | RD, Pratt, SY, bytecode | classics +5…+10 % corpus, +4…+13 % powchain, +25…+35 % nestchain (RD/Pratt/arena) | yes | 42927364429, 42983981414, 43112168784 |
| Python | the same treatment | fold, multipass candidates | within the controls | no | 42927364429 |
| Python | `^ leaf` fast path | fold (tree + direct) | powchain +1…+5 %; other cells within the controls | yes | 42983981414, 43112168784 |
| Haskell | strict `case` on (result, rest) pairs instead of a lazy `let` | RD, Pratt, scannerless | relative to the five controls: direct-rd corpus +2…+5 %, powchain +6…+7 %; ast-arena powchain +5…+7 % | yes | 43111184403, 43112168784, 43180075680, 43181187054 |
| Haskell | operator kinds tested directly, not by `elem` | SY, bytecode | within noise; kept for symmetry with the fold | yes | 43111184403 |
| Haskell | the same `elem` change | multipass candidates, reverse-mp | within noise | no (they serve as controls) | 43111184403 |
| Haskell | pending level as (kind, operand) arguments, not `Maybe (Op, r)`; sign and `^ leaf` fast paths | fold (tree + direct) | relative to controls: tree powchain +4 %, but sumchain −2.6 % and nestchain −3.9 % | no | 43180075680 |
| Haskell | sign and `^ leaf` fast paths only (old `Maybe` pending) | fold (tree + direct) | relative to controls: tree powchain +4 %, sumchain −2.1 %, nestchain −2.0 %; direct form flat | no | 43181187054 |

## What it changed

Only rivals' changes survived in Rust and Haskell. The one change to ours
that was kept is the Python `^ leaf` fast path (powchain only). C++ is
unchanged. In Haskell all fifteen strategies moved a few percent between
builds, untouched ones included, so the Haskell numbers above are taken
relative to the median of five untouched strategies in the same job. The published tables are regenerated from a fresh 3× EPYC 9V74
batch after these changes, and the labels are pooled only over batches with
unchanged code.

## Effect on the published scoreboard

The published tables were regenerated from runs 43186179671, 43186221304
and 43201629859 (2026-11-12, commit a7f2a16, EPYC 9V74). Against the
previously published batch, 19 of the 72 labels changed; 13 went against
the fused form:

- **Python, tree form:** the corpus at n=1000 (tie → loses), at n=10000
  (narrowly ahead → loses), towerchain and nestchain (tie → loses).
- **Python, no-tree form:** n=1000 and sumchain (best → tie), n=10000
  (best → loses), powchain (tie → loses).
- **Rust, no-tree form:** the corpus at both sizes (tie → loses).
- **Rust, tree form:** towerchain (best → narrowly ahead).
- **Haskell, no-tree form:** powchain (narrowly ahead → loses) and
  towerchain (tie → loses).

Six went for it, and none comes from the fold getting faster:

- **Rust, tree form:** the corpus at both sizes, powchain and sumchain,
  tie → narrowly ahead. These moved because the column is no longer pooled
  with the earlier batches: `ast-arena` shares the recursive-descent parser
  that changed.
- **C++, no-tree form:** n=10000, loses → tie. C++ code is unchanged; this
  is run-to-run noise.
- **Haskell, no-tree form:** sumchain, loses → tie.

