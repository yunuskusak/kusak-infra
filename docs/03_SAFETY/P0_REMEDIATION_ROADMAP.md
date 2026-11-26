# P0 Düzeltme Yol Haritası ve Çelişen Yapılar

> Kaynak: 2026-11-23 tarihli statik inceleme (`include/`, `src/`, `tools/`, `*.patch`/`*.rej`/`*.orig`).
> Derleme veya test **çalıştırılmadı**. Satır numaraları o günkü kaynak anlık görüntüsüne aittir; kod değişirse önce yeniden doğrula.
> Bu doküman düzeltme yapıldığı anlamına gelmez. KZ-P0-01/02/03 **AÇIK**tır (CLAUDE.md §2).
> Her faz için kod değişikliği ayrı talimat, test planı ve proje sahibi onayı gerektirir.
> Etiketler: **[KOD]** koddan doğrulandı · **[ÇIKARIM]** okumadan çıkarım · **[DOĞRULANMADI]** kanıt yok.

## 1. Çelişen yapılar

| # | Çelişki | Yer | Etiket |
|---|---|---|---|
| C1 | Başlıkta `startup_decision(const StartupProbe&)` var, test eski 5 argümanlı çağrıyı kullanıyor (`startup_decision(true, 0, 0.0, 5.0, 30.0)`). Test derlenmez. `REFUSE_ORDERS_UNVERIFIED`, `REFUSE_CANCEL_STUCK` ve `REFUSE_BALANCE_UNVERIFIED` testte kapsanmıyor. Testin kullandığı diğer API'ler (`Limits`, `Order`, `cap_qty`, `total_exposure`, `pending_remainder`, `RateGate`) başlıkla uyumlu. | `include/order_guard.hpp:142-170`, `tools/order_guard_test.cpp:131-138` | [KOD] — **Faz 0'da giderildi (2026-11-23): 63 ok / 0 FAIL, WSL g++ 13.3** |
| C2 | İki ayrı `tsc_ns()` var. `ws_client.hpp` içindeki `double ns_per_tick` kullanıyor ve `g_tsc_ns_mult`'e bağlı değil. WS sessizlik kontrolü (`MAX_SILENCE_NS`) bu ayrı saati kullanıyor. | `include/ws_client.hpp:254`, `include/infra/time.hpp:75` | [KOD] |
| C2a | Çakışma yok: ws sürümü `kusak::ws` namespace'inde ve global `tsc_ns()` ile aynı ada sahip değil. Ws sürümü base çıkarıp `double` ile çarpıyor, epoch ns döndürüyor ve taşmıyor. Semantik farklı olduğu için birleştirme KZ-P0-01'in parçası değil. Faz 4'te ele alınacak. | `ws_client.hpp:68,219-258` | [ÇIKARIM] (2026-11-23 güncellendi) |
| C3 | Firewall yorumu "Yalniz SEND: EXIT azaltici…" diyor, fonksiyon ise `t.kind`/`t.is_exit`'e hiç bakmıyor. | `include/strategy_core.hpp:583-611` | [KOD] |
| C4 | `active_order_id` yorumu "borsa ID" diyor, gerçekte yerel sayaç tutuyor (`++ctx.fsm_entry_seq`). | `include/strategy_core.hpp:360`, `:1351` | [KOD] |
| C5 | CANCEL borsa ID'sini slottan okuyor, POLL ise `ctx.active_order_id` kullanıyor. Aynı sorun iki yolda farklı çözülmüş. | `strategy_core.hpp:543,931,964`; `src/main.cpp:1392`, `strategy_core.hpp:1910` | [KOD] |
| C6 | `paper_runner`, `optimize_runner` ve `regime_runner` hiçbir CMake hedefinde yok. `record_runner.cpp` da listede yok. | `CMakeLists.txt:62-67`, `:98-101` | [KOD] |
| C7 | `vpin_debug_hook.patch` uygulanmış, geçici debug `cout` araştırma kodunda kalmış. | `src/app/research/replay_runner.cpp:129-140` | [KOD] |
| C8 | Yama kalıntılarının durumu karışık; ayrıntı §2'de. | kök, `include/`, `src/` | karışık |
| C9 | Test kaynağında sabit API key/secret var. Proje sahibi bunları testnet anahtarı olarak beyan etmiş; değerler burada yazılmaz. | `tools/mvp_order_bridge_test.cpp:30-31` | [KOD] — **Faz 0'da kaynaktan çıkarıldı (2026-11-23), `getenv` + fail-closed exit 2 doğrulandı; anahtar rotasyonu proje sahibinde** |
| C10 | `make_research_vpin_config(base.vpin_buckets, 0.8)` çağrısının yeni otomatik bucket moduyla (`bucket_volume_scaled == 0`) uyumu belli değil. | `src/app/research/replay_runner.cpp:87` | [DOĞRULANMADI] |
| C11 | `src/main.cpp.orig` eski yerel ID davranışını içeriyor. Doğruluk kaynağı olarak kullanılmamalı. | `src/main.cpp.orig` | [KOD] |
| C14 | MARKET EXIT borsaya hiç gitmiyor. Order thread MARKET emirde `req.price`'ı set etmiyor, `new_order` ise yerel filtrede `validate_order(price=0)` çalıştırıyor. Sonuç: PRICE_TOO_LOW / NOTIONAL_TOO_LOW → REJECTED. Firewall EXIT'i geçirse bile pozisyon kapanmıyor. Sahte borsa testinde I4, I5 ve I6 FAIL. **Sınıf: KZ-P0-03 kapsamı.** Proje sahibi 2026-11-24'te karar verdi. | `src/main.cpp:440-441,504,788`, `include/binance_rest_client.hpp:438-447` | [KOD] + test (2026-11-24) |
| C15 | Transport hatası ve zaman aşımı "gönderilmedi" (REJECTED) olarak sınıflanıyor. `new_order` hiçbir yolda `error.code=0` bırakmadığı için `main.cpp:~580`'deki `NET_UNKNOWN` dalına ulaşılamıyor. Sonuçta UNKNOWN, kill switch ve POLL hiçbiri devreye girmiyor. CLAUDE.md §3.4 ihlali. Sahte borsa testinde I2 FAIL. **Sınıf: KZ-P0-02 kapsamı.** Proje sahibi 2026-11-24'te karar verdi. | `include/binance_rest_client.hpp` `new_order` hata yolları, `src/main.cpp:~575-590` | [KOD] + test (2026-11-24) |
| C16 | SEND sonucu kaybolursa slot süresiz SUBMITTED kalıyor. `enforce_timeouts` ve `check_kill_switch` SUBMITTED'ı işlemiyor; KZ-P0-02 kuralı gereği bu durumda POLL da üretilmiyor. Sahte borsa testinde I10 FAIL. **Sınıf: P1** (Faz 3.6, proje sahibi 2026-11-24). | `strategy_core.hpp:~567,785-900`, `main.cpp:304,588` | [KOD] + test |
| C17 | `live_order_client` yoksa SEND/EXIT sonuçsuz düşüyor (`continue`). Sahte borsa testinde I11 FAIL. **Sınıf: P1** (Faz 3.6, proje sahibi 2026-11-24). | `src/main.cpp:432` | [KOD] + test |
| C18 | FUTURES startup FLATTEN fail-closed değil. `su_flatten_reduce_only` spot bakiye (`/api/v3/account`) ve sabit SELL kullanıyor; FUTURES için yön ve reduceOnly belirlenmiyor. Bu dala iki yoldan giriliyor: doğrudan FLATTEN kararı ve ADOPT'ta quote yetersiz olduğunda `[[fallthrough]]`. İkisi de `REFUSE_FUTURES` kontrolünden geçmiyor. Planlanan düzeltme: FLATTEN dalı FUTURES'ta başlatmayı reddeder. **Sınıf: KZ-P0-03 kapsamı, Faz 3.6** (proje sahibi 2026-11-24). Startup yolu `http_client` istediği için birim testi yok; kanıt statik. | `src/main.cpp:748-803,1035,1057-1061` | [KOD]; FUTURES emir etkisi [ÇIKARIM] (2026-11-24) |
| C13 | Startup ADOPT_POSITION, `ctx.position` alanını doldurmuyor. Sonuç olarak adopt edilen borsa pozisyonu için `fw_pos_side=0` ve `verified=false` kalıyor; motor bu pozisyonu EXIT edemiyor. Ayrıca `fw_position_usd=0` olduğundan SEND maruziyet kontrolü bu pozisyonu saymıyor. **Sınıf: P1. Proje sahibi bu sınıflandırmayı 2026-11-23'te yaptı.** Faz 3.5'te C12 ile birlikte ele alınacak. | `src/main.cpp:~1029-1037` | [KOD]; maruziyet etkisi [ÇIKARIM] (2026-11-23) |
| C12 | `FlatOrderMap::slots[]` ilklenmiyor. `OrderSlot` alanlarının varsayılan değeri yok ve `flat_order_map` init'siz tanımlanmış. Testte heap yeniden kullanılınca senaryolar arası sızıntı gözlendi. Üretimdeki etkisi bilinmiyor; tanımsız davranış. **Sınıf: P1 (yüksek öncelikli güvenlik maddesi). Proje sahibi bu sınıflandırmaya 2026-11-23'te karar verdi.** P0 düzeltmelerine karıştırılmayacak; P0'lardan sonra ayrı ele alınacak (Faz 3.5). | `include/engine/types.hpp:235-249,278`, `strategy_core.hpp:359`, `main.cpp:1112` | [KOD] + test; üretim etkisi [ÇIKARIM] (2026-11-23) |

