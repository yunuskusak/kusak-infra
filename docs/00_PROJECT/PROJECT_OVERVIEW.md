# PROJECT_OVERVIEW — KUSAK-INFRA

Etiketler: **[KOD]** koddan doğrulandı · **[ÇIKARIM]** kod okumasından çıkarım · **[DOĞRULANMADI]** kanıt yok.

Kaynak: Proje kaynak anlık görüntüsü (`include/`, `src/`, `tools/`, `CMakeLists.txt`) ve ana yol haritası.

## 1. Kimlik

- C++20, Linux, x86-64 tabanlıdır. `x86intrin.h` ve `rdtscp` kullanımı nedeniyle TSC altyapısı taşınabilir değildir [KOD].
- Tek sembol ve tek market (`cfg.market = SPOT | FUTURES`) modeli vardır [KOD].
- Aynı anda tek pozisyon / tek giriş emri hedeflenir [KOD].
- Kod etiketleri V13 "Ghost Engine" dönemini taşır. CMake projesi `kusak_v13` olarak tanımlanmıştır.
- Yol haritasındaki hedef sürüm `kusak_v14.0` — Safety / Execution Firewall'dır.
- Bağımlılıklar: OpenSSL, libcurl, pthread ve nlohmann/json v3.11.3 (`FetchContent`) [KOD].
- Derleme bayrakları: `-Wall -Wextra -Wpedantic`; RelWithDebInfo için `-O2`, Release için `-O3 -march=native` [KOD].

## 2. Çalışma modeli (canlı motor)

```text
Binance WS (bookTicker, aggTrade; depth abone ama callback=nullptr)
        │
        ▼ market_thread
SpscRing<131072> (MarketEvent 64B)
        │
        ▼
Strateji thread (main)
        │
        ├── OBI
        ├── risk / VPIN / rejim filtreleri
        └── maybe_send_order
                 │
                 ▼
        OrderChannel<1024>.tasks
                 │
                 ▼
             order_thread
        (firewall → kill-switch kapısı → REST)
                 │
                 ▼
        OrderChannel.results
                 │
                 ▼
        Strateji thread / FSM

Strateji thread ── telemetry_cmd_ring ──► telemetry_thread
```

Giriş kararı `maybe_send_order` içinde bir dizi kontrolden geçer [KOD]:

1. Sembol filtresi ve `trading_paused` kontrolü.
2. Kill switch kontrolü.
3. Pozisyon ve `awaiting_fill` kontrolü.
4. Cooldown ve son ticker zamanının tazeliği.
5. Bid/ask/mid değerlerinin pozitif olması.
6. Rejim ve risk skoru filtreleri.
7. VPIN filtresi.
8. Top-of-book OBI eşiği.
9. `buys_disabled` / `sells_disabled` kontrolü.
10. Serbest sermaye ve fiyat kontrolü.
11. Tick/step yuvarlama.
12. Miktar ve notional sınırları.
13. `validate_order`.
14. Reversal ve aktif EXIT slotu kontrolü.
15. FSM kaydı ve `push_task`.

OBI varsayılan eşiği 0.35'tir. Hesaplama `(bid_qty - ask_qty) / (bid_qty + ask_qty)` şeklindedir; payda `1e-12` altındaysa 0 kullanılır.

Çıkış tetikleyicileri arasında stop-loss, trailing stop, VPIN stres çıkışı (`vpin_force_exit`) ve `dynamic_momentum_exit` bulunur. Çıkış emri `MARKET` tipindedir [KOD].

## 3. Çalışma modları ve ortam ayrımı

