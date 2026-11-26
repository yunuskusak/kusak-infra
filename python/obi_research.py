#!/usr/bin/env python3
"""
KUSAK-INFRA Phase 6 -- OBI Edge Research analizcisi  (yalniz Python stdlib; pandas/numpy GEREKMEZ)

C++ motorunun urettigi obi_research/obi_telemetry_*.csv dosyalarini okur; OBI esigi asilan her olay icin
+10ms ... +5s getirilerini (yon duzeltmeli, bps) analiz eder ve terminale tablo basar.

BELLEK: veri AKIS (streaming) olarak islenir -- hicbir satir bellekte tutulmaz. Tepe bellek veri boyutundan
        BAGIMSIZDIR (hucre basina sabit sayida sayac; Welford cevrimici varyans). Milyarlarca satir islenebilir.
SIRALAMA: dosyalar `signal_wall_us` (evrensel saat) uzerinden k-yollu birlestirilir (heapq.merge); dedupe de
        yalniz wall saati ile yapilir. `signal_mono_ns` makineye/acilisa ozgudur, dosyalar arasi KARSILASTIRILAMAZ.

Kullanim:
  python3 obi_research.py obi_research/                        # dizindeki tum obi_telemetry_*.csv
  python3 obi_research.py d1.csv d2.csv --fee-bps 1.0 --exit-spread-mult 2 --slippage-bps 0.3
  python3 obi_research.py obi_research/ --thresholds 0.3,0.5 --min-gap-ms 500 --max-lag-us 2000 --csv-out ozet.csv

Tanimlar (hepsi MID fiyat uzerinden, yon-duzeltmeli):
  getiri_bps(h) = side * (price_after_h - signal_price) / signal_price * 1e4     side=+1 AL baskisi, -1 SAT baskisi
  Kazanma / Kayip / Flat : getiri > 0 / < 0 / == 0   (flat = fiyat degismedi; kayip SAYILMAZ -- yuksek frekansta cok yaygindir)
  AvgWin/AvgLoss: yalniz kazananlarin / yalniz kaybedenlerin ortalamasi (MUTLAK deger)
  EV (brut)     : WinRate*AvgWin - LossRate*AvgLoss  (= ortalama getiri)
  EV (NET)      : brut - maliyet.   MALIYET (market giris + market cikis varsayimi):
                    2*fee_bps + slippage_bps + spread_bps * (1 + exit_spread_mult) / 2
                  Giris spread'i VERIDEN okunur; CIKIS spread'i VERIDE OLCULMEZ => giris spread'inin
                  `exit_spread_mult` kati VARSAYILIR (adverse selection: OBI patladiginda spread acilir). Varsayilan 1.5.
  k*            : EV_NET'in sifirlandigi cikis-spread carpani (kar-zarar esigi). k* < 1 => esit spread'de bile edge yok.
  p(t)          : Student-t (df=N-1), IKI YONLU, tam dagilim. (z-yaklasimi N<30'da p'yi YAPAY DUSUK gosterir.)
  p_bonf        : Bonferroni: min(1, p(t) * test_sayisi).  Cok hucreli tabloda ham p'ye guvenmeyin.
  MFE / MAE     : olay basina, ORNEKLENEN 7 horizon icindeki en iyi / en kotu getiri. Surekli yol OLMADIGINDAN
                  gercek MFE'nin ALT, gercek MAE'nin UST (daha az negatif) siniridir.
"""
import argparse, csv, glob, heapq, math, os, sys, time
from operator import itemgetter

try:
    import resource
except ImportError:          # Windows
    resource = None

HORIZONS = [10, 50, 100, 250, 500, 1000, 5000]
REQUIRED = ["event_id", "signal_wall_us", "side", "threshold", "spread_bps", "signal_price", "max_lag_us", "n_filled"] \
           + [f"price_after_{h}ms" for h in HORIZONS]
NaN = float("nan")