## 2. Yama kalıntıları

| Dosya | Hedef | Durum | Etiket |
|---|---|---|---|
| `vpin_bucket.patch` + `vpin_velocity_tracker.hpp.rej` | VPIN otomatik bucket hedefi | UYGULANMIŞ (`calib_trades`, `vpin_velocity_tracker.hpp:69,202`). `.rej` artık gereksiz. | [KOD] |
| `vpin_spillover.patch` | Bucket taşma hacmi aktarımı | UYGULANMIŞ (`vpin_velocity_tracker.hpp:216,303`) | [KOD] |
| `vpin_debug_hook.patch` + `replay_runner.cpp.rej` | VPIN debug çıktısı | UYGULANMIŞ (C7). `.rej` gereksiz. | [KOD] |
| `src/main.cpp.rej` (FIX-QUANT-6) | RegimeSnapshot besleme | Mantık `strategy_core.hpp:333,1962` civarına taşınmış. `main.cpp` içinde etiket yok. | [KOD] |
| `include/strategy_core.hpp.rej`, `include/config.hpp.rej` | — | Durum bilinmiyor | [DOĞRULANMADI] |
| `kusak_config_mvp.json.rej/.orig` | — | Gizli değer içerebileceği için açılmadı | [DOĞRULANMADI] |
| `fsm.patch`, `phase2_surgical.patch`, `rollback.patch`, `kill_switch.patch`, `amnesia.patch`, `phase5_obi.patch`, `strategy_core_phase0.patch`, `kusak_research_v13.patch`, `KUSAK-INFRA_ALL_CHANGES.patch` | `types.hpp`, `strategy_core.hpp`, `main.cpp` vb. | Hunk düzeyinde karşılaştırılmadı. `rollback.patch` ile `fsm.patch`/`phase2_surgical.patch` aynı dosyalara dokunuyor, çelişki olabilir. | [DOĞRULANMADI] |

