//! Differential fuzz, mirroring python/test_fuzz.py: 3000 well-formed random
//! expressions (every strategy must agree with the first) and 3000 mutated
//! ones (every strategy must agree on value-or-error). Deterministic
//! generator, seed 20260702.

use mathparser::all_evaluators;
use std::path::{Path, PathBuf};
use std::process::Command;

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        // xorshift64*
        let mut x = self.0;
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        self.0 = x;
        x.wrapping_mul(0x2545F4914F6CDD1D)
    }
    fn below(&mut self, n: usize) -> usize { (self.next() % n as u64) as usize }
    fn chance(&mut self, p: f64) -> bool { ((self.next() >> 11) as f64 / ((1u64 << 53) as f64)) < p }
}

fn leaf(r: &mut Rng, out: &mut String) {
    match r.below(10) {
        0 => out.push((b'a' + r.below(6) as u8) as char),
        1 => out.push_str(&format!("{}.{}", r.below(100), r.below(100))),
        2 => out.push_str(&format!("{}e{}", r.below(10), r.below(4) as i64 - 2)),
        3 => out.push_str(&format!(".{}", r.below(100))),
        _ => out.push_str(&format!("{}", r.below(1000))),
    }
}

fn gen(r: &mut Rng, depth: u32, out: &mut String) {
    if depth == 0 || r.chance(0.3) {
        if r.chance(0.15) { out.push(if r.chance(0.5) { '-' } else { '+' }); }
        leaf(r, out);
        return;
    }
    match r.below(10) {
        0 => { out.push('('); gen(r, depth - 1, out); out.push(')'); }
        1 => { out.push('-'); gen(r, depth - 1, out); }
        _ => {
            gen(r, depth - 1, out);
            let ops = ["+", "-", "*", "/", "^"];
            let sp = if r.chance(0.5) { " " } else { "" };
            out.push_str(sp);
            out.push_str(ops[r.below(5)]);
            out.push_str(sp);
            gen(r, depth - 1, out);
        }
    }
}

fn mutate(r: &mut Rng, s: &str) -> String {
    let mut b: Vec<char> = s.chars().collect();
    let pool = ['+', '-', '*', '/', '^', '(', ')', 'a', '1', '.', ' ', 'e'];
    for _ in 0..1 + r.below(3) {
        if b.is_empty() { b.push(pool[r.below(pool.len())]); continue; }
        let i = r.below(b.len());
        match r.below(3) {
            0 => { b.remove(i); }
            1 => b.insert(i, pool[r.below(pool.len())]),
            _ => b[i] = pool[r.below(pool.len())],
        }
    }
    b.into_iter().collect()
}

// `cargo test` runs with cwd = the crate root (rust/), matching
// src/bin/bench.rs's "../bench/corpus" default — but probe a couple of
// candidates anyway rather than hard-assuming one relative depth.
fn find_bench_dir() -> Option<PathBuf> {
    for cand in ["../bench", "../../bench", "bench"] {
        let p = Path::new(cand);
        if p.join("gen_fuzz.py").exists() {
            return Some(p.to_path_buf());
        }
    }
    None
}

// Loads bench/fuzz/{well_formed,mutated}.txt — the corpus every other
// language's fuzz test also reads (bench/gen_fuzz.py), on top of (not
// instead of) this file's own generator above. Generates the files via
// `python3 bench/gen_fuzz.py` if missing; if that also fails, prints a
// note and returns None rather than failing the whole suite over missing
// tooling.
fn load_shared_fuzz() -> Option<(Vec<String>, Vec<String>)> {
    let bench = match find_bench_dir() {
        Some(b) => b,
        None => {
            println!("note: bench/ not found from cwd={:?}; skipping shared-corpus fuzz check",
                std::env::current_dir().ok());
            return None;
        }
    };
    let wf = bench.join("fuzz").join("well_formed.txt");
    let mu = bench.join("fuzz").join("mutated.txt");
    if !wf.exists() || !mu.exists() {
        let script = bench.join("gen_fuzz.py");
        println!("shared fuzz corpus missing; generating via `python3 {}`", script.display());
        let ok = Command::new("python3").arg(&script).status().map(|s| s.success()).unwrap_or(false);
        if !ok || !wf.exists() || !mu.exists() {
            println!("note: could not generate shared fuzz corpus (python3 unavailable?); \
                      skipping shared-corpus fuzz check");
            return None;
        }
    }
    // .lines() splits on the sole trailing '\n' without an extra blank
    // entry, but keeps blank lines *between* entries — some mutated lines
    // are legitimately the empty string, a valid malformed-input case.
    let read = |p: &Path| -> Vec<String> {
        std::fs::read_to_string(p).unwrap_or_default().lines().map(str::to_owned).collect()
    };
    Some((read(&wf), read(&mu)))
}