| Ayar                    | Varsayılan                                                                 | Anlamı                                                                                  |
| ----------------------- | -------------------------------------------------------------------------- | --------------------------------------------------------------------------------------- |
| `dry_run`               | `true`                                                                     | Borsaya emir gönderilmez; giriş/çıkış sentetik dolum ve FSM üzerinden terminale taşınır |
| `testnet`               | `false`                                                                    | Binance testnet/mainnet uç noktasını belirler; `TESTNET` ortam değişkeni config'i ezer  |
| `market`                | `SPOT`                                                                     | Spot veya Futures API uç noktaları                                                      |
| `kusak_config_mvp.json` | `PENDLEUSDT`, SPOT, `dry_run=false`, `testnet=true`, `max_position_usd=30` | MVP testnet yapılandırması                                                              |
| `kusak_config.json`     | `BTCUSDT`, `dry_run=true`, `max_position_usd=500`                          | Dry-run örneği                                                                          |

`dry_run=false` ve `testnet=false` birleşimi gerçek piyasa işlemi anlamına gelir. Bu kombinasyon açık insan onayı olmadan etkinleştirilmez.

`main.cpp`, `dry_run=true` iken de `BinanceHttpClient` ve `ExchangeInfoManager` kurar. Bu nedenle anahtarlar boşsa dry-run başlangıcı da başarısız olabilir [KOD].

## 4. Modül haritası

| Alan                 | Dosyalar                                                                                                                                                                                                                                          | Sorumluluk                                                                            |
| -------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------- |
| Giriş / orkestrasyon | `src/main.cpp`                                                                                                                                                                                                                                    | Config, thread başlatma, `run_order_thread`, başlangıç uzlaştırma, kapanış            |
| Strateji çekirdeği   | `include/strategy_core.hpp`                                                                                                                                                                                                                       | `StrategyContext`, giriş/çıkış, firewall, kill switch, FSM sonuç dağıtımı             |
| Emir tipleri / FSM   | `include/engine/types.hpp`                                                                                                                                                                                                                        | `OrderTask`, `OrderResult`, `OrderSlot`, `OrderState`, `FlatOrderMap`, `OrderChannel` |
| Halkalar / zaman     | `include/infra/spsc_ring.hpp`, `include/infra/time.hpp`, `include/engine/market_event.hpp`                                                                                                                                                        | SPSC halkalar, TSC zaman altyapısı, piyasa olayları                                   |
| Güvenlik mantığı     | `include/order_guard.hpp`                                                                                                                                                                                                                         | Cap/qty, `final_check`, maruziyet, `RateGate`, başlangıç kapısı                       |
| Borsa iletişimi      | `binance_http_client.hpp`, `binance_rest_client.hpp`, `binance_signer.hpp`, `exchange_info_manager.hpp`, `ws_client.hpp`, `ws_client_multipath.hpp`                                                                                               | REST/WS, imza, filtre cache'i                                                         |
| Sinyal / risk        | `vpin_velocity_tracker.hpp`, `regime_detector.hpp`, `risk_scorer.hpp`, `auto_param_adapter.hpp`, `hft_models_v9.hpp`                                                                                                                              | VPIN, rejim, risk skoru, parametre adaptasyonu                                        |
| Ölçüm                | `telemetry_sink.hpp`, `latency_monitor.hpp`, `adverse_selection_tracker.hpp`, `slippage_models.hpp`, `logger.hpp`                                                                                                                                 | CSV/Prometheus, gecikme, ters seçim, slippage                                         |
| Araştırma            | `market_data_recorder.hpp`, `replay_engine.hpp`, `backtester.hpp`, `paper_trader.hpp`, `genetic_optimizer.hpp`, `parameter_optimizer.hpp`, `walk_forward_optimizer.hpp`, `research_types.hpp`, `core_types.hpp`, `src/app/research/*`, `python/*` | Kayıt, replay, backtest, optimizasyon                                                 |
| Simülasyon           | `IExchangeClient.hpp`, `SimulatedExchangeClient.hpp`                                                                                                                                                                                              | Ağsız borsa istemcisi arayüzü                                                         |
| Bildirim             | `notification.hpp`                                                                                                                                                                                                                                | Telegram/Slack/webhook bildirim altyapısı                                             |