## 3. Yol haritası

Her faz şu protokolle yürür: sorun → beklenen davranış → test → değişiklik → testi çalıştır → sonucu kaydet (CLAUDE.md §3.6).

Sıra: **Faz 0 → P0-01 → P0-02 → P0-03 → Faz 3.5 (P1: C12) → Faz 3.6 (C14-C18) → testnet → Faz 4.**

Gerekçe: P0-01 zaman tabanıdır. Fill/maker timeout, kill switch, exit retry ve staleness kontrollerinin hepsi bu saate dayanır. P0-02 ve P0-03 testleri de doğru zamana ihtiyaç duyar, o yüzden önce P0-01 kapanmalı [ÇIKARIM].

### Faz 0 — Test zemini (P0 kapatmaz, ön koşul)
- **C1:** `tools/order_guard_test.cpp` `StartupProbe` API'sine taşınır. Üç `REFUSE_*` dalı için test eklenir. Mevcut testlerin beklentisi gevşetilmez.
- **C9:** Sabit anahtar kaynaktan çıkarılır ve env'den okunur. Testnet anahtarını döndürme işi proje sahibine aittir.
- **Build:** WSL'de `cmake` ile derlenir, mevcut testler çalıştırılır. Sonuç (geçti / kaldı / derlenmedi) olduğu gibi kaydedilir.
- **Kapı:** güncel kaynakla derlenen bir test seti olmalı.

### Faz 1 — KZ-P0-01 TSC taşması
**Kök neden [KOD]**
- `include/infra/time.hpp:75` içinde `(tsc * g_tsc_ns_mult) >> 32` hesabı yapılıyor.
- `g_tsc_ns_mult` `uint64_t` tipinde (`time.hpp:36`). Tanımı `src/main.cpp:107`, değeri `:1058`'de atanıyor. Kalibrasyon formülü `ns_mult = (delta_ns << 32) / delta_tsc` (`time.hpp:65`).
- Depoda `__int128`, `mulhi` veya `_umul128` tabanlı bir zaman yardımcısı yok.

