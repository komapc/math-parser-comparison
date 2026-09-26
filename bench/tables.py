"""Regenerate every bench-derived doc table from CI artifacts.

usage: python3 tables.py RUN1 RUN2 RUN3 [--rust R1 R2 R3] [--allow-mixed-cpu]
       python3 tables.py --cpus RUN...     (print each run's CPU model, grouped)
Each RUN is a directory holding benchmark-results/ (gh run download).

GitHub-hosted runners come on several CPU models (four in nine runs on
2026-09-26), and the ratios between strategies move with the
microarchitecture. A batch is therefore refused unless every run's env.txt
names the same CPU; --allow-mixed-cpu overrides that, for runs from before
env.txt existed.
"""
import re, statistics as st, sys

LANGS = ["cpp", "rust", "python", "haskell"]
LNAME = {"cpp": "C++", "rust": "Rust", "python": "Python", "haskell": "Haskell"}
ROW = re.compile(r"^([a-z][a-z-]+)\s+((?:[\d.]+\s+){2,3}[\d.]+)\s*(\(.*)?$")
CONTROL = "direct-scannerless"
TREE_CLASSICS = ["ast-recursive-descent", "ast-shunting-yard", "ast-pratt", "ast-arena"]
DIRECT_CLASSICS = ["direct-recursive-descent", "direct-shunting-yard"]
FAMILY = ["multipass", "multipass-arena", "direct-mp", "multipass-bfs"]
SHORT = {"ast-recursive-descent": "ast-rd", "ast-shunting-yard": "ast-sy", "ast-pratt": "ast-pratt",
         "ast-arena": "ast-arena", "direct-recursive-descent": "direct-rd",
         "direct-shunting-yard": "direct-sy", "direct-mp": "direct-mp", "multipass": "multipass",
         "multipass-arena": "multipass-arena", "multipass-bfs": "multipass-bfs"}


