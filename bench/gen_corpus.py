#!/usr/bin/env python3
"""Generate shared, deterministic expression corpora for cross-language benchmarks.

Every language reads the SAME files, so the benchmark measures the implementation,
not differences in input. Grammar mirrors cpp/bench/benchmark.cpp:
  leaves 1..20, ops + + - - * /, ~1/3 of sub-exprs parenthesised, ~1/7 use ^.
Numeric operands only (no variables) — keeps the cross-language harness trivial.

Usage:  python3 bench/gen_corpus.py
Writes: bench/corpus/n{10,100,1000,10000}.txt  (one expression per line)
"""
import os
import random

OUT = os.path.join(os.path.dirname(__file__), "corpus")
SIZES = [10, 100, 1000, 10000]
OPS = ["+", "+", "-", "-", "*", "/"]


def gen(n: int, rng: random.Random, out: list[str], variables: bool) -> None:
    if n <= 1:
        if rng.randint(0, 4) == 0:
            out.append("-")
        # variable corpus: ~half the leaves are variables a-d (for reeval, where
        # the compiled form is evaluated against changing variable bindings).
        if variables and rng.randint(0, 1) == 0:
            out.append(chr(ord("a") + rng.randint(0, 3)))
        else:
            out.append(str(rng.randint(1, 20)))
        return
    paren = rng.randint(0, 2) == 0
    if paren:
        out.append("(")
    if rng.randint(0, 6) == 0:
        gen(n - 1, rng, out, variables)
        out.append(" ^ ")
        out.append(str(rng.randint(1, 3)))
    else:
        left = rng.randint(1, n - 1)
        gen(left, rng, out, variables)
        out.append(" " + rng.choice(OPS) + " ")
        gen(n - left, rng, out, variables)
    if paren:
        out.append(")")


def make(leaves: int, seed: int, variables: bool = False) -> str:
    rng = random.Random(seed)
    parts: list[str] = []
    gen(leaves, rng, parts, variables)
    return "".join(parts)


def write_corpus(variables: bool) -> None:
    prefix = "vars_n" if variables else "n"
    for size in SIZES:
        count = max(4, 100_000 // size)  # ~constant total work, matches C++ harness
        seed0 = 0xABCDE if variables else 0xC0FFEE
        lines = [make(size, seed0 + size * 1000 + i, variables) for i in range(count)]
        path = os.path.join(OUT, f"{prefix}{size}.txt")
        with open(path, "w") as f:
            f.write("\n".join(lines) + "\n")
        print(f"wrote {path}: {count} expressions of {size} leaves")


def main() -> None:
    os.makedirs(OUT, exist_ok=True)
    write_corpus(variables=False)  # numeric — one-shot benches
    write_corpus(variables=True)   # variables a-d — reeval benches


if __name__ == "__main__":
    main()