**Taşma eşiği [ÇIKARIM]**
- Çarpım `tsc ≥ 2^64 / mult` olduğunda 64 biti aşar.
- 3 GHz'te mult yaklaşık 1,4e9'dur; eşik yaklaşık 2^33 tick, yani ~3 saniyedir.
- Sunucu uptime'ı büyük olduğunda TSC değeri zaten bu eşiğin çok üstündedir.

**Kırmızı test kanıtı (2026-11-23)**
- `tools/tsc_overflow_test.cpp`, mevcut kodda 2/2 çalıştırmada exit 1 ve fails=3 verdi.
  - T2 monotonluk: 6 s pencerede 1 geri sıçrama oldu.
  - T3: `Δtsc_ns − Δmono ≈ −2^32` ns.
  - T4: en büyük `tsc_ns()` değeri 2^32'nin altında kaldı.
- Ortam: WSL2, i7-13700HX, mult ≈ 1,864e9.
- Ham TSC, `2^64/mult` eşiğini aştı. Taşma ve ~4,29 s'de başa sarma ölçümle doğrulandı [KOD+test].
- Bu yalnızca birim seviyesinde bir kanıttır.

**Düzeltme (2026-11-23) [FIX-P0-01]**
- `time.hpp:71-74` satırlarına `tsc_to_ns(tsc, mult)` eklendi. Fonksiyon 128-bit ara çarpım kullanıyor.
- `tsc_ns()` artık bu fonksiyonu çağırıyor.
- `tsc_overflow_test` 2/2 çalıştırmada 22 ok / 0 FAIL verdi. Faz 0 testleri değişmedi.
- Tam build 0 hata verdi. Uyarı sayısı 12'den 13'e çıktı; yeni uyarı `time.hpp:73` satırındaki `-Wpedantic __int128` uyarısı.
- Seviye: **birim testi geçti**. KZ-P0-01 AÇIK; kapanış kapısı için §4'e bakın.

**Beklenen davranış:** her TSC değeri için (günlerce uptime dahil) monoton ve taşmasız ns dönüşümü.

**Değişiklik önerisi**
- 128-bit ara çarpım: `(uint64_t)(((unsigned __int128)tsc * mult) >> 32)`. GCC/Clang x86-64 destekler; depoda `__int128` kullanımına örnek `vpin_velocity_tracker.hpp` içinde var.
- İsteğe bağlı: kalibrasyon anında `tsc_base` çıkarılır.
- C2 giderilir: `ws_client.hpp` tek zaman kaynağına (`infra/time.hpp`) bağlanır.

**Testler**
- TSC = 2^32, 2^33, 2^40 ve 2^56 civarında çıktı doğru ve monoton olmalı.
- Eski taşma noktalarını kesen aralıklarda delta doğru çıkmalı.
- `clock_gettime` ile karşılaştırıldığında sapma tolerans içinde kalmalı. Tolerans testten **önce** sabitlenir.
- Sahte mult = `1<<32` (`phase2_fsm_test.cpp:2`) yanında gerçekçi mult (~1,4e9) ile de test edilir.

**Regresyon etki alanı [KOD]**
- `include/engine/types.hpp:343,358,376,419,423,472` (`last_update_ns`).
- `strategy_core.hpp:423` (`current_time_ns`). Kullanıcıları:
  - fill/maker timeout: 507, 534
  - kill switch `tripped_ns`: 619
  - exit retry: 793, 1793-1869
  - `awaiting_fill_since_ns`: 1423
- `src/main.cpp:303,330,443,478,494,594` (`ts_recv_ns`).
- `src/main.cpp:1313-1382` (flush ve rapor döngüleri).
- `RateGate` içindeki `gate_now_ms()` kaynağı [DOĞRULANMADI].

### Faz 2 — KZ-P0-02 İlk POLL yerel ID
**Kök neden [KOD]**
- SEND, `order_id = local_order_id = active_order_id` (yerel ID) ile üretiliyor (`strategy_core.hpp:1437-1438`).
- POLL görevi `pt.order_id = ctx.active_order_id` ile üretiliyor (`src/main.cpp:1392`, `strategy_core.hpp:1910`).
- Order thread bunu `symbol=…&orderId=…` sorgusuna çeviriyor (`main.cpp:316-318`, `:124-136`). `origClientOrderId` hiç kullanılmıyor.
- `active_order_id` yalnızca iki yerde borsa ID'siyle güncelleniyor: FILLED (`:1629`) ve POLL PENDING (`:1782`).
- `OrderSlot.exchange_order_id` mevcut (`types.hpp:247`). `OrderTask.client_oid` de mevcut (`types.hpp:185`) ve SEND'de kullanılıyor (`main.cpp:425`).

