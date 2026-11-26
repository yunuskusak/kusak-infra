#!/usr/bin/env python3
"""
KUSAK-INFRA Phase 4 -- VPIN denetim analizcisi (yalniz stdlib, kod degistirmez).

Kullanim:
  python3 vpin_audit.py --telemetry telemetry.csv          # ESKI loglardan kanit (yeni yama gerekmez)
  python3 vpin_audit.py --diag-dir vpin_diag               # yamali motorun urettigi CSV'lerden kok-neden hukmu
  python3 vpin_audit.py --telemetry telemetry.csv --diag-dir vpin_diag
"""
import argparse, csv, glob, os, sys, collections

S = 1_000_000
A = 50_000

def ewma_int(p, v=S):            # vpin_velocity_tracker.hpp::recompute_vpin ile birebir tamsayi EWMA
    return (A * v + (S - A) * p) // S

def load_telemetry_runs(path):
    runs, cur = [], None
    with open(path, newline='') as f:
        for line in f:
            line = line.strip()
            if line.startswith('ts_ns'):
                cur = []; runs.append(cur); continue
            p = line.split(',')
            if len(p) >= 8 and p[1] == 'VPIN_ALARM':
                cur.append((int(p[0]), float(p[6]), float(p[7])))
    return [r for r in runs if r]

def audit_telemetry(path):
    runs = load_telemetry_runs(path)
    rows = sum(len(r) for r in runs)
    print("=" * 78); print(f"[A] ESKI TELEMETRI: {path}"); print("=" * 78)
    print(f"  calisma (run) sayisi : {len(runs)}   VPIN_ALARM satiri : {rows}")
    v1 = sum(1 for r in runs for x in r if x[1] == 1.0)
    print(f"  VPIN == 1.000000 olan satir : {v1}/{rows}  (%{100.0*v1/max(rows,1):.4f})")
    model_i = [600_000]
    for _ in range(400): model_i.append(ewma_int(model_i[-1]))
    tot_steps = tot_consec = 0
    for i, r in enumerate(runs):
        thr = [round(x[2] * S) for x in r]
        d = []
        for t in thr:
            if not d or d[-1] != t: d.append(t)
        chg = [k for k in range(1, len(thr)) if thr[k] != thr[k-1]]
        consec = 0
        while consec < len(chg) and chg[consec] == consec + 1: consec += 1
        idx = model_i.index(d[0]) if d[0] in model_i else -1
        int_ok = idx >= 0 and all(d[k] == model_i[idx + k] for k in range(min(len(d), len(model_i) - idx)))
        build = "tamsayi-EWMA (guncel)" if int_ok else "double-EWMA (eski surum)"
        print(f"  run{i:2d}: satir={len(r):5d} esik-adimi={len(d):3d} ardisik-satir-adimi={consec:3d}  son_esik={thr[-1]/S:.6f}  -> {build}")
        tot_steps += len(d); tot_consec += consec
    print("\n  OKUMA:")
    print("   * Dinamik esik (EWMA) YALNIZ bucket kapanisinda guncellenir (recompute_vpin).")
    print("   * Esik her ardisik alarm satirinda bir adim ilerliyor => her ISLEM bir bucket kapatiyor.")
    print("   * Tek islemli bucket'ta buy=0 veya sell=0 => |b-s|/(b+s) = 1 TAM => VPIN = ortalama(1,1,..) = 1.0.")
    print("   * Esik, vpin=1.0 dizisinin EWMA parmak izini (0.62, 0.639, ... -> 0.999981) birebir izliyor.")

