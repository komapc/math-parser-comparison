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


def gen(n: int, rng: random.Random, out: list[str]) -> None:
    if n <= 1:
        if rng.randint(0, 4) == 0:
            out.append("-")
        out.append(str(rng.randint(1, 20)))
        return
    paren = rng.randint(0, 2) == 0
    if paren:
        out.append("(")
    if rng.randint(0, 6) == 0:
        gen(n - 1, rng, out)
        out.append(" ^ ")
        out.append(str(rng.randint(1, 3)))
    else:
        left = rng.randint(1, n - 1)
        gen(left, rng, out)
        out.append(" " + rng.choice(OPS) + " ")
        gen(n - left, rng, out)
    if paren:
        out.append(")")


def make(leaves: int, seed: int) -> str:
    rng = random.Random(seed)
    parts: list[str] = []
    gen(leaves, rng, parts)
    return "".join(parts)


def main() -> None:
    os.makedirs(OUT, exist_ok=True)
    for size in SIZES:
        count = max(4, 100_000 // size)  # ~constant total work, matches C++ harness
        lines = [make(size, 0xC0FFEE + size * 1000 + i) for i in range(count)]
        path = os.path.join(OUT, f"n{size}.txt")
        with open(path, "w") as f:
            f.write("\n".join(lines) + "\n")
        print(f"wrote {path}: {count} expressions of {size} leaves")


if __name__ == "__main__":
    main()