**Kırmızı test kanıtı (2026-11-23)**
- `tools/poll_id_test.cpp`, `drain_orders_on_shutdown` yolunu test ediyor. Mevcut kodda 2/2 çalıştırmada exit 1 ve fails=3 verdi:
  - P1: UNKNOWN durumunda, borsa ID'si yokken POLL `orderId` olarak yerel `1` değerini gönderiyor.
  - P2: SUBMITTED durumunda, borsa ID'si yokken POLL yine üretiliyor.
  - P3: Slotta borsa ID'si varken bile POLL yerel ID'yi kullanıyor.
  - P4 (yeşil): `HTTP400` yanıtı UNKNOWN'u erken kapatmıyor.
- `main.cpp:1386-1408` ve `:317` testten çağrılamadığı için statik kanıt olarak kayıtlı.

**Düzeltme (2026-11-23) [FIX-P0-02]**
- `build_poll_task` eklendi (`strategy_core.hpp:493-520`). Kimlik sırası:
  1. Slottaki borsa ID'si.
  2. Yalnızca UNKNOWN durumunda `active_client_oid`.
  3. İkisi de yoksa POLL üretilmez.
- `include/engine/poll_query.hpp` eklendi. Kimliği olmayan POLL için `NET_NOID` döner. `-2013` yanıtı `HTTP400_2013` olarak işaretlenir; bu da belirsiz sayılır, durum UNKNOWN kalır.
- `poll_id_test` 52/0 verdi. Faz 0/1 testleri değişmedi. Build 13 uyarı ile aynı kaldı.
- Seviye: **birim testi geçti**. KZ-P0-02 AÇIK.
- Test edilmeyen kısımlar:
  - Giriş SEND yolunda `active_client_oid` ataması.
  - Order thread'in gerçek HTTP ile `-2013` ayrıştırması.
  - `main.cpp` döngüsündeki POLL bloğu.
- Sonraki seviye: `SimulatedExchangeClient.hpp` ile sahte borsa entegrasyonu. Gecikmeli SEND ve SEND sonucunun hiç dönmemesi senaryoları da burada denenecek.

**Etkisi [ÇIKARIM]:** SEND zaman aşımına uğrarsa durum UNKNOWN olur (`strategy_core.hpp:1613`). Bu durumda POLL yerel sayacı `orderId` diye gönderir. Borsa ya -2013 (emir yok) döner ya da sorgu başka bir emre denk gelir.

**Beklenen davranış**
- Yerel ID **asla** `orderId` olarak gönderilmez.
- POLL önce slottaki `exchange_order_id`'yi kullanır. CANCEL'de aynı yöntem zaten var (`strategy_core.hpp:543`).
- Borsa ID'si yoksa `origClientOrderId=<client_oid>` ile sorgulanır.
- İkisi de yoksa fail-closed: UNKNOWN korunur, yeni risk alınmaz, uyarı loglanır.

**Değişiklik önerisi**
- POLL üretim noktaları (`main.cpp:1392`, `strategy_core.hpp:1910`) ID'yi slottan okur.
- `clientOrderId` sorgusu için bir yardımcı eklenir.
- C4 yorumu düzeltilir.
- SEND "NEW" dalında slot işlemesi (`types.hpp:482`) doğrulanır [ÇIKARIM].

**Testler**
- SEND zaman aşımı → UNKNOWN → POLL sorgusu `origClientOrderId` içerir, yerel sayacı içermez.
- SEND NEW → POLL borsa ID'sini kullanır.
- -2013 yanıtı UNKNOWN'u kendiliğinden "gönderilmedi" yapmaz (`FSM_GUIDE.md`).
- Sahte borsada yerel ID'nin başka bir emre denk gelme senaryosu denenir.

### Faz 3 — KZ-P0-03 EXIT firewall reddi
**Kök neden [KOD]**
- `final_order_firewall` (`strategy_core.hpp:584-611`) şu kontrolleri yapıyor: BAD_INPUT, POSITION_CAP, ORDER_CAP, SYMBOL_MISMATCH, FILTER, EXPOSURE_CAP (`fw_position_usd + notional > pos_cap`, 606) ve BUY için INSUFFICIENT_BALANCE (608).
- `src/main.cpp:429-435` firewall'u SEND ve EXIT için koşulsuz çağırıyor. Red durumunda `FW_REJECT` ile `REJECTED` sonucu üretiliyor (`:438-445`).
- Kill switch flatten yolu `send_exit_order` ile aynı firewall'dan geçiyor (`strategy_core.hpp:1001-1029`).
- `OrderTask` içinde `kind` ve `is_exit` alanları var (`types.hpp:177-181`). `reduce_only` alanı yok.

