# KUSAK-INFRA

[![DOI](https://zenodo.org/badge/DOI/10.5281/zenodo.24562190.svg)](https://doi.org/10.5281/zenodo.24562190)

> **11-month pre-registered quantitative research & low-latency C++20 execution engine archive (Dec 2025 – Nov 2026).** Evaluates 504 pre-registered hypothesis cells on Binance Spot / USDⓈ-M Futures with a lock-free SPSC, fail-closed engine — finding 0 net-positive candidates under retail VIP 0 costs (`DOI: 10.5281/zenodo.24562190`).

C++20 / Linux / x86-64 üzerinde, tek sembollü (Binance SPOT veya USDⓈ-M FUTURES) bir emir yürütme motoru ve bu motorla 11 ay boyunca yürütülen, **504 hipotez hücresini** kapsayan ön kayıtlı sinyal araştırmasının mühendislik ve araştırma arşivi.

---

> ## 📌 Proje durumu
>
> - **Tamamlandı ve arşivlendi.** Proje Aralık 2025 – Kasım 2026 arasında, yaklaşık 11 ay boyunca geliştirildi ve test edildi. Araştırma sonuçlarına dayanarak bilinçli bir kararla kapatıldı. Gerekçe: [Neden devam edilmedi?](#neden-devam-edilmedi)
> - **Test kapsamı:** Birim ve bellek içi sahte borsa testlerinin yanı sıra Binance Spot testnet, canlı piyasa verisi ve kontrollü gerçek sermaye senaryolarında yüzlerce saatlik çalıştırma ve gözlem.
> - **Araştırma sonucu:** 504 ön kayıtlı hipotez hücresinden hiçbiri bireysel VIP 0 maliyet yapısı altında net pozitif aday üretmedi.
> - **Güvenlik kararı:** Üç P0 bulgusunun (`KZ-P0-01/02/03`) kod düzeltmeleri yazıldı ve testlerden geçti. Testnet kapanış kapısı tamamlanmadan ve ortada net pozitif bir aday yokken motor kalıcı canlı riske alınmadı ([P0 durumu](#p0-durumu)).
>
> **Kullanım notu:** Depo bir mühendislik ve araştırma referansı olarak paylaşılmıştır; canlı işlem için hazır bir ürün değildir. `TESTNET=false` ortam değişkeni config'i ezerek motoru mainnet'e yönlendirir ve `dry_run=false` + `testnet=false` birleşimini engelleyen bir kod kapısı yoktur (`src/main.cpp`). Yazılım MIT lisansıyla "olduğu gibi" sunulur.

---

<a id="kisa-ozet"></a>

## Kısa özet

- **Süre:** Aralık 2025 – Kasım 2026, yaklaşık 11 ay süren kesintisiz geliştirme, test ve araştırma.
- **Mühendislik:** C++20 / Linux x86-64 üzerinde tek sembollü bir Binance (Spot / USDⓈ-M Futures) emir yürütme motoru. Thread'ler arasında lock-free SPSC halkalar, 14 durumlu tablo güdümlü emir FSM'i ve fail-closed güvenlik katmanları (final order firewall, kill switch, başlangıç uzlaştırması) içerir. 11 test paketinde 492 kontrol, 0 hata.
- **Test ve gözlem:** Motor; farklı piyasa koşullarında testnet, canlı piyasa verisi ve kontrollü gerçek sermaye senaryolarında yüzlerce saat çalıştırıldı ve gözlemlendi. Bu süreçte bulunan hatalar (`C1`–`C20`) numaralı fazlar hâlinde düzeltildi.
- **Araştırma:** 8 çalışma ailesinde toplam **504** ön kayıtlı hipotez hücresi test edildi. Izgaralar, maliyet modelleri ve karar kuralları veriye bakılmadan önce sabitlendi; çoklu hücreli ızgaralarda Bonferroni veya Holm düzeltmesi uygulandı. **Aday sayısı: 0.**
- **Belirleyici bulgu:** 5 saniye ile 24 saat arası ufuklarda sinyallerin yakaladığı brüt hareket birkaç bps ile ~+20 bps arasında kaldı. Bu, bireysel VIP 0 hesabın 10–30 bps'lik gidiş-dönüş maliyetini ve maker girişteki ters seçilim duvarını aşmaya yetmedi. Net pozitif çıkan tek yol olan funding/baz taşıma ise bir sinyal avantajı değil, piyasa risk primidir.
- **Karar:** Net pozitif aday olmadan ve P0 kapanış kapısı tamamlanmadan canlı sermayeyi kalıcı olarak riske atmak hiçbir yeni bilgi üretmeyecekti. Proje bu mühendislik gerekçesiyle kapatıldı. Sonucun hangi koşullar için geçerli olduğu [Neyi kanıtlar, neyi kanıtlamaz?](#kanitlar) bölümünde tanımlanmıştır.

<a id="english-summary"></a>

## English summary and key findings

**KUSAK-INFRA** is an archived **C++20 algorithmic trading execution engine** for **Binance Spot / USD-M Futures**, together with the records of an **11-month pre-registered research program (Dec 2025 – Nov 2026) covering 504 hypothesis cells**. The engine trades a single symbol with a single open position.

**Engine design:**
- **lock-free SPSC ring buffers** between pinned threads
- a table-driven, 14-state order state machine with an explicit `UNKNOWN` state: a timeout never counts as "not sent"
- a **fail-closed order firewall**, a kill switch and startup reconciliation against the exchange
- 11 test suites, 492 checks, 0 failures (unit and in-memory fake-exchange level)

**Testing scope:** Over 11 months the engine went through hundreds of hours of runs and observation across different market conditions: Binance Spot testnet, live market data and controlled real-capital scenarios. Defects found along the way (`C1`–`C20`) were fixed in numbered remediation phases. Three P0 safety findings (`KZ-P0-01/02/03`) are fixed in code and pass their tests; the engine was deliberately not moved to permanent live risk before their testnet closing gate was completed.

**Key findings** from **pre-registered hypothesis testing** (grids, cost models and decision rules fixed before the data was examined; Bonferroni or Holm correction on multi-cell grids):

- **504 hypothesis cells, 0 candidates** after retail **Binance VIP 0 fees**.
- **Binance VIP 0 fee barrier.** At 5 s – 24 h horizons, the gross moves captured by the signals ranged from a few bps to about +20 bps, against 10–30 bps round-trip costs.
  - 5-second top-of-book OBI: +0.6 bps (BTCUSDT) and +1.4 bps (PENDLEUSDT) gross, against ~20 / ~30 bps cost.
- **Market microstructure and adverse selection.** A maker touch-fill scenario was negative on BTCUSDT even at zero fees, consistent with adverse selection on passive fills.
- **Out-of-sample failures and statistical power.**
  - H1: −34.3 bps net, p = 0.90.
  - H2: +6.0 bps net per trade, but p = 0.55 on the pre-registered unit, with ~18% statistical power.
  - Cross-sectional H3: best t = 0.96 across 66 cells, while ≈ 2.3 would be expected from chance alone.
  - Daily trend (240 cells): best t = 2.73, below both the chance-expected maximum (≈ 2.8) and the Holm threshold (≈ 3.5); t = 0.39 after 2023.
  - Liquidation / open-interest / aggressive-flow shocks (24 cells): 0 candidates.
- **The only net-positive path** was funding/basis carry: BTC about 3.5% per year over 12 months. This is a market risk premium, not a signal edge, and it needs two simultaneous legs, which is outside the single-symbol architecture.

**On retail crypto trading bot profitability:**
- **What this shows.** Under these conditions, none of the pre-registered signal families produced a statistically defensible net-positive result:
  - retail VIP 0 fees
  - a standard internet connection with no colocation
  - small capital
  - a single-symbol engine
  - 5 s – 24 h horizons
  - Binance data from 2023-11 to 2026-11
- **Scope boundary.** The result is specific to these constraints. Outside its scope:
  - institutional fee or rebate tiers
  - colocation
  - L2/L3 order-book and queue modelling
  - large multi-leg portfolios
  - weekly or monthly horizons
  - non-crypto markets
  - alternative data and machine learning

  In low-power tests, a "fail" verdict means "not proven", not "no effect".

---

## İçindekiler

1. [Kısa özet](#kisa-ozet)
2. [English summary and key findings](#english-summary)
3. [Geliştirme süreci ve test kapsamı](#surec)
4. [Mimari](#mimari)
5. [Güvenlik katmanları](#guvenlik)
6. [P0 durumu](#p0-durumu)
7. [Araştırma özeti: 504 hipotez](#arastirma)
8. [Neden devam edilmedi?](#neden-devam-edilmedi)
9. [Neyi kanıtlar, neyi kanıtlamaz?](#kanitlar)
10. [Derleme ve testler](#derleme)
11. [Depo düzeni](#depo)
12. [Sık sorulan teknik sorular (FAQ)](#faq)
13. [İletişim ve iş birliği](#iletisim)
14. [Atıf (Citation)](#atif)
15. [Lisans](#lisans)

---

<a id="surec"></a>

## Geliştirme süreci ve test kapsamı

Proje Aralık 2025 – Kasım 2026 arasında, yaklaşık 11 ay boyunca geliştirildi. Çalışma iki paralel hatta yürüdü:

| Hat | Kapsam |
|---|---|
| **Yürütme motoru** | Lock-free thread mimarisi, emir FSM'i, REST/WebSocket istemcileri, fail-closed güvenlik katmanları, başlangıç uzlaştırması ve P0 düzeltme fazları (Faz 0 … Faz 3.7). |
| **Sinyal araştırması** | 8 çalışma ailesinde 504 ön kayıtlı hipotez hücresi; 5 saniyeden 24 saate ve günlük ufka kadar uzanan testler; Binance spot ve USDⓈ-M, 2023-11 – 2026-11 verisi. |

Test ve gözlem katmanları:

| Katman | Ne doğrulandı |
|---|---|
| Birim testleri | FSM geçişleri, firewall, maruziyet limitleri, rate limit (429/418), TSC saati, VPIN, EXIT miktar hizalama |
| Bellek içi sahte borsa | Gerçek `src/main.cpp`, gerçek order thread ve gerçek `RestOrderClient` ile SEND/EXIT/POLL kimliği, hata eşlemesi, zaman aşımı ve kill switch davranışı |
| Binance Spot testnet | Gerçek borsa API'sine karşı emir yaşam döngüsü, ağ hatası ve belirsiz sonuç çözümü (`T0`, `T0b`) |
| Canlı piyasa ve gerçek sermaye | Farklı piyasa koşullarında canlı veriyle motor davranışı, gecikme ve veri seyrekliği gözlemi; kontrollü gerçek sermaye senaryoları |

Toplamda yüzlerce saatlik çalıştırma ve gözlem yapıldı. Bu süreçte bulunan ek hatalar `C1`–`C20` olarak numaralandırıldı ve düzeltildi; ayrıntılar `docs/03_SAFETY/P0_REMEDIATION_ROADMAP.md` içindedir.

<a id="mimari"></a>

## Mimari

Aşağıdaki bileşenler kaynak kodda bulunur. Satır numaraları bu arşiv anlık görüntüsüne aittir.

| Bileşen | Nerede | Not |
|---|---|---|
| Dil / platform | `CMakeLists.txt` | C++20, GCC/Clang, Linux x86-64. `__rdtscp`, `_mm_pause`, `pthread_setaffinity_np` kullanır. MSVC ile derlenmez. |
| Model | `include/strategy_core.hpp` | Tek sembol, aynı anda **tek pozisyon / tek giriş emri**. Giriş pasif `LIMIT_MAKER`, çıkış `MARKET`. |
| Sinyal | `strategy_core.hpp`, `vpin_velocity_tracker.hpp`, `regime_detector.hpp`, `risk_scorer.hpp` | WebSocket `bookTicker` + `aggTrade` → top-of-book OBI. VPIN, rejim ve risk skoru ile filtrelenir. |
| Thread'ler | `src/main.cpp:1395-1421` | Market, order ve telemetry thread'leri; CPU pinning (`main.cpp:257`). |
| Thread arası iletişim | `include/infra/spsc_ring.hpp` | Lock-free SPSC halka: `alignas(64)` head/tail, acquire/release sıralaması, dolu halkada drop sayacı. Her halkada tek üretici / tek tüketici. |
| Emir FSM'i | `include/engine/types.hpp:117-157` | 14 durumlu `OrderState`. Geçişler `ALLOWED[]` bit maskesi tablosuyla sınırlı; tabloda olmayan geçiş reddedilir. |
| Zaman | `include/infra/time.hpp` | TSC tabanlı ns saati. `tsc_to_ns` 128-bit ara çarpım kullanır (KZ-P0-01 düzeltmesi). |
| REST / imza | `binance_rest_client.hpp`, `binance_http_client.hpp`, `binance_signer.hpp` | libcurl + OpenSSL HMAC-SHA256. Emir POST'unda otomatik retry yok. |

<a id="guvenlik"></a>

## Güvenlik katmanları

Ayrıntı: `docs/03_SAFETY/SAFETY_POLICY.md`.

- **Fail-closed tasarım.** Doğrulanamayan, 0, NaN veya bilinmeyen değer reddedilir. Zaman aşımı "gönderilmedi" sayılmaz, emir `UNKNOWN` durumuna geçer. Belirsiz emir varken yeni risk alınmaz.
- **Final order firewall** (`strategy_core.hpp:740`). Her SEND görevinden önce limit, maruziyet ve pozisyon kontrolü yapar. EXIT görevleri ayrı bir dala (`exit_firewall`, `:696`) gider; bu dal emrin pozisyonu azalttığını doğrular. FUTURES'ta `reduceOnly` kullanılır.
- **Kill switch** (`strategy_core.hpp:205`, `:775-782`). Tetiklenince yeni emirleri kapatır ve açık emirleri iptal eder. Kayıp/belirsiz sonuç, istemci yokluğu ve dolu sonuç halkası durumlarında da tetiklenir.
- **Başlangıç uzlaştırması** (`order_guard.hpp` → `startup_decision`, `src/main.cpp:737-893`). Motor açılışta borsadaki açık emir ve bakiyeye bakar. Sonuç `CLEAN_START`, `ADOPT_POSITION`, `IGNORE_DUST`, `FLATTEN_REDUCE_ONLY` ya da `REFUSE_*` olur. Belirsizlikte emir gönderilmez. Kör MARKET SELL yoktur.
- **`order_guard.hpp`.** Bağımsız, saf mantık güvenlik kontrolleri: `max_position_usd`, maruziyet, 429/418 için `RateGate`.
- **Bellek içi sahte borsa entegrasyon testi** (`tools/sim_exchange_test.cpp`). Gerçek `src/main.cpp`'yi (`#define main kusak_main`), gerçek `run_order_thread`'i ve gerçek `RestOrderClient`'ı ağsız bir `FakeExchange` ile çalıştırır. SEND/EXIT/POLL kimliğini, hata eşlemesini, firewall/kill switch kapısını ve SUBMITTED zaman aşımını ölçer.

<a id="p0-durumu"></a>

## P0 durumu

Üç P0 bulgusunun tamamı için kod düzeltmesi yazıldı ve birim / sahte borsa testlerinden geçti. Resmî kapanış için ön kayıtlı testnet `dry_run=false` senaryoları (T1–T6) ve onay adımları tanımlandı; net pozitif bir strateji adayı olmadığından bu son kapı bilinçli olarak açık bırakıldı (`docs/03_SAFETY/P0_REMEDIATION_ROADMAP.md` §4).

| ID | Kod düzeltmesi | Birim testi | Sahte borsa | Testnet `dry_run=false` gözlemi | Durum |
|---|---|---|---|---|---|
| KZ-P0-01 TSC taşması (~4,29 sn'de başa sarma) | `time.hpp` 128-bit çarpım | `tsc_overflow_test` 22/0 | — | — | Düzeltildi, kapanış kapısı açık |
| KZ-P0-02 ilk POLL yerel ID ile | `build_poll_task`: borsa ID → `origClientOrderId` → POLL yok | `poll_id_test` 52/0 | `sim_exchange_test` 65/0 | kısmi (T0b) | Düzeltildi, kapanış kapısı açık |
| KZ-P0-03 EXIT firewall reddi | `exit_firewall`, seqlock'lu pozisyon, reduceOnly | `exit_firewall_test` 63/0 | `sim_exchange_test` 65/0 | — | Düzeltildi, kapanış kapısı açık |

Testnet gözlem koşuları (`T0`, `T0b`; 2026-11-24, PENDLEUSDT SPOT):

- **T0:** Tek bir ağ hatasından sonra emrin süresiz `UNKNOWN` durumunda kaldığı tespit edildi. Bu, `C19` bulgusuna yol açtı ve düzeltildi.
- **T0b:** 8 saniyelik zaman aşımı → belirsiz sonuç → 12 adet `-2013` → `NOTFOUND_RSLV` ile doğru biçimde çözüldü. Kill switch tasarlandığı gibi kilitli kaldı.
- İlk POLL'larda `-1021` saat kayması gözlendi.

`C1`–`C20`, P0 düzeltme çalışması sırasında bulunan ek hataların numaralarıdır. Örneğin `C19`, "ağ hatasından sonra emrin `UNKNOWN` durumunda takılı kalması" demektir. Bulgu listesi ve düzeltme geçmişi `docs/03_SAFETY/P0_REMEDIATION_ROADMAP.md` içindedir.

<a id="arastirma"></a>

## Araştırma özeti: 504 hipotez

Her deney sonuçlar görülmeden önce ön kayda yazıldı: hücre ızgarası, maliyet modeli ve karar kuralı baştan sabitlendi. Çoklu hücreli ızgaralarda Bonferroni veya Holm düzeltmesi kullanıldı; tek hipotezli testlerde (H1, H2) düzeltme gerekmez. Başarısız deneyler yeni parametrelerle "kurtarılmadı". Veriler Binance public API'sinden ve `data.binance.vision`'dan alındı.

| Deney | Soru | Hücre | Maliyet (gidiş-dönüş) | Sonuç |
|---|---|---|---|---|
| Kapı 1: 5 sn OBI | Top-of-book OBI'nin 5 saniyelik mid getirisi maliyeti geçiyor mu? | 28 | ~20 bps (BTC), ~30 bps (PENDLE) | Brüt BTC +0,6 bps, PENDLE +1,4 bps; EV_NET ≈ −20 / −30 bps. Maker ve sıfır ücret senaryoları da negatif. Fark bir mertebeden büyük olduğu için **kapatıldı**. |
| v14.2 kırılım | Ekstrem hareket sonrası MOM/REV; L ∈ {1, 5, 15} dk, H ∈ {1, 5, 10, 15} dk | 96 | 20 / 30 bps | 96 hücrenin tamamında eğitim net EV < 0; seçim kuralını geçen hücre yok. En iyisi PENDLE L15/H15 REV: brüt +23,4, net −6,6 bps, N=98. |
| H1 | PENDLE, L15 dk REV, H15 dk; tek test | 1 | 30 bps | Eğitim brüt +24,0 / net −6,0 bps. Test N=21, net −34,3 bps, p=0,90. **KALDI.** |
| 1-4 saat keşif | 6 varlık × L{4,24} × H{1,4} (saat) × MOM/REV | 48 | 20 bps | Yalnız 1 hücre net pozitif: +0,4 bps (gürültü düzeyi). |
| H2 | SOL+DOGE 4 saatlik REV, maker giriş, tek test | 1 | 12 bps | Kapanış modeli net +6,0 bps, p=0,55; maker modeli +2,0, p=0,67. 7 bps duyarlılıkta p=0,43. İstatistiksel güç ~%18. **KALDI.** |
| H3 kesitsel | Çoklu coin REV / MOM / funding (USDⓈ-M, dinamik ilk 35 coin, delist dahil; L 4-72 sa, H 4-24 sa) | 66 | 10 / 14 bps | İstikrarlı 0, zayıf 0. En iyi t = 0,96; şansla beklenen en büyük t ≈ 2,3. |
| Günlük trend | Günlük ufukta trend takibi ızgarası | 240 | bireysel hesap | En iyi t = 2,73; şansla beklenen en büyük t ≈ 2,8, Holm eşiği ≈ 3,5. 2023 sonrası t = 0,39. Aday yok. |
| Likidasyon / OI / agresif akış şokları | Likidasyon, açık pozisyon ve agresif emir şoklarından sonraki hareket | 24 | bireysel hesap | 24 hücrede aday yok. |
| **Toplam** | | **504** | **10–30 bps** | **Aday hücre: 0** |

**v14.2 ve H1 test dönemi:** v14.2 ızgarasında eğitimde hiçbir hücre seçim kuralını geçmediği için test dönemi (2026-07 … 2026-10) o ızgara için açılmadı. H1, eğitim gözleminden türetilen ve test öncesi ön kayda yazılan tek hipotezdi; test dönemini bir kez kullandı ve dönem bundan sonra yeniden kullanılmadı.

**Kayıtlar ve betikler:**
- Ön kayıt ve sonuç dokümanları: `docs/04_RESEARCH/` (`EDGE_PREREGISTRATION.md` → Kapı 1; `V142_BREAKOUT_PREREGISTRATION.md` → v14.2 ve H1; `H2_REVERSAL_PREREGISTRATION.md` → 1-4 saat keşif ve H2; `H3_CROSS_SECTIONAL_EXPLORATION.md` ve `h3_train_output.txt` → H3).
- Analiz betikleri: `python/` altında `obi_research.py` (+ `test_obi_research.py`), `gen_obi_csv.py`, `signal_analyzer.py` (Kapı 1 / OBI), `h2_reversal_test.py` (H2), `h3_fetch.py`, `h3_pool.py`, `h3_explore.py` (H3).
- Günlük trend ve şok çalışmalarının ayrıntıları [LinkedIn kapanış raporunda](#rapor) yer alır.

<a id="neden-devam-edilmedi"></a>

## Neden devam edilmedi?

Proje bir hata ya da kaynak yetersizliği yüzünden yarım kalmadı. 11 aylık ölçüm sürecinin sonunda **bilinçli bir mühendislik ve araştırma kararıyla** kapatıldı. Önceden yazılmış kurallarla test edilen 504 hücrenin hiçbiri, bireysel hesap koşullarında maliyet sonrası istatistiksel olarak savunulabilir bir avantaj göstermedi. Bu karar beş somut gerekçeye dayanır.

### 1. VIP 0 maliyet duvarı ve ters seçilim

- **5 sn OBI (Kapı 1):** Sinyalin yakaladığı brüt hareket BTCUSDT'de olay başına +0,6 bps, PENDLEUSDT'de +1,4 bps. Gidiş-dönüş maliyeti ise ~20 ve ~30 bps; net beklenti ≈ −20 / −30 bps (`EDGE_PREREGISTRATION.md` §11). İki ön kontrol, farkın bir mertebeden büyük olduğunu gösterdi; bu nedenle 3 haftalık resmî ölçüme gerek kalmadan çalışma kapatıldı.
- **Maker girişi ve sıfır ücret:** Fiyata dokunulduğunda emrin dolduğunu varsayan maker senaryosu BTCUSDT'de sıfır ücretle bile negatif çıktı. PENDLEUSDT'de pozitif çıkan tek senaryo spread yakalamaydı (market making) ve yalnız bacak başına ≤ ~1-5 bps ücretle mümkündü. Bu ücret seviyesi bireysel hesapta bulunmaz; motor da market making için tasarlanmadı. Tablo, ters seçilimle (adverse selection) tutarlıdır: pasif emir genellikle fiyat aleyhe döndüğünde dolar.
- **v14.2 kırılım:** BTC'de ekstrem hareketten sonraki brüt hareket her hücrede −3,7 … +3,7 bps aralığında kaldı; maliyet 20 bps. PENDLE'daki en iyi hücre brüt +23,4 bps yakaladı, ancak 30 bps maliyetle net −6,6 bps'e düştü.

### 2. Eğitim-test kopukluğu ve istatistiksel gürültü

- **H1:** Eğitimde brüt +24,0 bps görülen geri dönüş (net −6,0), dokunulmamış test döneminde brüt −4,3 / net −34,3 bps'e döndü (p=0,90).
- **H2:** Eğitimde net +8,07 bps (p=0,15). Testte işlem başı net +6,0 bps görüldü, ancak ön kayıtlı istatistik birimi olan 4 saatlik dilim ortalamasında p=0,55 çıktı; maliyet 7 bps'e indirildiğinde de p=0,43. En iyi ay çıkarıldığında maker modelinin neti −5,47 bps. Ön kayıtlı güç ~%18 olduğundan bu sonuç "kanıtlanamadı" anlamına gelir.
- **1-4 saat keşif:** 48 hücreden yalnız 1'i net pozitif çıktı (+0,4 bps); bu, 48 denemede şansla beklenecek düzeydedir.
- **H3 kesitsel:** 66 hücrede gözlenen en büyük t 0,96. Saf şansla beklenen en büyük t ≈ 2,3, Bonferroni eşiği t ≥ 3,17. Kısa ufuklu kesitsel REV, devir maliyeti nedeniyle anlamlı biçimde negatif çıktı.
- **Günlük trend:** 240 hücrede en iyi t = 2,73; bu değer şansla beklenen en büyük t'nin (≈ 2,8) ve Holm eşiğinin (≈ 3,5) altında kaldı. 2023 sonrası alt dönemde t = 0,39'a düştü.
- **H3 FUND, aritmetik ve bileşik getiri farkı:** En iyi FUND hücresinin aritmetik getirisi yıllık +%74 görünüyordu. Bileşik hesapta ise 24 ayda 1.000 USDT 733 USDT'ye indi ve en büyük düşüş %92 oldu (short sıkışması). Yüksek oynaklıkta aritmetik ortalama, gerçek sermaye eğrisini yanıltıcı biçimde iyimser gösterir.

### 3. Veri seyrekliği, zamanlama ve başabaş IC sınırı

- **Ufuk fiyatı ve veri seyrekliği:** Kapı 1'de ufuk fiyatı, vadeden **sonraki ilk** `bookTicker` tick'indeki mid'dir. `max_lag_us`, yedi ufuk içinde vade anı ile sonraki tick arasındaki en büyük süredir (`EDGE_PREREGISTRATION.md` §12 S3). Bu süre p50 olarak BTCUSDT'de 71 ms, PENDLEUSDT'de 1,05 saniye ölçüldü. Bu bir **motor gecikmesi değil**, piyasa verisinin seyrekliğidir: tick gelmeyen aralıkta top-of-book değişmez. Ön kayıtlı 2 ms filtre iki sembolde de olayların %100'ünü eledi; filtreyi gevşetmek ise ileri bakış ve seçim yanlılığı getirir.
- **Başabaş bilgi katsayısı (IC):** Getiriyle korelasyonu IC olan bir sinyalin işlem başı beklentisi yaklaşık IC · 0,8 · σ_H'dir; başabaş için gereken değer IC ≈ c / (0,8 · σ_H) olur. 4 saatlik ufukta σ BTC'de ~100, SOL'de ~176 bps; c = 20 bps ile gereken IC BTC'de 0,25, SOL'de 0,14. 5 saniyelik ufukta σ çok daha küçük olduğu için gereken IC pratikte ulaşılamaz düzeydedir.
- **Genel desen:** 5 saniye ile 24 saat arasındaki ufuklarda yakalanan brüt hareket birkaç bps ile ~+20 bps arasında; bireysel hesabın maliyeti ise 10-30 bps.

### 4. Net pozitif çıkan tek yol neden motorun avantajı değil?

Ölçümlerde net pozitif çıkan tek yol funding/baz taşımaydı (spot long + perp short, çift bacaklı). 12 aylık funding getirisi BTC'de yıllık ~%3,5 çıktı (ETH ~%2,3, LINK ~%4,7, SOL ~%−0,1). Bu getiri bir sinyalin öngörü gücünden değil, kaldıraçlı long talebinin ödediği bir **piyasa risk priminden** gelir. Negatif funding dönemleri, borsa ve karşı taraf riski taşır ve iki bacağın eşzamanlı yönetilmesini gerektirir; bu da tek sembollü, tek pozisyonlu motor mimarisinin dışında kalır. Aynı primi yönlü bir sinyale çevirmeye çalışan H3 FUND hücresi ise %92 düşüş verdi.

### 5. P0 kapıları tamamlanmadan canlı sermayeyi riske atmama kararı

Üç P0 bulgusu kodda düzeltildi ve testlerden geçti. Resmî kapanış için testnet senaryoları (T1–T6) ve onay adımları gerekir; bu da ek bir doğrulama döngüsü demektir. Elde çalıştırılacak net pozitif bir strateji adayı yokken bu maliyeti üstlenmek ve canlı emir yolunu kalıcı olarak açmak yeni bilgi üretmeyecek, yalnız risk üretecekti. Bu nedenle motor kalıcı canlı işleme geçirilmedi ve arşivlendi. Bu, güvenlik açısından kritik bir sistemde doğru sıralamadır: önce kanıtlanmış avantaj ve kapanmış güvenlik kapıları, sonra canlı risk.

<a id="kanitlar"></a>

## Neyi kanıtlar, neyi kanıtlamaz?

**Kanıtladığı:** Aşağıdaki koşullarda, ön kayıtlı sinyal ailelerinin hiçbiri maliyet sonrası net pozitif ve istatistiksel olarak savunulabilir bir sonuç vermedi:

- Bireysel VIP 0 ücretleri: spot bacak başına ~10 bps, USDⓈ-M maker/taker ~2/5 bps.
- Standart internet bağlantısı ve kişisel çalışma ortamı (WSL); borsaya fiziksel yakınlık (colocation) yok.
- Küçük sermaye: testnet config'inde 30 USDT, analizlerde 1.000 USDT varsayımı.
- Tek sembol / tek pozisyon yürütme motoru.
- Kısa ve orta ufuklar (5 saniye ile 24 saat arası) ve günlük trend ufku.
- Binance spot ve USDⓈ-M, 2023-11 ile 2026-11 arasındaki dönemlerin verisi.

**Kapsam sınırı:** Sonuç bu kısıtlara özgüdür; "kripto piyasalarında kısa vadeli avantaj her koşulda imkânsızdır" genellemesine varmaz. Şu koşullar araştırmanın kapsamı dışındadır:

- düşük ücret ya da rebate veren kurumsal ve market-maker programları
- colocation ve düşük gecikmeli bağlantı
- derin emir defteri (L2/L3) ve kuyruk sırası modellemesi
- büyük sermayeyle çok bacaklı, çok varlıklı portföyler
- haftalık ve aylık ufuklar
- kripto dışı piyasalar
- alternatif veri ve makine öğrenmesi modelleri

Düşük güçlü testlerde "KALDI" sonucu "etki yok" değil, "kanıtlanamadı" demektir (H2'nin gücü ~%18). Bu ayrım, sonuçları doğru okumanın ve ileride farklı kısıtlarla yeniden test etmenin temelidir.

<a id="rapor"></a>

**LinkedIn kapanış raporu:** "504 Hipotez, Sıfır Aday ve Bir Kapanış Kararı: KUSAK-INFRA Neden Sonlandırıldı?" (26 Kasım 2026): <https://lnkd.in/p/dbjayzZC>. Rapor [LinkedIn profilimde](https://www.linkedin.com/in/yunuskusak) de yer alıyor.

<a id="derleme"></a>

## Derleme ve testler

Gereksinim: Linux x86-64 (WSL2 Ubuntu dahil), GCC ≥ 13 veya Clang, CMake ≥ 3.16, OpenSSL, libcurl, nlohmann/json.

```bash
sudo apt install build-essential cmake libssl-dev libcurl4-openssl-dev nlohmann-json3-dev
```

### Birim ve sahte borsa testleri

Testler borsaya bağlanmaz ve API anahtarı istemez. `curl_sent_probe_test` yalnız `127.0.0.1` üzerinde kendi açtığı soketleri kullanır. Testler çalışma dizinine `kusak.log` yazar.

```bash
mkdir -p out
CXX="g++ -std=c++20 -O2 -Wall -Wextra -Iinclude"
LIBS="-lssl -lcrypto -lcurl -lpthread"

$CXX tools/tsc_overflow_test.cpp    -o out/tsc_overflow_test
$CXX tools/order_guard_test.cpp     -o out/order_guard_test
$CXX tools/vpin_spillover_test.cpp  -o out/vpin_spillover_test
$CXX tools/curl_sent_probe_test.cpp -o out/curl_sent_probe_test -lcurl -lpthread
$CXX phase2_fsm_test.cpp            -o out/phase2_fsm_test $LIBS
for t in poll_id exit_firewall phase35 phase36 phase37 sim_exchange; do
  $CXX tools/${t}_test.cpp -o out/${t}_test $LIBS
done

for b in out/*_test; do "$b" > "$b.txt" 2>&1; echo "$b -> exit $?"; done   # 0 = geçti
```

Derleme yaklaşık 1,5 dakika, testler yaklaşık 10 saniye sürer. Derleme 0 hata ve 39 uyarı verir:

- 28 × `-Wdeprecated-declarations`, `binance_http_client.hpp`
- 8 × `-Wunused-variable`, `infra/time.hpp`
- 3 × `-Wunused-but-set-variable`, `src/main.cpp`

Her testin çıktısı `out/<test>.txt` dosyasına yazılır, son satırı `SONUC fails=0` olmalıdır. Bazı test çıktılarında "mevcutta FAIL beklenir" ifadesi geçer; bu bir başarısızlık değil, düzeltme öncesi yazılmış senaryo açıklamasıdır.

Sonuçlar (2026-11-26, WSL2 Ubuntu 24.04, x86-64, g++ 13.3.0, yukarıdaki komutlarla; `ok/FAIL`):

| Test | Sonuç | Kapsam |
|---|---|---|
| `phase37_test` | 39/0 | C19a/b, C20: kanıtlı "gönderilmedi", `UNKNOWN` + `-2013` çözümü |
| `phase36_test` | 120/0 | C14-C17: MARKET EXIT, belirsiz SEND, kayıp SUBMITTED, halka dolu |
| `phase35_test` | 37/0 | C12, EXIT miktar hizalama, `floor_qty` |
| `sim_exchange_test` | 65/0 | Gerçek order thread + sahte borsa |
| `exit_firewall_test` | 63/0 | KZ-P0-03 |
| `poll_id_test` | 52/0 | KZ-P0-02 |
| `curl_sent_probe_test` | 11/0 | Gerçek libcurl ile "hiç bayt gönderilmedi" sinyali |
| `order_guard_test` | 63/0 | Faz 0 regresyonu: limitler, firewall, maruziyet, 429/418, soğuk başlangıç |
| `tsc_overflow_test` | 22/0 | KZ-P0-01 |
| `vpin_spillover_test` | 15/0 | VPIN bucket taşma hacmi |
| `phase2_fsm_test` | 5/0, GEÇTİ | FSM geçişleri |
| **Toplam** | **492/0** | **11 test paketi** |

Bu sonuçlar Faz 3.7'de kaydedilen sonuçlarla birebir aynıdır. P0 düzeltme çalışması numaralı fazlar hâlinde yürütüldü; Faz 3.7 bu serinin son aşamasıdır (2026-11-24; C19a, C19b ve C20 düzeltmeleri).

> **Notlar:**
> - Bu tablo birim ve bellek içi sahte borsa seviyesini kapsar. Testnet ve canlı piyasa gözlemleri [Geliştirme süreci ve test kapsamı](#surec) ile [P0 durumu](#p0-durumu) bölümlerindedir.
> - `phase36`, `phase37` ve `sim_exchange` testlerinde nlohmann/json başlıkları gerekir. Bu testler `nlohmann-json3-dev` (3.11.3) sistem paketiyle ek `-I` bayrağı olmadan derlendi. Paket yoksa `-I<yol>/nlohmann_json-src/include` ekleyin.
> - `tsc_overflow_test`, değişmez (invariant) TSC'li bir x86-64 CPU gerektirir.
> - `python/test_obi_research.py` numpy/scipy gerektirir ve yukarıdaki C++ test matrisinin dışındadır.

### Motor (CMake)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
./build/kusak_v13 kusak_config.json     # dry_run=true; anahtar alanları yer tutucu
```

- CMake yapılandırması nlohmann/json'u (v3.11.3) `FetchContent` ile GitHub'dan indirir; ilk `cmake` adımı için **ağ bağlantısı gerekir**. Sistemdeki `nlohmann-json3-dev` paketi CMake derlemesinde kullanılmaz.
- Son tam CMake derlemesi (Faz 3.7): 0 hata, 13 uyarı. Yukarıdaki 39 uyarı ayrı test derlemelerine aittir.
- `kusak_config.json` ve `kusak_config_scalping.json` dosyalarında `testnet=false` ayarlıdır. `dry_run` kapatılırsa bu config'ler doğrudan mainnet'e emir gönderir.
- `dry_run` modunda da config'te boş olmayan bir API anahtarı alanı zorunludur. Kod akışına göre bu modda imzalı istek gönderilmez; yine de bu alanlarda yer tutucu değer kullanılması önerilir.
- Market WebSocket her zaman prod uç noktasına bağlanır.
- `kusak_config_mvp.json` dosyasında `dry_run=false`, `testnet=true` ayarlıdır; testnet'e gerçek emir gönderir, genel kullanım için uygun değildir.
- `mvp_order_bridge_test` CMake hedefi testnet'e imzalı emir gönderir. `API_KEY`/`API_SECRET` yoksa ağ isteği yapmadan exit 2 ile çıkar.

Diğer CMake seçenekleri: `-DBUILD_RESEARCH=ON`, `-DBUILD_RESEARCH_RECORD=ON`, `-DBUILD_REPLAY_HARNESS=ON`.

<a id="depo"></a>

## Depo düzeni

| Yol | İçerik |
|---|---|
| `src/main.cpp` | Motor giriş noktası: order thread, başlangıç uzlaştırması, sıcak döngü, kapanış |
| `include/` | Header-ağırlıklı kaynak (`strategy_core.hpp`, `engine/`, `infra/`, `order_guard.hpp`, REST/WS istemcileri) |
| `src/research_main.cpp`, `src/app/research/` | Araştırma binary'si (record / replay / backtest) |
| `tools/` | Birim ve sahte borsa testleri, replay harness |
| `phase2_fsm_test.cpp` | FSM geçiş testi |
| `python/` | Kapı 1 (OBI), H2 ve H3 analiz betikleri |
| `docs/` | Proje, mimari, FSM, güvenlik, P0 yol haritası, araştırma ön kayıtları |
| `kusak_config*.json` | Örnek config'ler; anahtar alanları yer tutucudur |
| `.env.example` | Ortam değişkeni şablonu |
| `KULLANIM_KLAVUZU.md` | V10 dönemi kullanım kılavuzu; güncel davranış için kod esastır |
| `bench.cpp`, `bridge.cpp`, `test_asm.cpp`, `test_main.cpp`, `test_signer_minimal.cpp`, `vpin_audit.py` | Yardımcı deneme dosyaları; derleme ve test matrisinin parçası değildir |

Arşivi sade tutmak için şunlar dışarıda bırakıldı: yama geçmişi dosyaları (`*.patch`, `*.orig`, `*.rej`), eski derleme artefaktları, loglar, ham tick verisi, yardımcı analiz betikleri, geliştirmede kullanılan yapay zekâ kodlama asistanı talimat dosyaları (`CLAUDE.md`, `AGENTS.md`) ve oturum günlüğü (`PROGRESS.md`). `docs/` içindeki bazı referanslar bu dosyalara işaret eder.

<a id="faq"></a>

## Sık sorulan teknik sorular (FAQ)

**KUSAK-INFRA neden durduruldu, yarım mı bırakıldı?**
Hayır. 11 aylık geliştirme ve test sürecinin sonunda, ölçüm sonuçlarına dayanarak bilinçli olarak kapatıldı. 504 ön kayıtlı hipotez hücresinin hiçbiri bireysel maliyetler sonrasında net pozitif aday çıkarmadı. Örneğin 5 sn OBI sinyali BTCUSDT'de olay başına +0,6 bps brüt hareket yakaladı, maliyet ise ~20 bps idi. Strateji adayı olmadan P0 kapanış kapısını tamamlamanın ve canlı riske kalıcı geçmenin bir getirisi olmayacaktı ([Neden devam edilmedi?](#neden-devam-edilmedi)).

**Motor hangi ortamlarda test edildi?**
Birim testleri ve bellek içi sahte borsa (11 paket, 492 kontrol, 0 hata), Binance Spot testnet, canlı piyasa verisi ve kontrollü gerçek sermaye senaryoları. Farklı piyasa koşullarında toplam yüzlerce saatlik çalıştırma ve gözlem yapıldı ([Geliştirme süreci ve test kapsamı](#surec)).

**Bireysel kripto al-sat botları Binance VIP 0 komisyonlarını yenebilir mi?**
Bu çalışmanın tanımlı kısıtları altında cevap hayırdır: VIP 0 ücretleri (spot bacak başına ~10 bps), colocation olmadan, küçük sermayeyle, tek sembolle ve 5 sn–24 saat ile günlük ufuklarda, ön kayıtlı sinyal ailelerinin hiçbiri maliyet sonrası istatistiksel olarak savunulabilir bir sonuç vermedi; yakalanan brüt hareket birkaç bps ile ~+20 bps arasında kaldı. Kurumsal ücret ve rebate programları, colocation, haftalık/aylık ufuklar ve kripto dışı piyasalar araştırmanın kapsamı dışındadır ([Neyi kanıtlar, neyi kanıtlamaz?](#kanitlar)).

**Why was the KUSAK-INFRA C++20 trading engine archived instead of deployed live?**
After 11 months of development and hundreds of hours of testing across testnet, live market data and controlled real-capital scenarios, pre-registered research across 504 hypothesis cells found 0 net-positive candidates under retail VIP 0 costs. Three P0 safety findings (KZ-P0-01/02/03) are fixed in code and pass unit and in-memory fake-exchange tests. With no strategy worth running, completing the testnet closing gate (T1–T6) and committing permanent live capital would have added risk without adding information, so the project was archived ([P0 durumu](#p0-durumu)).

**Can this C++20 engine be used for live trading?**
It is published as an engineering and research reference, not as a production trading product. The P0 closing gate is open, a `TESTNET=false` environment variable overrides the config and points the engine at mainnet, and no code gate blocks the `dry_run=false` + `testnet=false` combination. Most importantly, no strategy was found to be profitable after retail costs.

<a id="iletisim"></a>

## İletişim ve iş birliği

Bu depo **salt-okunur bir arşivdir**. GitHub Issues ve Pull Requests kapalıdır; katkı kabul edilmez ve bakım yapılmaz.

Aşağıdaki konularda görüşmeye açığım:

- Bulgulara, ön kayıt tasarımına, maliyet modeline veya istatistiksel yönteme dair **teknik ya da metodolojik bir değerlendirmeniz** varsa,
- Farklı kısıtlar altında (ücret yapısı, altyapı, ufuk, varlık evreni) **doğrulanabilir bir çözüm veya yaklaşım** geliştirdiyseniz,
- Düşük gecikmeli C++, güvenlik açısından kritik gerçek zamanlı sistemler ya da ölçüm disiplini odaklı araştırma konularında **mühendislik veya araştırma iş birliği** yapmak istiyorsanız.

İletişim:

- LinkedIn: [linkedin.com/in/yunuskusak](https://www.linkedin.com/in/yunuskusak)
- Web: [yunuskusak.cloud](https://yunuskusak.cloud)
- E-posta: <kusakyunus54@gmail.com>

Değerlendirmenizi veri, kod veya ön kayıt üzerinden somutlaştırırsanız görüşmemiz daha verimli olur.

<a id="atif"></a>

## Atıf (Citation)

Bu arşive atıf yapmak için kalıcı Zenodo DOI kaydını kullanın: <https://doi.org/10.5281/zenodo.24562190>

Atıf meta verisi depo kökündeki `CITATION.cff` dosyasındadır. GitHub'daki **"Cite this repository"** düğmesi bu dosyadan APA ve BibTeX biçimlerini otomatik üretir.

<a id="lisans"></a>

## Lisans

MIT — `LICENSE`.
