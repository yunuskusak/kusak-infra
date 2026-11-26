# SAFETY_POLICY — Güvenlik İlkeleri ve Sınırlar

Etiketler: **[KOD]** · **[ÇIKARIM]** · **[DOĞRULANMADI]**

Kapsam: `include/order_guard.hpp`, `include/strategy_core.hpp`, `include/engine/types.hpp`, `src/main.cpp`, `include/config.hpp`, Binance REST ve `RateGate` entegrasyonu.

## 1. Değiştirilemez ilkeler

1. Güvenlik kontrolleri performans veya kolaylık gerekçesiyle kaldırılamaz.
2. Doğrulanamayan değer fail-closed davranışla ele alınır.
3. Zaman aşımı, emrin borsaya ulaşmadığı anlamına gelmez.
4. Belirsiz emir `UNKNOWN` durumuna alınır; yeni risk açılmaz.
5. Kill switch otomatik olarak temizlenmez.
6. Gerçek para işlemi varsayılan değildir.
7. Güvenlik testi ile strateji performans testi birbirinden ayrıdır.
8. Bir testin geçmesi, üretim yolunun aynı davranışı gösterdiğini kanıtlamaz.
9. Proje sahibi onayı olmadan güvenlik katmanları gevşetilmez.
10. Bilinen P0 zafiyetleri giderilmeden güvenli yürütme iddiası kurulamaz.

## 2. Savunma katmanları

| Katman | Konum | Amaç |
|---|---|---|
| Strateji ön kontrolleri | `maybe_send_order` | Giriş öncesi sinyal, pozisyon, sermaye ve piyasa koşulları |
| Emir doğrulama | `validate_order`, filtre kontrolleri | Fiyat, miktar, tick/step ve notional geçerliliği |
| Bağımsız guard | `include/order_guard.hpp` | Cap, miktar, maruziyet, rate gate ve startup kararları |
| Final firewall | `final_order_firewall` | Order thread'de REST öncesi son emir kontrolü |
| Kill switch gönderim kapısı | `run_order_thread` / `strategy_core.hpp` | Kill switch tetiklenirken yeni SEND görevini engelleme |
| FSM | `FlatOrderMap` / `OrderState` | Emir yaşam döngüsü ve belirsiz sonuç yönetimi |
| Başlangıç uzlaştırma | `startup_reconcile` | Canlı başlangıçta açık emir/bakiye kontrolleri |
| RateGate | REST entegrasyonu | İstek hızını sınırlama |

Bu katmanlar birbirinin yerine geçmez. Özellikle `order_guard.hpp` içindeki test edilen bir fonksiyonun üretimdeki `final_order_firewall` ile aynı olduğunu varsayma.

## 3. `order_guard.hpp` — sınırları

`order_guard.hpp` bağımsız STL güvenlik mantığı olarak tasarlanmıştır.

İçerdiği mekanizmalar arasında:

- Miktar ve cap hesapları.
- `final_check`.
- Maruziyet kontrolü.
- `RateGate`.
- Başlangıç kararı (`startup_decision`).

### Üretim ve test ayrımı

Kaynak anlık görüntüsünde `kusak::guard::final_check` ve `cap_qty` üretim emir yolunda çağrılmamaktadır. Bu fonksiyonlar `tools/order_guard_test.cpp` içinde test edilir.

Gerçek emir yolundaki son kontrol `strategy_core.hpp::final_order_firewall` fonksiyonudur.

Bu nedenle:

- `order_guard_test` başarılı olsa bile üretim firewall'ının doğru olduğu kanıtlanmış olmaz.
- Test edilen yardımcı fonksiyonlar ile üretimdeki gerçek kapı birbirine eşit kabul edilmez.
- Bu iki yol arasındaki ayrışma büyütülmez.
- Birleştirme veya yeniden tasarım ayrı, testli bir iştir.

### Test uyumsuzluğu

Kaynak anlık görüntüsünde `order_guard.hpp` içindeki `startup_decision` imzası `startup_decision(const StartupProbe&)` şeklindedir. `tools/order_guard_test.cpp` ise eski konumsal argüman imzasını kullanmaktadır.

Bu nedenle test dosyası güncel header ile uyumsuzdur. Güncel kaynakla derlenip çalıştırılmadan test başarılı kabul edilmez.

## 4. `final_order_firewall` (`strategy_core.hpp`) [KOD]

