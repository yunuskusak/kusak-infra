# H3 on kesif (YALNIZ EGITIM, <= 2025-11-25 08:00 UTC). Kesitsel REV / MOM / FUNDING, dolar-notr.
import json, sys, os, math, statistics as st, datetime as dt
D = sys.argv[1]
CUT = 1764057600000; START = CUT - 731 * 86400000; DAY = 86400000; BAR = 4 * 3600000
COSTS = [10.0, 14.0]
pool = json.load(open(os.path.join(D, "pool.json")))
univ = {int(k): v for k, v in json.load(open(os.path.join(D, "univ.json"))).items()}
px, fund = {}, {}
for s in pool:
    k = json.load(open(os.path.join(D, "k4", s + ".json")))
    px[s] = {r[0] + BAR: float(r[4]) for r in k}            # anahtar = bar kapanis zamani
    fr = json.load(open(os.path.join(D, "fund", s + ".json")))
    fund[s] = sorted((int(r["fundingTime"]), float(r["fundingRate"])) for r in fr)
assert max(max(v) for v in px.values()) <= CUT, "TEST VERISI SIZDI"
assert max((f[-1][0] for f in fund.values() if f), default=0) < CUT, "TEST FUNDING SIZDI"
def price(s, t):
    p = px[s]; 
    if t in p: return p[t]
    ks = [x for x in (t - BAR * i for i in range(1, 7)) if x in p]   # delist: son fiyat (en fazla 24s geri)
    return p[ks[0]] if ks else None
import bisect
FT, FC = {}, {}
for _s, _f in fund.items():
    FT[_s] = [t for t, _ in _f]; acc = [0.0]
    for _, r in _f: acc.append(acc[-1] + r)
    FC[_s] = acc
def fsum(s, a, b):  # (a, b] araligindaki funding toplami
    return FC[s][bisect.bisect_right(FT[s], b)] - FC[s][bisect.bisect_right(FT[s], a)]
BTC = "BTCUSDT"
def backtest(kind, L, H, K):
    Lb, Hb = L * 3600000, H * 3600000
    t = START + ((-START) % (24 * 3600000))     # ilk 00:00 UTC
    prev = {}; out = []
    while t + Hb <= CUT:
        dkey = t - (t % DAY)
        U = univ.get(dkey)
        if not U: t += Hb; continue
        rows = []
        for s in U:
            p0, pl = px[s].get(t), px[s].get(t - Lb)
            if p0 is None or pl is None: continue
            r = p0 / pl - 1
            f = fsum(s, t - 24 * 3600000, t) if kind == "FUND" else 0.0
            rows.append((s, r, f))
        if len(rows) < 2 * K + 4: t += Hb; continue
        if kind in ("REV", "MOM"):
            srt = sorted(rows, key=lambda x: x[1])
            lo, hi = [x[0] for x in srt[:K]], [x[0] for x in srt[-K:]]
            longs, shorts = (lo, hi) if kind == "REV" else (hi, lo)
        else:
            rr = {x[0]: i for i, x in enumerate(sorted(rows, key=lambda x: x[1]))}
            rf = {x[0]: i for i, x in enumerate(sorted(rows, key=lambda x: x[2]))}
            srt = sorted(rows, key=lambda x: rr[x[0]] + rf[x[0]])
            longs, shorts = [x[0] for x in srt[:K]], [x[0] for x in srt[-K:]]
        w = {s: 1 / (2 * K) for s in longs}; w.update({s: -1 / (2 * K) for s in shorts})
        gross = 0.0; fpay = 0.0
        for s, wi in w.items():
            p0, p1 = px[s][t], price(s, t + Hb)
            if p1 is None: p1 = p0
            gross += wi * (p1 / p0 - 1)
            fpay += -wi * fsum(s, t, t + Hb)
        turn = sum(abs(w.get(s, 0) - prev.get(s, 0)) for s in set(w) | set(prev))
        b0, b1 = px[BTC].get(t), price(BTC, t + Hb)
        out.append(dict(t=t, gross=gross * 1e4, fund=fpay * 1e4, turn=turn, btc=(b1 / b0 - 1) * 1e4 if b0 and b1 else 0.0))
        prev = w; t += Hb
    return out
