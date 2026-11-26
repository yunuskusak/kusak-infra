# EDGE ÖN KAYDI — Kapı 1: OBI sinyalinin maliyet sonrası avantajı

> **DURUM: KAPATILDI (2026-11-24, proje sahibi kararı).** 21 günlük asıl veri toplama hiç başlamadı. Karar iki adet 1 saatlik WSL ön kontrolüne dayanıyor (BTCUSDT ve PENDLEUSDT). Bu koşular karar verisi değildi; ancak brüt hareketle maliyet arasındaki fark (5 s ufukta brüt +0,6 / +1,4 bps, maliyet ~20-30 bps) bu ölçütlerle geçişi çok olasılıksız kıldı [ÇIKARIM]. Ayrıntı §11 ve `PROGRESS.md`. Ölçütler sonradan değiştirilmedi. Bu doküman tarihsel kayıt olarak korunur.

> **Ön kayıt tarihi: 2026-11-24, veri toplama henüz başlamadı.** Son düzeltme: 2026-11-24, bağımsız denetim sonrası (§11), veri toplama başlamadan önce.
> Bu doküman veri toplanmadan önce yazılır ve yayınlanır. Sonuçlar yalnız buradaki ölçütlere göre değerlendirilir.
> Etiketler: **[KOD]** koddan doğrulandı (dosya:satır) · **[ÇIKARIM]** kod okumasından çıkarım · **[DOĞRULANMADI]** kanıt yok.
> Satır numaraları 2026-11-24 kaynak anlık görüntüsüne aittir.
> §12'deki **ENGELLEYİCİ** açık sorular kapanmadan veri toplama başlamaz.

> **ENGELLEYİCİ AÇIK SORULAR — güncel liste (2026-11-24, bağımsız denetim sonrası; ayrıntı §12)**
>
> | ID | Konu | Karar sahibi |
> |---|---|---|
> | S1 | Eşik ızgarası: tek eşik (0.35, M=28) mi, ek süreçler mi | Proje sahibi |
> | S3 | `--max-lag-us 2000` filtresinin atma oranı ve seçim yanlılığı; karar kuralı | Proje sahibi |
> | S6 | Dışlama / hafta ayırma aracı ve sha256'sı | Proje sahibi |
> | S7 | Örtüşen pencereler (otokorelasyon) için düzeltme yöntemi ve aracı | Proje sahibi |
> | S9 | EC2 instance tipi, vCPU sayısı ve süreç yerleşimi (maliyet kararı) | Proje sahibi |
> | S10 | TSC kararlılık kontrolünün eşiği ve başarısızlıkta yapılacak | Proje sahibi |
> | S11 | "> 60 s olay boşluğu" dışlamasının dayanağı (olay boşluğu mu, süreç/WS sağlığı mı) | Proje sahibi |
>
> **S2** 2026-11-24'te yeniden değerlendirildi ve **engelleyici olmaktan çıkarılması önerildi** (gerekçe §12). Proje sahibi itiraz ederse yeniden engelleyici olur.

## 1. Amaç ve hipotez

Soru: OBI sinyalinin yakaladığı fiyat hareketi, gidiş-dönüş komisyon ve spread maliyetini aşıyor mu?

- **H0:** Her (eşik × ufuk × yön × sembol) hücresinde maliyet sonrası ortalama mid getirisi ≤ 0'dır.
- **H1:** En az bir hücrede maliyet sonrası ortalama mid getirisi > 0'dır. Bu sonuç §7 ölçütleriyle, çoklu test düzeltmesinden sonra ve haftalar arasında tutarlı olmalıdır.

Kapı 1 **gerekli ama yeterli değildir**. Burada yalnız **mid fiyat** getirisi ölçülür. `LIMIT_MAKER` dolum olasılığı ve ters seçim (fiyat lehe giderse emir dolmaz, aleyhe giderse dolar) **Kapı 2'de** ölçülecektir. Kapı 1'i geçmek kârlılık kanıtı değildir.

### 1.1 Kapı tanımları ve yol haritası ilişkisi

| Ad | Tanım | `dry_run` |
|---|---|---|
| **Kapı 1** (bu doküman) | Filtresiz OBI sinyalinin sabit ufuklu mid getirisi, maliyet modelinden sonra pozitif mi | `true` |
| **Kapı 2** | Gerçek `LIMIT_MAKER` dolum oranı, ters seçim ve kapasite ölçümü. Önce testnet'te, sonra proje sahibi onayıyla küçük tutarla gerçek piyasada | `false` |
| **Aşama 5B** | Kapı 1 veya Kapı 2 geçilmezse projenin portföy / altyapı / eğitim değerine dönüştürülmesi | — |

Kaynak: proje sahibinin 2026-11-24 denetim talimatı. Bu tanımlar `PROJECT_OVERVIEW.md`'de henüz yazılı değildir [DOĞRULANMADI: o dokümanda "Kapı" veya "5B" geçmiyor, grep].

**v14.0 sırasıyla ilişki:**

- Kapı 1 `dry_run=true` ile yürür, borsaya emir göndermez ve kaynak kodu değiştirmez. Bu yüzden P0 kapanışını (CLAUDE.md §2) beklemeden **paralel** yürüyebilir. İstisna: KZ-P0-01 düzeltmesi ufuk ölçümünü doğrudan etkiler; kullanılan binary'nin düzeltmeyi içerdiği kanıtlanmalıdır (§13).
- Kapı 2 `dry_run=false` gerektirir. CLAUDE.md §2 kısıt 1 gereği **üç P0 kapanmadan Kapı 2 başlamaz** (testnet dahil).
- Kapı 1'i geçmek yol haritası sırasını (v14.0 → v14.1 → v14.2, CLAUDE.md §3.5) değiştirmez.

## 2. Kapsam ve sınırlar

- Yalnız `dry_run=true` kullanılır, borsaya emir gönderilmez. Semboller: **PENDLEUSDT** ve **BTCUSDT**, SPOT.
- Ölçülen şey sabit ufuklu mid getirisidir. Motorun gerçek çıkış mantığı (stop, trail, momentum) ölçülmez [ÇIKARIM].
- Sinyal **filtresizdir**. Örnekleme `process_book_ticker` içinde yapılır: önce `dynamic_momentum_exit` çalışır [KOD `strategy_core.hpp:2309`], sonra kitap güncellenir [KOD `:2316-2322`], sonra örnekleme yapılır [KOD `:2327-2328`]. Pozisyon, cooldown, VPIN, risk ve kill switch filtreleri örneklemeden sonra gelir. `dynamic_momentum_exit` OBI izleyicisine dokunmaz; `obi_tracker` yalnız `:2294`, `:2298` ve `:2327`'de kullanılır [KOD grep]. Bu yüzden örneklemeyi etkilemez. `:2278` yorumundaki "başında" ifadesi bu açıdan kesin değildir. Sonuç, motorun gerçekte işlem açacağı anlara koşullu değildir.
- **SPOT'ta SAT yönü [ÇIKARIM]:** SAT yönlü giriş (`LIMIT_MAKER SELL`) baz varlık envanteri gerektirir. PROGRESS T0 kaydında (B5) 19,1 PENDLE'lık SELL giriş sinyali, hesapta yalnız 2 PENDLE olduğu için borsada reddedilecek durumdaydı. Kapı 1 SAT hücreleri mid getirisini ölçer ve envanter varsaymaz. Bir SAT hücresinin GEÇMESİ, SPOT'ta işlenebilir olduğu anlamına gelmez. Kapı 2'de işlenebilmesi için önceden tutulan envanter ve bu envanterin fiyat riski gerekir. Kapı 2 yorumu ve tasarımı bunu ayrıca ele almalıdır. Kapı 1 ölçütleri bu nottan etkilenmez.
- Kapsam dışı: yeni indikatör, parametre taraması, ML, çoklu sembol stratejisi (CLAUDE.md §3.5).

