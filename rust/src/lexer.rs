//! Shared lexer used by every parsing strategy, so the comparison measures the
//! parsing approach rather than differences in lexing. Mirrors cpp/include/parser/lexer.hpp:
//! one set of rules, two ways to consume it — `Lexer::next()` streams one token
//! at a time (for strategies that read left to right with bounded lookahead),
//! `tokenize()` builds the whole array (for the divide-and-conquer family and
//! the buffered bottom-up reducer, whose algorithms index it).

use crate::Error;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum Kind {
    Num,
    Ident,
    Plus,
    Minus,
    Star,
    Slash,
    Caret,
    LParen,
    RParen,
    End,
}

/// 16 bytes, like the C++ Token: value is the literal for `Num`, the variable
/// index 0..26 for `Ident`, 0.0 otherwise.
#[derive(Clone, Copy, Debug)]
pub struct Token {
    pub value: f64,
    pub pos: u32,
    pub kind: Kind,
}

pub struct Lexer<'a> {
    src: &'a [u8],
    p: usize,
}

#[inline]
fn is_digit(c: u8) -> bool {
    c.is_ascii_digit()
}

impl<'a> Lexer<'a> {
    #[inline]
    pub fn new(src: &'a str) -> Self {
        Lexer { src: src.as_bytes(), p: 0 }
    }

    /// Next token; `End` for every call once the input is exhausted.
    #[inline]
    pub fn next(&mut self) -> Result<Token, Error> {
        let s = self.src;
        loop {
            if self.p >= s.len() {
                return Ok(Token { value: 0.0, pos: self.p as u32, kind: Kind::End });
            }
            let c = s[self.p];
            if c == b' ' || c == b'\t' || c == b'\n' || c == b'\r' {
                self.p += 1;
                continue;
            }
            let pos = self.p as u32;
            if is_digit(c) || c == b'.' {
                return self.number(pos);
            }
            if c.is_ascii_alphabetic() || c == b'_' {
                return self.ident(pos);
            }
            let kind = match c {
                b'+' => Kind::Plus,
                b'-' => Kind::Minus,
                b'*' => Kind::Star,
                b'/' => Kind::Slash,
                b'^' => Kind::Caret,
                b'(' => Kind::LParen,
                b')' => Kind::RParen,
                _ => return Err(fail_char(c, pos)),
            };
            self.p += 1;
            return Ok(Token { value: 0.0, pos, kind });
        }
    }

    // Lexeme: digits '.'? digits | '.' digits, then an optional exponent that
    // needs at least one digit (otherwise the 'e' is left for the identifier
    // rule, exactly as std::from_chars leaves it). Overflow/underflow is a
    // value (inf / 0), not a syntax error — Rust's parser gives IEEE results.
    #[cold]
    #[inline(never)]
    fn number_slow(&mut self, pos: u32) -> Result<Token, Error> {
        let s = self.src;
        let start = self.p;
        let mut q = start;
        while q < s.len() && is_digit(s[q]) {
            q += 1;
        }
        let int_digits = q - start;
        let mut frac_digits = 0;
        if q < s.len() && s[q] == b'.' {
            q += 1;
            let f0 = q;
            while q < s.len() && is_digit(s[q]) {
                q += 1;
            }
            frac_digits = q - f0;
        }
        if int_digits == 0 && frac_digits == 0 {
            return Err(Error::at("invalid number", pos));
        }
        if q < s.len() && (s[q] == b'e' || s[q] == b'E') {
            let mut e = q + 1;
            if e < s.len() && (s[e] == b'+' || s[e] == b'-') {
                e += 1;
            }
            if e < s.len() && is_digit(s[e]) {
                while e < s.len() && is_digit(s[e]) {
                    e += 1;
                }
                q = e;
            }
        }
        // SAFETY-free: the slice is ASCII digits/./e/sign, valid UTF-8.
        let text = std::str::from_utf8(&s[start..q]).unwrap();
        let value: f64 = text.parse().map_err(|_| Error::at("invalid number", pos))?;
        self.p = q;
        Ok(Token { value, pos, kind: Kind::Num })
    }

    /// Fast path for the common case — a run of digits with no '.', 'e' —
    /// which is every operand in the shared corpus; accumulates exactly for
    /// up to 15 digits (2^53 > 10^15), else falls back to the full parser.
    #[inline]
    fn number(&mut self, pos: u32) -> Result<Token, Error> {
        let s = self.src;
        let start = self.p;
        let mut q = start;
        let mut acc: u64 = 0;
        while q < s.len() && is_digit(s[q]) && q - start < 15 {
            acc = acc * 10 + (s[q] - b'0') as u64;
            q += 1;
        }
        let next = if q < s.len() { s[q] } else { b' ' };
        if q > start && !(is_digit(next) || next == b'.' || next == b'e' || next == b'E') {
            self.p = q;
            return Ok(Token { value: acc as f64, pos, kind: Kind::Num });
        }
        self.number_slow(pos)
    }

    #[inline]
    fn ident(&mut self, pos: u32) -> Result<Token, Error> {
        let s = self.src;
        let c = s[self.p];
        let mut q = self.p + 1;
        while q < s.len() && (s[q].is_ascii_alphanumeric() || s[q] == b'_') {
            q += 1;
        }
        if q - self.p != 1 || c == b'_' {
            return Err(Error::at("unknown identifier", pos));
        }
        self.p = q;
        Ok(Token { value: ((c | 0x20) - b'a') as f64, pos, kind: Kind::Ident })
    }
}

#[cold]
#[inline(never)]
fn fail_char(c: u8, pos: u32) -> Error {
    Error(format!("unexpected character '{}' at position {}", c as char, pos).into())
}

/// The whole token array. Always ends with a `Kind::End` token.
pub fn tokenize(src: &str) -> Result<Vec<Token>, Error> {
    let mut lx = Lexer::new(src);
    let mut out = Vec::with_capacity(src.len() / 2 + 2);
    loop {
        let t = lx.next()?;
        let end = t.kind == Kind::End;
        out.push(t);
        if end {
            return Ok(out);
        }
    }
}
