"""obi_research.py'yi BAGIMSIZ numpy/scipy oracle'ina karsi dogrular. Gereken: numpy, scipy (YALNIZ bu test araci; obi_research.py saf stdlib).
Kullanim:  python3 test_obi_research.py"""
import subprocess, sys, csv, math, os, glob, tempfile
import numpy as np
from scipy import stats
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
from gen_obi_csv import gen, HZ
SCRIPT = os.path.join(HERE, 'obi_research.py')
fails = 0
def CHECK(c, msg):
    global fails
    if not c: print("  FAIL:", msg); fails += 1

def oracle(files, fee, mult, slip, gap_ms, spread_cost=True, thr_filter=None, max_lag=None):
    rows = []
    for f in files:
        with open(f, newline='') as fh:
            for r in csv.DictReader(fh): rows.append(r)
    rows = [r for r in rows if int(r['n_filled']) == 7 and (max_lag is None or int(r['max_lag_us']) <= max_lag)
            and (thr_filter is None or round(float(r['threshold']), 3) in thr_filter)]
    rows.sort(key=lambda r: int(r['signal_wall_us']))
    if gap_ms > 0:
        last, kept = {}, []
        for r in rows:
            k = (round(float(r['threshold']), 3), int(r['side'])); w = int(r['signal_wall_us'])
            if k in last and w - last[k] < gap_ms * 1000: continue
            last[k] = w; kept.append(r)
        rows = kept
    out = {}
    sf = (1 + mult) / 2 if spread_cost else 0.0
    groups = {}
    for r in rows: groups.setdefault((round(float(r['threshold']), 3), int(r['side'])), []).append(r)
    cells = {}
    for (thr, side), rs in groups.items():
        p0 = np.array([float(r['signal_price']) for r in rs]); sp = np.array([float(r['spread_bps']) for r in rs])
        for i, h in enumerate(HZ):
            ph = np.array([float(r[f'price_after_{h}ms']) for r in rs]); ok = ~np.isnan(ph)
            g = side * (ph[ok] - p0[ok]) / p0[ok] * 1e4; cost = 2 * fee + slip + sf * sp[ok]; net = g - cost; n = len(g)
            if n == 0: continue
            wins, loss = g[g > 0], -g[g < 0]
            t = net.mean() / (net.std(ddof=1) / math.sqrt(n)) if n >= 2 and net.std(ddof=1) > 0 else float('nan')
            ms = sp[ok].mean(); base = g.mean() - 2 * fee - slip
            ks = (2 * base / ms - 1) if (ms > 0 and base > 0) else (float('inf') if base > 0 else float('nan'))
            cells[(thr, side, h)] = dict(n=n, win=(g > 0).mean(), loss=(g < 0).mean(), flat=(g == 0).mean(), avg_win=wins.mean() if len(wins) else 0.0,
                avg_loss=loss.mean() if len(loss) else 0.0, ev_gross=g.mean(), ev_net=net.mean(), t=t, p_t=2 * stats.t.sf(abs(t), n - 1) if t == t else float('nan'),
                p_z=math.erfc(abs(t) / math.sqrt(2)) if t == t else float('nan'), win_net=(net > 0).mean(), mean_spread_bps=ms, k_star=ks)
    nt = sum(1 for c in cells.values() if c['n'] >= 2 and c['p_t'] == c['p_t'])
    for c in cells.values(): c['p_bonf'] = min(1.0, c['p_t'] * nt) if c['p_t'] == c['p_t'] else float('nan')
    return cells

def run(args, files_or_dir):
    out = tempfile.mktemp(suffix='.csv')
    r = subprocess.run([sys.executable, SCRIPT] + list(files_or_dir) + ['--csv-out', out, '--no-color'] + args, capture_output=True, text=True)
    cells = {}
    if os.path.exists(out):
        for row in csv.DictReader(open(out)):
            cells[(float(row['threshold']), int(row['side']), int(row['horizon_ms']))] = {k: float(v) for k, v in row.items() if k not in ('threshold', 'side', 'horizon_ms')}
        os.remove(out)
    return r, cells

def compare(cells, ref, label, tol=1e-8):
    CHECK(set(cells) == set(ref), f"{label}: hucre kumesi farkli ({len(cells)} vs {len(ref)})")
    worst = 0.0; worst_k = None
    for k in ref:
        if k not in cells: continue
        for f in ref[k]:
            a, b = cells[k][f], ref[k][f]
            if (a != a and b != b) or (a == b): continue            # NaN==NaN, inf==inf
            rel = abs(a - b) / max(1e-12, abs(b)); 
            if rel > worst: worst, worst_k = rel, (k, f, a, b)
    CHECK(worst < tol, f"{label}: max goreceli sapma {worst:.2e} @ {worst_k}")
    print(f"  {label:<62} {len(ref)} hucre | max goreceli sapma = {worst:.1e}")

d = tempfile.mkdtemp()
gen(f'{d}/obi_telemetry_a.csv', 60000, 1_790_000_000_000_000, 10**9, seed=21, gap_ms=40, edge_bps={500: 0.4, 1000: 0.9, 5000: 1.5}, sigma_bps=3.0, spread_bps=0.6, incomplete_frac=0.03)
gen(f'{d}/obi_telemetry_b.csv', 40000, 1_790_000_000_000_000 + 20_000_000_000, 5*10**8, seed=22, gap_ms=60, edge_bps={500: 0.4, 1000: 0.9}, sigma_bps=3.0, spread_bps=0.9)   # ayrik mono alani, wall'da ILERI ve ust uste biner
files = sorted(glob.glob(f'{d}/*.csv'))
print("== oracle karsilastirmasi (2 dosya, ust uste binen wall araligi, %3 eksik-horizon satiri)")
for label, args, kw in [
    ("fee=0.8 exit_mult=2.0 slip=0.25, min-gap yok", ['--fee-bps', '0.8', '--exit-spread-mult', '2.0', '--slippage-bps', '0.25'], dict(fee=0.8, mult=2.0, slip=0.25, gap_ms=0)),
    ("varsayilan maliyet (exit_mult=1.5), min-gap 300ms (wall)", ['--min-gap-ms', '300'], dict(fee=0, mult=1.5, slip=0, gap_ms=300)),
    ("--no-spread-cost, fee=1.0, --thresholds 0.3,0.5, --max-lag-us 300", ['--no-spread-cost', '--fee-bps', '1.0', '--thresholds', '0.3,0.5', '--max-lag-us', '300'], dict(fee=1.0, mult=1.5, slip=0, gap_ms=0, spread_cost=False, thr_filter={0.3, 0.5}, max_lag=300)),
]:
    r, cells = run(args, files); CHECK(r.returncode == 0, f"{label}: exit {r.returncode}\n{r.stderr[-300:]}")
    compare(cells, oracle(files, **kw), label)
print("\n== DOSYA SIRASI degismez (wall birlestirme):")
r1, c1 = run(['--min-gap-ms', '300'], files); r2, c2 = run(['--min-gap-ms', '300'], files[::-1]); compare(c1, c2, "[a,b] vs [b,a] sirasi")
print(f"\n{'TUM TESTLER GECTI' if not fails else 'BASARISIZ: %d' % fails}"); sys.exit(1 if fails else 0)