### 4.1 Görevi

`final_order_firewall`, order thread'in SEND/EXIT görevlerini REST'e göndermeden önce kontrol eden son güvenlik katmanıdır.

Kontroller arasında:

- Miktar ve fiyat geçerliliği.
- Notional sınırı.
- Pozisyon cap'i.
- Emir başına cap.
- Sembol ve filtre doğrulaması.
- Toplam maruziyet.
- BUY tarafında kullanılabilir quote bakiye kontrolü.

Firewall reddi, emir gönderimini engeller ve ilgili hata yolu kill switch davranışını tetikleyebilir.

### 4.2 Önemli ayrım

`final_order_firewall` ile `order_guard.hpp::final_check` aynı fonksiyon değildir.

Üretim yolunda kullanılan firewall'ın davranışı, yalnızca `order_guard_test.cpp` sonuçlarına bakılarak doğrulanamaz.

### 4.3 Bilinen Zafiyetler

#### KZ-P0-03 — EXIT emirlerinin final firewall tarafından reddedilmesi

<badge>KRİTİK — P0</badge>

Mevcut çağrı akışında `main.cpp` order thread içinde final firewall kontrolünü hem SEND hem EXIT görevlerine uygular.

`strategy_core.hpp::final_order_firewall` içindeki pozisyon ve maruziyet kontrolleri EXIT yönünü, yani mevcut pozisyonu azaltmaya yönelik emri, ayrı bir güvenli azaltma davranışı olarak ele almaz.

Bu nedenle EXIT emri, kapatmaya çalıştığı mevcut pozisyon veya maruziyet nedeniyle firewall tarafından reddedilebilir.

Bu bulgu proje sahibi tarafından doğrulanmıştır ve kesin zafiyet olarak kabul edilir.

**İlgili kod ilişkisi:**

- `main.cpp` içindeki firewall çağrı noktası SEND ve EXIT görevlerini kapsar.
- `strategy_core.hpp` içindeki `POSITION_CAP` kontrolü mevcut pozisyon ve emir miktarını değerlendirir.
- `EXPOSURE_CAP` kontrolü de emir yönüne göre güvenli azaltma ayrımı yapmaz.
- Kill switch gönderim kapısında EXIT için farklı davranış bulunması, EXIT'in final firewall'dan muaf olduğu anlamına gelmez.

**Önemli ayrım:**

EXIT'in kill switch gönderim kapısından muaf olması, EXIT'in bütün güvenlik kontrollerinden muaf olduğu anlamına gelmez.

Mevcut sorun, EXIT'in güvenlik kontrollerinden tamamen çıkarılması gerektiğini göstermez. Sorun, pozisyon azaltıcı emirlerin maruziyeti artıran emirlerle aynı yönde değerlendirilmesidir.

**Düzeltme çalışmasında korunacak ilkeler:**

1. EXIT'in gerçekten mevcut pozisyonu azaltıp azaltmadığı doğrulanmalıdır.
2. Emir miktarı mevcut pozisyon miktarını aşmamalıdır.
3. EXIT'in yanlışlıkla yeni pozisyon açmasına izin verilmemelidir.
4. Sembol, miktar, fiyat ve borsa filtre kontrolleri korunmalıdır.
5. SELL envanter kontrolü ve ilgili hesaplama güvenliği ayrıca ele alınmalıdır.
6. EXIT için genel firewall bypass'ı oluşturulmamalıdır.
7. Hem pozisyonu azaltan EXIT hem de hatalı yönde/ölçüde EXIT senaryoları test edilmelidir.
8. Testler doğrudan üretim `final_order_firewall` fonksiyonunu kapsamalıdır.
9. Kill switch tetiklenmişken EXIT davranışı ayrıca doğrulanmalıdır.

## 5. Kill switch

### 5.1 Tetikleyiciler

Kill switch eşikleri `KillSwitchConfig` üzerinden yönetilir.

Başlıca tetikleyiciler:

