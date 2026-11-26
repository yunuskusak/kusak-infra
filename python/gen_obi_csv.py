"""Test verisi uretici: C++ ObiTelemetry CSV semasiyla BIREBIR ayni kolonlar (yalniz test araci; numpy kullanir)."""
import numpy as np, csv
HZ = [10, 50, 100, 250, 500, 1000, 5000]
HDR = ["event_id","signal_wall_us","signal_mono_ns","side","threshold","obi","bid","ask","bid_qty","ask_qty","spread_bps","signal_price"] \
      + [f"price_after_{h}ms" for h in HZ] + ["max_lag_us","n_filled"]

def gen(path, n, wall0_us, mono0_ns, seed=1, gap_ms=100.0, thr=(0.3, 0.4, 0.5), edge_bps=None, sigma_bps=3.0, spread_bps=0.5,
        flat_tick=0.01, incomplete_frac=0.0, chunk=200_000):
    """edge_bps: {horizon_ms: beklenen yon-duzeltmeli getiri bps}; sigma_bps: 1 sn'lik gurultu (sqrt(h) olceklenir)."""
    rng = np.random.default_rng(seed); edge_bps = edge_bps or {}
    t_us = wall0_us; t_ns = mono0_ns; eid = 0
    with open(path, "w", newline="") as f:
        w = csv.writer(f); w.writerow(HDR)
        for c0 in range(0, n, chunk):
            m = min(chunk, n - c0)
            gaps = rng.exponential(gap_ms, m)
            side = rng.choice([-1, 1], m); th = rng.choice(thr, m); p0 = np.round(60000 + rng.normal(0, 50, m), 2)
            spr = np.maximum(0.01, rng.lognormal(np.log(spread_bps), 0.4, m))
            cols = []
            for h in HZ:
                drift = side * edge_bps.get(h, 0.0) * 1e-4
                noise = rng.normal(0, sigma_bps * np.sqrt(h / 1000.0), m) * 1e-4
                ph = np.round(p0 * (1 + drift + noise) / flat_tick) * flat_tick
                cols.append(ph)
            lag = rng.integers(20, 400, m); nfill = np.full(m, 7)
            if incomplete_frac > 0:
                inc = rng.random(m) < incomplete_frac; nfill[inc] = 6; cols[-1] = np.where(inc, np.nan, cols[-1])
            for k in range(m):
                t_us += int(gaps[k] * 1000); t_ns += int(gaps[k] * 1_000_000); eid += 1
                half = p0[k] * spr[k] * 1e-4 / 2
                w.writerow([eid, t_us, t_ns, int(side[k]), f"{th[k]:.3f}", f"{0.5*side[k]:.6f}", f"{p0[k]-half:.8f}", f"{p0[k]+half:.8f}",
                            3.0, 1.0, f"{spr[k]:.4f}", f"{p0[k]:.8f}"] + [("nan" if np.isnan(c[k]) else f"{c[k]:.8f}") for c in cols] + [int(lag[k]), int(nfill[k])])
    return t_us, t_ns