def cpu_of(run):
    """CPU model from the run's env.txt, or None for runs that predate it."""
    try:
        for line in open(f"{run}/benchmark-results/env.txt"):
            if line.startswith("Model name:"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return None


def check_one_cpu(runs, allow_mixed):
    cpus = {r: cpu_of(r) for r in runs}
    models = set(cpus.values())
    if len(models) == 1 and None not in models:
        print(f"CPU: {models.pop()} (all {len(runs)} runs)")
        return
    for r, c in cpus.items():
        print(f"  {r}: {c or 'unknown (no env.txt)'}")
    if not allow_mixed:
        sys.exit("error: runs are not all on one known CPU model "
                 "(re-dispatch until they are, or pass --allow-mixed-cpu)")
    print("warning: mixed or unknown CPU models, ratios may shift between runs")


def group_by_cpu(runs):
    groups = {}
    for r in runs:
        groups.setdefault(cpu_of(r) or "unknown (no env.txt)", []).append(r)
    for cpu, rs in sorted(groups.items(), key=lambda g: -len(g[1])):
        print(f"{len(rs)}  {cpu}: {' '.join(rs)}")


def parse_bench(path):
    d = {}
    for line in open(path):
        m = ROW.match(line)
        if m:
            d[m.group(1)] = [float(x) for x in m.group(2).split()]
    return d


def parse_adv(path):
    shapes, cur = {}, None
    for line in open(path):
        if line.startswith("-- "):
            cur = line[3:].split(":")[0].strip()
            shapes[cur] = {}
        elif cur:
            m = ROW.match(line)
            if m:
                shapes[cur][m.group(1)] = [float(x) for x in m.group(2).split()]
    return shapes


def load(runs):
    out = {}
    for lang in LANGS:
        rs = runs[lang]
        out[lang] = {
            "bench": [parse_bench(f"{r}/benchmark-results/bench-{lang}.txt") for r in rs],
            "adv": [parse_adv(f"{r}/benchmark-results/adversarial-{lang}.txt") for r in rs],
        }
    return out


def cell(runs, strat, i):
    """median over runs of one size column"""
    return st.median(r[strat][i] for r in runs)


def fmt(x):
    x = round(x)
    return f"{x:,}".replace(",", " ") if x >= 10000 or x >= 1000 else str(x)


def xfastest(D):
    print("### FINDINGS cross-language ×fastest (median over sizes of per-size run-medians)")
    strats = list(D["cpp"]["bench"][0])
    med = {}
    for lang in LANGS:
        runs = D[lang]["bench"]
        med[lang] = {s: st.median(cell(runs, s, i) for i in range(4)) for s in strats}
    base = {lang: min(v for s, v in med[lang].items() if s != CONTROL) for lang in LANGS}
    for s in strats:
        print(f"| {s} | " + " | ".join(f"{med[l][s]/base[l]:.1f}" for l in LANGS) + " |")
    print("raw fastest:", {l: (min((v, s) for s, v in med[l].items() if s != CONTROL)) for l in LANGS})
    print("control:", {l: round(med[l][CONTROL]) for l in LANGS})
    print("spread max/min:", {l: round(max(v for s, v in med[l].items() if s != CONTROL) / base[l], 1) for l in LANGS})
    return med


def control(D, med):
    print("### control table (median over sizes; ratio range over runs)")
    for l in LANGS:
        rr = []
        for r in D[l]["bench"]:
            a = st.median(r["direct-recursive-descent"]); b = st.median(r[CONTROL])
            rr.append(b / a)
        print(f"| {LNAME[l]} | {fmt(med[l]['direct-recursive-descent'])} | {fmt(med[l][CONTROL])} | "
              f"{med[l][CONTROL]/med[l]['direct-recursive-descent']:.2f} ({min(rr):.2f}–{max(rr):.2f}) |")


def size_table(D, lang):
    print(f"### {lang} README per-size medians")
    runs = D[lang]["bench"]
    for s in runs[0]:
        vals = [cell(runs, s, i) for i in range(4)]
        print(f"| {s} | " + " | ".join(fmt(v) for v in vals) + " |")


def at(D, lang, i):
    runs = D[lang]["bench"]
    return {s: cell(runs, s, i) for s in runs[0]}


def result1(D, i=3):
    print(f"### Result 1 table (size col {i})")
    for l in LANGS:
        m = at(D, l, i)
        best_cl = min(TREE_CLASSICS, key=m.get)
        real = {s: v for s, v in m.items() if s != CONTROL}
        fo = min(real, key=real.get)
        print(f"| {LNAME[l]} | {fmt(m['multipass-reverse-fold'])} | {fmt(m['multipass-reverse'])} | "
              f"{SHORT[best_cl]} {fmt(m[best_cl])} | {fo} {fmt(m[fo])} | control {fmt(m[CONTROL])}")


def pct_range(ratios):
    lo, hi = min(ratios), max(ratios)
    return lo, hi


def label(lo, hi):
    if lo >= 5: return "**best**"
    if lo >= 0.5: return "ahead, narrowly"
    if hi <= -0.5: return "loses"
    return "tie"


def compare(runs_vals, me, pool, rule):
    """% = (1 - me/rival)*100 per run. rule 'median': rival = best of pool on the
    run medians; rule 'perrun': rival = best of pool within each run."""
    med = {s: st.median(r[s] for r in runs_vals) for s in pool + [me]}
    if rule == "median":
        rival = min(pool, key=med.get)
        adv = [(1 - r[me] / r[rival]) * 100 for r in runs_vals]
        fac = [r[me] / r[rival] for r in runs_vals]
    else:
        riv = [min(pool, key=r.get) for r in runs_vals]
        rival = max(set(riv), key=riv.count)
        adv = [(1 - r[me] / r[x]) * 100 for r, x in zip(runs_vals, riv)]
        fac = [r[me] / r[x] for r, x in zip(runs_vals, riv)]
    return rival, min(adv), max(adv), sorted(fac)


def cellstr(rival, lo, hi):
    a, b = round(lo), round(hi)
    rng = (f"{a:+d}…{b:+d} %" if a != b else f"{a:+d} %").replace("+0…", "0…").replace("…+0 %", "…0 %")
    if rng in ("+0 %", "-0 %"): rng = "0 %"
    return f"{label(lo, hi)} ({rng} vs `{SHORT[rival]}`)"


def scoreboard(D, rule="median"):
    rows = [("random corpus (n=1000)", "bench", 2), ("random corpus (n=10000)", "bench", 3)]
    shapes = ["powchain", "towerchain", "sumchain", "nestchain"]
    for title, me, pool in [("tree", "multipass-reverse-fold", TREE_CLASSICS),
                            ("direct", "direct-reverse", DIRECT_CLASSICS),
                            ("family", "multipass-reverse-fold", FAMILY)]:
        print(f"### one-pager {title}: {me}  (rival rule: {rule})")
        for name, kind, i in rows + [(s, "adv", -1) for s in shapes]:
            cells = []
            for l in LANGS:
                if kind == "bench":
                    rv = [{s: r[s][i] for s in r} for r in D[l]["bench"]]
                else:
                    rv = [{s: r[name][s][-1] for s in r[name]} for r in D[l]["adv"]]
                rival, lo, hi, fac = compare(rv, me, pool, rule)
                if title == "tree" and l == "haskell" and hi <= -0.5:
                    c = f"loses ({fac[0]:.1f}–{fac[-1]:.1f}× slower than `{SHORT[rival]}`)"
                else:
                    c = cellstr(rival, lo, hi)
                cells.append(c)
            print(f"| {name} | " + " | ".join(cells) + " |")


def n1000_tiers(D):
    print("### n=1000 medians (rust README / multipass-reverse.md)")
    keys = ["direct-recursive-descent", "direct-shunting-yard", "direct-reverse", "ast-arena",
            "multipass-reverse-fold", "ast-recursive-descent", "ast-pratt", "ast-shunting-yard",
            "multipass-reverse", "bytecode-vm", CONTROL, "direct-mp", "multipass", "multipass-arena", "multipass-bfs"]
    for l in LANGS:
        m = at(D, l, 2)
        print(LNAME[l], {SHORT.get(k, k): fmt(m[k]) for k in keys})


def adv_cpp(D):
    print("### C++ adversarial largest-m medians (multipass-reverse.md 237 / README rescue 'after')")
    runs = D["cpp"]["adv"]
    for sh in runs[0]:
        m = {s: st.median(r[sh][s][-1] for r in runs) for s in runs[0][sh]}
        print(sh, {SHORT.get(k, k): round(v) for k, v in m.items()})
    for l in ["python", "haskell", "rust"]:
        runs = D[l]["adv"]
        sh = "powchain"
        m = {s: st.median(r[sh][s][-1] for r in runs) for s in runs[0][sh]}
        print(l, sh, {SHORT.get(k, k): round(v) for k, v in m.items()})


def readme_tree(D, i=2):
    print("### README tree-builder table (n=1000, ×fastest tree builder)")
    trees = TREE_CLASSICS + ["multipass", "multipass-arena", "multipass-bfs", "multipass-reverse", "multipass-reverse-fold"]
    ms = {l: at(D, l, i) for l in LANGS}
    base = {l: min(ms[l][s] for s in trees) for l in LANGS}
    for s in trees:
        print(f"| {s} | " + " | ".join(f"{ms[l][s]/base[l]:.2f}" for l in LANGS) + " |")


def sliver(D):
    print("### per-run % : fold vs ast-arena / best classic tree, direct-reverse vs direct-rd/sy (all sizes)")
    for l in LANGS:
        for me, rivals in [("multipass-reverse-fold", TREE_CLASSICS), ("direct-reverse", DIRECT_CLASSICS)]:
            for rv in rivals:
                vals = [(r[rv][i] / r[me][i] - 1) * 100 for r in D[l]["bench"] for i in range(4)]
                vals1000 = [(r[rv][2] / r[me][2] - 1) * 100 for r in D[l]["bench"]]
                print(f"  {l:8} {me[:14]:14} vs {SHORT[rv]:10} all12 {min(vals):+.1f}…{max(vals):+.1f}  n=1000 {min(vals1000):+.1f}…{max(vals1000):+.1f}")


if __name__ == "__main__":
    args = sys.argv[1:]
    if args and args[0] == "--cpus":
        group_by_cpu(args[1:])
        sys.exit(0)
    allow_mixed = "--allow-mixed-cpu" in args
    args = [a for a in args if a != "--allow-mixed-cpu"]
    rust = None
    if "--rust" in args:
        k = args.index("--rust"); rust = args[k + 1:k + 4]; args = args[:k]
    runs = {l: args for l in LANGS}
    if rust: runs["rust"] = rust
    check_one_cpu(sorted(set(args + (rust or []))), allow_mixed)
    D = load(runs)
    med = xfastest(D); control(D, med)
    for l in ["python", "haskell"]: size_table(D, l)
    result1(D); readme_tree(D); n1000_tiers(D); adv_cpp(D); sliver(D); scoreboard(D, "median"); scoreboard(D, "perrun")
