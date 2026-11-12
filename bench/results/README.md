# Archived benchmark outputs

GitHub deletes workflow artifacts after 90 days, so every CI run that the
docs cite is copied here verbatim from its `benchmark-results` artifact
(`gh run download <id> -R komapc/math-parser-comparison`). One directory per
run id; the files are the unmodified outputs of `.github/workflows/bench.yml`.

| runs | date | commit | used for |
|---|---|---|---|
| 43227113612, 43227138258, 43231342538 | 2026-11-12 | fold-only pass | every published table (EPYC 9V74), after the fold-only tuning pass ([TUNING.md](../TUNING.md)) |
| 43227145326, 43227152139, 43231354248 | 2026-11-12 | fold-only pass | the same commit on EPYC 7763 (one-pager's model-dependence note) |
| 43186179671, 43186221304, 43201629859 | 2026-11-12 | a7f2a16 | the previous published tables, after the same-rules tuning pass; `--prior` batch (EPYC 9V74) |
| 43186189949, 43186200491, 43201661232 | 2026-11-12 | a7f2a16 | the same commit on EPYC 7763 |
| 43186170714, 43186210593, 43201619944 | 2026-11-12 | a7f2a16 | the same commit on Xeon Platinum 8573C |
| 42805297064, 42806389267, 42806442518 | 2026-11-10 | 7b83ad4 | `--prior` batch (EPYC 9V74) |
| 42805321027, 42805332588, 42806415065 | 2026-11-10 | 7b83ad4 | the same commit on EPYC 7763 |
| 42791921436, 42791964111, 42793955904 | 2026-11-10 | d2474e2 | `--prior` batch (EPYC 9V74) |
| 36226353574, 36226831278, 36227295521 | 2026-09-26 | 921ec7f | `--prior` batch (EPYC 9V74) |
| 36163486136, 36163570591, 36163577544 | 2026-09-25 | 3064d0f | earlier batch, cited in FINDINGS |
| 34136843367, 34137622680, 34138407724 | 2026-09-07 | 5674fb5 | cited in FINDINGS (predate `env.txt`) |

Regenerate the published tables (priors newest first — the order matters):

```bash
R=bench/results
python3 bench/tables.py $R/43227113612 $R/43227138258 $R/43231342538 \
    --prior $R/43186179671 $R/43186221304 $R/43201629859 \
    --prior $R/42805297064 $R/42806389267 $R/42806442518 \
    --prior $R/42791921436 $R/42791964111 $R/42793955904 \
    --prior $R/36226353574 $R/36226831278 $R/36227295521
```

The `--prior` batches ran on the same CPU model. A label is pooled over
the newest *k* of them, where *k* (`POOL_DEPTH` in `tables.py`, per
language and column) is how far back the code on both sides of the
comparison stayed unchanged. Between 921ec7f and 7b83ad4 the code changed
only in the top-down family (C++ `multipass`, `multipass-arena`,
`multipass-bfs`; Rust's whole family including `direct-mp`, plus its
`tokenize_into` lexer entry point), the Haskell `multipass-reverse` error
path, the Haskell lexer's long-exponent path, and tests. Between 7b83ad4
and a7f2a16 the same-rules tuning pass changed both sides of every Python
column and the Rust and Haskell classics. The fold-only pass after a7f2a16
changed only the Python fold. So:

| column | C++ | Rust | Python | Haskell |
|---|--:|--:|--:|--:|
| tree | 4 | 1 | 0 | 1 |
| no tree | 4 | 1 | 0 | 1 |
| family | 3 | 2 | 0 | 4 |

Ranges and every other number come from the main batch. Labels that the
main batch alone would have given differently are marked †. The one
exception is hand-applied: C++ family sumchain is published as *best*
with the range against the faster of `direct-mp` and `multipass-arena`
written out, because the bimodal `direct-mp` makes the per-rival range
misleading (see the one-pager's note).

For the model-dependence note, run the same script on the 7763 (or older 8573C)
directories alone and compare labels.
