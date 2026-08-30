#!/usr/bin/env python3
"""Correctness suite — every case is run against every strategy.

Mirrors cpp/tests/test_parsers.cpp: variable environment a=2, b=3, c=4, d=5,
plus a few numeric-literal cases for features a-d cannot express. Exit 0 on
success, 1 on any failure (so it works as a CI/ctest-style check).
"""
import math
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from mathparser import all_evaluators  # noqa: E402

VARS = [0.0] * 26
VARS[0], VARS[1], VARS[2], VARS[3] = 2.0, 3.0, 4.0, 5.0  # a,b,c,d

CASES = [
    ("a + b * c", 14),
    ("(a + b) * c", 20),
    ("a * (b + c) - d", 9),
    ("(a + b) * b / d", 3),
    ("-(b + c) * a", -14),
    ("d - a - b", 0),
    ("d * c / a", 10),
    ("a ^ a ^ a", 16),
    ("-a ^ a", -4),
    ("a ^ -b", 0.125),
    ("3.5 * a + 1.5", 8.5),
    ("2e2 + a", 202),
    ("--a", 2),
    ("-a * b", -6),
    ("+d - +a", 3),
    # IEEE special values — must match C++ std::pow / IEEE division exactly
    ("0 ^ -1", math.inf),
    ("1 / (0 * -1)", -math.inf),
    ("(0 - 1e155) ^ 3", -math.inf),
    ("(0 / 0) / 0", math.nan),
    ("(0 - a) ^ 0.5", math.nan),
    # literal overflow/underflow is a value (inf / 0), not a syntax error
    ("1e400", math.inf),
    ("1e-400 + a", 2),
]

ERRORS = [
    "a +", "(a + b", "a b", "* a", "a + * b", "",
    # stray ')' and adjacent operand groups (regressions: the shunting-yard
    # family accepted these), and a digitless number
    "a)", "(a)(b)", "a(3)", ".",
]


def nearly(a, b):
    if math.isnan(b):
        return math.isnan(a)
    if a == b:  # covers ±inf
        return True
    return abs(a - b) <= 1e-9 * max(1.0, abs(a), abs(b))


def main():
    checks = failures = 0
    evs = all_evaluators()
    for ev in evs:
        for expr, expected in CASES:
            checks += 1
            try:
                got = ev.eval(expr, VARS)
                if not nearly(got, expected):
                    print(f"FAIL [{ev.name:<26}] {expr!r} = {got:g}, expected {expected:g}")
                    failures += 1
            except Exception as e:  # noqa: BLE001
                print(f"FAIL [{ev.name:<26}] {expr!r} threw: {e}")
                failures += 1
        for expr in ERRORS:
            checks += 1
            try:
                ev.eval(expr)
                print(f"FAIL [{ev.name:<26}] {expr!r} should have raised")
                failures += 1
            except Exception:  # noqa: BLE001
                pass  # expected

    print(f"\n{checks} checks across {len(evs)} evaluators, {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.setrecursionlimit(200_000)
    sys.exit(main())