# ───────────────────────── Student-t (tam dagilim, stdlib) ─────────────────────────
def _betacf(a, b, x):
    """Duzgun-incomplete-beta icin Lentz surekli kesri (Numerical Recipes)."""
    FPMIN, EPS = 1e-300, 3e-16
    qab, qap, qam = a + b, a + 1.0, a - 1.0
    c, d = 1.0, 1.0 - qab * x / qap
    d = 1.0 / (d if abs(d) > FPMIN else FPMIN)
    h = d
    for m in range(1, 400):
        m2 = 2 * m
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1.0 + aa * d; d = d if abs(d) > FPMIN else FPMIN
        c = 1.0 + aa / c; c = c if abs(c) > FPMIN else FPMIN
        d = 1.0 / d; h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1.0 + aa * d; d = d if abs(d) > FPMIN else FPMIN
        c = 1.0 + aa / c; c = c if abs(c) > FPMIN else FPMIN
        d = 1.0 / d; de = d * c; h *= de
        if abs(de - 1.0) < EPS:
            break
    return h


def _betai(a, b, x, y):
    """Duzgun-incomplete-beta I_x(a,b). y = 1-x'in HASSAS degeri (x 1'e yuvarlandiginda bile tumleyen kaybolmasin)."""
    if x <= 0.0: return 0.0
    if y <= 0.0: return 1.0
    lx = math.log(x) if x < 0.5 else math.log1p(-y)
    ly = math.log(y) if y < 0.5 else math.log1p(-x)
    bt = math.exp(math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b) + a * lx + b * ly)
    if x < (a + 1.0) / (a + b + 2.0):
        return bt * _betacf(a, b, x) / a
    return 1.0 - bt * _betacf(b, a, y) / b


def p_t(t, df):
    """Iki yonlu p-degeri, Student-t, df serbestlik derecesi.
    Dogruluk (scipy/mpmath'e karsi olculdu): goreceli hata <4e-13 (df<=100, yani t-z farkinin onemli oldugu bolge),
    <1e-10 (df<=1e4), <1e-8 (df<=1e6), ~1e-7 (df=1e7). p 4 ondalikla basildigindan pratik etkisi yoktur."""
    if df < 1 or t != t:
        return NaN
    if t == 0.0:
        return 1.0
    t2 = t * t
    return _betai(df / 2.0, 0.5, df / (df + t2), t2 / (df + t2))


def p_z(t):
    """Normal (z) yaklasimi -- YALNIZ karsilastirma/uyari icin."""
    return math.erfc(abs(t) / math.sqrt(2.0)) if t == t else NaN


# ───────────────────────── cevrimici biriktiriciler ─────────────────────────
class Cell:
    """(esik, yon, horizon) hucresi: sabit bellek. Welford ile NET getirinin ortalama/varyansi."""
    __slots__ = ("n", "wins", "losses", "flat", "sum_w", "sum_l", "sum_g", "nwin_net", "mean", "m2", "sum_s")

    def __init__(self):
        self.n = self.wins = self.losses = self.flat = self.nwin_net = 0
        self.sum_w = self.sum_l = self.sum_g = self.mean = self.m2 = self.sum_s = 0.0

    def add(self, g, net, spread):
        self.n += 1
        self.sum_g += g
        if g > 0:    self.wins += 1;   self.sum_w += g
        elif g < 0:  self.losses += 1; self.sum_l -= g
        else:        self.flat += 1
        if net > 0:  self.nwin_net += 1
        d = net - self.mean
        self.mean += d / self.n
        self.m2 += d * (net - self.mean)
        self.sum_s += spread


def cell_stats(c, fee2, slip):
    n = c.n
    avg_w = c.sum_w / c.wins if c.wins else 0.0
    avg_l = c.sum_l / c.losses if c.losses else 0.0
    ev_g = c.sum_g / n
    t = NaN
    if n >= 2 and c.m2 > 0:
        t = c.mean / math.sqrt(c.m2 / (n - 1) / n)
    mean_s = c.sum_s / n
    base = ev_g - fee2 - slip
    kstar = (2.0 * base / mean_s - 1.0) if (mean_s > 0 and base > 0) else (float("inf") if base > 0 else NaN)
    return dict(n=n, wr=c.wins / n, lr=c.losses / n, flat=c.flat / n, avg_w=avg_w, avg_l=avg_l, ev_g=ev_g, ev_n=c.mean,
                t=t, pt=p_t(t, n - 1), pz=p_z(t), wr_net=c.nwin_net / n, mean_spread=mean_s, kstar=kstar)