def audit_diag(d):
    bfiles = sorted(glob.glob(os.path.join(d, 'vpin_buckets_*.csv')))
    tfiles = sorted(glob.glob(os.path.join(d, 'vpin_trades_*.csv')))
    print("\n" + "=" * 78); print(f"[B] TANILAMA CIKTISI: {d}"); print("=" * 78)
    if not bfiles and not tfiles:
        print("  CSV bulunamadi. vpin_diag_enabled=true ile calistirdiniz mi?"); return
    verdict = []
    # ---- bucket duzeyi ----
    nb = single = one = full = 0; hist = collections.Counter(); over = []; vp = []; imb_bins = collections.Counter()
    first_valid = None
    for fpath in bfiles:
        with open(fpath, newline='') as f:
            for r in csv.DictReader(f):
                nb += 1; n = int(r['n_trades']); single += int(r['single_sided']); one += (n == 1)
                im = float(r['imbalance']); full += (im >= 0.999999)
                imb_bins[min(int(im * 5), 4)] += 1
                hist['1' if n == 1 else '2' if n == 2 else '3-5' if n <= 5 else '6-20' if n <= 20 else '21-100' if n <= 100 else '>100'] += 1
                over.append(float(r['total_over_target']))
                if int(r['valid']): vp.append(int(r['vpin_x1e6'])); first_valid = first_valid or int(r['bucket_id'])
    print(f"  dosya: {len(bfiles)} bucket CSV, {len(tfiles)} islem CSV")
    if nb:
        print(f"  kapanan bucket           : {nb}")
        print(f"  tek-yonlu (buy=0|sell=0) : {single}  (%{100.0*single/nb:.2f})")
        print(f"  tek-islemli bucket       : {one}  (%{100.0*one/nb:.2f})")
        print(f"  imbalance == 1.0 olan    : {full}  (%{100.0*full/nb:.2f})")
        print(f"  islem/bucket dagilimi    : " + "  ".join(f"{k}:{hist[k]}" for k in ['1','2','3-5','6-20','21-100','>100']))
        print(f"  imbalance dagilimi       : " + "  ".join(f"[{i/5:.1f}-{(i+1)/5:.1f}):{imb_bins[i]}" for i in range(5)))
        so = sorted(over); print(f"  bucket_hacmi/hedef       : min={so[0]:.3f} medyan={so[len(so)//2]:.3f} max={so[-1]:.3f}  (1.0 ~ ideal; >>1 = tasma)")
        if vp: print(f"  GECERLI VPIN araligi     : {min(vp)/S:.4f} .. {max(vp)/S:.4f}   (ilk gecerli bucket_id={first_valid})")
        else:
            print(f"  GECERLI VPIN             : YOK (min_buckets_valid dolmadi -> vpin=0, alarm uretilmez)")
            verdict.append(f"VPIN HIC GECERLI OLMADI: yalniz {nb} bucket kapandi (gecerlilik icin >= min_buckets_valid gerekir). "
                           "Bucket hedefi piyasa hacmine gore cok BUYUK -> VPIN fiilen devre disi (vpin=0).")
        if single / nb > 0.95 and one / nb > 0.95:
            verdict.append("KOK NEDEN: her bucket TEK islemden olusuyor (islem hacmi >= bucket hedefi) => imbalance=1.0 => VPIN=1.0.")
        elif single / nb > 0.5:
            verdict.append("UYARI: bucket'larin yarisindan fazlasi tek yonlu; bucket hedefi islem boyutuna gore cok kucuk.")
    else:
        verdict.append("BUCKET HIC KAPANMADI: bucket hedefi piyasa hacmine gore cok buyuk -> VPIN gecerli olmuyor (vpin=0).")
    # ---- islem duzeyi ----
    n = pm = tm = tk = viol = ge1 = 0; maxr = 0.0
    for fpath in tfiles:
        with open(fpath, newline='') as f:
            for r in csv.DictReader(f):
                n += 1; pm += (r['side_prod'] == r['side_aggressor'])
                if r['side_tick'] != 'U': tk += 1; tm += (r['side_tick'] == r['side_aggressor'])
                q8, qs = int(r['qty_x1e8']), int(r['qty_scaled'])
                if qs * 100 > q8 or q8 - qs * 100 >= 100: viol += 1
                ratio = float(r['trade_over_target']); ge1 += (ratio >= 1.0); maxr = max(maxr, ratio)
    if n:
        print(f"\n  islem satiri (ornek)     : {n}")
        print(f"  yon uyumu uretim~borsa-agresor : %{100.0*pm/n:.1f}   tick-rule~agresor : %{100.0*tm/max(tk,1):.1f}")
        print(f"  olcek ihlali (qty_scaled != qty_x1e8/100) : {viol}/{n}")
        print(f"  islem >= bucket hedefi   : {ge1}/{n}  (en buyuk islem/hedef={maxr:.4g})")
        if viol: verdict.append("OLCEK HATASI: qty_scaled, qty_x1e8/100 degil -> bucket hesabi bozuk.")
        if pm / n < 0.05: verdict.append("YON ETIKETI TERS: uretim is_buy=buyer_is_maker veriyor (borsada m=true => agresif SATIS). "
                                         "|buy-sell| simetrik oldugundan VPIN buyuklugunu etkilemez; yon-bagimli kullanimlari bozar.")
        elif pm / n < 0.55 and pm / n > 0.45: verdict.append("UYARI: yon etiketi agresorle rastgele uyumlu -> siniflandirma anlamsiz.")
    # ---- golge tracker'lar (parametrik test) ----
    sfiles = sorted(glob.glob(os.path.join(d, 'vpin_shadow_*.csv')))
    last = {}
    for fpath in sfiles:
        with open(fpath, newline='') as f:
            for r in csv.DictReader(f): last[r['label']] = r      # her etiket icin SON goruntu
    if last:
        print("\n  PARAMETRIK TEST (son goruntu; hedef = bucket hacmi, taban birim):")
        print(f"    {'etiket':<11}{'hedef':>14}{'bucket':>9}{'tek-yonlu%':>12}{'tek-islemli%':>14}{'VPIN':>9}{'gecerli':>9}")
        for lab in ['uretim','bkt_x10','bkt_x20','bkt_x50','bkt_x100','tick_x50','mult_probe']:
            r = last.get(lab)
            if not r: continue
            b = int(r['buckets']) or 1
            print(f"    {lab:<11}{int(r['target_scaled'])/S:>14.6f}{int(r['buckets']):>9}{100.0*int(r['single_sided'])/b:>12.1f}"
                  f"{100.0*int(r['one_trade_buckets'])/b:>14.1f}{int(r['vpin_x1e6'])/S:>9.4f}{int(r['valid']):>9}")
        u, m = last.get('uretim'), last.get('mult_probe')
        if u and m:
            same = all(u[k] == m[k] for k in ('target_scaled','buckets','single_sided','vpin_x1e6'))
            print(f"    vpin_bucket_mult degisince sonuc degisti mi?  {'HAYIR -> parametre OLU (hesaplamada kullanilmiyor)' if same else 'EVET'}")
            if same: verdict.append("vpin_bucket_mult OLU PARAMETRE: degeri degistirmek bucket boyutunu/VPIN'i degistirmiyor "
                                    "(tracker yalniz bucket_volume_scaled kullaniyor; mult yalnizca atanip hic okunmuyor).")
    print("\n  >>> HUKUM:")
    if not verdict: verdict.append("Belirgin bir hata isareti yok; golge tracker satirlarini (konsol/vpin_shadow_*.csv) inceleyin.")
    for v in verdict: print("   - " + v)

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--telemetry'); ap.add_argument('--diag-dir')
    a = ap.parse_args()
    if not a.telemetry and not a.diag_dir: ap.print_help(); sys.exit(1)
    if a.telemetry: audit_telemetry(a.telemetry)
    if a.diag_dir:  audit_diag(a.diag_dir)
