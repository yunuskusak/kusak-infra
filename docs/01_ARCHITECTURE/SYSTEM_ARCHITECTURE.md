# SYSTEM_ARCHITECTURE — Thread, SPSC, Pinning, Bellek, Zaman

Etiketler: **[KOD]** · **[ÇIKARIM]** · **[DOĞRULANMADI]**

Kapsam: `src/main.cpp`, `include/engine/types.hpp`, `include/engine/market_event.hpp`, `include/infra/spsc_ring.hpp`, `include/infra/time.hpp`, `include/strategy_core.hpp`.

## 1. Genel görünüm

```text
 market_thread                strateji thread (= main)                order_thread
 ─────────────                ────────────────────────                ────────────
 BinanceWsClient::run ──try_push──► market_ring ──drain(128)──► process_agg_trade/
                                    SpscRing<131072>             process_book_ticker
                                                                     │ maybe_send_order / send_exit_order
                                                                     │ enforce_timeouts / ks_cancel_all / POLL
                                                                     ▼
                                              order_channel.tasks ──────────────► pop_task
                                              OrderSpscRing<OrderTask,1024>        │
                                                                                   │ SEND/EXIT:
                                                                                   │ final_order_firewall
                                                                                   │ → KS kapısı → new_order
                                              order_channel.results ◄─────────────┘
                                              OrderSpscRing<OrderResult,1024>
                                                   │
                                                   ▼
                                       dispatch_order_result (FSM)

                                       telemetry_cmd_ring
                                              │
                                              ▼
                                       telemetry_thread
```

## 2. Thread envanteri [KOD]

| Thread                          | Başlangıç                | CPU pin               | Görev                                                                       | Notlar                                                         |
| ------------------------------- | ------------------------ | --------------------- | --------------------------------------------------------------------------- | -------------------------------------------------------------- |
| Strateji (main)                 | `main()` sıcak döngüsü   | `KUSAK_STRATEGY_CORE` | Piyasa verisi, karar, FSM, kill switch servisi, POLL üretimi, config reload | Pin, market thread başlatıldıktan sonra ana thread'e uygulanır |
| `market_thread`                 | `market_ws->run(g_stop)` | Yok                   | WS okuma, callback'ten `market_ring.try_push`                               | Callback'ler `noexcept`                                        |
| `order_thread`                  | `run_order_thread(ctx)`  | `KUSAK_ORDER_CORE`    | SEND/EXIT/CANCEL/POLL REST çağrıları                                        | Bloklayan libcurl çağrıları                                    |
| `telemetry_thread`              | Lambda                   | Yok                   | `FLUSH_CSV` / `STOP` komutları ve 1 s flush                                 | Boş döngüde `_mm_pause()` kullanır; uyumaz                     |
| `ExchangeInfoManager::bg_loop`  | `ei_mgr->start()`        | Yok                   | Periyodik exchangeInfo → `FilterValidator`                                  | Mutex ve condition variable kullanır                           |
| Bekçi thread'leri               | `join_with_timeout`      | Yok                   | Kapanışta thread join'ini zaman aşımına bağlar                              | Yalnız kapanışta                                               |
| `BinanceHttpClient::io_thread_` | `start_io_thread()`      | —                     | —                                                                           | `main.cpp` bu fonksiyonu çağırmaz                              |

Strateji ve order thread'leri sıcak döngüde kademeli `_mm_pause()` ve `tsc_pause_cycles()` kullanır. `condition_variable` tabanlı bekleme sıcak döngünün ana mekanizması değildir.

## 3. SPSC halkalar [KOD]