| Sebep | Config anahtarı | Varsayılan | Not |
|---|---|---|---|
| `MAX_POSITION_USD` | `max_position_usd` | 0 → config cap | Pozisyon mark değeri cap'i aşarsa |
| `MAX_TOTAL_EXPOSURE` | `max_total_exposure_usd` | 0 → pozisyon cap'i | Pozisyon + pending remainder |
| `MAX_DAILY_LOSS` | `max_daily_loss_usd` | 0 / kapalı | Gerçekleşen + floating, UTC gün |
| `MAX_CONSECUTIVE_LOSSES` | `max_consecutive_losses` | 0 / kapalı | Ardışık zarar |
| `MAX_ORDER_REJECTS` | `max_order_rejects` | 0 / kapalı | UTC gün ret sayısı |
| `MAX_CANCEL_RATE` | `max_cancel_rate` | 0 / kapalı | Kayan pencere |
| `MAX_LATENCY` | `max_latency_ms` | 0 / kapalı | SEND → yanıt RTT |
| `MAX_STALE_MARKET_DATA` | `max_stale_data_ms` | 0 / kapalı | Pozisyon/bekleyen emir varken |
| `MAX_WS_DISCONNECT` | `max_ws_disconnects` | 0 / kapalı | Kopma sayısı penceresi |
| `MAX_ACCOUNT_BALANCE_DROP` | `max_balance_drop_pct` | 0 / kapalı | Equity HWM'e göre |
| `EXTERNAL_TRIP` | — | — | Başka bir kod yolu kill switch bayrağını yazarsa |
| `LOCK_FILE_PRESENT` | `lock_file` | `kusak_kill_switch.lock` | Başlangıçta kilit dosyası varsa |

Doğrudan bayrak yazılmasına neden olabilen hata yolları arasında firewall reddi, API anahtarı hatası, timestamp hatası, imza hatası ve UNKNOWN durumu bulunur.

Eşiklerin 0 olması ilgili eşik kontrolünün kapalı olması anlamına gelebilir. Config'te tanımlı olmayan bir eşiğin aktif olduğunu varsayma.

Değerlendirme periyodik olarak, piyasa olayları üzerinden ve ilgili olay kancalarında yapılır. Günlük sayaçlar UTC gün değişiminde sıfırlanır; kill switch kilidi kendiliğinden sıfırlanmaz.

### 5.2 Tetiklenince (`ks_latch` → `ks_service`)

1. `ks_latch` bir kez işlenir.
2. `kill_switch=true` yapılır.
3. `CRITICAL` log üretilir.
4. Kilit dosyası yazılır.
5. Ana döngü `trading_paused` durumunu etkinleştirir.
6. `ks_service`, SAFE MODE'a girilene kadar periyodik olarak `ks_cancel_all` çağırır.
7. Canlı emir kalmadığında SAFE MODE'a geçilir.
8. `flatten_on_trip=true` ve pozisyon açık ise `send_exit_order(MARKET)` üzerinden pozisyon kapatılmaya çalışılır.
9. `flatten_on_trip=false` ise pozisyon olduğu gibi bırakılır.

Bildirim altyapısı `ctx.notifier` üzerinden çalışır. `main.cpp` içinde notifier atanmadığı kaynak anlık görüntüsünde belirtilmiştir; bu nedenle bildirimlerin çalıştığı varsayılmamalıdır.

### 5.3 Gönderim kapısı (Dekker)

Gönderici:

1. `inflight` sayacını artırır.
2. `kill_switch` değerini okur.
3. Kill switch aktifse gönderimi durdurur.

Tetikleyici:

1. `kill_switch` değerini yazar.
2. `inflight` sayacını okur.
3. Devam eden gönderimlerin iptal kapsamına alınmasını sağlar.

Bu kapıdaki atomic sıralama ve sayaç ilişkisi güvenlik açısından kritiktir. Gevşetilmemelidir.

EXIT'in gönderim kapısında ayrı ele alınması, EXIT'in final firewall kontrolünden muaf olduğu anlamına gelmez. KZ-P0-03 için §4.3'e bak.

### 5.4 Reset

Reset yalnızca SAFE MODE ve kilit dosyası prosedürü üzerinden yapılır.

- Operatör kilit dosyasını siler.
- `ks_service` kilit dosyasının bulunmadığını tespit eder.
- `ks_manual_reset` latched/safe_mode ve ilgili pencere sayaçlarını temizler.
- `equity_hwm` güncel sermayeye göre yeniden kurulur.
- `trading_paused=false` ve `kill_switch=false` yapılır.
- Günlük PnL ve gün indeksi korunur.
- Limit hâlâ aşılmışsa kill switch yeniden tetiklenebilir.

Kill switch'in otomatik olarak sıfırlanması kabul edilmez.

