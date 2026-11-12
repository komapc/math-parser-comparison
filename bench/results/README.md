# Archived benchmark outputs

GitHub deletes workflow artifacts after 90 days, so every CI run that the
docs cite is copied here verbatim from its `benchmark-results` artifact
(`gh run download <id> -R komapc/math-parser-comparison`). One directory per
run id; the files are the unmodified outputs of `.github/workflows/bench.yml`.

| runs | date | commit | used for |
|---|---|---|---|
| 43186179671, 43186221304, 43201629859 | 2026-11-12 | a7f2a16 | every published table (EPYC 9V74), after the same-rules tuning pass ([TUNING.md](../TUNING.md)) |
| 43186189949, 43186200491, 43201661232 | 2026-11-12 | a7f2a16 | the same commit on EPYC 7763 (one-pager's model-dependence note) |
| 43186170714, 43186210593, 43201619944 | 2026-11-12 | a7f2a16 | the same commit on Xeon Platinum 8573C (one-pager's model-dependence note) |
| 42805297064, 42806389267, 42806442518 | 2026-11-10 | 7b83ad4 | the previous published tables; `--prior` batch (EPYC 9V74) |
| 42805321027, 42805332588, 42806415065 | 2026-11-10 | 7b83ad4 | the same commit on EPYC 7763 |
| 42791921436, 42791964111, 42793955904 | 2026-11-10 | d2474e2 | `--prior` batch (EPYC 9V74) |
| 36226353574, 36226831278, 36227295521 | 2026-09-26 | 921ec7f | `--prior` batch (EPYC 9V74) |
| 36163486136, 36163570591, 36163577544 | 2026-09-25 | 3064d0f | earlier batch, cited in FINDINGS |
| 34136843367, 34137622680, 34138407724 | 2026-09-07 | 5674fb5 | cited in FINDINGS (predate `env.txt`) |

Regenerate the published tables:

```bash
R=bench/results
python3 bench/tables.py $R/43186179671 $R/43186221304 $R/43201629859 \
    --prior $R/42805297064 $R/42806389267 $R/42806442518 \
    --prior $R/36226353574 $R/36226831278 $R/36227295521 \
    --prior $R/42791921436 $R/42791964111 $R/42793955904
```

The `--prior` batches ran on the same CPU model. A label is pooled over
them only where the code on both sides of the comparison did not change in
between (`NOT_POOLED` in `tables.py` lists the rest). Between 921ec7f and
7b83ad4 the code changed only in the top-down family (C++ `multipass`,
`multipass-arena`, `multipass-bfs`; Rust's whole family including
`direct-mp`, plus its `tokenize_into` lexer entry point), the Haskell
`multipass-reverse` error path, the Haskell lexer's long-exponent path, and
tests. Between 7b83ad4 and a7f2a16 the same-rules tuning pass changed both
sides of every Python column and the Rust and Haskell classics. So only the
C++ columns and the Haskell family column are labelled over all twelve
runs; every other column uses the main batch alone. Ranges and every other
number come from the main batch. Labels that the main batch alone would
have given differently are marked †. The one exception is hand-applied:
C++ family sumchain is published as *ahead, narrowly* (the `perrun` rule's
label) rather than the median rule's *best*, because the bimodal
`direct-mp` comes within +6 % in one run of the main batch and +4 % in one
2026-09-26 run.

For the model-dependence note, run the same script on the 7763 or 8573C
directories alone and compare labels.
