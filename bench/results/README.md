# Archived benchmark outputs

GitHub deletes workflow artifacts after 90 days, so every CI run that the
docs cite is copied here verbatim from its `benchmark-results` artifact
(`gh run download <id> -R komapc/math-parser-comparison`). One directory per
run id; the files are the unmodified outputs of `.github/workflows/bench.yml`.

| runs | date | commit | used for |
|---|---|---|---|
| 36226353574, 36226831278, 36227295521 | 2026-09-26 | 921ec7f | every published table (EPYC 9V74) |
| 36163486136, 36163570591, 36163577544 | 2026-09-25 | 3064d0f | earlier batch, cited in FINDINGS |
| 34136843367, 34137622680, 34138407724 | 2026-09-07 | 5674fb5 | cited in FINDINGS (predate `env.txt`) |

Regenerate the published tables:

```bash
python3 bench/tables.py bench/results/36226353574 bench/results/36226831278 bench/results/36227295521
```