**Etkisi [ÇIKARIM]**
- Pozisyon cap'e yakınken EXIT'in notional'ı mevcut maruziyete ekleniyor; bu yüzden EXPOSURE_CAP aşılıyor ve azaltıcı EXIT reddediliyor.
- Short kapatan BUY bakiye kontrolüne takılabilir.

**Kırmızı test kanıtı (2026-11-23)**
- `tools/exit_firewall_test.cpp` mevcut kodda 2/2 çalıştırmada exit 1 ve fails=3 verdi.
- E1 (SPOT LONG, SELL EXIT), E2 (FUTURES SHORT, BUY EXIT) ve E4 (`ks_service` flatten EXIT) üçü de **EXPOSURE_CAP** ile reddedildi.
- E3 SEND regresyonu 5/5 ok.
- E5 bayat maruziyeti gösterdi: pozisyon 15 USD'ye düştüğü halde `fw_position_usd` 29 USD'de kaldı.
- Statik kanıt: `main.cpp:452` firewall'u koşulsuz çağırıyor; `:469` red durumunda kill switch'i tetikliyor [KOD].
- E2'de EXPOSURE_CAP aşılsa bile sonraki INSUFFICIENT_BALANCE kontrolü de BUY EXIT'i reddeder [ÇIKARIM]. Bu ayrıca test edilmedi.

**Düzeltme (2026-11-23) [FIX-P0-03]: kısmen kırmızı**
- `exit_firewall` dalı eklendi. Pozisyon kopyası `fw_pos_*`, `reduceOnly` (yalnız FUTURES EXIT) ve verdict logu eklendi.
- X1-X5 geçti; X4 uçtan uca flatten senaryosu da dahil. E3 regresyonu ve Faz 0/1/2 testleri geçti. Build 13 uyarı ile aynı kaldı.
- **E1/E2/E4 hâlâ FAIL** (EXIT_POS_UNKNOWN). Testin kurulumu pozisyonu doğrulanmamış bir yoldan kuruyor. Proje sahibinin kararı bekleniyor.
- Plandan sapmalar:
  - `verified` bayrağı, kısmi dolumla biten terminal sonuçlarda da true yapılıyor.
  - Bu bayrak yapışkan; bir kez true olduktan sonra false'a dönmüyor.
- Yeni bulgu C13: startup ADOPT, `ctx.position` alanını doldurmuyor.
- **Takip (2026-11-23):** E1/E2/E4 kurulumu gerçek FILLED yoluna taşındı; beklentiler değişmedi. `verified` artık yapışkan değil; kuralı `FwVerify` ile tanımlanıyor:
  - Pozisyon kapanırsa veya bir emir UNKNOWN olursa false olur.
  - Borsanın onayladığı bir sonuç gelince true olur.
- `exit_firewall_test` sonucu **63/0** (E1-E5, X1-X5, V1-V5). Faz 0/1/2 testleri değişmedi. Build 13 uyarı ile aynı kaldı.
- Seviye: **birim testi geçti**. KZ-P0-03 AÇIK.

**Beklenen davranış:** EXIT yalnızca kanıtlanmış azaltıcı emir olduğunda geçer. Genel firewall kontrolleri kaldırılmaz (CLAUDE.md §3.3).

**Değişiklik önerisi**
- `kind == EXIT` için ayrı dal açılır.
- Bu dalda korunan kontroller: BAD_INPUT (0/NaN), SYMBOL_MISMATCH, FILTER.
- Zorunlu yeni kontroller:
  - Yön mevcut pozisyonun tersi olmalı.
  - `qty ≤ |pozisyon qty|` olmalı.
  - Pozisyon doğrulanmış olmalı; bilinmiyorsa red.
  - SPOT SELL'de baz varlık envanteri `≥ qty` olmalı.
- EXPOSURE_CAP ve POSITION_CAP EXIT'te toplama ile değil, "sonrası maruziyet ≤ öncesi" kuralıyla değerlendirilir.
- FUTURES'ta `reduceOnly=true` gönderilir.
- SEND dalı aynen kalır. C3 yorumu koda uydurulur.
- `main.cpp:446` sonrasındaki red akışı okunacak [DOĞRULANMADI]. EXIT reddi gizlenmez: alarm üretilir, `exit_retry_not_before_ns` backoff'u ile tekrar denenir, yeni giriş engellenir.