Yumuşak `trading_paused` ile kill switch birbirinden farklıdır. VPIN/fill-rate kaynaklı yumuşak duraklatma otomatik devam edebilir; kill switch otomatik devam etmez.

Restart sırasında kilit dosyası varsa canlı başlangıç uzlaştırması `REFUSE_LOCKED` ile sonlanabilir. Dry-run akışı bu başlangıç kapısını atlayabilir.

## 6. Başlangıç uzlaştırma (canlı) [KOD]

`startup_reconcile`, order ve strateji thread'leri başlamadan önce çalışır.

Genel akış:

1. `tickSize` ve `minNotional` okunamazsa fail-closed ret.
2. En fazla 3 epoch ve her epoch'ta en fazla 3 iptal turu.
3. Her karar öncesinde taze `openOrders` sorgusu.
4. İptal başarısı yalnızca istek yanıtıyla varsayılmaz; sonraki sorguyla doğrulanır.
5. Bakiye kontrolü yapılır (`base_free + base_locked` ve bid fiyatı).
6. `ADOPT_POSITION` kararı yalnızca yeterli quote bakiyesi koşuluyla ilerleyebilir.
7. Flatten gerektiğinde SELL LIMIT IOC kullanılır.
8. Fiyat, bid ve slip değerine göre aşağı yuvarlanır.
9. Slip kademeleri 30/60/120 bps olarak uygulanır.
10. MARKET emrine düşülmez.
11. Yakınsama sağlanmazsa başlangıç reddedilir.
12. `CLEAN_START/IGNORE_DUST` için de quote bakiye koşulu aranır.
13. HTTP çağrıları `RateGate` üzerinden geçer.

### Bilinen sınırlar

- Yalnızca açık emir sayısı ve bakiye kontrol edilir.
- Geçmiş fill/işlem uzlaştırması bulunmamıştır.
- `ADOPT_POSITION` mevcut `ctx.position` nesnesini yeniden kurmaz.
- Kalıcı emir kaydı bulunmamıştır.
- Restart sonrası gerçek pozisyonun ve emir geçmişinin eksiksiz yeniden kurulması garanti edilmez.

## 7. Konfigürasyon güvenlik semantiği [KOD]

### `max_position_usd`

- Config yorumunda 0 değeri "no hard cap" olarak açıklanmıştır.
- Canlı `maybe_send_order` ve firewall yollarında 0/NaN değer fail-closed şekilde reddedilebilir.
- Dry-run davranışı farklı olabilir.
- Kill switch tarafında 0, eşik kapalı anlamına gelebilir.

Aynı alanın farklı tüketicilerde farklı 0 anlamları vardır. Bu fark sessizce değiştirilmez.

### Kill switch config bloğu

`kill_switch` bloğu yoksa `KillSwitchConfig` varsayılanları geçerlidir:

- `enabled=true`
- Tüm eşikler 0 / kapalı
- `flatten_on_trip=true`

Kaynak anlık görüntüsündeki MVP config'te `kill_switch` bloğu bulunmamaktadır.

Bu nedenle `max_position_usd` değerinden türeyen `MAX_POSITION_USD` ve `MAX_TOTAL_EXPOSURE`, firewall ve harici tetikler dışında kalan eşiklerin aktif olduğu varsayılmaz.

### Config reload

Config reload yaklaşık 1 saniyelik kontrol döngüsünde yapılır. Yeniden yüklemede yalnızca belirli risk ağırlıkları/eşikleri ve `fw_publish_limits` güncellenir.

### Maruziyet hesabı

`MAX_POSITION_USD` mark-to-market (`qty × mid`) değerine dayanır ve toleranssız olabilir. Cap'e çok yakın pozisyonlarda küçük fiyat hareketleri tetiklemeye neden olabilir [ÇIKARIM].

## 8. Yol haritası v14.0 ↔ kod durumu

