//! direct-scannerless: direct-recursive-descent with the lexer fused into the
//! grammar. The CONTROL row — it measures what the shared lexer costs, it is
//! not a contender (it breaks the "shared lexer" rule on purpose).

use crate::builder::{lookup, Vars};
use crate::Error;

struct S<'a> {
    s: &'a [u8],
    p: usize,
    vars: Vars<'a>,
}

impl<'a> S<'a> {
    #[inline]
    fn skip(&mut self) {
        while self.p < self.s.len() && matches!(self.s[self.p], b' ' | b'\t' | b'\n' | b'\r') { self.p += 1; }
    }
    #[inline]
    fn peek(&self) -> u8 { if self.p < self.s.len() { self.s[self.p] } else { 0 } }
    /// Consume `c` and the whitespace after it.
    #[inline]
    fn take(&mut self, c: u8) -> bool {
        if self.peek() == c { self.p += 1; self.skip(); true } else { false }
    }

    fn expr(&mut self) -> Result<f64, Error> {
        let mut v = self.term()?;
        loop {
            if self.take(b'+') { v += self.term()?; }
            else if self.take(b'-') { v -= self.term()?; }
            else { return Ok(v); }
        }
    }
    fn term(&mut self) -> Result<f64, Error> {
        let mut v = self.unary()?;
        loop {
            if self.take(b'*') { v *= self.unary()?; }
            else if self.take(b'/') { v /= self.unary()?; }
            else { return Ok(v); }
        }
    }
    fn unary(&mut self) -> Result<f64, Error> {
        if self.take(b'-') { return Ok(-self.unary()?); }
        if self.take(b'+') { return self.unary(); }
        self.power()
    }
    fn power(&mut self) -> Result<f64, Error> {
        let base = self.primary()?;
        if self.take(b'^') { let e = self.unary()?; return Ok(base.powf(e)); }
        Ok(base)
    }
    fn primary(&mut self) -> Result<f64, Error> {
        let c = self.peek();
        if c.is_ascii_digit() || c == b'.' { return self.number(); }
        if c.is_ascii_alphabetic() {
            let q = self.p + 1;
            if q < self.s.len() && (self.s[q].is_ascii_alphanumeric() || self.s[q] == b'_') {
                return Err(Error::at("unknown identifier", self.p as u32));
            }
            self.p = q; self.skip();
            return Ok(lookup(self.vars, ((c | 0x20) - b'a') as usize));
        }
        if c == b'_' { return Err(Error::at("unknown identifier", self.p as u32)); }
        if self.take(b'(') {
            let v = self.expr()?;
            if !self.take(b')') { return Err(Error::at("expected ')'", self.p as u32)); }
            return Ok(v);
        }
        Err(Error::at("expected number or '('", self.p as u32))
    }
    fn number(&mut self) -> Result<f64, Error> {
        let s = self.s;
        let start = self.p;
        let mut q = start;
        let mut acc: u64 = 0;
        while q < s.len() && s[q].is_ascii_digit() && q - start < 15 { acc = acc * 10 + (s[q] - b'0') as u64; q += 1; }
        let next = if q < s.len() { s[q] } else { b' ' };
        if q > start && !(next.is_ascii_digit() || matches!(next, b'.' | b'e' | b'E')) {
            self.p = q; self.skip();
            return Ok(acc as f64);
        }
        // full form: digits '.'? digits | '.' digits, optional exponent
        q = start;
        while q < s.len() && s[q].is_ascii_digit() { q += 1; }
        let int_digits = q - start;
        let mut frac_digits = 0;
        if q < s.len() && s[q] == b'.' {
            q += 1;
            let f0 = q;
            while q < s.len() && s[q].is_ascii_digit() { q += 1; }
            frac_digits = q - f0;
        }
        if int_digits == 0 && frac_digits == 0 { return Err(Error::at("invalid number", start as u32)); }
        if q < s.len() && (s[q] == b'e' || s[q] == b'E') {
            let mut e = q + 1;
            if e < s.len() && (s[e] == b'+' || s[e] == b'-') { e += 1; }
            if e < s.len() && s[e].is_ascii_digit() {
                while e < s.len() && s[e].is_ascii_digit() { e += 1; }
                q = e;
            }
        }
        let v: f64 = std::str::from_utf8(&s[start..q]).unwrap().parse().map_err(|_| Error::at("invalid number", start as u32))?;
        self.p = q; self.skip();
        Ok(v)
    }
}

pub fn scannerless_eval(src: &str, vars: Vars) -> Result<f64, Error> {
    let mut st = S { s: src.as_bytes(), p: 0, vars };
    st.skip();
    let v = st.expr()?;
    if st.p != st.s.len() {
        let c = st.s[st.p];
        if matches!(c, b'0'..=b'9' | b'.' | b'a'..=b'z' | b'A'..=b'Z' | b'_' | b'(' | b')') {
            return Err(Error::at("unexpected token", st.p as u32));
        }
        return Err(Error(format!("unexpected character '{}' at position {}", c as char, st.p).into()));
    }
    Ok(v)
}