# ───────────────────────── akis: dosya -> generator -> k-yollu birlestirme ─────────────────────────
def find_files(paths):
    out = []
    for p in paths:
        if os.path.isdir(p):
            out += sorted(glob.glob(os.path.join(p, "obi_telemetry_*.csv")))
        else:
            out += sorted(glob.glob(p)) or [p]
    return [f for f in out if os.path.isfile(f)]


def iter_file(path, a, cnt, want):
    """Satir satir okur (bellekte tutmaz). Yield: (wall_us, thr, side, spread_bps, p0, [7 fiyat], lag_us)."""
    with open(path, newline="") as fh:
        rd = csv.reader(fh)
        hdr = next(rd, None)
        if not hdr:
            return
        ix = {name: i for i, name in enumerate(hdr)}
        miss = [c for c in REQUIRED if c not in ix]
        if miss:
            raise SystemExit(f"HATA: {os.path.basename(path)} icinde eksik kolon(lar): {miss}")
        i_w, i_t, i_s, i_sp, i_p, i_l, i_n = (ix[k] for k in ("signal_wall_us", "threshold", "side", "spread_bps", "signal_price", "max_lag_us", "n_filled"))
        pcols = [ix[f"price_after_{h}ms"] for h in HORIZONS]
        full = len(HORIZONS)
        for r in rd:
            cnt["rows"] += 1
            try:
                p0 = float(r[i_p]); wall = int(r[i_w]); side = int(r[i_s]); thr = round(float(r[i_t]), 3)
                lag = int(r[i_l]); nf = int(r[i_n]); spread = float(r[i_sp]); px = [float(r[c]) for c in pcols]
            except (ValueError, IndexError):
                cnt["bad"] += 1
                continue
            if p0 <= 0 or side not in (1, -1):
                cnt["bad"] += 1
                continue
            if want and thr not in want:
                continue
            if a.complete_only and nf < full:
                cnt["incomplete"] += 1
                continue
            if a.max_lag_us is not None and lag > a.max_lag_us:
                cnt["lag"] += 1
                continue
            yield (wall, thr, side, spread, p0, px, lag)


def peak_rss_mb():
    if resource is None:
        return NaN
    kb = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return kb / 1024.0 if sys.platform != "darwin" else kb / 1048576.0


# ───────────────────────── cikti ─────────────────────────
class Fmt:
    def __init__(self, color): self.c = color
    def g(self, s): return f"\033[32m{s}\033[0m" if self.c else s
    def r(self, s): return f"\033[31m{s}\033[0m" if self.c else s
    def b(self, s): return f"\033[1m{s}\033[0m" if self.c else s
    def d(self, s): return f"\033[2m{s}\033[0m" if self.c else s
    def y(self, s): return f"\033[33m{s}\033[0m" if self.c else s


def signed(v, f, w=9, dec=3):
    s = f"{v:+{w}.{dec}f}"
    return f.g(s) if v > 0 else f.r(s) if v < 0 else s


def strip_ansi(s):
    out, skip = [], False
    for ch in s:
        if ch == "\033": skip = True
        if not skip: out.append(ch)
        if skip and ch == "m": skip = False
    return "".join(out)


def print_table(head, rows, al, f):
    w = [max(len(strip_ansi(str(c))) for c in col) for col in zip(head, *rows)] if rows else [len(h) for h in head]

    def fit(cells):
        out = []
        for i, c in enumerate(cells):
            s = str(c); pad = w[i] - len(strip_ansi(s))
            out.append((s + " " * pad) if al[i] == "l" else (" " * pad + s))
        return "| " + " | ".join(out) + " |"
    sep = "+" + "+".join("-" * (x + 2) for x in w) + "+"
    print(sep); print(fit([f.b(h) for h in head])); print("+" + "+".join("=" * (x + 2) for x in w) + "+")
    for r_ in rows: print(fit(r_))
    print(sep)


