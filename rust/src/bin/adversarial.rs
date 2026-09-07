//! Adversarial (structured, non-random) inputs — the same four shapes and
//! sizes as cpp/bench/adversarial_bench.cpp, which documents why each shape
//! exists. ns/leaf; flat across a row = linear, ~4x per column = quadratic.
//!
//! Usage: cargo run --release --bin adversarial

use mathparser::{all_evaluators, with_big_stack, Evaluator};
use std::time::Instant;

/// m factors b^e; ops alternate * and /; adjacent factor pairs are identical,
/// so the value stays exactly 9.0 and the cross-check is an equality test.
fn pow_chain(m: usize) -> String {
    let mut s = String::from("3 ^ 2");
    for i in 1..m {
        let k = (i - 1) / 2;
        s.push_str(if i % 2 == 1 { " * " } else { " / " });
        s.push_str(&format!("{} ^ {}", 2 + k % 8, 2 + k % 3));
    }
    s
}

/// m/2 ^ operators in one run, then m-m/2 * factors, all operands 1. m+1 leaves.
fn tower_chain(m: usize) -> String {
    let mut s = String::from("1");
    for _ in 0..m / 2 { s.push_str(" ^ 1"); }
    for _ in m / 2..m { s.push_str(" * 1"); }
    s
}

/// m terms; ops alternate + and -; value stays small.
fn sum_chain(m: usize) -> String {
    let mut s = String::from("1");
    for i in 1..m {
        s.push_str(if i % 2 == 1 { " + " } else { " - " });
        s.push_str(&(1 + i % 9).to_string());
    }
    s
}

/// m nested paren groups: (((...(1 + 1) + 1)...) + 1) — value m+1, m+1 leaves.
fn nest_chain(m: usize) -> String {
    let mut s = "(".repeat(m);
    s.push('1');
    for _ in 0..m { s.push_str(" + 1)"); }
    s
}

fn best_ns(ev: &mut dyn Evaluator, expr: &str, reps: usize) -> f64 {
    let mut best = f64::INFINITY;
    for _ in 0..reps {
        let t0 = Instant::now();
        let v = ev.eval(expr, None).unwrap_or(f64::NAN);
        best = best.min(t0.elapsed().as_nanos() as f64);
        std::hint::black_box(v);
    }
    best
}

fn run_shape(title: &str, ms: &[usize], gen: fn(usize) -> String, leaves: fn(usize) -> usize) {
    println!("-- {} --", title);
    print!("{:<26}", "strategy");
    for &m in ms { print!("{:>12}", format!("m={}", m)); }
    println!("   (ns/leaf; flat = linear, ~4x/col = quadratic)");

    let exprs: Vec<String> = ms.iter().map(|&m| gen(m)).collect();
    let mut evs = all_evaluators();
    // cross-check on the largest input
    let last = exprs.last().unwrap();
    let reference = evs[0].eval(last, None).unwrap_or(f64::NAN);
    for ev in evs.iter_mut() {
        let got = ev.eval(last, None).unwrap_or(f64::NAN);
        if got != reference { println!("MISMATCH [{}]: {} != {}", ev.name(), got, reference); }
    }
    for ev in evs.iter_mut() {
        print!("{:<26}", ev.name());
        for (i, &m) in ms.iter().enumerate() {
            let ns = best_ns(ev.as_mut(), &exprs[i], 5);
            print!("{:>12.1}", ns / leaves(m) as f64);
        }
        println!();
    }
    println!();
}

fn main() {
    with_big_stack(|| {
        println!("== Rust: adversarial chains (structured inputs) ==\n");
        let ms = [512, 2048, 8192];
        run_shape("powchain: b^e * b^e / ... (mixed precedence)", &ms, pow_chain, |m| 2 * m);
        run_shape("towerchain: 1^1^...^1 * 1 * ... (flat-check attack)", &ms, tower_chain, |m| m + 1);
        run_shape("sumchain: 1 + 2 - 3 + ... (single precedence — control)", &ms, sum_chain, |m| m);
        run_shape("nestchain: (((...(1 + 1)...) + 1) (deep nesting — bottom-up's turn)", &ms, nest_chain, |m| m + 1);
    });
}
