//! Cross-language check, Rust side: evaluate every line of the given files
//! with the first registered strategy and print one result per line —
//! `E` (rejected), `nan`, or the 16-hex-digit IEEE-754 bits of the value.
//! bench/xlang_compare.py compares this with the other three languages'.
//! Within a language every strategy shares one lexer, so the in-language
//! fuzz cannot see a lexer bug; only this comparison can.
use std::io::{BufWriter, Write};

fn main() {
    let mut vars = [0.0f64; 26];
    vars[..4].copy_from_slice(&[2.0, 3.0, 4.0, 5.0]); // a b c d, as in every spec suite
    let mut evs = mathparser::all_evaluators();
    let ev = &mut evs[0];
    let out = std::io::stdout();
    let mut out = BufWriter::new(out.lock());
    for path in std::env::args().skip(1) {
        let text = std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("cannot open {path}: {e}"));
        for line in text.lines() {
            match ev.eval(line, Some(&vars)) {
                Err(_) => writeln!(out, "E"),
                Ok(v) if v.is_nan() => writeln!(out, "nan"),
                Ok(v) => writeln!(out, "{:016x}", v.to_bits()),
            }
            .unwrap();
        }
    }
}