| Halka                   | Tip                               | Üretici → Tüketici         | Kullanılabilir kapasite | Eleman              | Tampon  |
| ----------------------- | --------------------------------- | -------------------------- | ----------------------- | ------------------- | ------- |
| `market_ring`           | `SpscRing<131072>`                | `market_thread` → strateji | 131071                  | `MarketEvent` 64 B  | 8 MiB   |
| `order_channel.tasks`   | `OrderSpscRing<OrderTask,1024>`   | strateji → order           | 1023                    | `OrderTask` 128 B   | 128 KiB |
| `order_channel.results` | `OrderSpscRing<OrderResult,1024>` | order → strateji           | 1023                    | `OrderResult` 128 B | 128 KiB |
| `telemetry_cmd_ring`    | `OrderSpscRing<TelemetryCmd,256>` | strateji → telemetry       | 255                     | 1 B                 | —       |
| `LogRing<8192>`         | `LogMsg` 384 B                    | —                          | —                       | —                   | —       |

### Halka davranışları

- `market_ring` dolarsa olay düşer ve `drop_count_` artar. `monitor_drops` bu farkı periyodik olarak loglar.
- `order_channel.tasks` dolarsa `push_task` başarısız olur. Çağıran FSM durumunu uygun şekilde geri alır veya tekrar denenecek durumda bırakır.
- `order_channel.results` için bazı firewall/KS_GATE ret yolları `push_result_bounded` kullanır. Diğer yolların `push_result` dönüş değerini kontrol etmediği durumlarda sonuç kaybı mümkündür.
- `telemetry_cmd_ring` dolarsa flush komutu düşebilir ve sayaç artırılır.
- `g_log_ring` tanımlıdır ancak canlı Logger akışında kullanılmaz; Logger senkron çalışır.

### SPSC kuralları

- Kapasite `N` ikinin kuvveti olmalıdır: `static_assert((N & (N-1)) == 0)`.
- Dizinler `uint32_t` `head_` / `tail_` ve `& (N-1)` maskelemesiyle çalışır.
- Bir slot daima boş bırakılır.
- `head_`, `tail_`, `drop_count_` ve tampon hizalamaları false sharing'i azaltmak için ayrılır.
- Üretici ve tüketici bellek sıralaması acquire/release ilkelerine dayanır.
- `tasks` halkasının tek üreticisi strateji thread'idir. Order thread bu halkaya yazmaz.
- `results` halkasının üreticisi order thread, tüketicisi strateji thread'idir. Kapanışta sonuçları ana thread tüketir.

`push_task` başarısızlığında:

- SEND → slot `REJECTED`, `awaiting_fill=false`.
- EXIT → slot `REJECTED`.
- POLL → `poll_in_flight` açılmaz; sonraki turda tekrar denenebilir.
- CANCEL → slot `CANCEL_REQUEST` durumunda kalır; sonraki turda yeniden denenebilir.

## 4. CPU pinning [KOD]

- `read_core_from_cfg_or_env`, config içinde `strategy_core` ve `order_core` alanları varsa bunları kullanabilecek şekilde tasarlanmıştır.
- Mevcut `KusakConfig` içinde bu alanlar bulunmadığından çekirdek seçimi ortam değişkenleriyle yapılır:
  - `KUSAK_STRATEGY_CORE`
  - `KUSAK_ORDER_CORE`
- Tanımsız, negatif veya ayrıştırılamayan değer → `-1` (pin yok).
- `pin_current_thread_to_core(core)` için `core < 0` ise işlem yapılmaz.
- Çekirdek mevcut CPU sayısının dışındaysa pinleme başarısız olur.
- `pthread_setaffinity_np` başarısızlığı ölümcül değildir; uyarı loglanır.
- Canlı yol için `isolcpus`, IRQ affinity, `SCHED_FIFO`, `mlockall` ve huge-page kurulumu bulunmamıştır.
- Pinlenmeyen thread'ler pinli çekirdeklerle yarışabilir [ÇIKARIM].

## 5. Bellek ve tahsis kısıtları

### Yapısal olarak uygulananlar [KOD]

