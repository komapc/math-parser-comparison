#!/usr/bin/env python3
"""Differential fuzz — every strategy must agree with every other.

Mirrors cpp/tests/fuzz_differential.cpp: random well-formed expressions must
produce the same value from all fourteen strategies; randomly mutated (usually
malformed) ones must draw the same accept/reject verdict. Deterministic seed,
so failures reproduce. Exit 0 on success, 1 on any mismatch.

This is what backs the "all strategies implement one specification" claim;
the hand-written cases in test_parsers.py document the specification, this
enforces it in bulk. The mutation stage matters: a pure well-formed generator
never produces adjacent operand groups like "(a)(b)", which is exactly the
input the shunting-yard family once silently accepted.
"""
import random
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from mathparser import all_evaluators  # noqa: E402
from test_parsers import VARS, nearly  # noqa: E402

WELL_FORMED = 3000
MUTATED = 3000

rng = random.Random(20260702)


def gen(depth=0):
    r = rng.random()
    if depth > 6 or r < 0.30:
        if rng.random() < 0.3:
            return rng.choice("abcd")
        return rng.choice(["1", "2", "3", "0.5", "10", "7"])
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
        return s[:i] + rng.choice("1a+-*/^()... ") + s[i:]
    return s[:i] + s[i] + s[i:]


def main():
    evs = all_evaluators()
    mismatches = 0

    def try_eval(ev, expr):
        try:
            return ev.eval(expr, VARS), False
        except Exception:  # noqa: BLE001 — any rejection counts the same
            return 0.0, True

    def check_all(expr):
        nonlocal mismatches
        base, base_raised = try_eval(evs[0], expr)
        for ev in evs[1:]:
            val, raised = try_eval(ev, expr)
            if raised != base_raised or (not raised and not nearly(val, base)):
                mismatches += 1
                print(f"MISMATCH on {expr!r}: {evs[0].name}="
                      f"{'raise' if base_raised else base!r} vs "
                      f"{ev.name}={'raise' if raised else val!r}")

    for _ in range(WELL_FORMED):
        check_all(gen())
    for _ in range(MUTATED):
        check_all(mutate(gen()))

    print(f"{WELL_FORMED} well-formed + {MUTATED} mutated exprs x "
          f"{len(evs)} strategies, {mismatches} mismatch(es)")
    return 0 if mismatches == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
