# Dinamik evren: her gun D icin, D'den ONCEKI 30 gunun quote hacmine gore ilk 35.
import json, sys, os, glob
D = sys.argv[1]; N = 35
CUT = 1764057600000; START = CUT - 731 * 86400000; DAY = 86400000
vol = {}
for f in glob.glob(os.path.join(D, "d1", "*.json")):
    sym = os.path.basename(f)[:-5]
    rows = json.load(open(f))
    vol[sym] = {r[0]: float(r[7]) for r in rows}
days = sorted({d for v in vol.values() for d in v if START - DAY <= d < CUT})
univ = {}; pool = set()
for d in days:
    sc = []
    for sym, v in vol.items():
        w = [v.get(d - k * DAY) for k in range(1, 31)]
        if any(x is None for x in w): continue
        if v.get(d - DAY, 0) <= 0: continue           # dun islem gormemis (delist) -> disari
        sc.append((sum(w), sym))
    top = [s for _, s in sorted(sc, reverse=True)[:N]]
    univ[d] = top; pool.update(top)
json.dump(sorted(pool), open(os.path.join(D, "pool.json"), "w"))
json.dump(univ, open(os.path.join(D, "univ.json"), "w"))
print("gun:", len(days), "pool:", len(pool))
cnt = {}
for t in univ.values():
    for s in t: cnt[s] = cnt.get(s, 0) + 1
print("evrende en az gun gecirenler (ilk 15 / son 15):")
srt = sorted(cnt.items(), key=lambda x: -x[1]); print(srt[:15]); print(srt[-15:])