- `MarketEvent`: 64 B.
- `OrderTask`: 128 B.
- `OrderResult`: 128 B.
- `OrderSlot`: 64 B.
- Bu boyutlar `static_assert` ile kilitlenmiştir.
- Metin alanlarında `FixedString<N>` kullanılır.
- `FlatOrderMap`, `MAX_ORDERS = 4096` sabit boyutlu dizi, lineer probing ve tombstone yaklaşımı kullanır.
- `StrategyContext`, market ring ve diğer sabit tamponlar nedeniyle büyüktür; `main` içinde heap üzerinde kurulur.
- `order_guard.hpp` STL dışında bağımlılık kullanmayan, tahsissiz ve `noexcept` tasarıma sahiptir.
- `final_order_firewall` atomic yüklemeler ve aritmetik kontroller kullanır.
- Order thread durum ayrıştırmasında `string_view` ve sabit boyutlu karşılaştırma mekanizmaları kullanılır.

### Uygulanmayanlar / sınırlar [KOD]

- Logger sıcak yolda tahsissiz ve kilitsiz değildir. `std::ostringstream`, `std::string`, `std::mutex` ve senkron stdout/dosya yazımı kullanır.
- Order thread REST yolu `std::string` gövde döndürür ve ağ çağrıları bloklanabilir.
- `FilterValidator` önbelleğinin `atomic_load/store(shared_ptr)` kullanımının lock-free olduğu kanıtlanmamıştır.
- `std::atomic<double>` alanları için lock-free `static_assert` bulunmaz.
- `trading_paused` düz `bool` olarak tanımlanmıştır. Mevcut erişimlerin thread sahipliği korunmalıdır; başka thread'lerden erişim eklemek veri yarışı oluşturabilir.

## 6. Zaman altyapısı

### Mevcut mekanizma [KOD]

- `calibrate_tsc()` ana başlangıçta bir kez çalışır.
- Kalibrasyon yaklaşık 100 ms `nanosleep` penceresinde `rdtscp` ile `CLOCK_MONOTONIC_RAW` farkını ölçer.
- `ns_mult = (delta_ns << 32) / delta_tsc` hesaplanır.
- `tsc_ns()` şu yapıyı kullanır:

  `((rdtscp * g_tsc_ns_mult) >> 32)`
- `lfence` ile TSC okuması sıralanır.
- `dry_run` ve `mock_time_ns != 0` durumunda `current_time_ns()` simülasyon zamanını döndürebilir.
- Kapanış ve sıcak olmayan alanlarda `std::chrono::steady_clock` kullanılır.

### Bilinen Zafiyetler

#### KZ-P0-01 — TSC çarpım taşması ve saat sarması

KRİTİK — P0

`tsc_ns()` hesaplamasında `rdtscp` sonucu ile `g_tsc_ns_mult` 64-bit unsigned çarpıma girer. Çarpım 64-bit sınırını aşınca taşar. Mevcut hesaplama biçiminde dönen değer yaklaşık `2^32 ns`, yani 4,29 saniyelik aralıkta başa sarar.

Bu bulgu proje sahibi tarafından doğrulanmıştır ve kesin zafiyet olarak kabul edilir.

Etkilenen zaman tabanlı mantıklar:

- 5 saniyelik piyasa verisi tazeliği.
- 30 saniyelik emir TTL.
- 6 saniye / 2 saniye emir zaman aşımları.
- 500 ms / 1 s / 10 s periyotlar.
- Kill switch zaman pencereleri.
- `cooldown_until_ns`.
- `current_time_ns()` kullanan diğer canlı zaman kontrolleri.

Olası etki:

- Zamanın geriye gitmiş gibi görünmesi.
- Yanlış stale-data tespiti.
- Emir zaman aşımının yanlış tetiklenmesi veya tetiklenmemesi.
- Cooldown ve pencere hesaplarının bozulması.
- Kill switch zaman ölçümlerinin güvenilmez hale gelmesi.

Bu zafiyet düzeltilip bağımsız testlerle doğrulanmadan canlı zaman altyapısına güvenilmez.

Düzeltme yapılırken uyulacak sınırlar:

