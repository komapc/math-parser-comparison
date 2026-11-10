# Archived benchmark outputs

GitHub deletes workflow artifacts after 90 days, so every CI run that the
docs cite is copied here verbatim from its `benchmark-results` artifact
(`gh run download <id> -R komapc/math-parser-comparison`). One directory per
run id; the files are the unmodified outputs of `.github/workflows/bench.yml`.

| runs | date | commit | used for |
|---|---|---|---|
| 42805297064, 42806389267, 42806442518 | 2026-11-10 | 7b83ad4 | every published table (EPYC 9V74) |
| 42805321027, 42805332588, 42806415065 | 2026-11-10 | 7b83ad4 | the same commit on EPYC 7763 (one-pager's model-dependence note) |
| 42791921436, 42791964111, 42793955904 | 2026-11-10 | d2474e2 | `--prior` batch for the tree/direct labels (EPYC 9V74) |
| 36226353574, 36226831278, 36227295521 | 2026-09-26 | 921ec7f | the previous published tables; `--prior` batch for the tree/direct labels (EPYC 9V74) |
| 36163486136, 36163570591, 36163577544 | 2026-09-25 | 3064d0f | earlier batch, cited in FINDINGS |
| 34136843367, 34137622680, 34138407724 | 2026-09-07 | 5674fb5 | cited in FINDINGS (predate `env.txt`) |

Regenerate the published tables:

```bash
R=bench/results
python3 bench/tables.py $R/42805297064 $R/42806389267 $R/42806442518 \
    --prior $R/36226353574 $R/36226831278 $R/36227295521 \
    --prior $R/42791921436 $R/42791964111 $R/42793955904
```

The `--prior` batches ran on the same CPU model with the same code for the
fold, `direct-reverse` and the classics (d2474e2 and 7b83ad4 differ from
921ec7f only in the top-down family, the Haskell `multipass-reverse` error
path and the Haskell lexer's long-exponent path). Only the tree and direct
scoreboard labels use them: each such cell is labelled over all nine runs,
so a margin that changes sign between batches reads as a tie. Ranges and
every other number come from the main batch alone. Labels that the main
batch alone would have given differently are marked †.