def stats(o, H, c):
    net = [x["gross"] + x["fund"] - c / 2 * x["turn"] for x in o]
    n = len(net); m = st.mean(net); sd = st.stdev(net); ppy = 365 * 24 / H
    b = [x["btc"] for x in o]; mb = st.mean(b)
    beta = sum((x - m) * (y - mb) for x, y in zip(net, b)) / sum((y - mb) ** 2 for y in b)
    half = START + 365 * DAY + 12 * 3600000
    y1 = [v for v, x in zip(net, o) if x["t"] < half]; y2 = [v for v, x in zip(net, o) if x["t"] >= half]
    q = {}
    for v, x in zip(net, o): q.setdefault((x["t"] - START) // (91 * DAY + 8 * 3600000), []).append(v)
    qpos = sum(1 for v in q.values() if sum(v) > 0)
    return dict(n=n, net=m, ann=m * ppy / 100, sh=m / sd * math.sqrt(ppy), t=m / sd * math.sqrt(n), beta=beta,
                y1=sum(y1) / 100, y2=sum(y2) / 100, qpos=qpos, qn=len(q))
GRID = [("REV", L, H, K) for L in (4, 12, 24) for H in (4, 8, 24) for K in (2, 3, 5)] + \
       [("MOM", L, H, K) for L in (12, 24, 72) for H in (4, 8, 24) for K in (2, 3, 5)] + \
       [("FUND", L, H, K) for L in (24, 72) for H in (8, 24) for K in (2, 3, 5)]
M = len(GRID); TBONF = 3.17   # tek yonlu 0.05/66
print(f"M={M} hucre, Bonferroni t esigi {TBONF}. Donem {dt.datetime.utcfromtimestamp(START/1000):%Y-%m-%d} .. {dt.datetime.utcfromtimestamp(CUT/1000):%Y-%m-%d %H:%M} UTC")
print(f"{'hucre':16} {'N':>5} {'brut':>7} {'fund':>6} {'turn':>5} | {'net10':>7} {'t10':>6} | {'net14':>7} {'yil%14':>7} {'Sh14':>6} {'t14':>6} {'beta':>6} {'Y1%':>6} {'Y2%':>6} {'Q+':>4} | aday")
res = []
for g in GRID:
    o = backtest(*g); H = g[2]
    s10, s14 = stats(o, H, 10), stats(o, H, 14)
    gm = st.mean(x["gross"] for x in o); fm = st.mean(x["fund"] for x in o); tm = st.mean(x["turn"] for x in o)
    strict = s14["net"] > 0 and s14["t"] >= TBONF and s14["y1"] > 0 and s14["y2"] > 0 and s14["qpos"] >= 5 and abs(s14["beta"]) < 0.3
    weak = (not strict) and s14["net"] > 0 and s14["t"] >= 2 and s14["y1"] > 0 and s14["y2"] > 0
    tag = "ISTIKRARLI" if strict else ("zayif" if weak else "")
    res.append((g, s10, s14, strict, weak))
    name = f"{g[0]} L{g[1]} H{g[2]} K{g[3]}"
    print(f"{name:16} {s14['n']:5} {gm:+7.1f} {fm:+6.2f} {tm:5.2f} | {s10['net']:+7.2f} {s10['t']:+6.2f} | {s14['net']:+7.2f} {s14['ann']:+7.1f} {s14['sh']:+6.2f} {s14['t']:+6.2f} {s14['beta']:+6.2f} {s14['y1']:+6.1f} {s14['y2']:+6.1f} {s14['qpos']}/{s14['qn']} | {tag}")
print(f"\nISTIKRARLI: {sum(r[3] for r in res)}/{M}  zayif: {sum(r[4] for r in res)}/{M}  net14>0: {sum(r[2]['net']>0 for r in res)}/{M}  net10>0: {sum(r[1]['net']>0 for r in res)}/{M}")