**Testler**
- Pozisyon cap'teyken EXIT geçer.
- EXIT qty pozisyondan büyükse red.
- Yanlış yön red.
- Pozisyon bilinmiyorsa red.
- NaN veya 0 değer red.
- SEND cap'teyken hâlâ red (regresyon).
- Kill switch flatten'ı uçtan uca: önce dry-run, sonra sahte borsa.

### Faz 3.5 — P1 sertleştirme (sahte borsa ve testnet'ten ÖNCE; proje sahibi 2026-11-23'te onayladı)
**Kapsam (6 madde):**
- C12
- C13
- `fw_pos_*` için seqlock
- EXIT `floor_qty` ve firewall toleransı `step*1e-3`
- `build_order_params` public yapılması ve ham `reduceOnly=true&` testi
- `poll_id_test` CHECK sayısının determinizmi

**Kırmızı test kanıtı (2026-11-24):** `tools/phase35_test.cpp` fails=5 verdi.
- R1: 0x01 ile doldurulmuş bellekte 4096/4096 slot çöp kaldı ve `insert` false döndü.
- R2a: EXIT qty (0.29001) pozisyondan (0.290006) büyük çıktı.
- R2b: pozisyon + 1 step'lik EXIT PASS aldı.
- R3: `floor_qty(0.3, 0.1)` sonucu 0.2 çıktı.
- R4: `poll_id_test` 10 çalıştırmada 52/0 verdi, sabit kaldı.
- C13, seqlock ve `build_order_params` için kanıt statik [KOD].

**Düzeltme (2026-11-24) [FIX-P35]:** altı maddenin tamamı uygulandı.
- `phase35_test` 37/0 verdi.
- `poll_id_test` 10 çalıştırmanın hepsinde 54/0 verdi.
- Mevcut tüm testler ve tam build (13 uyarı) değişmedi.
- FUTURES ADOPT fail-closed. SPOT ADOPT `floor_qty(base_free)` kullanıyor; `base_locked ≥ step` ise reddediliyor.
- Test edilmeyenler:
  - `main.cpp` ADOPT dalının gerçek çalıştırması.
  - İki thread'li seqlock yarışı.
  - ADOPT'ta quote yetersizken FLATTEN'a düşme (fallthrough) yolunun FUTURES davranışı [DOĞRULANMADI].

#### C12 `FlatOrderMap` ilklenmemesi
- **Sorun:** `OrderSlot` alanlarının varsayılan değeri yok ve `StrategyContext::flat_order_map` init'siz. `slots[]` bu yüzden belirsiz bellekle başlıyor [KOD]. Testte heap yeniden kullanılınca senaryolar arası sızıntı gözlendi.
- **Beklenen davranış:** her slot `EMPTY`, `exchange_order_id=0` ve `fsm_state` başlangıç durumunda başlamalı. Bu, ayırma yönteminden bağımsız olmalı (`make_unique`, stack, yeniden kullanılan heap).
- **Test (önce kırmızı):** bellek `0xAB` gibi bir desenle doldurulur, üzerine `placement-new StrategyContext` kurulur. Ardından `find` ve `active_count` boş map gibi davranmalı.
- **Düzeltme önerisi:** `OrderSlot` alanlarına `{}` varsayılan değeri verilir ya da `FlatOrderMap` value-init edilir. 64 B `static_assert` ve `alignas(64)` korunur.
- Testteki `memset` izolasyonu (`poll_id_test.cpp` `fresh()`) düzeltmeden sonra kaldırılabilir; kaldırma ayrı karar.
- **C13 (P1):** startup ADOPT_POSITION, `ctx.position` alanını doldurmuyor. Bu yüzden adopt edilen pozisyon EXIT edilemiyor ve maruziyet hesabına girmiyor.
  - Beklenen: adopt edilen pozisyon borsa bakiyesinden `ctx.position`'a yazılmalı ve `fw_publish_exposure` ile yayınlanmalı. Yayında kanıt olarak startup uzlaştırması kullanılmalı.
  - Test (önce kırmızı): ADOPT sonrası `position.is_open()` ve `fw_position_usd > 0` olmalı. Bu pozisyon için EXIT firewall'dan PASS almalı.
  - Geçici önlem (ayrı karar): düzeltilene kadar ADOPT yerine FLATTEN_REDUCE_ONLY kullanılabilir.

### Faz 3.6 — C14, C15, C16, C17, C18 (proje sahibi 2026-11-24'te onayladı)
**Kırmızı test kanıtı (2026-11-24).** Yeni `tools/phase36_test.cpp` davranış seviyesinde ve mevcut kodla derleniyor.
- Sonuç: 71 ok / 49 FAIL.
- FAIL'lerin dağılımı: M1/M4/M6 (C14), U1a-h (C15), T3 (C16), L1-L3 (C17).
- `sim_exchange_test` 21 FAIL verdi (I2, I4-I6, I10, I11).

