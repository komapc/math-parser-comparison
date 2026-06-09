"""Shared tokenizer — one grammar for every strategy (mirrors cpp/src/lexer.cpp).

Token kinds are small ints for cheap comparison. A variable's value is its
index a-z -> 0..25; a number's value is the parsed float.
"""
import re
from typing import NamedTuple

# Token kinds.
NUM, IDENT, PLUS, MINUS, STAR, SLASH, CARET, LPAREN, RPAREN, END = range(10)

_NUMBER_RE = re.compile(r"(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?")
_IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_SINGLE = {
    "+": PLUS, "-": MINUS, "*": STAR, "/": SLASH,
    "^": CARET, "(": LPAREN, ")": RPAREN,
}


class Token(NamedTuple):
    kind: int
    value: float  # NUM: literal; IDENT: variable index 0-25; else 0.0
    pos: int


def tokenize(src: str) -> list[Token]:
    out: list[Token] = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c in " \t\n\r":
            i += 1
            continue
        if c.isdigit() or c == ".":
            m = _NUMBER_RE.match(src, i)
            if not m:
                raise ValueError(f"invalid number at position {i}")
            out.append(Token(NUM, float(m.group()), i))
            i = m.end()
            continue
        if c.isalpha() or c == "_":
            m = _IDENT_RE.match(src, i)
            assert m  # c already matches the first char class
            word = m.group()
            if len(word) != 1 or c == "_":
                raise ValueError(f"unknown identifier at position {i}")
            out.append(Token(IDENT, float(ord(c.lower()) - ord("a")), i))
            i = m.end()
            continue
        kind = _SINGLE.get(c)
        if kind is None:
            raise ValueError(f"unexpected character {c!r} at position {i}")
        out.append(Token(kind, 0.0, i))
        i += 1
    out.append(Token(END, 0.0, n))
    return out
