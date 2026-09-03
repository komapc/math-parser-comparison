//! Shared spec suite — the same 22 value cases and 10 error cases as
//! python/test_parsers.py, cpp/tests/test_parsers.cpp and the Haskell suite,
//! run against every strategy: 32 × 15 = 480 checks.

use mathparser::all_evaluators;

const INF: f64 = f64::INFINITY;
const CASES: &[(&str, f64)] = &[
    ("a + b * c", 14.0),
    ("(a + b) * c", 20.0),
    ("a * (b + c) - d", 9.0),
    ("(a + b) * b / d", 3.0),
    ("-(b + c) * a", -14.0),
    ("d - a - b", 0.0),
    ("d * c / a", 10.0),
    ("a ^ a ^ a", 16.0),
    ("-a ^ a", -4.0),
    ("a ^ -b", 0.125),
    ("3.5 * a + 1.5", 8.5),
    ("2e2 + a", 202.0),
    ("--a", 2.0),
    ("-a * b", -6.0),
    ("+d - +a", 3.0),
    ("0 ^ -1", INF),
    ("1 / (0 * -1)", -INF),
    ("(0 - 1e155) ^ 3", -INF),
    ("(0 / 0) / 0", f64::NAN),
    ("(0 - a) ^ 0.5", f64::NAN),
    ("1e400", INF),
    ("1e-400 + a", 2.0),
];
const ERRORS: &[&str] = &["a +", "(a + b", "a b", "* a", "a + * b", "", "a)", "(a)(b)", "a(3)", "."];

fn nearly(a: f64, b: f64) -> bool {
    if a.is_nan() || b.is_nan() { return a.is_nan() && b.is_nan(); }
    if a == b { return true; }
    (a - b).abs() <= 1e-9 * a.abs().max(b.abs()).max(1.0)
}

#[test]
fn spec() {
    let mut vars = [0.0f64; 26];
    vars[0] = 2.0; vars[1] = 3.0; vars[2] = 4.0; vars[3] = 5.0;
    let mut evs = all_evaluators();
    let mut checks = 0;
    let mut failures = 0;
    for ev in evs.iter_mut() {
        for &(src, want) in CASES {
            checks += 1;
            match ev.eval(src, Some(&vars)) {
                Ok(got) if nearly(got, want) => {}
                Ok(got) => { failures += 1; println!("FAIL {:<24} {:<20} got {} want {}", ev.name(), src, got, want); }
                Err(e) => { failures += 1; println!("FAIL {:<24} {:<20} error: {}", ev.name(), src, e); }
            }
        }
        for &src in ERRORS {
            checks += 1;
            if let Ok(v) = ev.eval(src, Some(&vars)) {
                failures += 1;
                println!("FAIL {:<24} {:<20} expected error, got {}", ev.name(), src, v);
            }
        }
    }
    println!("{} checks across {} evaluators, {} failure(s)", checks, evs.len(), failures);
    assert_eq!(checks, 480);
    assert_eq!(failures, 0);
}
