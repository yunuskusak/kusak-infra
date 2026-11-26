# KUSAK-INFRA

C++20 / Linux / x86-64 üzerinde, tek sembollü (Binance SPOT veya USDⓈ-M FUTURES) bir emir yürütme motoru ve bu motorla yapılmış sinyal araştırmalarının kayıtları.

---

> ## ⚠️ Arşiv ve güvenlik uyarısı
>
> - **Proje sonlandırılmıştır.** Bakım yapılmayacak, yeni özellik eklenmeyecektir. Gerekçe: [Neden devam edilmedi?](#neden-devam-edilmedi)
> - **Gerçek sermayeyle hiç çalıştırılmamıştır.** Kayıtlara göre motor yalnız `dry_run=true` modunda (emir göndermeden) ve Binance Spot **testnet** üzerinde çalıştırıldı.
> - **Üç P0 güvenlik bulgusu AÇIK durumdadır:**
>   - `KZ-P0-01`: TSC saatinin yaklaşık 4,29 saniyede bir başa sarması
>   - `KZ-P0-02`: ilk POLL görevinin borsa ID'si yerine yerel ID ile gönderilmesi
>   - `KZ-P0-03`: azaltıcı EXIT emirlerinin final firewall tarafından reddedilebilmesi
>
>   Üçü için de kod düzeltmesi yazıldı ve birim testleri geçti. Kapanış kapısının testnet `dry_run=false` gözlemi ve onay adımları ise tamamlanmadı (`docs/03_SAFETY/P0_REMEDIATION_ROADMAP.md` §4).
> - Ayrıca bilinen ve giderilmemiş riskler var. Örnek: `TESTNET=false` ortam değişkeni config'i ezerek motoru mainnet'e yönlendirir, ve `dry_run=false` + `testnet=false` birleşimini engelleyen bir kod kapısı yoktur (`src/main.cpp`).
> - Araştırmalarda maliyet sonrası pozitif beklentili **hiçbir strateji bulunamadı** (aşağıda).
>
> **Bu kodu canlı işlemde, gerçek parayla veya gerçek anahtarlarla kullanmayın.** Depo yalnızca mühendislik ve araştırma arşivi olarak paylaşılmıştır. Yazılım MIT lisansıyla "olduğu gibi" sunulur; hiçbir garanti verilmez.

---

## Kısa özet: 11 ayın sonunda neden durduruldu?

- **Süre:** Aralık 2025 – Kasım 2026, yaklaşık 11 ay. Bu süre proje sahibinin beyanıdır; depodaki en eski tarihli kayıt 2026-03'e aittir.
- **Proje:** C++20 / Linux x86-64 üzerinde, tek sembollü bir Binance (Spot / USDⓈ-M Futures) emir yürütme motoru. Thread'ler arasında lock-free SPSC halkalar, fail-closed güvenlik katmanları (final order firewall, kill switch, başlangıç uzlaştırması) kullanır. Motorla yapılan ön kayıtlı sinyal araştırmalarının kayıtları da bu depodadır.
- **Ana bulgu:** Bu depoda kalıcı kaydı bulunan **240** ön kayıtlı hücrenin hiçbiri bireysel VIP 0 maliyetleri sonrasında kanıtlanabilir net pozitif aday çıkarmadı; aday sayısı **0**. LinkedIn kapanış raporu ek çalışmalarla birlikte toplam **504** hipotez hücresi sayar. Ek 264 hücrenin kaydı bu depoda yoktur ([240 ve 504](#6-sayı-şeffaflığı-240-ve-504)).
- **Tıkanma noktası:** 5 saniye ile 24 saat arası ufuklarda sinyallerin yakaladığı brüt hareket birkaç bps ile ~+20 bps arasında kaldı. Bu, 10-30 bps'lik gidiş-dönüş maliyetini ve maker girişteki ters seçilim duvarını aşamadı. Üç P0 güvenlik bulgusu açıkken canlı riske de geçilmedi.
- **Sınır:** Bu sonuç belirli kısıtlar altında "kanıt bulunamadı" demektir, "her koşulda imkânsız" demek değildir ([Neyi kanıtlar, neyi kanıtlamaz?](#bu-çalışma-neyi-kanıtlar-neyi-kanıtlamaz)).

## English summary and key findings

**KUSAK-INFRA** is an archived **C++20 algorithmic trading execution engine** for **Binance Spot / USD-M Futures**. It trades a single symbol with a single open position, and was developed from Dec 2025 to Nov 2026 (about 11 months, per the author). The repository also holds the records of the pre-registered signal research run with it.

Engine design:
- **lock-free SPSC ring buffers** between pinned threads
- a table-driven order state machine with an explicit `UNKNOWN` state: a timeout never counts as "not sent"
- a **fail-closed order firewall**, a kill switch and startup reconciliation against the exchange

The engine was never traded with real capital. Only `dry_run` and Binance Spot testnet runs were made, and **3 P0 safety findings remain open**.

Key findings from **pre-registered hypothesis testing**. Grids, cost models and decision rules were fixed before the data was examined, and Bonferroni correction was used for multi-cell grids:

- **240 cells documented in this repository, 0 candidates** after retail **Binance VIP 0 fees**. The author's LinkedIn closing report counts **504** in total. The additional 264 cells (daily trend: 240, liquidation / open interest / aggressive-flow shocks: 24) have no record here and cannot be verified from this repository.
- **Binance VIP 0 fee barrier.** At 5 s – 24 h horizons, the gross moves captured by the signals ranged from a few bps to about +20 bps. Round-trip cost assumptions were 10–30 bps.
  - 5-second top-of-book OBI: +0.6 bps (BTCUSDT) and +1.4 bps (PENDLEUSDT) gross, against ~20 / ~30 bps cost.
- **Market microstructure and adverse selection.** A maker touch-fill scenario was negative on BTCUSDT even at zero fees †. This is consistent with adverse selection on passive fills, but the mechanism itself was not measured separately.
- **Out-of-sample failures and low power.**
  - H1: −34.3 bps net, p = 0.90.
  - H2: +6.0 bps net per trade, but p = 0.55 on the pre-registered unit, with ~18% statistical power.
  - Cross-sectional H3: best t = 0.96 across 66 cells, while ≈ 2.3 would be expected from chance alone.
- **The only net-positive path** was funding/basis carry: BTC about 3.5% per year over 12 months †. This is a market risk premium, not a signal edge. It needs two simultaneous legs, which is outside the single-symbol architecture.

**On retail crypto trading bot profitability:**
- **What this proves.** Under these conditions, none of the pre-registered signal families produced a statistically defensible net-positive result:
  - retail VIP 0 fees
  - a standard internet connection with no colocation
  - small capital
  - a single-symbol engine
  - 5 s – 24 h horizons
  - Binance data from 2023-11 to 2026-11
- **What it does not prove.** It does not prove that short-horizon edge is impossible in general. Out of scope:
  - institutional fee or rebate tiers
  - colocation
  - L2/L3 order-book and queue modelling
  - large multi-leg portfolios
  - weekly or monthly horizons
  - non-crypto markets
  - alternative data and machine learning

  In low-power tests, "failed" means "not proven", not "no effect".

† Taken from the project's working log, which is not included in this archive; not verifiable from this repository.

---

## İçindekiler

1. [Kısa özet: 11 ayın sonunda neden durduruldu?](#kısa-özet-11-ayın-sonunda-neden-durduruldu)
2. [English summary and key findings](#english-summary-and-key-findings)
3. [Mimari](#mimari)
4. [Güvenlik katmanları](#güvenlik-katmanları)
5. [P0 durumu](#p0-durumu)
6. [Araştırma özeti](#araştırma-özeti)
7. [Neden devam edilmedi?](#neden-devam-edilmedi)
8. [Derleme ve testler](#derleme-ve-testler)
9. [Depo düzeni](#depo-düzeni)
10. [Sık sorulan teknik sorular (FAQ)](#sık-sorulan-teknik-sorular-faq)
11. [İletişim ve iş birliği](#iletisim)
12. [Lisans](#lisans)

---

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

## Güvenlik katmanları

Ayrıntı: `docs/03_SAFETY/SAFETY_POLICY.md`.

- **Fail-closed tasarım.** Doğrulanamayan, 0, NaN veya bilinmeyen değer reddedilir. Zaman aşımı "gönderilmedi" sayılmaz, emir `UNKNOWN` durumuna geçer. Belirsiz emir varken yeni risk alınmaz.
- **Final order firewall** (`strategy_core.hpp:740`). Her SEND görevinden önce limit, maruziyet ve pozisyon kontrolü yapar. EXIT görevleri ayrı bir dala (`exit_firewall`, `:696`) gider; bu dal emrin pozisyonu azalttığını doğrular. FUTURES'ta `reduceOnly` kullanılır.
- **Kill switch** (`strategy_core.hpp:205`, `:775-782`). Tetiklenince yeni emirleri kapatır ve açık emirleri iptal eder. Kayıp/belirsiz sonuç, istemci yokluğu ve dolu sonuç halkası durumlarında da tetiklenir.
- **Başlangıç uzlaştırması** (`order_guard.hpp` → `startup_decision`, `src/main.cpp:737-893`). Motor açılışta borsadaki açık emir ve bakiyeye bakar. Sonuç `CLEAN_START`, `ADOPT_POSITION`, `IGNORE_DUST`, `FLATTEN_REDUCE_ONLY` ya da `REFUSE_*` olur. Belirsizlikte emir gönderilmez. Kör MARKET SELL yoktur.
- **`order_guard.hpp`.** Bağımsız, saf mantık güvenlik kontrolleri: `max_position_usd`, maruziyet, 429/418 için `RateGate`.
- **Bellek içi sahte borsa entegrasyon testi** (`tools/sim_exchange_test.cpp`). Gerçek `src/main.cpp`'yi (`#define main kusak_main`), gerçek `run_order_thread`'i ve gerçek `RestOrderClient`'ı ağsız bir `FakeExchange` ile çalıştırır. SEND/EXIT/POLL kimliğini, hata eşlemesini, firewall/kill switch kapısını ve SUBMITTED zaman aşımını ölçer.

## P0 durumu

| ID | Kod düzeltmesi | Birim testi | Sahte borsa | Testnet `dry_run=false` gözlemi | Durum |
|---|---|---|---|---|---|
| KZ-P0-01 TSC taşması | `time.hpp` 128-bit çarpım | `tsc_overflow_test` 22/0 | — | yok | **AÇIK** |
| KZ-P0-02 ilk POLL yerel ID | `build_poll_task`: borsa ID → `origClientOrderId` → POLL yok | `poll_id_test` 52/0 | `sim_exchange_test` 65/0 | kısmi (T0b) | **AÇIK** |
| KZ-P0-03 EXIT firewall reddi | `exit_firewall`, seqlock'lu pozisyon, reduceOnly | `exit_firewall_test` 63/0 | `sim_exchange_test` 65/0 | yok | **AÇIK** |

Testnet gözlem koşuları (`T0`, `T0b`; 2026-11-24, PENDLEUSDT SPOT):

- **T0:** Tek bir ağ hatasından sonra emir süresiz `UNKNOWN` durumunda kaldı. Bu, C19 bulgusuna yol açtı ve düzeltildi.
- **T0b:** 8 saniyelik zaman aşımı → belirsiz sonuç → 12 adet `-2013` → `NOTFOUND_RSLV` ile çözüldü. Kill switch kilitli kaldı.
- İlk POLL'larda `-1021` saat kayması görüldü.

`C1`-`C20`, P0 düzeltme çalışması sırasında bulunan ek hataların numaralarıdır. Örneğin `C19`, "ağ hatasından sonra emrin `UNKNOWN` durumunda takılı kalması" demektir. Ön kayıtlı T1-T6 testnet senaryoları hiç çalıştırılmadı. Bulgu listesi ve düzeltme geçmişi `docs/03_SAFETY/P0_REMEDIATION_ROADMAP.md` içindedir.

## Araştırma özeti

Her deney sonuçlar görülmeden önce ön kayda (`docs/04_RESEARCH/`) yazıldı: hücre ızgarası, maliyet modeli ve karar kuralı baştan sabitlendi. Çoklu hücreli ızgaralarda Bonferroni düzeltmesi kullanıldı. Tek hipotezli testlerde (H1, H2) düzeltme gerekmedi. Kalan deneyler yeni parametrelerle "kurtarılmadı". Veriler Binance public API'sinden ve `data.binance.vision`'dan alındı. Ham veri bu depoda yoktur.

| Deney | Soru | Hücre | Maliyet (gidiş-dönüş) | Sonuç |
|---|---|---|---|---|
| Kapı 1: 5 sn OBI | Top-of-book OBI'nin 5 saniyelik mid getirisi maliyeti geçiyor mu | 28 (ön kayıtlı) | ~20 bps (BTC), ~30 bps (PENDLE) | 1 saatlik ön kontrolde brüt BTC +0,6 bps, PENDLE +1,4 bps; EV_NET ≈ −20 / −30 bps. Maker ve sıfır ücret senaryoları da negatif. 3 haftalık resmî ölçüme geçilmeden **kapatıldı**. |
| v14.2 kırılım | Ekstrem hareket sonrası MOM/REV; hareket penceresi L ∈ {1, 5, 15} dk, tutma süresi H ∈ {1, 5, 10, 15} dk | 96 | 20 / 30 bps | 96 hücrenin hepsinde eğitim net EV < 0. Seçilen hücre olmadığı için bu ızgara için **test seti açılmadı**. En iyisi PENDLE L15/H15 REV: brüt +23,4, net −6,6 bps, N=98. |
| H1 | PENDLE, L15 dk REV, H15 dk; tek test | 1 | 30 bps | Eğitim brüt +24,0 / net −6,0 bps. Test N=21, net −34,3 bps, p=0,90. **KALDI.** |
| 1-4 saat keşif | 6 varlık × L{4,24} × H{1,4} (saat) × MOM/REV | 48 | 20 bps | Yalnız 1 hücre net pozitif: +0,4 bps (gürültü düzeyi) †. |
| H2 | SOL+DOGE 4 saatlik REV, maker giriş, tek test | 1 | 12 bps | Kapanış modeli net +6,0 bps, p=0,55; maker modeli +2,0, p=0,67. 7 bps duyarlılıkta da p=0,43. Ön kayıtlı istatistiksel güç ~%18. **KALDI.** |
| H3 | Çoklu coin kesitsel REV / MOM / funding (USDⓈ-M, dinamik ilk 35 coin, delist dahil; L 4-72 saat, H 4-24 saat) | 66 | 10 / 14 bps | İstikrarlı 0, zayıf 0. En iyi t = 0,96, şansla beklenen en büyük t ≈ 2,3. |
| **Toplam** | | **240** | | **Aday hücre: 0** |

**v14.2 ve H1 test dönemi:** v14.2 ızgarasında eğitimde hiçbir hücre seçilmediği için test dönemi (2026-07 … 2026-10) o ızgara için açılmadı. H1 ise eğitim gözleminden sonradan türetilen ve test öncesi ön kayda yazılan tek hipotezdi. Aynı test dönemini bir kez kullandı; dönem bundan sonra bir daha kullanılmadı.

**† Kaynak notu:** Sayıların büyük çoğunluğu `docs/04_RESEARCH/` altındaki ön kayıt ve sonuç dokümanlarında kayıtlıdır. † ile işaretli sayılar ise yalnız projenin çalışma günlüğünde kayıtlıydı. Bu günlük arşive dahil edilmedi, dolayısıyla bu sayılar depodan doğrulanamaz.

**Betikler:** `python/` altında yalnız şu betikler var: `obi_research.py` (+ `test_obi_research.py`), `gen_obi_csv.py` ve `signal_analyzer.py` (Kapı 1 / OBI), `h2_reversal_test.py` (H2), `h3_fetch.py`, `h3_pool.py` ve `h3_explore.py` (H3). v14.2, H1 ve 1-4 saat keşif betikleri ile Kapı 1 maker senaryosu betiği bu arşivde **yer almıyor**. Bu deneylerin sonuçları depodan yeniden üretilemez; yalnız dokümanlardaki kayıtlara dayanır.

## Neden devam edilmedi?

Proje bir hata ya da kaynak yetersizliği yüzünden yarım kalmadı. Ölçümler sonucunda **bilinçli olarak** durduruldu. Önceden yazılmış kurallarla denenen ve bu depoda kaydı bulunan 240 hücrenin hiçbiri, bireysel hesap koşullarında maliyet sonrası kanıtlanabilir bir avantaj göstermedi. Aşağıdaki sayılar [Araştırma özeti](#araştırma-özeti) bölümündeki kayıtlardan gelir.

### 1. Maliyet ve ters seçilim duvarı

- **5 sn OBI (Kapı 1):** Sinyalin yakaladığı brüt hareket BTCUSDT'de olay başına +0,6 bps, PENDLEUSDT'de +1,4 bps. Gidiş-dönüş maliyeti ise ~20 ve ~30 bps. Net beklenti ≈ −20 / −30 bps (`EDGE_PREREGISTRATION.md` §11). Aradaki fark bir mertebeden büyük olduğu için ön kayıtlı 3 haftalık ölçüm hiç başlatılmadı.
- **Maker girişi ve sıfır ücret:** Fiyata dokunulduğunda emrin dolduğunu varsayan maker senaryosu BTCUSDT'de sıfır ücretle bile negatif çıktı †. PENDLEUSDT'de pozitif çıkan tek senaryo spread yakalamaydı (market making), o da yalnız bacak başına ≤ ~1-5 bps ücretle mümkündü †. Bu ücret seviyesi perakende hesapta yok, motor da market making için tasarlanmadı. Bu tablo ters seçilimle (adverse selection) tutarlı: pasif emir genellikle fiyat aleyhe döndüğünde dolar. Mekanizmanın kendisi ayrıca ölçülmedi.
- **v14.2 kırılım:** BTC'de ekstrem hareketten sonraki brüt hareket her hücrede −3,7 … +3,7 bps aralığında kaldı; maliyet 20 bps. PENDLE'daki en iyi hücre brüt +23,4 bps yakaladı ama 30 bps maliyetle net −6,6 bps'e düştü.

### 2. Eğitim-test kopukluğu ve istatistiksel gürültü

- **H1:** Eğitimde brüt +24,0 bps görülen geri dönüş (net zaten −6,0), dokunulmamış test döneminde brüt −4,3 / net −34,3 bps'e döndü (p=0,90).
- **H2:** Eğitimde bile anlamlı değildi (net +8,07 bps, p=0,15). Testte işlem başı net +6,0 bps görüldü. Ancak ön kayıtlı istatistik birimi olan 4 saatlik dilim ortalamasında p=0,55 çıktı. Maliyet 7 bps'e indirildiğinde de p=0,43. En iyi ay çıkarılınca maker modelinin neti −5,47 bps. Ön kayıtlı güç ~%18 olduğu için bu "KALDI" sonucu "etki yok" değil, "kanıtlanamadı" anlamına gelir.
- **1-4 saat keşif:** 48 hücreden yalnız 1'i net pozitif çıktı (+0,4 bps) †. Bu, 48 denemede şansla beklenecek düzeydedir.
- **H3 kesitsel:** 66 hücrede gözlenen en büyük t 0,96. Saf şansla beklenen en büyük t ≈ 2,3, Bonferroni eşiği t ≥ 3,17. Kısa ufuklu kesitsel REV, devir maliyeti yüzünden anlamlı biçimde negatif çıktı.
- **H3 FUND, aritmetik ve bileşik getiri farkı:** En iyi FUND hücresinin aritmetik getirisi yıllık +%74 görünüyordu. Bileşik hesapta ise 24 ayda 1.000 USDT 733 USDT'ye indi ve en büyük düşüş %92 oldu (short sıkışması). Yüksek oynaklıkta aritmetik ortalama, gerçek sermaye eğrisini yanıltıcı biçimde iyimser gösterir.

### 3. Veri seyrekliği, zamanlama ve başabaş IC sınırı

- **Ufuk fiyatı ve veri seyrekliği:** Kapı 1'de ufuk fiyatı, vadeden **sonraki ilk** `bookTicker` tick'indeki mid'dir. `max_lag_us`, yedi ufuk içinde vade anı ile sonraki tick arasındaki en büyük süredir (`EDGE_PREREGISTRATION.md` §12 S3). Bu süre p50 olarak BTCUSDT'de 71 ms, PENDLEUSDT'de 1,05 saniye ölçüldü †. Bu bir **motor gecikmesi değildir**. Piyasa verisinin seyrekliğini gösterir: tick gelmeyen aralıkta top-of-book değişmez. Ön kayıtlı 2 ms filtre iki sembolde de olayların %100'ünü attı. Filtreyi gevşetmek ise ileri bakış ve seçim yanlılığı getirir (aynı bölüm).
- **Başabaş bilgi katsayısı (IC):** Getiriyle korelasyonu IC olan bir sinyalin işlem başı beklentisi yaklaşık IC · 0,8 · σ_H'dir. Bu yüzden başabaş için gereken değer IC ≈ c / (0,8 · σ_H) olur. 4 saatlik ufukta σ BTC'de ~100, SOL'de ~176 bps †. c = 20 bps ile gereken IC BTC'de 0,25, SOL'de 0,14 †. 5 saniyelik ufukta σ çok daha küçük olduğu için gereken IC pratikte ulaşılamaz düzeydedir.
- Genel desen: 5 saniye ile 24 saat arasındaki ufuklarda sinyallerin yakaladığı brüt hareket birkaç bps ile ~+20 bps arasında. Bu depodaki maliyet varsayımları ise 10-30 bps.

### 4. Net pozitif çıkan tek yol neden motorun avantajı değil?

Ölçümlerde net pozitif çıkan tek yol funding/baz taşımaydı (spot long + perp short, çift bacaklı). 12 aylık funding getirisi BTC'de yıllık ~%3,5 çıktı (ETH ~%2,3, LINK ~%4,7, SOL ~%−0,1) †. Bu getiri bir sinyalin öngörü gücünden gelmez. Kaldıraçlı long talebinin ödediği bir **piyasa risk primidir**. Negatif funding dönemleri, borsa ve karşı taraf riski taşır. Ayrıca iki bacağın eşzamanlı yönetilmesini gerektirir. Bu da tek sembollü, tek pozisyonlu motor mimarisinin dışında kalır. Aynı primi yönlü bir sinyale çevirmeye çalışan H3 FUND hücresi ise %92 düşüş verdi.

### 5. Açık P0 riskini ve canlı test maliyetini üstlenmeme kararı

Üç P0 bulgusu kodda düzeltildi, ama testnet kapanış senaryoları (T1-T6) ve onay adımları tamamlanmadı. Bu kapıyı kapatmak ek testnet çalışması ve yeni bir doğrulama döngüsü gerektiriyor. Elde çalıştırılacak tek bir strateji adayı yokken bu maliyet ve canlı emir yolunun riski bilgi üretmeyecekti. Bu nedenle motor `dry_run=false` ile doğrulama aşamasına geçirilmedi ve arşivlendi.

### 6. Sayı şeffaflığı: 240 ve 504

- **Bu depoda kalıcı kaydı olan:** 240 hücre (28 + 96 + 1 + 48 + 1 + 66). Yukarıdaki bütün sonuçlar yalnız bu kümeye dayanır, ve bu küme tek başına sıfır aday verir.
- **LinkedIn kapanış raporundaki toplam:** 504 hipotez hücresi. Aradaki 264 hücre iki çalışmadan gelir: "günlük trend" (240 hücre) ve "likidasyon, açık pozisyon ve agresif emir şokları" (24 hücre). Bu iki çalışmanın ön kaydı, betiği, verisi ve çıktısı **bu depoda yoktur**. Raporda bunlar için verilen sayılar buradan doğrulanamaz: en iyi hücrede t = 2,73, şansla beklenen en büyük t ≈ 2,8, Holm eşiği ≈ 3,5, 2023 sonrası t = 0,39 ve 24 hücrede aday olmaması.
- **Düzeltme yöntemi:** LinkedIn raporu her çalışmada "Bonferroni veya Holm" uygulandığını söyler. Bu depodaki kayıtlarda Kapı 1, v14.2 ve H3 Bonferroni kullanır. H1 ve H2 tek hipotezli testlerdir ve düzeltme gerektirmez. Holm düzeltmesinin bu depoda kaydı yoktur.
- **Maliyet aralığı:** LinkedIn raporu bireysel hesap maliyet varsayımlarını 10-20 bps olarak verir. Bu depodaki ön kayıtlarda aralık 10-30 bps'tir (PENDLE 30, BTC 20, H2 12, H3 10/14).

### Bu çalışma neyi kanıtlar, neyi kanıtlamaz?

**Kanıtladığı (yalnız şu kısıtlar altında):** Aşağıdaki koşullarda, ön kayıtlı sinyal ailelerinin hiçbiri maliyet sonrası net pozitif ve istatistiksel olarak savunulabilir bir sonuç vermedi:

- Bireysel VIP 0 ücretleri: spot bacak başına ~10 bps, USDⓈ-M maker/taker ~2/5 bps. Hesaba özgü ücret oranı ayrıca doğrulanmadı.
- Standart internet bağlantısı ve kişisel çalışma ortamı (WSL). Borsaya fiziksel yakınlık (colocation) yok.
- Küçük sermaye: testnet config'inde 30 USDT, analizlerde 1.000 USDT varsayımı.
- Tek sembol / tek pozisyon yürütme motoru.
- Kısa ve orta ufuklar (5 saniye ile 24 saat arası).
- Binance spot ve USDⓈ-M, 2023-11 ile 2026-11 arasındaki dönemlerin verisi.

**Kanıtlamadığı:**

- "Kripto piyasalarında kısa vadeli avantaj her koşulda imkânsızdır" sonucunu **kanıtlamaz**. Şu koşullar kapsam dışında kaldı:
  - düşük ücret ya da rebate veren kurumsal ve market-maker programları
  - colocation ve düşük gecikmeli bağlantı
  - derin emir defteri (L2/L3) ve kuyruk sırası modellemesi
  - büyük sermayeyle çok bacaklı, çok varlıklı portföyler
  - haftalık ve aylık ufuklar
  - kripto dışı piyasalar
  - alternatif veri ve makine öğrenmesi modelleri
- Düşük güçlü testlerde "KALDI", "etki yok" değil "kanıtlanamadı" demektir (H2'nin gücü ~%18).
- Kapı 1'in kapanışı 1 saatlik iki ön kontrole dayanır. Ön kayıtlı 3 haftalık ölçüm yapılmadı.
- Motorun canlı ortamda doğru ve güvenli çalıştığını kanıtlamaz. Üç P0 bulgusu açıktır.

**LinkedIn kapanış raporu:** "504 Hipotez, Sıfır Aday ve Bir Kapanış Kararı: KUSAK-INFRA Neden Sonlandırıldı?" (26 Kasım 2026): <https://lnkd.in/p/dbjayzZC>. Rapor [LinkedIn profilimde](https://www.linkedin.com/in/yunuskusak) de yer alıyor. Rapordaki ve bu depodaki sayılar arasındaki farklar yukarıda, [6. maddede](#6-sayı-şeffaflığı-240-ve-504) açıkça listelenmiştir.

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

Her testin çıktısı `out/<test>.txt` dosyasına yazılır, son satırı `SONUC fails=0` olmalıdır. Bazı test çıktılarında "mevcutta FAIL beklenir" ifadesi geçer. Bu bir başarısızlık değil, düzeltme öncesi yazılmış senaryo açıklamasıdır.

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

Bu sonuçlar Faz 3.7'de kaydedilen sonuçlarla aynıdır. P0 düzeltme çalışması numaralı fazlar hâlinde yürütüldü. Faz 3.7 bu serinin son aşamasıdır (2026-11-24; C19a, C19b ve C20 düzeltmeleri).

> **Notlar:**
> - Bu testler birim ve bellek içi sahte borsa seviyesindedir. Testnet veya gerçek piyasa davranışını kanıtlamaz; P0'lar bu yüzden AÇIK kalır.
> - `phase36`, `phase37` ve `sim_exchange` testlerinde nlohmann/json başlıkları gerekir. Bu testler `nlohmann-json3-dev` (3.11.3) sistem paketiyle ek `-I` bayrağı olmadan derlendi. Paket yoksa `-I<yol>/nlohmann_json-src/include` ekleyin.
> - `tsc_overflow_test`, değişmez (invariant) TSC'li bir x86-64 CPU gerektirir.
> - `python/test_obi_research.py` (numpy/scipy) hiç çalıştırılmadı.

### Motor (CMake)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
./build/kusak_v13 kusak_config.json     # dry_run=true; anahtar alanları yer tutucu
```

- CMake yapılandırması nlohmann/json'u (v3.11.3) `FetchContent` ile GitHub'dan indirir. İlk `cmake` adımı için **ağ bağlantısı gerekir**. Sistemdeki `nlohmann-json3-dev` paketi CMake derlemesinde kullanılmaz.
- Son tam CMake derlemesi: 0 hata, 13 uyarı. Bu sayı Faz 3.7 kaydıdır. CMake derlemesi bu arşiv hazırlanırken yeniden çalıştırılmadı; yukarıdaki 39 uyarı ayrı test derlemelerine aittir.
- `kusak_config.json` ve `kusak_config_scalping.json` dosyalarında `testnet=false` ayarlıdır. `dry_run` kapatılırsa bu config'ler doğrudan mainnet'e emir gönderir.
- `dry_run` modunda bile config'te boş olmayan bir API anahtarı alanı zorunludur. Kod okumasına göre bu modda imzalı istek gönderilmez, ancak bu ayrıca test edilmedi.
- Market WebSocket her zaman prod uç noktasına bağlanır.
- `kusak_config_mvp.json` dosyasında `dry_run=false`, `testnet=true` ayarlıdır. Bu dosyayı **kullanmayın**.
- `mvp_order_bridge_test` CMake hedefi testnet'e imzalı emir gönderir. `API_KEY`/`API_SECRET` yoksa ağ isteği yapmadan exit 2 ile çıkar.

Diğer CMake seçenekleri: `-DBUILD_RESEARCH=ON`, `-DBUILD_RESEARCH_RECORD=ON`, `-DBUILD_REPLAY_HARNESS=ON`.

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
| `KULLANIM_KLAVUZU.md` | Eski (V10 dönemi) kullanım kılavuzu; güncel davranış için kod esastır |
| `bench.cpp`, `bridge.cpp`, `test_asm.cpp`, `test_main.cpp`, `test_signer_minimal.cpp`, `vpin_audit.py` | Eski / yardımcı deneme dosyaları; derleme ve testlerin parçası değildir |

Bu arşive dahil edilmeyenler: yama geçmişi dosyaları (`*.patch`, `*.orig`, `*.rej`), eski derleme artefaktları, loglar, ham tick verisi, depo dışı analiz betikleri, geliştirmede kullanılan yapay zekâ kodlama asistanı talimat dosyaları (`CLAUDE.md`, `AGENTS.md`) ve oturum günlüğü (`PROGRESS.md`). `docs/` içindeki bazı referanslar bu dosyalara işaret eder.

## Sık sorulan teknik sorular (FAQ)

**KUSAK-INFRA neden durduruldu, yarım mı bırakıldı?**
Hayır, yarım bırakılmadı; ölçümler sonucunda bilinçli olarak kapatıldı. Bu depoda kaydı bulunan 240 ön kayıtlı hücrenin hiçbiri bireysel maliyetler sonrasında net pozitif aday çıkarmadı. Örneğin 5 sn OBI sinyali BTCUSDT'de olay başına +0,6 bps brüt hareket yakaladı, maliyet ise ~20 bps idi. Strateji adayı olmadığı için açık üç P0 bulgusunu testnet üzerinde kapatmanın maliyeti üstlenilmedi ([Neden devam edilmedi?](#neden-devam-edilmedi), `docs/04_RESEARCH/`).

**Bireysel kripto al-sat botları Binance VIP 0 komisyonlarını yenebilir mi?**
Bu çalışma bu soruya genel bir cevap vermez. Gösterdiği şey şu: VIP 0 ücretleri (spot bacak başına ~10 bps), colocation olmadan, küçük sermayeyle, tek sembolle ve 5 sn–24 saat ufuklarında, ön kayıtlı sinyal ailelerinin hiçbiri maliyet sonrası istatistiksel olarak savunulabilir bir sonuç vermedi; yakalanan brüt hareket birkaç bps ile ~+20 bps arasında kaldı. Kurumsal ücret ve rebate programları, colocation, haftalık/aylık ufuklar ve kripto dışı piyasalar kapsam dışıdır. Ayrıca düşük güçlü testlerde "KALDI" sonucu "etki yok" değil, "kanıtlanamadı" anlamına gelir (H2'nin gücü ~%18) ([Neyi kanıtlar, neyi kanıtlamaz?](#bu-çalışma-neyi-kanıtlar-neyi-kanıtlamaz)).

**Why was the KUSAK-INFRA C++20 trading engine archived instead of deployed live?**
Pre-registered research found 0 net-positive candidates among the 240 cells documented in this repository; the author's LinkedIn report counts 504 in total, and 264 of those are not documented here. Three P0 safety findings (KZ-P0-01/02/03) are fixed in code and pass unit and in-memory fake-exchange tests. Their testnet closing scenarios (T1–T6) and sign-off were never completed. With no strategy to run, paying for that extra validation and accepting live order-path risk would not have produced new information, so the project was archived ([P0 durumu](#p0-durumu)).

**Can this C++20 engine be used for live trading?**
No. Three P0 safety findings remain open. A `TESTNET=false` environment variable overrides the config and points the engine at mainnet, and no code gate blocks the `dry_run=false` + `testnet=false` combination. No strategy has been shown to be profitable after costs. The repository is published only as an engineering and research reference.

<a id="iletisim"></a>

## İletişim ve iş birliği

Bu depo **salt-okunur bir arşivdir**. GitHub Issues ve Pull Requests kapalıdır; katkı kabul edilmez ve bakım yapılmaz.

Yine de aşağıdaki konularda görüşmeye açığım:

- Bulgulara, ön kayıt tasarımına, maliyet modeline veya istatistiksel yönteme **teknik ya da metodolojik bir itirazınız** varsa,
- Farklı kısıtlar altında (ücret yapısı, altyapı, ufuk, varlık evreni) **doğrulanabilir bir çözüm veya yaklaşım** bulduysanız,
- Düşük gecikmeli C++, güvenlik açısından kritik gerçek zamanlı sistemler ya da ölçüm disiplini odaklı araştırma konularında **mühendislik veya araştırma iş birliği** yapmak istiyorsanız.

İletişim:

- LinkedIn: [linkedin.com/in/yunuskusak](https://www.linkedin.com/in/yunuskusak)
- Web: [yunuskusak.cloud](https://yunuskusak.cloud)
- E-posta: <yunuskusak54@gmail.com>

İtirazınızı veri, kod veya ön kayıt üzerinden somutlaştırırsanız değerlendirmesi kolaylaşır.

## Lisans

MIT — `LICENSE`.