| Yol haritası maddesi | Kod durumu | Not |
|---|---|---|
| Maksimum pozisyon miktarı | VAR | `maybe_send_order` cap formülü ve firewall `POSITION_CAP` |
| Emir başına maksimum notional | VAR | `fw_max_order_notional_usd` |
| Toplam maruziyet | KISMİ | Firewall pozisyon + mevcut emir; kill switch pozisyon + pending remainder |
| Bakiye kontrolü | VAR (BUY) | İç muhasebe üzerinden; her emir öncesi borsa bakiyesi sorgulanmaz |
| Spot envanter (SELL) kontrolü | YOK | Firewall'da base envanter kontrolü bulunmamıştır |
| Borsa filtreleri | VAR | `FilterValidator::validate_order` |
| Final order firewall | VAR | Order thread'de SEND + EXIT |
| Kill switch | VAR, eşiklere bağlı | MVP config'te eşik bloğu yok |
| Eski/kesik veri koruması | KISMİ | Strateji tarafında giriş için sabit 5 s; firewall'da yok |
| Ret/bağlantı hatası korumaları | KISMİ | Bazı API/timestamp/imza hataları kill switch tetikler |
| Emir yaşam döngüsü güvenliği | VAR (bellekte) | FSM ve UNKNOWN yönetimi |
| Kalıcı emir kaydı / restart uzlaştırma | YOK / KISMİ | Kalıcı kayıt yok; yalnız açık emir ve bakiye kapısı |
| 30 USDT / qty 1936,5 / fiyat 2,582 zorunlu testi | BULUNAMADI | Bu senaryo ve `send_order` sayacı testlerde bulunmamıştır |

## 9. Testler

### `tools/order_guard_test.cpp`

Test kapsamı:

- Cap/qty hesapları.
- Rastgele durumlar.
- `final_check` ret yolları.
- Maruziyet.
- `RateGate`.
- Başlangıç kararı.

Güncel `order_guard.hpp` ile test arasında imza uyumsuzluğu vardır. Test `startup_decision(bool, int, double, double, double)` çağırırken header `startup_decision(const StartupProbe&)` imzasını kullanır.

Ayrıca test `final_check` / `cap_qty` fonksiyonlarını sınar; bunlar üretim emir yolundaki `final_order_firewall` ile aynı değildir.

### `tools/mvp_order_bridge_test.cpp`

İmza ve libcurl ile testnet `POST /api/v3/order` köprü testi yapar. Tek başına güvenlik mantığının doğrulandığını göstermez.

### `phase2_fsm_test.cpp`

FSM ve `dispatch_order_result` senaryolarını test eder. KZ-P0-02 için ilk POLL sırası ayrıca kapsanmalıdır.

### Test doğrulama kuralı

Bu dokümanda listelenen testlerin çalıştırıldığı varsayılmaz. Test sonucu yalnızca gerçekten çalıştırıldığında kaydedilir.

## 10. AI için güvenlik değişikliği protokolü

1. Güvenlik-kritik dosyalar:
   - `include/order_guard.hpp`
   - `include/strategy_core.hpp`
   - `include/engine/types.hpp`
   - `src/main.cpp`
   - `include/config.hpp`
   - `binance_http_client.hpp`

2. Bu dosyalardaki her değişiklik için:
   - Sorun tanımlanır.
   - Beklenen davranış yazılır.
   - Test senaryosu hazırlanır.
   - Değişiklik yapılır.
   - Test gerçekten çalıştırılır.
   - Sonuç kaydedilir.

3. Kontrol silme, tolerans artırma, fail-closed davranışını değiştirme, EXIT muafiyetini genişletme, kill switch otomatik reset ekleme veya `dry_run` varsayılanını değiştirme insan onayı olmadan yapılmaz.

4. Devre dışı bir kontrolün durumu sessizce gizlenmez. Yeni eşik/kontrol eklenirken kapalı durum loglanabilir olmalıdır.

5. `order_guard.hpp` bağımsız tutulur. STL dışı include, tahsis veya ağ bağımlılığı eklenmez.

6. Üretimdeki firewall ile `order_guard.hpp::final_check` arasındaki ayrışma büyütülmez.

7. Birim testin geçmesi üretim firewall'ının doğru çalıştığını kanıtlamaz.

8. KZ-P0-01 düzeltilmeden TSC tabanlı zaman kontrollerinin güvenilir olduğu kabul edilmez.

9. KZ-P0-02 düzeltilmeden ilk POLL görevinin doğru borsa emir ID'sini kullandığı kabul edilmez.

10. KZ-P0-03 düzeltilmeden EXIT emirlerinin final firewall'dan güvenli biçimde geçebildiği kabul edilmez.

11. Hiçbir P0 bulgusu yalnızca dokümantasyon değişikliğiyle çözülmüş sayılamaz.

12. P0 düzeltmeleri proje sahibi onayı, hedefli testler ve doğrulama sonuçları olmadan tamamlandı olarak işaretlenmez.