fn same(a: f64, b: f64) -> bool {
    if a.is_nan() || b.is_nan() { return a.is_nan() && b.is_nan(); }
    if a == b { return true; }
    (a - b).abs() <= 1e-6 * a.abs().max(b.abs()).max(1.0)
}

#[test]
fn differential() {
    let mut vars = [0.0f64; 26];
    for (i, v) in vars.iter_mut().enumerate() { *v = (i as f64 + 1.0) * 0.5; }
    let mut evs = all_evaluators();
    let mut r = Rng(20260702);
    let mut mismatches = 0;
    let report = |src: &str, what: &str, mismatches: &mut i32| {
        *mismatches += 1;
        if *mismatches <= 10 { println!("MISMATCH ({}) {:?}", what, src); }
    };
    let mut well_formed = Vec::new();
    for _ in 0..3000 {
        let mut s = String::new();
        let d = 1 + r.below(7) as u32; gen(&mut r, d, &mut s);
        well_formed.push(s);
    }
    for src in &well_formed {
        let reference = evs[0].eval(src, Some(&vars));
        let r0 = match &reference { Ok(v) => *v, Err(e) => { report(src, &format!("reference errors: {}", e), &mut mismatches); continue; } };
        for ev in evs.iter_mut().skip(1) {
            match ev.eval(src, Some(&vars)) {
                Ok(v) if same(v, r0) => {}
                Ok(v) => report(src, &format!("{} = {} vs {}", ev.name(), v, r0), &mut mismatches),
                Err(e) => report(src, &format!("{} errors: {}", ev.name(), e), &mut mismatches),
            }
        }
    }
    for i in 0..3000 {
        let src = mutate(&mut r, &well_formed[i]);
        let reference = evs[0].eval(&src, Some(&vars));
        for ev in evs.iter_mut().skip(1) {
            let got = ev.eval(&src, Some(&vars));
            let agree = match (&reference, &got) {
                (Ok(a), Ok(b)) => same(*a, *b),
                (Err(_), Err(_)) => true,
                _ => false,
            };
            if !agree {
                report(&src, &format!("{}: {:?} vs {:?}", ev.name(), got.as_ref().map_err(|e| e.0.clone()), reference.as_ref().map_err(|e| e.0.clone())), &mut mismatches);
            }
        }
    }
    println!("3000 well-formed + 3000 mutated exprs x {} strategies, {} mismatch(es)", evs.len(), mismatches);

    if let Some((shared_wf, shared_mu)) = load_shared_fuzz() {
        for src in &shared_wf {
            let reference = evs[0].eval(src, Some(&vars));
            let r0 = match &reference {
                Ok(v) => *v,
                Err(e) => { report(src, &format!("reference errors: {}", e), &mut mismatches); continue; }
            };
            for ev in evs.iter_mut().skip(1) {
                match ev.eval(src, Some(&vars)) {
                    Ok(v) if same(v, r0) => {}
                    Ok(v) => report(src, &format!("{} = {} vs {}", ev.name(), v, r0), &mut mismatches),
                    Err(e) => report(src, &format!("{} errors: {}", ev.name(), e), &mut mismatches),
                }
            }
        }
        for src in &shared_mu {
            let reference = evs[0].eval(src, Some(&vars));
            for ev in evs.iter_mut().skip(1) {
                let got = ev.eval(src, Some(&vars));
                let agree = match (&reference, &got) {
                    (Ok(a), Ok(b)) => same(*a, *b),
                    (Err(_), Err(_)) => true,
                    _ => false,
                };
                if !agree {
                    report(src, &format!("{}: {:?} vs {:?}", ev.name(),
                        got.as_ref().map_err(|e| e.0.clone()),
                        reference.as_ref().map_err(|e| e.0.clone())), &mut mismatches);
                }
            }
        }
        println!("{} well-formed + {} mutated exprs (shared corpus) x {} strategies, {} mismatch(es) total",
            shared_wf.len(), shared_mu.len(), evs.len(), mismatches);
    }

    assert_eq!(mismatches, 0);
}
