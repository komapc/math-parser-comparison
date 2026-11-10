#!/usr/bin/env python3
"""Cross-language check, Python side: evaluate every line of the given files
with the first registered strategy and print one result per line — `E`
(rejected), `nan`, or the 16-hex-digit IEEE-754 bits of the value.
bench/xlang_compare.py compares this with the other three languages'.
Within a language every strategy shares one lexer, so the in-language fuzz
cannot see a lexer bug; only this comparison can."""
import math
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from mathparser import all_evaluators  # noqa: E402
from test_parsers import VARS  # noqa: E402


def main():
    ev = all_evaluators()[0]
    out = []
    for path in sys.argv[1:]:
        with open(path) as f:
            for line in f.read().splitlines():
                try:
                    v = ev.eval(line, VARS)
                except Exception:  # noqa: BLE001 — any rejection counts the same
                    out.append("E")
                    continue
                out.append("nan" if math.isnan(v) else struct.pack(">d", v).hex())
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