## 3. Veri toplama

| Konu | Karar |
|---|---|
| Karar verisi | AWS Tokyo (ap-northeast-1) EC2, **en az 21 gün (3 geçerli hafta)** sürekli kayıt |
| Teknik ön kontrol | PC/WSL'de sembol başına 1 saatlik koşu. **Karar verisi DEĞİLDİR**; analize katılmaz. EC2'de de veri toplama öncesi aynı ön kontrol yapılır (§3.1) |
| Süreç düzeni | Her sembol için ayrı süreç ve ayrı çalışma dizini (gerekçe aşağıda) |
| Config | `dry_run=true`, `market=SPOT`, `obi_threshold` sabit, `initial_capital>0`, anahtar alanı yok; dosya `chmod 444`, sha256 kaydedilir |
| API anahtarı | **EC2'ye gerçek sır konmaz.** Sabit, gizli olmayan bir yer tutucu zorunludur (gerekçe aşağıda) |
| Saat | EC2 saat dilimi UTC; **süreç ortamında `TZ=UTC` zorunlu**; chrony/NTP durumu saatte bir loglanır |
| Log toplama | stdout ve stderr **satır başına zaman damgalı** toplanır (journald veya `ts`); `kusak.log` ayrıca saklanır |
| Kaynak | Süreç başına 4 busy-spin thread; iki süreç için en az 8 adanmış vCPU. Instance tipi veri toplamadan önce kaydedilir (S9) |
| Hafta tanımı | `T0` = iki süreç ve ön kontrol tamamlandıktan sonraki ilk 00:00 UTC. `Wk = [T0+7(k-1) gün, T0+7k gün)` |

Kodla ilgili gerekçeler:

- CSV'de **sembol kolonu yoktur** [KOD `telemetry_sink.hpp:456-458`]. `obi_dir` config'ten ayarlanmaz; her zaman çalışma dizinine göre `obi_research/` kullanılır [KOD `telemetry_sink.hpp:328`, `strategy_core.hpp:471-480`]. İki sembol aynı dizinde koşarsa veriler karışır. Bu yüzden her sembol ayrı dizinde koşar: `<kök>/PENDLEUSDT/`, `<kök>/BTCUSDT/`.
- **API anahtarı: yer tutucu yeterli ve zorunlu.**
  - Süreç, `dry_run` değerinden bağımsız olarak boş anahtarla çıkar [KOD `main.cpp:1243-1247`, `if (true)` bloğu].
  - `api_secret` yalnız HMAC anahtar tamponuna kopyalanır [KOD `binance_http_client.hpp:102-105`, çağrı `:453`] ve imzalayıcıya verilir. `SignedRequestBuilder::init` yalnız imzasız saat senkronu yapar [KOD `binance_signer.hpp:564-583`]. `api_key` yalnız `X-MBX-APIKEY` başlığına yazılır [KOD `binance_http_client.hpp:462`].
  - dry_run'da `live_order_client` kurulmaz [KOD `main.cpp:1282`], imzalı hesap GET'i yapılmaz [KOD `:1293`], başlangıç uzlaştırması atlanır [KOD `:1035`] ve POLL `!cfg.dry_run` ister [KOD `:1498`]. ExchangeInfo `get_public` kullanır [KOD `exchange_info_manager.hpp:187`]; bu da `cold_get(..., authenticated=false)` çağırır [KOD `binance_http_client.hpp:566`].
  - İmzalı çağrıların hepsi `main.cpp` içinde [KOD grep]: `:305` CANCEL ve `:354` POLL (order thread), `:783` ve `:794` startup, `:1298` hesap. dry_run'da bunlara ulaşılmadığı **kod okumasıyla** çıkarılmıştır [ÇIKARIM; order thread `:460-461` yorumu]. Yer tutucu anahtarla ulaşılsa bile istek kimlik doğrulamada reddedilir ve gerçek hesaba etki edemez [ÇIKARIM].
  - Bu nedenle **EC2'ye gerçek anahtar (testnet dahil) konmaz**. Süreç `env -i` ile temiz ortamda, `TESTNET=1` ve yer tutucu `API_KEY`/`API_SECRET` ile başlatılır. `TESTNET` ortam değişkeni config'i ezer; `false`, `0` veya `HAYIR` mainnet REST demektir [KOD `main.cpp:1193-1200`, PROGRESS B1].
- **Config koruması.** Config dosyasının mtime'ı değişirse `cfg` tümüyle yeniden yüklenir (`dry_run` ve `obi_threshold` dahil) [KOD `config.hpp:203-207`, `main.cpp:1555-1562`; PROGRESS B3]. Yükleme olduğunda `kusak.log`'a `[CONFIG] Konfigurasyon yeniden yuklendi` yazılır [KOD `main.cpp:1561`]. Korumalar:
  1. Süreç root olmayan bir kullanıcıyla koşar.
  2. Config `chmod 444` yapılır ve root tarafından `chattr +i` ile değiştirilemez hale getirilir. `+i` dosyanın sahibine `touch` dahil her yazmayı engeller; kaldırmak root gerektirir.
  3. Koşu sonunda config'in sha256 ve mtime değerleri başlangıçtaki değerlerle karşılaştırılır.
  4. `kusak.log` içinde `Konfigurasyon yeniden yuklendi` aranır. Bulunursa sapma kaydı açılır (§11) ve §9 dışlaması uygulanır.
- Piyasa WS'i her zaman prod'a bağlanır (`stream.binance.com`) [KOD `ws_client.hpp:1165`]. Araştırma için istenen davranış budur.
- **Logger zamanı:** `kusak.log` satırları `localtime_r` ile yerel saatte, saat dilimi ofseti olmadan yazılır [KOD `logger.hpp:185-197`]. `TZ` ayarı olmadan log zamanı belirsizdir; bu yüzden `TZ=UTC` zorunludur. Logger satırları hem stdout'a (renk kodlu) hem `kusak.log`'a gider [KOD `strategy_core.hpp:487-496`, `logger.hpp:171-180`]. `[ws]` ve `[TSC]` satırları ise zaman damgasız doğrudan stdout/stderr'e yazılır (§9).
- **Kaynak (busy-spin) [KOD]:** süreç başına dört thread sürekli döner, uyumaz:
  - WS okuma döngüsü: syscall yok, `_mm_pause` [`ws_client.hpp:1358-1368`].
  - Strateji ana döngüsü [`main.cpp:1464-1476`].
  - Order thread: uyarlamalı bekleme, uyku yok [`main.cpp:655-662`].
  - Telemetri thread'i [`main.cpp:1425-1440`].

  ExchangeInfo thread'i `cv.wait_for` ile uyur [`exchange_info_manager.hpp:138-139`]. `BinanceHttpClient` io thread'i başlatılmaz; `start_io_thread` çağrısı yok [grep]. CPU pinning yalnız strateji ve order thread'leri için, yalnız `KUSAK_STRATEGY_CORE` / `KUSAK_ORDER_CORE` ortam değişkenleriyle yapılır; config'te bu alanlar yok [`main.cpp:227-245`, `config.hpp` grep]. Varsayılan durumda pinning yoktur. İki süreç toplam **8 busy-spin thread** demektir. Burstable (kredi tabanlı) instance'larda sürekli %100 CPU kısılmaya ve `max_lag_us` şişmesine yol açar [ÇIKARIM]. Seçim: S9.

