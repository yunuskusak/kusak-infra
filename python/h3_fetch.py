# H3 veri cekimi. TEST DONEMI INDIRILMEZ: tum istekler endTime <= CUT.
import json, sys, os, time, urllib.request, urllib.parse
EXI, OUT = sys.argv[1], sys.argv[2]
CUT = 1764057600000            # 2025-11-25 08:00 UTC (H2 ile ayni kesim)
START = CUT - 731 * 86400000   # ~24 ay egitim
WARM = 40 * 86400000           # bakis/hacim isinmasi icin onceki 40 gun
FA = "https://fapi.binance.com"
STABLE = {"USDC","FDUSD","TUSD","BUSD","USDP","DAI","USDE","USD1","PYUSD","RLUSD","U","EUR","XUSD","USDD","FRAX","GUSD","LUSD","BFUSD","AEUR","USDT","PAXG","XAUT","USDS"}
def get(path, params):
    url = FA + path + "?" + urllib.parse.urlencode(params)
    for i in range(5):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "research-client"}), timeout=25) as r:
                return json.loads(r.read())
        except Exception as e:
            print("retry", path, params.get("symbol"), e, file=sys.stderr); time.sleep(3 + 3 * i)
    raise SystemExit("fail " + url)
ex = json.load(open(EXI))["symbols"]
cand = [s for s in ex if s["contractType"] == "PERPETUAL" and s["quoteAsset"] == "USDT" and s.get("underlyingType") == "COIN"
        and s["status"] in ("TRADING", "SETTLING") and s["baseAsset"] not in STABLE and s["onboardDate"] < CUT - 30 * 86400000]
print("aday:", len(cand))
step = sys.argv[3]
if step == "d1":
    for s in cand:
        fn = os.path.join(OUT, "d1", s["symbol"] + ".json")
        if os.path.exists(fn): continue
        rows = get("/fapi/v1/klines", {"symbol": s["symbol"], "interval": "1d", "startTime": START - WARM, "endTime": CUT - 1, "limit": 1000})
        rows = [r for r in rows if r[6] < CUT]
        json.dump(rows, open(fn, "w")); time.sleep(0.12)
elif step == "pool":
    pool = json.load(open(os.path.join(OUT, "pool.json")))
    for sym in pool:
        fk = os.path.join(OUT, "k4", sym + ".json")
        if not os.path.exists(fk):
            rows = []; t = START - WARM
            while t < CUT:
                b = get("/fapi/v1/klines", {"symbol": sym, "interval": "4h", "startTime": t, "endTime": CUT - 1, "limit": 1500})
                if not b: break
                rows += b; t = b[-1][0] + 1
                if len(b) < 1500: break
                time.sleep(0.2)
            rows = [r for r in rows if r[6] < CUT]
            json.dump(rows, open(fk, "w"))
        ff = os.path.join(OUT, "fund", sym + ".json")
        if not os.path.exists(ff):
            rows = []; t = START - WARM
            while t < CUT:
                b = get("/fapi/v1/fundingRate", {"symbol": sym, "startTime": t, "endTime": CUT - 1, "limit": 1000})
                if not b: break
                rows += b; t = b[-1]["fundingTime"] + 1
                if len(b) < 1000: break
                time.sleep(0.3)
            rows = [r for r in rows if r["fundingTime"] < CUT]
            json.dump(rows, open(ff, "w"))
        time.sleep(0.15)
    print("pool indirildi:", len(pool))
