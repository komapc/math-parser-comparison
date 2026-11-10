#!/usr/bin/env python3
"""Generate the shared, deterministic differential-fuzz corpora.

Every language's fuzz test cross-checks all fifteen strategies against the
SAME generated inputs, so a bug can't hide in one language's own generator
(e.g. it never producing a construct another language's parser mishandles).
Each language ALSO keeps its own self-generated fuzz (different PRNG, own
input distributions — e.g. C++'s very-deep-nesting genLong()) as additional,
implementation-specific coverage; this shared corpus is on top of that, not
instead of it.

Generator and mutation logic carried over from python/test_fuzz.py's
original, pre-shared-corpus generator (same seed, same recursive shape),
broadened with scientific-notation/overflow literals ("2e3", "1e-5",
"1e400"), random literals of every accepted form, hard IEEE cases, and
tab/exponent-char mutations ("eE\t") to also cover the number grammar's
edge cases — this is a superset of what Python's fuzz test
exercised before, not a byte-for-byte port.

Usage:  python3 bench/gen_fuzz.py
Writes: bench/fuzz/well_formed.txt  (3000 lines)
        bench/fuzz/mutated.txt      (3000 lines)
"""
import os
import random

OUT = os.path.join(os.path.dirname(__file__), "fuzz")
WELL_FORMED = 3000
MUTATED = 3000
SEED = 20260702  # same seed value python/test_fuzz.py, cpp and rust fuzz use

rng = random.Random(SEED)


# Literals where a lexer's number conversion is most likely to go wrong:
# halfway cases, the subnormal and overflow boundaries, 17+ significant
# digits, and exponents too long for a machine integer.
HARD_LITERALS = [
    "9007199254740993", "9007199254740992.5", "0.1", "0.30000000000000004",
    "2.2250738585072011e-308", "2.2250738585072014e-308", "4.9406564584124654e-324",
    "2.4703282292062327e-324", "2.4703282292062328e-324", "1.7976931348623157e308",
    "1.7976931348623158e308", "1.7976931348623159e308", "123456789012345678901234567890",
    "0.000000000000000000000000000001e30", "1e-10000000000000000000", "0e99999999999999999999",
    "1e9223372036854775807", "1e9223372036854775808", "1e-9223372036854775809",
]


def digits(lo, hi):
    return "".join(rng.choice("0123456789") for _ in range(rng.randint(lo, hi)))


def literal():
    """A random literal in every form the grammar accepts: `12`, `1.5`,
    `.5`, `5.`, with an optional exponent of 1-20 digits (`e`/`E`, signed or
    not) — so number parsing, not only arithmetic, is compared across
    languages (bench/xlang_compare.py)."""
    if rng.random() < 0.25:
        return rng.choice(HARD_LITERALS)
    whole = digits(1, 20) if rng.random() < 0.85 else ""
    frac = "." + digits(0 if whole else 1, 20) if rng.random() < 0.5 else ""
    if not whole and not frac:
        whole = digits(1, 3)
    exp = ""
    if rng.random() < 0.5:
        n = digits(1, 3) if rng.random() < 0.85 else digits(18, 20)
        exp = rng.choice("eE") + rng.choice(["", "+", "-"]) + n
    return whole + frac + exp


def gen(depth=0):
    r = rng.random()
    if depth > 6 or r < 0.30:
        if rng.random() < 0.3:
            return rng.choice("abcd")
        if rng.random() < 0.25:
            return literal()
        return rng.choice(["1", "2", "3", "0.5", "10", "7",
                            "2e3", "1e-5", "1e400"])
    if r < 0.42:
        return "-" + gen(depth + 1)
    if r < 0.48:
        return "+" + gen(depth + 1)
    if r < 0.60:
        return "(" + gen(depth + 1) + ")"
    op = rng.choice(["+", "-", "*", "/", "^", "^"])
    return gen(depth + 1) + " " + op + " " + gen(depth + 1)


def mutate(s):
    """Random single-character mutation; result is usually malformed."""
    if not s:
        return "("
    which = rng.randrange(3)
    i = rng.randrange(len(s))
    if which == 0:
        return s[:i] + s[i + 1:]
    if which == 1:
        return s[:i] + rng.choice("1a+-*/^()... eE\t") + s[i:]
    return s[:i] + s[i] + s[i:]


def write_atomic(path, text):
    # Every language's test suite generates this corpus on demand if it's
    # missing, so a fresh checkout can see cpp/rust/python/haskell all shell
    # out to this script at once. A plain open(path, "w") truncates in place,
    # so a concurrent reader can observe a partially-written file; writing to
    # a same-directory temp file and os.replace()-ing it into place is atomic
    # on POSIX, so every reader sees either the old (absent) or the complete
    # new file, never a partial one.
    tmp = path + f".tmp{os.getpid()}"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, path)


def main():
    os.makedirs(OUT, exist_ok=True)

    well_formed = [gen() for _ in range(WELL_FORMED)]
    mutated = [mutate(gen()) for _ in range(MUTATED)]

    wf_path = os.path.join(OUT, "well_formed.txt")
    mu_path = os.path.join(OUT, "mutated.txt")
    write_atomic(wf_path, "\n".join(well_formed) + "\n")
    write_atomic(mu_path, "\n".join(mutated) + "\n")

    print(f"wrote {wf_path}: {len(well_formed)} expressions")
    print(f"wrote {mu_path}: {len(mutated)} expressions")


if __name__ == "__main__":
    main()