### 3.1 Veri toplama öncesi EC2 ön koşulları

Bunların hepsi sağlanmadan `T0` başlamaz. Sonuçlar §10.8'e yazılır.

1. **TSC kararlılığı.** Ufuklar ve `max_lag_us`, `tsc_ns()` ile ölçülür. Zincir: `process_book_ticker` `tick_ns = current_time_ns()` [KOD `strategy_core.hpp:2325`] → canlı dry_run'da `tsc_ns()` [KOD `:430-435`, `mock_time_ns=0`] → `__rdtscp` + `tsc_to_ns` 128-bit çarpım [KOD `time.hpp:72-81`]. Motor TSC'yi doğrudan okur; kernel saat kaynağından bağımsızdır.
   - `/proc/cpuinfo`: her vCPU'da `constant_tsc` **ve** `nonstop_tsc` bayrakları bulunmalı. Komut: `grep -o -w 'constant_tsc\|nonstop_tsc' /proc/cpuinfo | sort | uniq -c`.
   - `/sys/devices/system/clocksource/clocksource0/current_clocksource` = `tsc`. Kernel TSC'yi güvenilmez bulursa saat kaynağını değiştirir; bu değer kernel'in TSC'ye güvendiğinin göstergesidir [ÇIKARIM]. EC2'de varsayılan değer [DOĞRULANMADI].
   - **CSV mono/wall oran testi:** her süreç dosyasında, event_id'ye göre sıralı ilk ve son olay için `r = Δsignal_mono_ns / (Δsignal_wall_us · 1000)` hesaplanır; `r ≈ 1` beklenir. `signal_mono_ns` TSC'den [KOD `strategy_core.hpp:277`, `:2298`], `signal_wall_us` `system_clock`'tan gelir [KOD `:2295-2297`, `telemetry_sink.hpp:124-127`]. Sapmanın beklenen kaynakları: 100 ms'lik kalibrasyon hatası (`CLOCK_MONOTONIC_RAW`'a karşı) [KOD `time.hpp:43-68`] ve NTP'nin `system_clock`'u kaydırması [ÇIKARIM]. Ek olarak, ID sırasında `signal_mono_ns` kesin artan olmalıdır. **Eşik ve başarısızlıkta yapılacak: S10.**
   - Bilgi (gözlem, 2026-11-24): WSL ön kontrol makinesinde 24/24 vCPU'da `constant_tsc` ve `nonstop_tsc` vardı, `current_clocksource=tsc`.
2. **Binary.** EC2'de derlenen binary'nin sha256'sı, derleme kaydı ve aynı kaynakla `tsc_overflow_test` geçişi kaydedilir (§13).
3. **Config ve anahtar.** Yukarıdaki config korumaları uygulanır; ortamda yer tutucu anahtar kullanılır.
4. **Saat ve log.** `TZ=UTC`, chrony senkron, zaman damgalı stdout/stderr toplama.
5. **Instance kaydı.** Tip, vCPU, bölge/AZ, kernel ve saat kaynağı (S9).
6. **EC2 1 saatlik ön kontrolü** (sembol başına; karar verisi değildir): log satırları, `event_id` boşlukları, lag atılma oranı ve mono/wall oranı raporlanır. S3, S10 ve S11 kararlarının girdisidir.

## 4. Ölçüm aracı ve tanımlar

**Üretici (C++):**

- `track_obi_signal` [KOD `strategy_core.hpp:2282-2300`]. OBI = `(bid_qty-ask_qty)/(bid_qty+ask_qty)`, top-of-book `bookTicker` [KOD `:1208-1214`]. `obi > thr` ise yön +1 (AL), `obi < -thr` ise −1 (SAT).
- **Kaydedilen eşik yalnız `cfg.obi_threshold`'dur**, tek değer [KOD `:2288`]. Varsayılan 0.35 [KOD `config.hpp:113`]; depodaki config'lerde bu anahtar yok, yani 0.35 kullanılır [KOD grep]. Ayrıntı için AÇIK SORU S1.
- Motor içi seyreltme: yön başına 100 ms (`MIN_GAP_NS`, mono saat) [KOD `:251`, `:266-270`]. Takipçi kapasitesi 128 [KOD `:248`].
- Ufuklar: +10, 50, 100, 250, 500, 1000, 5000 ms MID [KOD `telemetry_sink.hpp:120-121`]. Her ufkun fiyatı, vade anından sonra gelen ilk `bookTicker` tick'inde güncellenmiş mid'den alınır [KOD `strategy_core.hpp:2318`, `:2327`, `:300-305`]. `max_lag_us`, yedi ufuktaki bu gecikmelerin en büyüğüdür [KOD `:302-304`]. Sonuç olarak kaydedilen fiyat vade anındaki fiyat değil, vadeden sonraki ilk tick'teki fiyattır (S3).
- Yalnız 7/7 ufku dolan olay yayınlanır; `n_filled` her zaman 7'dir [KOD `:307-313`]. Kapanışta bekleyen olaylar yazılmaz [ÇIKARIM].
- **Kayıp yolları ve görünürlük:**
  - **Takipçi dolması (`dropped_full`) yapısal olarak oluşamaz [ÇIKARIM].**
    - `poll` her tick'te `add`'den önce çağrılır [KOD `:2327-2328`]. Vadesi dolan olaylar aynı tick'te çıkarılır [KOD `:300`, `:307-317`].
    - Bu yüzden bir `add` anında bekleyen olaylar son 5000 ms içinde eklenmiş olanlardır. Yön başına en az 100 ms aralıkla [KOD `:266-270`] bu, en fazla 49 (aynı yön) + 50 (diğer yön) = 99 eder. Kapasite 128'dir [KOD `:248`; yorum `:249-250` "100 < 128"].
    - Öncül: mono saatin monoton olması (KZ-P0-01 düzeltmesi ve §3.1). Öncül analizde veriden sınanır: ID sırasında `signal_mono_ns` kesin artan ve yön içinde ardışık fark ≥ 100 ms olmalıdır.
  - **`event_id` yalnız başarılı `add`'de tüketilir** [KOD `:274-279`]. Dolu takipçi dalı (`:274`) ID tüketmeden döner.
  - Olaylar eklenme sırasında tamamlanır: vade = ekleme anı + 5000 ms ve ekleme anı ID ile artar. Aynı tick'te tamamlananlar swap-with-last ile karışık sırada yazılır [KOD `:315`]. Bu nedenle bir süreç dosyasında (event_id süreç başına 1'den başlar [KOD `:260`]) **ID'ler sıralandıktan sonra 1..max_id aralığındaki eksik ID'ler**, OBI halkasında (`record_obi_event`, `obi_drops_` [KOD `telemetry_sink.hpp:437-441`]) ve dosyaya yazmada kaybolan olayların **tam ölçüsüdür** [ÇIKARIM].
  - Kapanışta takipçide veya halkada kalan olaylar max_id'den büyük ID taşır; eksik sayılmaz ve dışlama gerektirmez [ÇIKARIM].
  - **Dosya ID sırasında değildir; kontrol, sıralamadan sonra yapılır.**