def fnum(x, fmt):
    return fmt.format(x) if x == x and x not in (float("inf"), float("-inf")) else ("inf" if x == float("inf") else "nan")


def main():
    ap = argparse.ArgumentParser(description="KUSAK OBI edge arastirma analizcisi", formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("paths", nargs="+", help="CSV dosyalari veya obi_research/ dizini")
    ap.add_argument("--thresholds", help="analiz edilecek esikler (virgullu); varsayilan: dosyadaki tum esikler")
    ap.add_argument("--horizons", help="gosterilecek horizon'lar ms (virgullu); varsayilan: hepsi")
    ap.add_argument("--min-gap-ms", type=float, default=0.0, help="ayni (esik,yon) icinde olaylar arasi asgari sure, WALL saatiyle (ust uste binmeyi azaltir)")
    ap.add_argument("--max-lag-us", type=int, default=None, help="ornekleme gecikmesi bunu asan olaylari at (ornek: 2000)")
    ap.add_argument("--fee-bps", type=float, default=0.0, help="islem basina komisyon (bps); maliyet = 2*fee")
    ap.add_argument("--exit-spread-mult", type=float, default=1.5, help="CIKIS spread'i = giris spread'i x bu carpan (adverse selection varsayimi; varsayilan 1.5)")
    ap.add_argument("--slippage-bps", type=float, default=0.0, help="ek sabit kayma maliyeti (bps, giris+cikis toplam; defter yurume/kuyruk)")
    ap.add_argument("--no-spread-cost", action="store_true", help="spread maliyetini KAPAT (onerilmez: NET kolonu iyimser olur)")
    ap.add_argument("--spread-cost", action="store_true", help=argparse.SUPPRESS)      # eski bayrak: artik varsayilan, geriye uyumluluk
    ap.add_argument("--include-incomplete", dest="complete_only", action="store_false", help="7 horizon'u dolmamis satirlari da al (varsayilan: at)")
    ap.add_argument("--min-n", type=int, default=30, help="bu N'in altindaki hucreler '*' ile isaretlenir")
    ap.add_argument("--show-z", action="store_true", help="karsilastirma icin normal-yaklasim p(z) kolonunu da goster")
    ap.add_argument("--csv-out", help="ozet tabloyu CSV olarak yaz")
    ap.add_argument("--no-color", action="store_true")
    a = ap.parse_args()
    if a.exit_spread_mult < 0 or a.slippage_bps < 0 or a.fee_bps < 0:
        ap.error("fee/slippage/exit-spread-mult negatif olamaz")
    f = Fmt(sys.stdout.isatty() and not a.no_color)
    t_start = time.time()

    files = find_files(a.paths)
    if not files:
        print("HATA: CSV bulunamadi:", a.paths); return 2
    want = {round(float(x), 3) for x in a.thresholds.split(",")} if a.thresholds else None
    hz = [int(x) for x in a.horizons.split(",")] if a.horizons else HORIZONS
    hz_idx = [HORIZONS.index(h) for h in hz if h in HORIZONS]
    hz_mask = [i in hz_idx for i in range(len(HORIZONS))]
    nH = len(HORIZONS)

    fee2, slip = 2.0 * a.fee_bps, a.slippage_bps
    sf = 0.0 if a.no_spread_cost else (1.0 + a.exit_spread_mult) / 2.0       # spread maliyeti = spread * sf
    base_cost = fee2 + slip
    gap_us = a.min_gap_ms * 1000.0

    cnt = dict(rows=0, bad=0, incomplete=0, lag=0, dedup=0, kept=0, disorder=0)
    cells, mm = {}, {}                  # (thr,side,i) -> Cell ; (thr,side) -> [n, sum_mfe, sum_mae]
    last, prev_wall, w_min, w_max = {}, None, None, None
    lag_hist, lag_max = [0] * 40, 0

    # ── AKIS: dosyalar wall saatiyle k-yollu birlestirilir; hicbir satir listede tutulmaz ──
    stream = heapq.merge(*[iter_file(fp, a, cnt, want) for fp in files], key=itemgetter(0))
    for wall, thr, side, spread, p0, px, lag in stream:
        if prev_wall is not None and wall < prev_wall:
            cnt["disorder"] += 1
        prev_wall = wall
        if gap_us > 0:                                   # dedupe: YALNIZ wall saati (dosyalar/acilislar arasi gecerli)
            k = (thr, side); lw = last.get(k)
            if lw is not None and wall - lw < gap_us:
                cnt["dedup"] += 1
                continue
            last[k] = wall
        cnt["kept"] += 1
        w_min = wall if w_min is None or wall < w_min else w_min
        w_max = wall if w_max is None or wall > w_max else w_max
        lag_hist[min(39, lag.bit_length())] += 1
        if lag > lag_max: lag_max = lag
        inv = side * 1e4 / p0
        cost = base_cost + sf * spread
        best, worst = -1e300, 1e300
        for i in range(nH):
            p = px[i]
            if p != p:
                continue
            g = (p - p0) * inv
            if g > best: best = g
            if g < worst: worst = g
            if hz_mask[i]:
                key = (thr, side, i)
                c = cells.get(key)
                if c is None:
                    c = cells[key] = Cell()
                c.add(g, g - cost, spread)
        if best > -1e300:
            m = mm.get((thr, side))
            if m is None:
                m = mm[(thr, side)] = [0, 0.0, 0.0]
            m[0] += 1; m[1] += best; m[2] += worst

    # ── baslik ──
    W = 118
    print(f.b("=" * W)); print(f.b("  KUSAK-INFRA  |  OBI EDGE ARASTIRMASI  |  yon-duzeltmeli getiri (bps), MID fiyat, AKIS modu")); print(f.b("=" * W))
    print(f"  dosya            : {len(files)} ({', '.join(os.path.basename(x) for x in files[:3])}{' ...' if len(files) > 3 else ''})  | birlestirme: signal_wall_us")
    print(f"  satir okunan     : {cnt['rows']}   kullanilan olay: {cnt['kept']}   | atilan: eksik-horizon {cnt['incomplete']}, "
          f"gecikme {cnt['lag']}, dedupe {cnt['dedup']}, bozuk {cnt['bad']}")
    if cnt["disorder"]:
        print(f.y(f"  UYARI: {cnt['disorder']} satir wall-saat sirasi disi geldi (dosya ici kucuk karisiklik / ust uste binen dosyalar); dedupe etkilenebilir."))
    if cnt["kept"]:
        dur_h = (w_max - w_min) / 3.6e9
        # gecikme p50/p99: log2 kovalarinin UST siniri
        tot, acc, p50, p99 = cnt["kept"], 0, 0, 0
        for b, v in enumerate(lag_hist):
            acc += v
            ub = 1 if b == 0 else (1 << b)
            if not p50 and acc * 2 >= tot: p50 = ub
            if acc * 100 >= tot * 99: p99 = ub; break
        rate = f"{cnt['kept'] / dur_h:.0f} olay/saat" if dur_h > 0 else "-"
        print(f"  kapsanan sure    : {dur_h:.2f} saat ({rate})  | ornekleme gecikmesi: p50<={p50} us, p99<={p99} us, max={lag_max} us")
    print(f"  maliyet modeli   : 2*fee({a.fee_bps}) + kayma({a.slippage_bps}) + spread x " +
          ("0 (KAPALI - NET iyimser!)" if a.no_spread_cost else f"(1+{a.exit_spread_mult})/2 = x{sf:.3f}  [cikis spread'i = {a.exit_spread_mult}x giris, VARSAYIM]") +
          f"   | min-gap: {a.min_gap_ms} ms (wall)")
    if not cnt["kept"]:
        print(f.r("\n  Analiz edilecek olay yok.")); return 1

    # ── hucre istatistikleri ──
    stats = {k: cell_stats(c, fee2, slip) for k, c in cells.items()}
    n_tests = sum(1 for s in stats.values() if s["n"] >= 2 and s["pt"] == s["pt"])
    for s in stats.values():
        s["pb"] = min(1.0, s["pt"] * n_tests) if s["pt"] == s["pt"] else NaN
        s["zlie"] = (s["n"] < 30 and s["pz"] == s["pz"] and s["pz"] < 0.05 <= s["pt"])     # z "anlamli" gosterir, t degil
    summary = []
    thrs = sorted({k[0] for k in stats})
    for thr in thrs:
        print("\n" + f.b(f"-- ESIK |OBI| > {thr:.2f} " + "-" * 92))
        head = ["Yon", "Horizon", "N", "Win%", "Flat%", "AvgWin", "AvgLoss", "EV brut", "EV NET", "t(NET)", "p(t)"] + (["p(z)"] if a.show_z else []) + ["p_bonf", "Win%NET"]
        al = ["l", "r", "r", "r", "r", "r", "r", "r", "r", "r", "r"] + (["r"] if a.show_z else []) + ["r", "r"]
        rows, mm_rows = [], []
        for side, name in ((+1, "AL"), (-1, "SAT")):
            for i in hz_idx:
                s = stats.get((thr, side, i))
                if not s: continue
                star = "*" if s["n"] < a.min_n else " "
                pcell = fnum(s["pt"], "{:.4f}") + ("!" if s["zlie"] else " ")
                rows.append([name, f"+{HORIZONS[i]}ms", f"{s['n']}{star}", f"{s['wr']*100:6.2f}", f"{s['flat']*100:6.2f}", signed(s["avg_w"], f, 8, 3),
                             f"{s['avg_l']:8.3f}", signed(s["ev_g"], f, 9, 4), signed(s["ev_n"], f, 9, 4), fnum(s["t"], "{:+7.2f}"), pcell]
                            + ([fnum(s["pz"], "{:.4f}")] if a.show_z else []) + [fnum(s["pb"], "{:.4f}"), f"{s['wr_net']*100:6.2f}"])
                summary.append(dict(threshold=thr, side=side, horizon_ms=HORIZONS[i], n=s["n"], win=s["wr"], loss=s["lr"], flat=s["flat"], avg_win=s["avg_w"],
                                    avg_loss=s["avg_l"], ev_gross=s["ev_g"], ev_net=s["ev_n"], t=s["t"], p_t=s["pt"], p_z=s["pz"], p_bonf=s["pb"],
                                    win_net=s["wr_net"], mean_spread_bps=s["mean_spread"], k_star=s["kstar"]))
            m = mm.get((thr, side))
            if m:
                am, aa = m[1] / m[0], m[2] / m[0]
                mm_rows.append([name, f"{m[0]}", signed(am, f, 9, 3), signed(aa, f, 9, 3), fnum(am / abs(aa), "{:.2f}") if aa != 0 else "inf"])
        if rows: print_table(head, rows, al, f)
        if mm_rows:
            print(f.d("  MFE/MAE (7 ornek noktasindan; surekli yol yok => MFE alt sinir, MAE ust sinir; maliyet DAHIL DEGIL):"))
            print_table(["Yon", "N", "avg MFE bps", "avg MAE bps", "MFE/|MAE|"], mm_rows, ["l", "r", "r", "r", "r"], f)

    # ── sonuc & uyarilar ──
    cand = [s for s in summary if s["n"] >= a.min_n and s["t"] == s["t"]]
    print("\n" + f.b("=" * W)); print(f.b("  SONUC"))
    if n_tests:
        print(f"  test sayisi (hucre): {n_tests}  -> Bonferroni: p_bonf = min(1, p(t) x {n_tests})")
    else:
        print(f.y("  Hicbir hucrede N>=2 yok: t-testi/p-degeri HESAPLANAMADI (cok az olay). Daha uzun toplayin."))
    if cand:
        best = max(cand, key=lambda s: s["t"])
        sig = [s for s in cand if s["ev_net"] > 0 and s["p_bonf"] < 0.05]
        raw = [s for s in cand if s["ev_net"] > 0 and s["p_t"] < 0.05]
        ks = fnum(best["k_star"], "{:.2f}")
        print(f"  en yuksek t(NET)   : esik {best['threshold']:.2f} {'AL' if best['side'] > 0 else 'SAT'} +{best['horizon_ms']}ms  N={best['n']}  "
              f"EV NET={best['ev_net']:+.4f} bps  t={best['t']:+.2f}  p(t)={best['p_t']:.4f}  p_bonf={best['p_bonf']:.4f}")
        print(f"                       ort. giris spread={best['mean_spread_bps']:.4f} bps  |  kar-zarar cikis-spread carpani k* = {ks}  (varsayilan {a.exit_spread_mult}; k*<1 => edge yok)")
        print(f"  net-pozitif & p(t)<0.05 (HAM)        : {len(raw)} / {len(cand)} hucre   <-- sans eseri ~{0.05*len(cand):.1f} beklenir")
        print(f"  net-pozitif & p_bonf<0.05 (DUZELTILMIS): {len(sig)} / {len(cand)} hucre   <-- gercek kanit adayi")
        if not sig: print(f.y("  => Coklu-test duzeltmesinden sonra istatistiksel olarak anlamli NET edge YOK."))
        if raw and not sig: print(f.y("     Ham p'ye gore 'anlamli' gorunen hucreler coklu karsilastirma illuzyonu olabilir."))
    else:
        print(f.r(f"  Yeterli orneklem yok (hicbir hucrede N >= {a.min_n})."))
    zl = [s for s in stats.values() if s["zlie"]]
    small = [s for s in stats.values() if s["n"] < 30]
    if small:
        ex_t, ex_n = 2.5, 10
        print(f.y(f"  KUCUK ORNEKLEM: {len(small)} hucrede N<30. Bu hucrelerde p(t) TAM Student-t ile hesaplandi; normal(z) yaklasimi p'yi yapay dusuk gosterir"))
        print(f.y(f"     (ornek: t={ex_t}, N={ex_n}: p(t)={p_t(ex_t, ex_n-1):.4f} vs p(z)={p_z(ex_t):.4f} -> z {p_t(ex_t, ex_n-1)/p_z(ex_t):.1f}x daha 'anlamli' gosterirdi)."))
        if zl: print(f.y(f"     '!' isaretli {len(zl)} hucre z ile p<0.05 gorunur ama t ile ANLAMSIZDIR."))
    print(f.d("  UYARI (DOLUM): tum maliyetler MARKET (taker) giris+cikis, dolum=%100 varsayar. Pasif (limit) emir varsayiyorsaniz dolum olasiligi < 1'dir ve"))
    print(f.d("     dolanlar TERS SECILMIS olaylardir (fiyat lehinize giderse emir dolmaz, aleyhinize giderse dolar) => buradaki EV bunu ISKALAR; gercek EV daha dusuktur."))
    print(f.d(f"  UYARI (CIKIS SPREAD): cikis spread'i veride OLCULMEZ; giris spread'inin {a.exit_spread_mult}x'i VARSAYILDI. OBI patlamasinda spread agilir, kuyruk/defter yurume ek"))
    print(f.d("     kayma yaratir (--slippage-bps). Sonuc k* ve --exit-spread-mult 2/3 ile duyarlilik taramasi yapmadan guvenilmemelidir."))
    print(f.d("  UYARI (OTOKORELASYON): ardisik olaylar ayni OBI rejiminde tekrar eder => t/p sisirilir; --min-gap-ms ile seyreltin."))
    print(f.d("  UYARI (SECIM YANLILIGI): 'en yuksek t' hucresi cok hucre arasindan secilmistir; yalniz p_bonf anlamlidir. Farkli gun/rejimlerle out-of-sample dogrulayin."))
    print(f.b("=" * W))
    mem = peak_rss_mb(); dt = time.time() - t_start
    print(f.d(f"  [islem] {cnt['rows']} satir, {dt:.1f} sn ({cnt['rows']/max(dt,1e-9):,.0f} satir/sn), tepe bellek {mem:.1f} MB (akis modu: veri boyutundan bagimsiz)"))

    if a.csv_out and summary:
        with open(a.csv_out, "w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=list(summary[0].keys())); w.writeheader(); w.writerows(summary)
        print(f"  ozet yazildi: {a.csv_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