1. Kaynak kodu değiştirmeden önce mevcut davranışı gösteren bağımsız test hazırlanır.
2. TSC ölçeği ve çarpım matematiği ayrı ayrı doğrulanır.
3. Uzun süreli monotonluk testi yapılır; yalnızca ilk birkaç saniyelik ölçüm yeterli değildir.
4. `current_time_ns()` kullanan tüm tüketiciler gözden geçirilir.
5. Düzeltme sonrası derleme, birim testi ve donanım üzerinde uzun süreli ölçüm ayrı ayrı kaydedilir.
6. Bu belgeye yalnızca gerçekten çalıştırılmış test sonuçları eklenir.

## 7. Thread'ler arası paylaşılan durum [KOD]

| Değişken                                                                                        | Yazan                                      | Okuyan           | Not                                                      |
| ----------------------------------------------------------------------------------------------- | ------------------------------------------ | ---------------- | -------------------------------------------------------- |
| `kill_switch`                                                                                   | Strateji ve bazı order thread hata yolları | Strateji / order | `seq_cst` yazım; gönderim kapısı için `SAFETY_POLICY.md` |
| `g_inflight_orders`                                                                             | Order thread                               | Strateji         | Kill switch gönderim kapısı                              |
| `fw_max_position_usd`, `fw_max_order_notional_usd`                                              | Strateji                                   | Order thread     | `fw_publish_limits` üzerinden                            |
| `fw_position_usd`, `fw_free_quote_usd`                                                          | Strateji                                   | Order thread     | Emir öncesi yayınlanan maruziyet                         |
| `poll_in_flight`                                                                                | Strateji ve order thread                   | Strateji         | Aynı anda tek POLL görevi hedeflenir                     |
| `order_rtt_ns`, `ws_disconnect_total`, `last_ticker_ns`, `cooldown_until_ns`, `vpin_force_exit` | Çeşitli                                    | Strateji / order | Her alanın atomikliği ve sahipliği korunmalıdır          |

Paylaşılan alanların yazıcı/okuyucu sahipliği değiştirilmeden önce tüm erişim noktaları incelenmelidir. Bir alanın atomic olması, ilgili iş akışının mantıksal olarak yarışsız olduğunu tek başına kanıtlamaz.

## 8. Başlatma ve kapanış sırası [KOD]

Başlatma akışı genel olarak:

1. Config yüklenir.
2. TSC kalibrasyonu yapılır.
3. Binance HTTP istemcisi ve ExchangeInfo altyapısı hazırlanır.
4. Canlı modda başlangıç uzlaştırma çalıştırılır.
5. Order ve piyasa veri thread'leri başlatılır.
6. Strateji ana döngüsü çalışır.
7. Telemetri thread'i komutları işler ve periyodik flush yapar.

Kapanışta:

- Yeni iş üretimi durdurulur.
- Emir kanalı ve aktif FSM kayıtları kontrol edilir.
- Bekleyen görevler drain edilmeye çalışılır.
- Aktif/belirsiz emirler için sınırlı bekleme yapılır.
- Kapanışta çözülemeyen emirler loglanır; borsada manuel kontrol gerekebilir.

## 9. AI değişiklik kuralları (mimari)

1. SPSC halkalarına ikinci üretici veya ikinci tüketici ekleme.
2. Sıcak döngüye mutex, sleep, heap tahsisi veya bloklayan I/O ekleme.
3. Yeni thread eklemek mevcut CPU pinning ve thread sahipliği modelinin yeniden incelenmesini gerektirir.
4. TSC altyapısını kullanan tüm süreler KZ-P0-01 kapsamında değerlendirilir.
5. TSC düzeltmesi yalnızca tek bir fonksiyonun çıktısını değiştirmek olarak ele alınmaz; bütün zaman tüketicileri incelenir.
6. `market_ring` doluluk ve olay kaybı davranışını sessizce değiştirme.
7. `order_channel.results` sonuç kaybı ihtimalini göz ardı etme.
8. Logger'ın sıcak yol maliyetini yok sayma.
9. Her değişiklik öncesi beklenen davranış ve test senaryosu tanımlanır.
10. Derlenmeyen veya çalıştırılmayan test başarılı kabul edilmez.