- Zaman: ufuklar `current_time_ns()` = `tsc_ns()` ile ölçülür (canlı dry_run'da `mock_time_ns=0`) [KOD `:430-435`; `mock_time_ns` yalnız `tools/` içinde atanıyor, grep]. `signal_wall_us` = `system_clock` [KOD `:2295-2297`, `telemetry_sink.hpp:124-127`].
- Çıktı dosyası `obi_research/obi_telemetry_<unix_s>.csv`. `<unix_s>` sürecin **ilk flush anıdır**; süreç yaşadığı sürece aynı dosyaya eklenir. Dosya adı haftayı göstermez [KOD `telemetry_sink.hpp:446-459`].

**Analizci (Python):** `python/obi_research.py`, yalnız stdlib. `python/test_obi_research.py` numpy/scipy oracle'ına karşı doğrular.

- Getiri: `g = side·(price_after_h − signal_price)/signal_price·1e4` bps [KOD `obi_research.py:19`, `:292-299`].
- `EV_NET` = hücrede net getirinin ortalaması (olay başına kendi spread'iyle) [KOD `:293`, `:307`].
- `t` ve `p(t)`: Student-t, **iki yönlü** [KOD `:82-91`, `:127-128`]. `t = ort/√(s²/n)` olayların **bağımsız** olduğunu varsayar (S7).
- `k*`: `EV_NET`'in sıfırlandığı çıkış-spread çarpanı, `2·(EV_brut − 2fee − slip)/ort_spread − 1` [KOD `:130-131`].
- `p_bonf = min(1, p(t)·n_tests)`. `n_tests` = **o tek çağrıda** N≥2 olan ve p'si sonlu hücre sayısıdır [KOD `:341-343`]. Hücre anahtarı `(threshold, side, horizon)`'dır; sembol anahtarda yoktur [KOD `:303`]. Tek sembol ve tek eşikle bütün hücreler geçerliyse `n_tests = 14`; script'in yazdığı `p_bonf` bu değerle hesaplanır, **karar için kullanılmaz** (§7).
- `--max-lag-us` filtresi, `--min-gap-ms` dedupe'sinden **önce** uygulanır [KOD `:177-179`, `:281-286`]. Başlıktaki "gecikme N" filtreyle atılan olay sayısıdır. Başlıktaki gecikme p50/p99 değerleri yalnız **kalan** olaylardan hesaplanır [KOD `:290`, `:318-332`].

## 5. Maliyet modeli

**Birincil model (karar için):** `--fee-bps 10 --exit-spread-mult 1.5 --slippage-bps 0.5 --max-lag-us 2000 --min-gap-ms 500`. Spread maliyeti açıktır (`--no-spread-cost` kullanılmaz). Eksik satırlar dahil edilmez (varsayılan `complete_only=True`). `--max-lag-us` ve `--min-gap-ms` değerleri sırasıyla S3 ve S7 kararlarına bağlıdır (ENGELLEYİCİ).

| Bayrak | Koddaki anlamı | Doğrulama |
|---|---|---|
| `--fee-bps 10` | bacak başına komisyon; maliyete `2·fee` = 20 bps eklenir | [KOD `:240`, `:265`] |
| `--exit-spread-mult 1.5` | çıkış spread'i = 1.5 × giriş spread'i (veride ölçülmez, varsayım) | [KOD `:241`, `:266`] |
| `--slippage-bps 0.5` | giriş+çıkış **toplam** sabit kayma | [KOD `:242`, `:267`] |
| `--max-lag-us 2000` | `max_lag_us > 2000` olan olay atılır (S3, ENGELLEYİCİ) | [KOD `:239`, `:177-179`] |
| `--min-gap-ms 500` | aynı (eşik, yön) içinde wall saatine göre 500 ms dedupe (S7, ENGELLEYİCİ) | [KOD `:238`, `:281-286`] |
| spread açık | `sf=(1+m)/2` = 1.25 | [KOD `:266`] |
| varsayılan complete-only | `n_filled<7` atılır; motor hep 7 yazdığı için etkisiz [KOD `strategy_core.hpp:313`] | [KOD `:174-176`, `:245`] |

Olay başına maliyet: `2·fee + slip + spread·(1+m)/2` [KOD `:23-26`, `:293`]. Bu formül **market giriş + market çıkış** (taker/taker) varsayar. Motor ise `LIMIT_MAKER` giriş + `MARKET` çıkış yapar (CLAUDE.md §1).

- **Dolan işlem başına maliyet açısından muhafazakâr [ÇIKARIM].** Maker giriş yarım spread ödemez; model ise `spread/2` ekler. VIP 0 maker ve taker oranları eşitse (her ikisi 10 bps [DOĞRULANMADI, Binance tarifesi]) komisyon kalemi nötrdür; maker daha ucuzsa model muhafazakâr kalır.
- **Brüt getiri açısından muhafazakâr değildir [ÇIKARIM].** Mid getirisi koşulsuzdur; pasif emrin dolması koşullu ve ters seçimlidir. Analizci de bunu belirtir [KOD `:401-402`]. Ayrıca sinyal anındaki mid'den ölçülür; emir gecikmesi yok sayılır, bu yüzden +10 ms ufku pratikte işlenemeyebilir. Bu eksikler Kapı 2'nin konusudur.
- Bilgi [ÇIKARIM]: PENDLEUSDT'de 1 tick spread ≈ 7,6 bps (PROGRESS P2). Birincil modelde olay başına maliyet ≈ 20 + 0,5 + 7,6·1,25 ≈ 30 bps olur. BTCUSDT'de spread ihmal edilebilir; maliyet ≈ 20,5 bps.

**Duyarlılık (yalnız bilgi, karar için değil):** `--fee-bps 7.5` (BNB indirimi [DOĞRULANMADI]) ve `--fee-bps 0`. Diğer bayraklar birincil modelle aynıdır. S3 kararına göre `--max-lag-us` olmadan bir "yalnız bilgi" koşusu da eklenebilir (S3 seçenek c).

## 6. Analiz komutu

Her sembol **ayrı** çalıştırılır (sembol kolonu yok; birlikte verilirse aynı hücrede karışır).

```bash
# Birincil — tam dönem (3 geçerli hafta, dışlamalar uygulanmış kopya)
python3 python/obi_research.py <SYM>/clean/all/ \
  --fee-bps 10 --exit-spread-mult 1.5 --slippage-bps 0.5 \
  --max-lag-us 2000 --min-gap-ms 500 --min-n 1000 --no-color \
  --csv-out results/<SYM>_all_primary.csv  > results/<SYM>_all_primary.txt

# Haftalık — aynı bayraklar, k = 1..3
python3 python/obi_research.py <SYM>/clean/w<k>/ <aynı bayraklar> \
  --csv-out results/<SYM>_w<k>_primary.csv > results/<SYM>_w<k>_primary.txt

# Duyarlılık — yalnız --fee-bps 7.5 ve 0 değişir
```

`--min-n 1000` ölçüt 3 (`n ≥ 1000`) ile aynı sınırı kullanır; yalnız terminal tablosundaki `*` işaretini ve SONUC bölümündeki aday hücre listesini etkiler [KOD `:246`, `:356`, `:374`]. **Script çıktısındaki `p_bonf` kolonu ve SONUC satırları `n_tests` ile hesaplanır (tek sembol, tek eşik, bütün hücreler geçerliyse M=14); karar §7'deki M=28 ile elle hesaplanır.** S7 kararı bu komutu değiştirebilir (örtüşmeyen alt örnek için ufuk başına ayrı çağrı).

**Haftalık ayırma.** Analizcide zaman aralığı bayrağı yoktur [KOD `obi_research.py:235-249`]. Dosya adı haftayı göstermez (§4). Bu nedenle ayırma `signal_wall_us` ile yapılır; bu kolon CSV'nin 2. sütunudur [KOD `telemetry_sink.hpp:456`]. Her ham dosya için başlık korunur ve dosya adı `obi_telemetry_*.csv` kalır:

```bash
awk -F, -v a=<Wk_bas_us> -v b=<Wk_son_us> 'FNR==1 || ($2>=a && $2<b)' raw/obi_telemetry_X.csv > w<k>/obi_telemetry_X.csv
```

Dışlama pencereleri (§9) aynı yolla uygulanır. Bir olay, `[signal_wall_us, signal_wall_us + 5 002 ms]` aralığı herhangi bir dışlama penceresiyle kesişiyorsa atılır. Bu filtre aracı veri toplama başlamadan yazılır, sha256'sı §11'e kaydedilir (AÇIK SORU S6).

## 7. Başarı ölçütleri

**Bonferroni test sayısı (önceden sabit).** `M = eşik sayısı × 7 ufuk × 2 yön × 2 sembol`. S1'e göre tek eşik (0.35) kullanılırsa **M = 28**.

Analizcinin kendi `p_bonf` değeri yalnız tek çağrıdaki hücreleri sayar (tek sembol için en fazla 14) [KOD `:341-343`]. Bu yüzden karar değeri csv-out'taki `p_t` kolonundan **elle** hesaplanır:

`p_bonf_kayıt = min(1, p_t × M)`

Bu değer, iki sembolün ayrı koşulduğu durumda `min(1, p_bonf_script × 2)` ile **yalnız o çağrıda `n_tests = 14` ise** aynıdır. Bu, çıktıdaki `test sayisi (hucre): 14` satırıyla doğrulanır [KOD `:341`, `:377`]. `n_tests < 14` ise (N<2 veya varyansı sıfır olan hücre varsa) script daha küçük çarpan kullanır. Karar her durumda `p_t × 28` ile verilir. Bir hücrenin N'si küçük olsa bile M düşürülmez. Haftalık koşular test sayısına eklenmez; yalnız işaret tutarlılığı için kullanılır.

**Hücre GEÇTİ**, ancak aşağıdakilerin **hepsi** sağlanırsa (sembol başına değerlendirilir):

1. Tam dönem birincil koşuda `ev_net > 0`.
2. `p_bonf_kayıt < 0.05`. `p(t)` iki yönlü olduğundan 1 ile birlikte etkin eşik tek yönlü ≈ 0.025'tir [ÇIKARIM].
3. `n ≥ 1000` (dedupe ve lag filtresinden sonra, csv-out `n`).
4. `k_star ≥ 1.5`. Not [KOD `:130-131`, `:293`, `:307`; cebirsel çıkarım, kayan nokta sınırı ihmal]:
   - `EV_NET = (EV_brut − 2fee − slip) − ort_spread·(1+m)/2`.
   - `ort_spread > 0` ve `EV_brut − 2fee − slip > 0` iken: **`k* ≥ 1.5 ⇔ EV_NET ≥ 0`** ve **`k* > 1.5 ⇔ EV_NET > 0`** (m = 1.5).
   - `EV_brut − 2fee − slip ≤ 0` ise `k*` NaN olur ve `EV_NET ≤ 0`'dır; iki ölçüt de sağlanmaz.
   - `ort_spread = 0` ve `EV_brut − 2fee − slip > 0` ise `k* = inf` ve `EV_NET > 0`'dır.
   - Dolayısıyla ölçüt 1 sağlandığında ölçüt 4 kendiliğinden sağlanır ve ek bilgi taşımaz (AÇIK SORU S4).
5. Haftalık tutarlılık: aynı hücrede W1, W2 ve W3 koşularının **her birinde ayrı ayrı** `ev_net > 0`.

**Kapı 1 GEÇTİ**, en az bir sembolde en az bir hücre GEÇTİ ise. GEÇEN sembol ve hücre(ler) Kapı 2'ye taşınır. Birden fazla hücre geçerse seçim kuralı için AÇIK SORU S5'e bakılır; hepsi raporlanır. SPOT SAT hücreleri için §2'deki envanter notu Kapı 2 yorumunda dikkate alınır.

## 8. Başarısızlık ve dur kuralı

- Hiçbir sembolde GEÇEN hücre yoksa **Kapı 1 GEÇMEDİ**. Canlı para kullanılmaz. Yol haritası **Aşama 5B**'ye geçer (§1.1: projeyi portföy / altyapı / eğitim değerine dönüştürme). Bu aşama `PROJECT_OVERVIEW.md`'de henüz yazılı değildir.
- **Kurtarma yok:** eşik, ufuk, yön, maliyet bayrağı, `min-gap`, `max-lag` veya dışlama kuralı değiştirilip yeniden denenmez. Duyarlılık koşuları kararı değiştirmez.
- Veri toplama süresince ara sonuç değerlendirmesi ve erken durdurma yapılmaz. Yalnız §9'daki veri kalitesi kontrolleri yapılır.
- 3 geçerli hafta toplam **35 gün** içinde elde edilemezse sonuç **GEÇERSİZ ÖLÇÜM** olur (GEÇTİ değildir). Tekrar ancak aynı ölçütlerle ve §11'e tarihli kayıtla yapılabilir.

## 9. Veri kalitesi ve dışlama (önceden sabit)

| Sorun | Kodda tespit | Dışlama penceresi |
|---|---|---|
| WS kopması | `[ws] Baglandi: <sym> stream aktif` **stdout**'a yazılır [KOD `ws_client.hpp:1227`, `std::cout`]. Kopma ve yeniden bağlanma satırları **stderr**'e yazılır: `[ws] Baglanti kesildi` [`:1234`], `Sunucu kapatti` [`:1356`], `TLS okuma hatasi` [`:1357`], `45s boyunca veri yok` [`:1363-1365`], `Yeniden baglaniliyor` [`:1206`], `TCP/TLS hatasi` [`:1219`], `Handshake hatasi` [`:1224`], `Close frame alindi` [`:1436`]. Bu satırlar **zaman damgasızdır**; iki akış da aynı saatle damgalanmalıdır (journald/`ts`). `g_ws_disconnect_total` sayacı loglanmıyor [KOD `:1233`] | kopma satırı − 60 s → `Baglandi` + 10 s |
| Piyasa halkası düşmesi | `kusak.log` `DROP_MONITOR Market event drop` [KOD `main.cpp:266-275`, 500 ms tarama `:1542`] | log zamanı ± 10 s |
| OBI halkası / dosya yazma kaybı | `obi_drops_` sayılıyor ama loglanmıyor [KOD `telemetry_sink.hpp:437-441`, grep]. **Tam ölçü:** süreç dosyası içinde event_id'ler **sıralandıktan sonra** 1..max_id aralığındaki eksik ID'ler (§4) [ÇIKARIM `strategy_core.hpp:274-279`, `:315`] | boşluğu çevreleyen olayların `signal_wall_us` aralığı ± 10 s |
| Takipçi dolması | `dropped_full` yapısal olarak oluşamaz (§4) [ÇIKARIM `strategy_core.hpp:248-251`, `:266-279`, `:2327-2328`]. Öncül (mono saat monotonluğu) TSC satırı ve S10 kapsamında sınanır | ayrı pencere yok; öncül bozulursa TSC satırı ve S10 kararı uygulanır |
| Kayıt boşluğu / süreç yeniden başlaması | ardışık olaylar arasında `signal_wall_us` farkı > 60 s; yeni `obi_telemetry_<unix_s>.csv` dosyası. Not: > 60 s olay boşluğu sakin piyasada da oluşabilir; dayanak S11'de açık (ENGELLEYİCİ) | boşluğun tamamı |
| Wall saat sorunu | chrony saatlik log; analizcideki `wall-saat sirasi disi` uyarısı [KOD `obi_research.py:320-321`]. Tek dosyada küçük karışıklık swap-with-last nedeniyle normal olabilir [ÇIKARIM `strategy_core.hpp:315`] | \|ofset\| > 100 ms veya saat adımı olan saat |
| TSC / mono saat | `[TSC] Calibrated: ns_mult=` stderr satırı [KOD `main.cpp:1170`]; §3.1 TSC ön koşulları; mono/wall oranı (eşik S10) | kalibrasyon satırı yoksa veya `max_lag_us` dağılımında ~4,29 s kümesi varsa o süreç dosyası tümüyle |
| Config değişimi | `kusak.log` `[CONFIG] Konfigurasyon yeniden yuklendi` [KOD `main.cpp:1561`]; config sha256/mtime farkı; CSV'de birden fazla `threshold` değeri | yükleme anından süreç sonuna kadar |

**Kural:** Bir haftada dışlanan süre (pencerelerin birleşimi) haftanın **%10'unu aşarsa o hafta geçersizdir** ve kayıt bir hafta uzatılır. Kullanılan haftalar **kronolojik sıradaki ilk 3 geçerli haftadır**; sonradan gelen haftalar seçilerek eklenmez. Tam dönem analizi bu 3 haftanın birleşimiyle yapılır. `--max-lag-us` ile atılan olaylar süre dışlaması sayılmaz; sayıları ve oranları sembol başına raporlanır (S3).

## 10. Yayın formatı

Her şey yayınlanır: kayıplar, başarısız ve N'si yetersiz hücreler dahil. Hiçbir hücre gizlenmez.

1. Her sembol için tam dönem ve W1-W3 birincil koşularının `--csv-out` dosyaları. Kolonlar: `threshold, side, horizon_ms, n, win, loss, flat, avg_win, avg_loss, ev_gross, ev_net, t, p_t, p_z, p_bonf, win_net, mean_spread_bps, k_star` [KOD `obi_research.py:361-363`]. Bunlara `p_bonf_kayıt` kolonu eklenir. Script'in `p_bonf` kolonu `n_tests` (≤ 14) ile hesaplandığı için "karar dışı" diye işaretlenir.
2. Terminal çıktıları (`.txt`). Başlıkta atılan satır sayıları (eksik, gecikme, dedupe, bozuk), kapsanan süre ve gecikme p50/p99 bulunur [KOD `:317-335`]. Lag filtresiyle atılan olay sayısı ve oranı ile filtresiz lag dağılımı sembol başına ayrıca verilir (S3).
3. Hücre × ölçüt karar tablosu: ölçüt 1-5 için ✓/✗ ve sonuç.
4. Haftalık tablo: hücre × hafta `ev_net`, `n`.
5. Duyarlılık tabloları (`fee 7.5`, `fee 0`). Bunlar "bilgi" olarak işaretlenir.
6. Dışlama günlüğü: başlangıç, bitiş, neden, kaynak log satırı; hafta başına toplam dışlanan süre ve oranı. Süreç dosyası başına eksik event_id sayısı.
7. Ham veri özeti: dosya adları, satır sayıları, `signal_wall_us` min/max, sha256'lar, süreç dosyası başına mono/wall oranı.
8. Sürümler:
   - `kusak_v13` binary sha256. WSL ön kontrol binary'si `f80c2d64bb31eae18c081617d9ad3517c7764bf4820536aec6566bbf7e4ff850` (PROGRESS Faz 3.7). **EC2 binary'si ayrı derleneceği için farklı olacaktır; sha256 deploy anında kaydedilir, §11'e ve rapora yazılır.**
   - Config sha256, `obi_research.py` ve dışlama aracı sha256.
   - EC2 instance tipi, vCPU, bölge/AZ, kernel, saat kaynağı ve §3.1 TSC kontrol çıktıları.

## 11. Değişiklik günlüğü

Veri toplama başladıktan sonra yapılan **her** değişiklik (kod, config, araç, süre, dışlama, ölçüt) tarihli olarak buraya yazılır. Sonuçlar her durumda **orijinal ölçütlere göre de** raporlanır.

| Tarih (UTC) | Değişiklik | Gerekçe | Onay |
|---|---|---|---|
| 2026-11-24 | Ön kayıt oluşturuldu | — | — |
| 2026-11-24 | **Bağımsız denetim sonrası düzeltmeler (veri toplama başlamadan önce; ön kayıt öncesi düzeltme).** Olgusal düzeltmeler: WS satırlarının stdout/stderr ayrımı (§9); `dropped_full`'ün yapısal olarak oluşamaması ve eksik event_id'nin tam ölçü olması (§4, §9; "tespit edilemez" ifadesi kaldırıldı); `p_bonf_script×2` eşitliğinin yalnız `n_tests=14` iken geçerli olması (§7); `k*` ile `EV_NET` denkliğinin kesin ifadesi (§7); örnekleme yerinin (`dynamic_momentum_exit` sonrası) düzeltilmesi (§2); API anahtarı gerekçesi: yer tutucu zorunlu, EC2'ye gerçek sır konmaz (§3); `TZ=UTC` zorunluluğu ve logger zamanı (§3); busy-spin thread sayısı [KOD] (§3); binary sha256 kaydı (§10.8); Kapı 2 ve Aşama 5B tanımları (§1.1); SPOT SAT envanter notu (§2). Eklenen ön koşul: EC2 TSC kararlılık kontrolü (§3.1). Yeni/yeniden sınıflanan açık sorular: S3 ve S7 ENGELLEYİCİ; S9, S10, S11 eklendi (ENGELLEYİCİ); S2 engelleyici değil önerisi. **Başarı ölçütleri (§7 1-5), M=28, maliyet bayrakları ve §9 dışlama pencereleri değiştirilmedi.** §6 birincil komutta `--min-n 1000` kontrol edildi: zaten vardı, değişiklik gerekmedi | Bağımsız denetim bulguları (Ö1-Ö7, K1-K10) | Proje sahibi onayı bekliyor |
| 2026-11-24 | **Deney kapatıldı.** 1 saatlik ön kontrol sonuçları (lag filtresiz, BİLGİ):<br>- BTC: 5978 olay, 5 s brüt +0,62 / +0,33 bps, EV_NET ≈ −20 bps.<br>- PENDLE: 2117 olay, spread 8 bps, 5 s brüt +1,41 / +1,43 bps, EV_NET ≈ −29/−30 bps.<br>- `--max-lag-us 2000` iki sembolde de olayların %100'ünü attı; birincil komut 0-1 olayla kaldı.<br>- Maker/sıfır ücret senaryoları da standart ücretle negatif (`maker_scenario.py`, repo dışı). | Proje sahibi 5 s OBI deneyini kapattı; v14.2 "nadir işlem" araştırmasına geçildi | Proje sahibi (2026-11-24) |

## 12. Açık sorular

**ENGELLEYİCİ** olarak işaretlenenler veri toplama başlamadan kapanmalıdır. Ölçüt etkileyen her karar **veri toplama başlamadan önce** proje sahibi tarafından yazılır ve §11'e kaydedilir.

- **S1 — ENGELLEYİCİ. Eşik ızgarası.** Motor süreç başına yalnız tek eşik (`cfg.obi_threshold`) kaydediyor [KOD `strategy_core.hpp:2288`]; analizci de hücreleri `threshold` kolonuna göre ayırıyor [KOD `obi_research.py:164`, `:303`]. Bu yüzden "eşik × ufuk × yön × sembol" ızgarasının eşik boyutu 1'dir. Seçenekler:
  - (a) Tek eşik 0.35, M = 28.
  - (b) Sembol başına farklı eşikli ek süreçler (ayrı dizinler); M orantılı artar. Her ek süreç 4 busy-spin thread ekler (S9).
  - (c) `obi` kolonundan sonradan alt küme almak. Motorun 100 ms seyreltmesi daha düşük eşikte uygulandığı için yanlıdır [ÇIKARIM] ve yeni araç gerektirir.
- **S2 — ENGELLEYİCİ DEĞİL (2026-11-24 yeniden değerlendirme, öneri). Düşen olay görünürlüğü.** `obi_dropped()`, `dropped_full` ve `throttled` hâlâ hiçbir yerde loglanmıyor [KOD grep]. Ancak §4'teki çözümleme şunu gösteriyor:
  - `dropped_full` yapısal olarak oluşamaz [ÇIKARIM]; öncülü (mono monotonluğu) veriden sınanır.
  - `event_id` yalnız başarılı `add`'de tükenir [KOD `:274-279`]. Bu yüzden süreç dosyasındaki sıralı eksik ID sayısı, halka ve yazma kayıplarının **dolaylı değil tam** ölçüsüdür [ÇIKARIM].
  - `throttled`, ön kayıtlı tasarımın bir parçasıdır (yön başına 100 ms seyreltme); kayıp sayılmaz.
  - Sonuç olarak "dolaylı yöntem kabul edilecek mi" sorusunun dayanağı kalmadı; dışlama penceresi §9'da zaten sabit. Kalan seçenek bilgi amaçlıdır: sayaçların loglanması C++ değişikliğidir ve ayrı onay gerektirir. Proje sahibi engelleyici kalmasını isterse S2 yeniden ENGELLEYİCİ olur.
- **S3 — ENGELLEYİCİ. `--max-lag-us 2000`: atma oranı ve seçim yanlılığı.**
  - Mekanizma [KOD `strategy_core.hpp:2318`, `:2327`, `:300-305`; `obi_research.py:177-179`]: ufuk fiyatı, vadeden **sonraki ilk** `bookTicker` tick'inde güncellenmiş mid'dir. `max_lag_us`, yedi ufkun en büyük gecikmesidir. Filtre, herhangi bir ufukta vade ile sonraki tick arası 2 ms'yi aşan olayı tümüyle atar.
  - Seyrek tick'li sembolde (PENDLEUSDT [ÇIKARIM]) olayların **çoğu** atılabilir. Kalan örnek, yedi vadenin hepsinin yoğun tick dönemine denk geldiği anlara kayar. Bu bir seçim yanlılığıdır.
  - Ek gözlem [ÇIKARIM]: tick'ler arasında top-of-book değişmez. Bu yüzden vade anındaki geçerli mid, vadeden **önceki** son tick'in mid'idir. Kaydedilen değer ise vadeden sonraki ilk tick'in mid'idir ve `lag` kadar ileriye bakar. Filtre bu ileri bakışı sınırlar, fakat yanlılık pahasına.
  - Seçenekler:
    - (a) 2000 µs korunur; atılma oranı sembol başına raporlanır.
    - (b) Eşik sembol başına ön kontrol ölçümüne göre, veri toplamadan önce yeniden belirlenir.
    - (c) Birincil filtre korunur ve `--max-lag-us` olmadan bir **"yalnız bilgi" duyarlılık koşusu** eklenir. Ön kontrol analiz scripti bunu zaten yapıyor; karar için kullanılmaz.
    - (d) Motorun vade anındaki geçerli mid'i (güncelleme öncesi mid) kaydetmesi. C++ değişikliğidir; ayrı talep, test ve onay gerektirir. Bu doküman setinin kapsamı dışındadır.
  - **Öneri:**
    1. Sembol başına 1 saatlik ön kontrolde (WSL ve EC2) filtresiz `max_lag_us` dağılımı ve > 2000 µs atılma oranı ölçülür. `analyze_dryrun.sh` bunu `analiz_kalite.txt`'ye yazar.
    2. Proje sahibi **veri toplamadan önce** bir karar kuralı yazar. Kural, örneğin hangi atılma oranında hangi seçeneğin uygulanacağını ve sonucun nasıl etiketleneceğini belirler.
    3. (c) eklenir.
  - Eski metindeki "> %50 ise temsil sınırlı" yalnız bir soru örneğiydi; karar değildir.
- **S4. `k* ≥ 1.5` ölçütü.** Birincil modelde (m=1.5) ölçüt 1 sağlandığında kendiliğinden sağlanır (§7 not). Daha sıkı bir eşik (örneğin `k* ≥ 2`) veya `--exit-spread-mult 2/3` duyarlılığı istenir mi? Analizci bunu önerir [KOD `obi_research.py:403-404`].
- **S5. Birden fazla GEÇEN hücre.** Kapı 2'ye hangisi taşınır? Öneri: en yüksek `t(NET)`; diğerleri listelenir.
- **S6 — ENGELLEYİCİ. Dışlama/hafta ayırma aracı** henüz yok. `awk` tarifi §6'da. Araç ve sha256 veri toplamadan önce sabitlenmeli. Araç §9'daki eksik ID kontrolünü **ID'leri sıralayarak** yapmalıdır.
- **S7 — ENGELLEYİCİ. Otokorelasyon (örtüşen pencereler).**
  - `--min-gap-ms 500` ile aynı (eşik, yön) hücresinde olaylar arası en az 500 ms vardır. +5000 ms ufkunda bir olayın penceresi sonraki ~10 olayın pencereleriyle örtüşebilir; +1000 ms'de ~2 olayla.
  - t-testi bağımsızlık varsayar: `t = ort/√(s²/n)` [KOD `obi_research.py:127-128`]. Örtüşme pozitif korelasyon yarattığından standart hata küçük, `p` olduğundan küçük çıkar [ÇIKARIM; analizci uyarısı `:405`].
  - Seçenekler:
    - (a) **Örtüşmeyen alt örnek:** h ≥ 1000 ms ufuklarında `min-gap ≥ h`. Mevcut bayraklarla yapılabilir: ufuk başına ayrı çağrı, `--horizons h --min-gap-ms h` [KOD `:237-238`]. N düşer (ölçüt 3 aynen uygulanır). M = 28 değişmez.
    - (b) **Newey-West / HAC standart hata.** Gecikme sayısı örtüşmeye göre (≈ h / gap) önceden sabitlenir.
    - (c) **Blok bootstrap.** Blok uzunluğu ≥ h; blok türü ve tekrar sayısı önceden sabitlenir.
  - (b) ve (c) analizcide yoktur; yeni araç gerektirir. Araç yazılır, sabit bir oracle'a karşı test edilir ve sha256'sı **veri toplamadan önce** §11'e kaydedilir.
  - Öneri [ÇIKARIM]: (a). Mevcut, test edilmiş araçla uygulanır ve muhafazakârdır. Rejim kalıcılığından gelen ufuk ötesi korelasyonu gidermez; bu sınırlama raporda yazılır.
  - Karar ve araç veri toplamadan önce sabitlenmelidir.
- **S8. KZ-P0-01 binary doğrulaması.** Bkz. §13 ve §3.1.
- **S9 — ENGELLEYİCİ (maliyet kararı). EC2 instance tipi ve vCPU.**
  - Süreç başına 4 busy-spin thread vardır [KOD, §3]: WS, strateji, order ve telemetri. İki sembol 8 sürekli dönen thread demektir. İşletim sistemi, chrony, journald ve SSH için de pay gerekir.
  - Seçenekler:
    - (a) Tek instance, en az 8 adanmış (burstable olmayan) vCPU. Öneri: payla birlikte daha fazlası.
    - (b) Sembol başına ayrı instance, her biri en az 4 adanmış vCPU + pay.
    - (c) Daha büyük instance ve `KUSAK_STRATEGY_CORE` / `KUSAK_ORDER_CORE` ile pinning. Yalnız iki thread pinlenebilir; WS ve telemetri thread'leri pinlenemez [KOD `main.cpp:227-245`].
  - Çoğu EC2 tipinde vCPU bir hyperthread'dir; iki dönen thread aynı fiziksel çekirdeği paylaşabilir [DOĞRULANMADI, instance tipine bağlı].
  - Instance tipi, vCPU, bölge/AZ ve fiyat proje sahibinin maliyet kararıdır. Veri toplamadan önce §10.8'e kaydedilir. Öneri: kaynak yetersizliği `max_lag_us`'u şişirir ve S3 atılma oranını etkiler. Bu yüzden EC2 1 saatlik ön kontrolünde lag dağılımı WSL ile karşılaştırılır.
- **S10 — ENGELLEYİCİ. TSC kararlılık eşiği.**
  - §3.1 kontrolleri tanımlandı: `constant_tsc` + `nonstop_tsc`, `current_clocksource=tsc`, mono/wall oranı ve ID sırasında mono monotonluğu.
  - Açık olanlar:
    - (i) Mono/wall oranı için kabul eşiği (ppm). Beklenen sapma kaynakları: 100 ms kalibrasyon ve NTP kayması [ÇIKARIM].
    - (ii) Eşik aşılırsa veya bayrak/saat kaynağı uygun değilse ne yapılacağı: instance değişimi, veri toplamanın başlamaması veya süreç dosyasının §9 TSC satırına göre tümüyle dışlanması.
    - (iii) Mono monotonluğu bozuk dosyanın aynı şekilde dışlanıp dışlanmayacağı.
  - Öneri: eşik, WSL ve EC2 ön kontrollerinde ölçülen orana bakılarak ama **karar verisi toplanmadan önce** yazılır. `analyze_dryrun.sh` oranı ppm olarak raporlar.
- **S11 — ENGELLEYİCİ. "> 60 s olay boşluğu" dışlamasının dayanağı.**
  - §9'daki "ardışık olaylar arasında > 60 s" kuralı, süreç veya bağlantı arızası olmadan da tetiklenebilir. Sakin piyasada 60 s boyunca |OBI| > 0.35 olmayabilir, ya da hiç tick gelmeyebilir. Bu durumda sakin dönemler sistematik olarak atılır. Bu bir seçim yanlılığıdır ve haftalık %10 kuralını da zorlayabilir [ÇIKARIM].
  - Seçenekler:
    - (a) Kural olduğu gibi kalır.
    - (b) Dışlama süreç/WS sağlık sinyaline dayandırılır. Sinyaller: süreç yeniden başlaması (yeni CSV dosyası, kapanış/başlangıç logları), §9 WS kopma satırları ve bir canlılık kaydı. Canlılık kaydı için örneğin `metrics.prom` 10 s'de bir yazılır [KOD `main.cpp:1567-1575`]; harici örnekleme gerektirir [ÇIKARIM].
    - (c) (b) uygulanır, > 60 s olay boşlukları yalnız bilgi olarak raporlanır.
  - Öneri: (b) veya (c). Ön kontrolde sembol başına > 60 s olay boşluğu sayısı ölçülür; `analyze_dryrun.sh` bunu raporlar. Seçim ve sağlık sinyallerinin tam tanımı **veri toplamadan önce** proje sahibince yazılır. Bu doküman kuralı kendiliğinden değiştirmez.

## 13. Uyarılar

- **Kârlılık kanıtlanmış değildir.** Kapı 1'i geçmek yalnız mid getirisinin maliyet varsayımını aştığını gösterir; işlenebilir avantaj göstermez.
- **P0'lar (CLAUDE.md §2):** KZ-P0-02 ve KZ-P0-03 emir yolunu etkiler; `dry_run=true`'da emir gönderilmediği için Kapı 1'i etkilemez [ÇIKARIM `main.cpp:460-461` yorumu]. **KZ-P0-01** ise ufuk zamanlamasını doğrudan etkiler: ufuklar ve `max_lag_us` `tsc_ns()` ile ölçülür, 5000 ms ufku ~4,29 s döngüsünü aşar. Düzeltme kaynakta mevcut: `time.hpp:71-74` 128-bit çarpım [KOD]. **Kullanılan binary'nin bu kaynaktan derlendiği** (sha256 + derleme kaydı + `tsc_overflow_test` geçişi) veri toplamadan önce doğrulanmalı ve §10.8'e yazılmalıdır. Doğrulanmazsa veri GEÇERSİZ ÖLÇÜM sayılır. Kapı 2 (`dry_run=false`) üç P0 kapanmadan başlamaz (§1.1).
- `strategy_core.hpp:227` yorumu `add()`'in `maybe_send_order`'dan çağrıldığını söylüyor. Gerçek çağıran `track_obi_signal` [KOD `:2298`]; yorum eski. `:2278` yorumundaki "process_book_ticker'ın başında" ifadesi de kesin değil (§2).
- WS her zaman prod verisi kullanır [KOD `ws_client.hpp:1165`]. Testnet bayrağı veri kaynağını değiştirmez.