## 5. Derleme hedefleri

| Hedef                   | Seçenek                    | Kaynak                            |
| ----------------------- | -------------------------- | --------------------------------- |
| `kusak_v13`             | Her zaman                  | `src/main.cpp`                    |
| `research`              | `BUILD_RESEARCH=ON`        | Research CLI / backtest / replay  |
| `research_record`       | `BUILD_RESEARCH_RECORD=ON` | `KUSAK_RECORD_ONLY` ile kayıt     |
| `replay_harness`        | `BUILD_REPLAY_HARNESS=ON`  | `tools/replay_harness.cpp`        |
| `mvp_order_bridge_test` | Her zaman                  | `tools/mvp_order_bridge_test.cpp` |

`order_guard_test.cpp` ve `phase2_fsm_test.cpp` CMake hedefi değildir; elle derlenir [KOD].

## 6. Çıktılar

- `kusak.log`
- `telemetry.csv`
- `metrics.prom`
- `order_lifecycle.csv`
- `data/ticks/*.tick`
- Kill switch kilit dosyası

## 7. Yol haritası ile ilişki

Öncelik sırası:

1. Güvenli emir yürütme.
2. Emir doğruluğu ve yaşam döngüsü.
3. Veri kalitesi ve ölçüm.
4. Strateji sinyalinin tahmin gücü.
5. Maliyet sonrası performans.
6. Kontrollü canlı doğrulama.

Sürümler:

- v14.0: Safety / Execution Firewall.
- v14.1: Measurement / Research.
- v14.2: Strategy Edge Validation.
- v15: Koşullu genişletme; HMM/ML/çoklu sembol gibi konular ancak önceki aşamalar doğrulanırsa değerlendirilir.

Kodda rejim dedektörü ve HMM parametreleri bulunması, bunların genişletilmesini mevcut öncelik haline getirmez.

## 8. Bilinen Zafiyetler

### KZ-P0-01 — TSC saat taşması

`include/infra/time.hpp` içindeki `tsc_ns()` hesaplamasında TSC değeri ile `g_tsc_ns_mult` 64-bit çarpıma girer. Çarpım taşması nedeniyle elde edilen zaman yaklaşık 4,29 saniyelik modüler döngüye girer.

Bu durum canlı motordaki zaman temelli kontrolleri etkileyebilir. Ayrıntı ve etki alanı `SYSTEM_ARCHITECTURE.md` §6'dadır.

### KZ-P0-02 — İlk POLL görevinde yerel ID kullanımı

İlk emir gönderiminde `active_order_id` henüz borsanın emir ID'si değildir; yerel FSM ID'si olarak atanır. İlk POLL görevi bu değeri borsa `orderId` alanında kullanabilir.

Ayrıntı ve etki alanı `FSM_GUIDE.md` §8'dedir.

### KZ-P0-03 — EXIT firewall reddi

Final firewall SEND ve EXIT görevlerine uygulanır. Pozisyon ve maruziyet kontrolleri azaltıcı EXIT yönünü ayrı ele almadığından EXIT emri mevcut pozisyon/maruziyet nedeniyle reddedilebilir.

Ayrıntı ve etki alanı `SAFETY_POLICY.md` §4.3'tedir.

### Diğer olgunluk notları

- Kalıcı emir kaydı, restart sonrası fill uzlaştırması ve `ADOPT_POSITION` ile `ctx.position`'ın yeniden kurulması bu kaynak anlık görüntüsünde bulunmamıştır.
- `vpin_velocity_tracker.hpp.rej` ve `vpin_*.patch` dosyaları VPIN yamalarının tam uygulanmadığını düşündürür; hangi yamanın uygulandığı ayrıca doğrulanmadan kesin hüküm verilmez.
- `KULLANIM_KILAVUZU.md` V10 dönemidir. Güncel davranış için kaynak kod esastır.
- Bileşenlerin varlığı kârlılık kanıtı değildir.
