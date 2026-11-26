# H2 on kayit araci. Kullanim: python -I h2.py <data_dir> train|test
import json, sys, os, math, hashlib, datetime as dt, statistics as st
D, MODE = sys.argv[1], sys.argv[2]
SYMS = ["SOLUSDT", "DOGEUSDT"]; LB = 4; H = 4; Q = 0.90
COST = 12.0; INFO_COSTS = [7.0, 20.0]; TEST_HOURS = 365 * 24
def betacf(a, b, x):
    qab, qap, qam = a + b, a + 1, a - 1; c, d = 1.0, 1 - qab * x / qap
    d = 1 / d if abs(d) > 1e-300 else 1e300; h = d
    for m in range(1, 300):
        m2 = 2 * m; aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1 + aa * d; c = 1 + aa / c; d = 1 / d; h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1 + aa * d; c = 1 + aa / c; d = 1 / d; de = d * c; h *= de
        if abs(de - 1) < 1e-12: break
    return h
def ibeta(a, b, x):
    if x <= 0: return 0.0
    if x >= 1: return 1.0
    bt = math.exp(math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b) + a * math.log(x) + b * math.log(1 - x))
    return bt * betacf(a, b, x) / a if x < (a + 1) / (a + b + 2) else 1 - bt * betacf(b, a, 1 - x) / b
def p_one_sided(xs):  # H0: ort <= 0, Student t
    n = len(xs)
    if n < 2: return float("nan"), float("nan")
    m, s = st.mean(xs), st.stdev(xs); t = m / (s / math.sqrt(n)); df = n - 1
    p_two = ibeta(df / 2, 0.5, df / (df + t * t))
    return t, (p_two / 2 if t > 0 else 1 - p_two / 2)
ts = lambda ms: dt.datetime.fromtimestamp(ms / 1000, dt.timezone.utc).strftime("%Y-%m-%d %H:%M")
def run(sym):
    fn = os.path.join(D, f"k1h_spot_{sym}.json"); raw = open(fn, "rb").read()
    k = json.loads(raw); n = len(k); cut = n - TEST_HOURS
    o = [x[0] for x in k]; c = [float(x[4]) for x in k]; hi = [float(x[2]) for x in k]; lo = [float(x[3]) for x in k]
    gaps = sum(1 for a, b in zip(o, o[1:]) if b - a != 3600000)
    sig_tr = [c[i] / c[i - LB] - 1 for i in range(LB, cut - H, H)]
    thr = sorted(abs(x) for x in sig_tr)[int(Q * len(sig_tr))]
    rng = range(LB, cut - H, H) if MODE == "train" else range(((cut + H - 1) // H) * H, n - 1 - H, H)
    tr = []
    for i in rng:
        s = c[i] / c[i - LB] - 1
        if abs(s) < thr: continue
        side = -1 if s > 0 else 1  # REV
        brut = side * (c[i + H] / c[i] - 1) * 1e4
        filled = (lo[i + 1] < c[i]) if side == 1 else (hi[i + 1] > c[i])
        tr.append(dict(t=o[i], sym=sym, side=side, brut=brut, filled=filled))
    meta = dict(sym=sym, sha=hashlib.sha256(raw).hexdigest(), n=n, gaps=gaps, first=ts(o[0]), cut=ts(o[cut]),
                last_used=ts(o[n - 2]), thr_bps=thr * 1e4)
    return meta, tr
def report(name, trades, cost):
    if not trades: print(f"  {name}: N=0"); return None
    b = [x["brut"] for x in trades]; net = [v - cost for v in b]
    slots = {}
    for x in trades: slots.setdefault(x["t"], []).append(x["brut"] - cost)
    sl = [st.mean(v) for v in slots.values()]
    t, p = p_one_sided(sl)
    print(f"  {name}: N={len(b)} slot={len(sl)} brut={st.mean(b):+.2f} net={st.mean(net):+.2f} kazanma%={sum(v>0 for v in net)/len(net)*100:.1f} t={t:+.2f} p={p:.4f}")
    return dict(N=len(b), net=st.mean(net), p=p, slots=sl)
allt = []
for s in SYMS:
    m, tr = run(s); allt += tr
    print("META", json.dumps(m))
print(f"\n=== MOD: {MODE.upper()} | L={LB}s H={H}s REV, esik=egitim |sig| {Q:.0%} kantili ===")
fill = [x for x in allt if x["filled"]]
print(f"Dolum orani (1s icinde fiyat girisin otesine gecti): {len(fill)}/{len(allt)} = {len(fill)/max(1,len(allt))*100:.1f}%")
print(f"\n[A] kapanis-kapanis, maliyet {COST} bps (birincil G2)")
A = report("havuz", allt, COST)
for s in SYMS: report(s, [x for x in allt if x["sym"] == s], COST)
print(f"\n[B] maker dolum modeli, maliyet {COST} bps (birincil G3)")
B = report("havuz", fill, COST)
for s in SYMS: report(s, [x for x in fill if x["sym"] == s], COST)
report("yalniz LONG (spot FDUSD yapilabilir)", [x for x in fill if x["side"] == 1], COST)
report("yalniz SHORT", [x for x in fill if x["side"] == -1], COST)
for cc in INFO_COSTS:
    print(f"\n[bilgi] maliyet {cc} bps"); report("A havuz", allt, cc); report("B havuz", fill, cc)
months = {}
for x in fill: months.setdefault(ts(x["t"])[:7], []).append(x["brut"] - COST)
print("\n[B] ay bazinda net (bps):", " ".join(f"{k}:{st.mean(v):+.1f}(n{len(v)})" for k, v in sorted(months.items())))
pos = sum(1 for v in months.values() if st.mean(v) > 0)
print(f"Pozitif ay: {pos}/{len(months)}")
if MODE == "test":
    g1 = A["N"] >= 100; g2 = A["net"] > 0 and A["p"] < 0.05; g3 = B is not None and B["net"] > 0 and B["p"] < 0.05
    best = max(months, key=lambda k: st.mean(months[k]))
    rest = [v for k, vs in months.items() if k != best for v in vs]
    g4 = len(rest) > 0 and st.mean(rest) > 0
    print(f"En iyi ay {best} cikarilinca B havuz net: {st.mean(rest):+.2f} bps (N={len(rest)})")
    print(f"\nKAPILAR: G1 N>=100 {g1} | G2 A net>0 & p<0.05 {g2} | G3 B net>0 & p<0.05 {g3} | G4 en iyi ay haric net>0 {g4}")
    print("SONUC:", "GECTI" if all([g1, g2, g3, g4]) else "KALDI")