**Düzeltme (2026-11-24) [FIX-P36]:**
- **C14:** `validate_market_order` eklendi. LOT_SIZE ve referans fiyatla notional kontrolü korunuyor; fiyat filtreleri atlanıyor; geçersiz referansta red.
- **C15:** `include/engine/send_outcome.hpp` eklendi. Belirsiz sonuç `NET_UNKNOWN` → UNKNOWN + kill switch + `origClientOrderId` POLL zincirine gidiyor.
  - Belirsiz sayılanlar: transport, 5xx, kodsuz 4xx, orderId'siz 2xx, exception, -1006/-1007.
- **Retry:** emir POST'u tek deneme yapıyor (`allow_retry=false`). RateGate değişmedi.
- **C16:** `SUBMITTED_LOST_TIMEOUT_NS = 40 s`. Süre aşılınca borsa ID'siz SUBMITTED → UNKNOWN + kill switch.
- **C17:**
  - İstemci yoksa `REJECTED NO_CLIENT`.
  - Bütün sonuç teslimleri sınırlı spin ile yapılıyor; teslim başarısızsa kill switch açılıyor.
  - `g_fw_result_lost` strateji tarafında okunuyor.
- **C18:** FUTURES'ta FLATTEN dalı başlatmayı reddediyor.

**Sonuç (WSL, 2×):**
- `phase36_test` 120/0, `sim_exchange_test` 65/0. Bütün regresyon testleri yeşil.
- Tam build 0 hata, 13 uyarı (değişmedi).

**Seviye:** birim + bellek içi sahte borsa. Test edilmeyenler:
- retry kaldırma (somut curl sınıfı)
- C18 startup yolu
- gerçek curl/TLS
- testnet

KZ-P0-02 ve KZ-P0-03 **AÇIK** (§4).

### Faz 3.7 — C19a, C19b, C20 (testnet T0 bulgusu; proje sahibi 2026-11-24'te onayladı)
**Bulgu:** testnet T0'da ilk SEND'in sonucu `http=-1` (bağlantı aşaması) oldu. Motor emri UNKNOWN'a aldı ve kill switch açıldı. `allOrders` emrin borsaya hiç ulaşmadığını gösterdi. POLL'a gelen -2013 UNKNOWN'u çözmediği için motor süresiz SAFE MODE'da kaldı.

**Düzeltme [FIX-P37]:**
- **C19a:** curl tek bayt bile gönderilmediğini kanıtlarsa (`pretransfer=0` ve `request_size=0`) sonuç `NOT_SENT` (REJECTED) olur. Kanıt yoksa sonuç yine belirsiz.
- **C19b:** UNKNOWN slot, borsa ID'si yok, art arda ≥3 POLL -2013 ve UNKNOWN'a girişten bu yana ≥ recvWindow + 10 s ise emir REJECTED olarak çözülür (`NOTFOUND_RSLV`). Kill switch kilitli kalır.
- **C20:** curl veya borsa gövdesi, POLL hataları ve C19b ilerlemesi loglanır.

**Sonuç (WSL, 2×):**
- `phase37_test` 39/0 (kırmızıda 24/15).
- `curl_sent_probe_test` 11/0 (gerçek libcurl 8.5.0, yalnız localhost).
- Bütün regresyon testleri yeşil. Build 13 uyarı.

**Seviye:** birim + bellek içi sahte borsa + yerel libcurl. Bağlantı zaman aşımı, TLS hatası ve gerçek testnet tekrarı henüz test edilmedi.

### Faz 4 — Hijyen (P0 sonrası, ayrı onay)
- C6: CMake hedefleri gerçek dosyalarla eşlenir.
- C7: debug `cout` kaldırılır.
- C8 ve C11: uygulanmış `.rej` ve `.orig` dosyaları arşive taşınır, [DOĞRULANMADI] yamalar hunk düzeyinde karşılaştırılır.
- C10 doğrulanır.

## 4. P0 kapanış kapısı
`PROGRESS.md`'de AÇIK → KAPALI geçişi yalnızca şu sırayla yapılır:
1. Birim testi geçer.
2. Sahte borsa entegrasyon testi geçer.
3. Testnet `dry_run=false` gözlemi yapılır.
4. Proje sahibi onaylar.

Bir seviyenin başarısı sonraki seviyeyi kanıtlamaz (CLAUDE.md §3.11).
