# FSM_GUIDE — Emir Yaşam Döngüsü ve UNKNOWN

Etiketler: **[KOD]** · **[ÇIKARIM]** · **[DOĞRULANMADI]**

Kaynak: `include/engine/types.hpp`, `include/strategy_core.hpp`, `src/main.cpp`.
Test: `phase2_fsm_test.cpp`.

## 1. İki ayrı durum alanı

- **`OrderStatus`:** Borsa/REST kaynaklı ham sonuç (`OrderResult.status`).
- **`OrderState`:** Yerel FSM durumu (`OrderSlot.fsm_state`); yerel yaşam döngüsünün tek doğru kaynağıdır.
- `OrderSlot.status`, eski uyumluluk aynasıdır ve `apply_state` ile FSM durumundan türetilir.

`OrderStatus` değerleri:

`NEW, ACK, PARTIALLY_FILLED, FILLED, CANCELED, REJECTED, EXPIRED, PENDING, ERROR, PENDING_CANCEL`

`OrderState` değerleri:

`SIGNAL, CREATED, PREFLIGHT, SUBMITTED, ACK, NEW, PARTIAL, FILLED, CANCEL_REQUEST, CANCEL_PENDING, CANCELED, REJECTED, EXPIRED, UNKNOWN`

## 2. Terminal ve terminal olmayan durumlar

Terminal durumlar:

- `FILLED`
- `CANCELED`
- `REJECTED`
- `EXPIRED`

Slot terminale girdiğinde `apply_state` slotu tombstone yapar (`SlotState::DELETED`), `active_slots` azalır ve bitmap biti temizlenir.

`UNKNOWN` terminal değildir. Slot `ACTIVE` kalır ve çözülene kadar emir yaşam döngüsünde tutulur.

## 3. Geçiş tablosu (`ALLOWED`) [KOD]

| Mevcut | İzinli sonraki |
|---|---|
| `SIGNAL` | `CREATED`, `REJECTED` |
| `CREATED` | `PREFLIGHT`, `REJECTED` |
| `PREFLIGHT` | `SUBMITTED`, `REJECTED` |
| `SUBMITTED` | `ACK`, `REJECTED`, `EXPIRED`, `UNKNOWN` |
| `ACK` | `NEW`, `PARTIAL`, `FILLED`, `CANCEL_REQUEST`, `REJECTED`, `EXPIRED`, `UNKNOWN` |
| `NEW` | `PARTIAL`, `FILLED`, `CANCEL_REQUEST`, `CANCELED`, `REJECTED`, `EXPIRED`, `UNKNOWN` |
| `PARTIAL` | `PARTIAL`, `FILLED`, `CANCEL_REQUEST`, `CANCELED`, `EXPIRED`, `UNKNOWN` |
| `CANCEL_REQUEST` | `CANCEL_PENDING`, `CANCELED`, `PARTIAL`, `FILLED`, `REJECTED`, `EXPIRED`, `UNKNOWN` |
| `CANCEL_PENDING` | `CANCELED`, `PARTIAL`, `FILLED`, `REJECTED`, `EXPIRED`, `UNKNOWN` |
| `UNKNOWN` | `NEW`, `PARTIAL`, `FILLED`, `CANCEL_REQUEST`, `CANCELED`, `REJECTED`, `EXPIRED` |
| `FILLED / CANCELED / REJECTED / EXPIRED` | Hiçbiri |

Önemli kurallar:

- `SUBMITTED` durumundan doğrudan `CANCEL_REQUEST` geçişi yoktur; borsa ID'si henüz bilinmeyebilir.
- `NEW/PARTIAL` durumundan doğrudan `CANCEL_PENDING` geçişi yoktur; önce `CANCEL_REQUEST` gerekir.
- `UNKNOWN` durumundan `ACK`, `SUBMITTED` veya `CANCEL_PENDING` geçişi yoktur.
- `SIGNAL/CREATED/PREFLIGHT` durumlarından `UNKNOWN` geçişi yoktur; bu aşamalarda emir borsaya gönderilmemiştir.

## 4. `FlatOrderMap` kuralları [KOD]

- Anahtar yerel ID'dir.
- Giriş emirleri `fsm_entry_seq` ile monoton ID alır.
- Çıkış emirleri `FSM_EXIT_BASE = 1<<56` tabanlı `fsm_exit_seq` kullanır.
- `local_order_id >= FSM_EXIT_BASE` olması EXIT emrini tanımlar.
- `r.order_id` borsa emir ID'sidir.
- `insert()` slotu `SIGNAL` durumunda açar.
- `active_slots >= 4096` ise ekleme başarısız olur.
- `SUBMITTED → NEW/PARTIAL/FILLED` geçişinde örtük `ACK` uygulanabilir.
- Geçersiz geçiş slotu `UNKNOWN` durumuna düşürür ve `INVALID` sonucu üretir.

`OrderResult` eşlemesi:

| REST sonucu | FSM sonucu |
|---|---|
| `NEW/PENDING` | `NEW` |
| `ACK` | `ACK` |
| `PARTIALLY_FILLED` | `PARTIAL` |
| `FILLED` | `FILLED` |
| `CANCELED` | `CANCELED` |
| `REJECTED` | `REJECTED` |
| `EXPIRED` | `EXPIRED` |
| `PENDING_CANCEL` | `CANCEL_PENDING` |
| `ERROR` | Belirsizse `UNKNOWN`, değilse `REJECTED` |

`FsmVerdict` değerleri:

- `APPLIED`
- `DUPLICATE`
- `STALE`
- `INVALID`
- `GHOST`

`STALE` örnekleri:

- `PARTIAL/CANCEL_REQUEST/CANCEL_PENDING` durumundayken gelen eski `NEW/PENDING` raporu.
- `filled_qty` değerinin önceki dolum miktarından küçük olması.
- Aynı `PARTIAL` raporunun yeni dolum olmadan tekrarlanması.

`has_live_exit()` aktif EXIT slotu olup olmadığını kontrol eder. `consistent()` aktif slot sayısı ile bitmap sayısının tutarlılığını kontrol eder.

## 5. Emir yaşam döngüsü — kodda nerede ne olur

### Giriş (canlı)

`maybe_send_order`:

1. `insert(local_id)` ile `SIGNAL` slotu açılır.
2. `CREATED` ve `PREFLIGHT` aşamalarına geçilir.
3. `fw_publish_exposure` çağrılır.
4. `push_task(SEND)` ile görev order thread'e aktarılır.
5. Halka doluysa slot `REJECTED` olur ve `awaiting_fill=false` yapılır.
6. Görev kuyruğa girdiyse FSM `SUBMITTED` durumuna geçer.
7. Order thread `final_order_firewall` kontrolünü uygular.
8. Firewall reddederse `REJECTED("FW_REJECT")` sonucu üretilir ve kill switch tetiklenir.
9. Firewall geçilirse gönderim kapısı kontrol edilir ve `new_order` çağrılır.
10. REST sonucu FSM'e aktarılır.

REST yanıtında:

- `FILLED` → `FILLED`
- `PARTIALLY_FILLED` → `PARTIAL`
- Diğer bekleyen yanıtlar → `PENDING` / FSM `NEW`

`OrderStatus::ACK` doğrudan üretilmez. `ACK`, bazı `SUBMITTED → NEW/PARTIAL/FILLED` geçişlerinde örtük uygulanır.

### Çıkış

`send_exit_order` benzer `CREATED → PREFLIGHT → SUBMITTED` akışını kullanır.

- Emir tipi `MARKET`'tir.
- Client ID `KUSAK_EXIT_*` biçimindedir.
- Halka doluysa slot `REJECTED` olur.
- EXIT emirleri de order thread'de final firewall kontrolüne girer. Bu davranış KZ-P0-03 ile ilişkilidir; ayrıntı `SAFETY_POLICY.md` §4.3'tedir.

### POLL

Strateji thread'i `awaiting_fill && !dry_run && !poll_in_flight` koşuluyla POLL görevi üretir. Order thread `get_signed` ile borsaya sorgu gönderir.

POLL yanıt ayrıştırmasında:

- `FILLED` → `FILLED`
- `PARTIALLY_FILLED` → `PARTIAL`
- `CANCELED` → `CANCELED`
- `REJECTED` → mevcut kod yolunda `CANCELED` olarak daraltılabilir.
- `EXPIRED` → mevcut kod yolunda `CANCELED` olarak daraltılabilir.
- Diğer durumlar → `PENDING`

HTTP hatası `ERROR("HTTPnnn")`, status alanı yoksa `ERROR("JSON_STATUS")` üretebilir. Bunlar belirsiz sonuçlardır.

### İptal

İptal akışında `CANCEL_REQUEST` ile iptal görevi hazırlanır. Görev kuyruğa girdikten sonra `CANCEL_PENDING` durumuna geçilir. Terminal `CANCELED` durumu borsa teyidiyle oluşmalıdır.

İptal tetikleyicileri:

- `enforce_timeouts`
- 30 saniyelik emir TTL kontrolü (`request_entry_cancel`)
- Kill switch `ks_cancel_all`

`enforce_timeouts` için mevcut zaman eşikleri:

- `NEW/ACK`: `MAKER_REST_TIMEOUT_NS = 6 s`
- `PARTIAL` ve iptal yeniden denemesi: `FILL_TIMEOUT_NS = 2 s`

`exchange_order_id == 0` ise borsa emir ID'si bulunmadığından iptal gönderilemez.

`request_entry_cancel` EXIT emirlerini iptal etmez. `CANCEL_PENDING` durumunda ikinci iptal isteği gönderilmez.

`ks_cancel_all` EXIT emirlerini atlar. Henüz borsaya gönderilmemiş `SIGNAL/CREATED/PREFLIGHT` slotları doğrudan `REJECTED` yapılabilir.

Order thread CANCEL görevinde `delete_signed` çağırır:

- `ok()` → `CANCELED`
- Başarısız yanıt → `ERROR("CANC_HTTPnnn")` ve belirsiz durum

### Dry-run

Giriş ve çıkış anında sentetik `FILLED` sonucu FSM'den geçirilir. Slot terminale geçer ve tombstone olur.

## 6. Sonuç dağıtımı (`dispatch_order_result`)

Her sonuç önce `flat_order_map.update()` üzerinden FSM'e girer. Yalnızca `APPLIED` sonuçlar yan etki üretir.

- `GHOST`, `DUPLICATE`, `STALE`: Loglanır; yan etki oluşturmaz.
- `INVALID`: `UNKNOWN_STATE` loglanır, kill switch tetiklenir, `awaiting_fill` açık kalır.
- `UNKNOWN`: Pozisyon ve kilitli sermaye korunur; kill switch tetiklenir.
- `FILLED`: Girişte pozisyon kurulur; çıkışta PnL gerçekleşir ve pozisyon kapatılır.
- `PARTIAL`: Kümülatif miktar ve maliyet güncellenir.
- `CANCELED/REJECTED/EXPIRED`: Terminal uzlaştırma ve kısmi dolum korunumu uygulanır.
- `ERROR`: Kesin ret veya belirsiz hata ayrımına göre işlenir.

## 7. UNKNOWN — tanım ve kısıtlar

### Tanım

`UNKNOWN`, emrin borsaya ulaşmış olabileceği ancak mevcut yanıtla durumunun kesin olarak belirlenemediği anlamına gelir.

Zaman aşımı veya ağ hatası, emrin gönderilmediğini kanıtlamaz.

### UNKNOWN'a giriş yolları [KOD]

1. `ERROR` sonucu ve `error_msg` öneki `NET_`, `HTTP`, `JSON` veya `CANC_` ise belirsiz hata kabul edilebilir.
2. Geçersiz FSM geçişi (`INVALID`) slotu `UNKNOWN` durumuna taşıyabilir.
3. SEND yolunda borsa hata kodu yoksa `NET_UNKNOWN` üretilebilir.

Kesin borsa hata kodları, ilgili hata ayrıştırma kurallarına göre `REJECTED` olarak işlenebilir.

### UNKNOWN durumundayken uygulanan kısıtlar

- `trip_kill_switch_on_unknown`, `kill_switch=true` yazar.
- `maybe_send_order` kill switch aktifken yeni giriş göndermez.
- Order thread gönderim kapısı SEND görevlerini `KS_GATE` ile reddedebilir.
- Pozisyon ve kilitli sermaye silinmez.
- `awaiting_fill` açık kalır.
- `exchange_order_id != 0` ise iptal akışına alınabilir.
- POLL ile emir durumu çözülmeye çalışılır.
- `UNKNOWN` terminal değildir; aktif slot olarak kalır.
- Kapanışta çözülemeyen emirler silinmez; operatörün borsada kontrol etmesi gerekir.
- Kill switch otomatik olarak temizlenmez. SAFE MODE ve operatör reset prosedürü gerekir.

### Bilinen Zafiyetler

#### KZ-P0-02 — İlk POLL görevinin yerel ID ile gönderilmesi

<badge>KRİTİK — P0</badge>

İlk giriş emri hazırlanırken:

- `strategy_core.hpp` içinde `active_order_id = ++ctx.fsm_entry_seq` ile yerel FSM ID'si atanır.
- `main.cpp` içindeki POLL üretimi `active_order_id` değerini `pt.order_id` olarak kullanır.
- `last_poll_ns` başlangıçta `0` değerindedir.
- İlk POLL kontrolü, SEND göreviyle aynı döngü/tur içerisinde POLL görevinin hazırlanmasına izin verebilir.
- Borsa emir ID'si ancak REST yanıtının ilgili sonuç dalında alınır ve `active_order_id` güncellenir.

Bu nedenle ilk POLL görevi, borsanın gerçek emir ID'si yerine yerel FSM ID'sini `orderId` olarak kullanabilir.

Bu bulgu proje sahibi tarafından doğrulanmıştır ve kesin zafiyet olarak kabul edilir.

**Etki zinciri:**

1. SEND görevi kuyruğa alınır.
2. Borsa emir ID'si henüz alınmamıştır.
3. İlk POLL görevi aynı yerel ID ile hazırlanabilir.
4. Order thread POLL sorgusunu borsa `orderId` alanıyla yapar.
5. Sorgu yanlış emir ID'sini kullanabilir.
6. Emir durumu çözülemezse `UNKNOWN` akışı ve kill switch devreye girebilir.
7. `exchange_order_id` bilinmiyorsa iptal de gönderilemeyebilir.

**Düzeltme çalışmasında korunacak kurallar:**

- Yerel FSM ID'si ile borsa emir ID'si birbirinin yerine kullanılamaz.
- POLL yalnızca geçerli borsa emir ID'si veya ayrıca doğrulanmış bir sorgu kimliğiyle yapılmalıdır.
- SEND yanıtı ile ilk POLL arasındaki sıra açıkça yönetilmelidir.
- `UNKNOWN` güvenlik davranışı devre dışı bırakılamaz.
- İlk POLL senaryosu ayrı test edilmelidir.
- Test, SEND/POLL kuyruk sırasını ve REST yanıtının gecikmesini de kapsamalıdır.

### Diğer kod sınırları

1. POLL sorgusu `orderId` kullanır; `origClientOrderId` sorgusunun kullanıldığı doğrulanmamıştır.
2. Kalıcı emir kaydı yoktur. `FlatOrderMap` yalnızca bellektedir.
3. Restart sonrası uzlaştırma yalnızca açık emir ve bakiye kontrolüne dayanır; geçmiş fill uzlaştırması ve `ctx.position` yeniden kurulumu bulunmamıştır.
4. POLL yolunda `REJECTED/EXPIRED` durumlarının `CANCELED` olarak daraltılması emir sonucunun ayrıntısını kaybettirebilir.
5. Tombstone kayıtlarının `EMPTY` durumuna dönmediği senaryolarda uzun vadeli slot arama maliyeti artabilir.
6. `run_order_thread` içinde `live_order_client` bulunmayan SEND/EXIT yolunun sonuç üretmeden devam ettiği durumlar ayrıca değerlendirilmelidir.

## 8. Test

`phase2_fsm_test.cpp`, FSM ve `dispatch_order_result` senaryolarını test eder. EXIT'in POLL'dan gelen FILLED sonucu, kısmi çıkış uzlaştırması ve benzeri senaryolar test kapsamındadır.

Kök dizindeki eski `fsm_test` ikilisi güncel kaynak kodun doğrulandığını göstermez.

### KZ-P0-02 için zorunlu test senaryoları

1. SEND görevi kuyruğa girer girmez POLL üretimi kontrol edilir.
2. Borsa emir ID'si henüz alınmamışken POLL gönderilmediği doğrulanır.
3. SEND yanıtı gecikmeli döndüğünde POLL davranışı test edilir.
4. REST yanıtı borsa emir ID'sini döndürdüğünde ilk POLL'un doğru ID kullandığı doğrulanır.
5. Ağ hatası ve belirsiz SEND sonucunda `UNKNOWN` güvenliği korunur.
6. Borsa emir ID'si bilinmiyorken iptal gönderildiği varsayılmaz.
7. Testler hem normal yanıt hem gecikme/hata senaryolarını kapsar.

Test bu çalışmada çalıştırılmış sayılmaz. Test sonucu yalnızca gerçekten çalıştırıldığında kaydedilir.

## 9. AI değişiklik kuralları (FSM)

1. `fsm_detail::ALLOWED` tablosuna geçiş ekleme/çıkarma yalnızca açık gerekçe, test ve insan onayıyla yapılır.
2. `UNKNOWN` çıkışlarını veya `SUBMITTED → UNKNOWN` girişini gerekçesiz değiştirme.
3. `UNKNOWN` durumunda güvenli varsayım adı altında yeni emir gönderen yol ekleme.
4. Terminal durumdan geri dönüş ekleme.
5. İptal isteğini gerçekleşmiş iptal sayma. `CANCELED` borsa teyidiyle oluşmalıdır.
6. `ERROR` → `REJECTED/UNKNOWN` ayrımını değiştirmek güvenlik değişikliğidir.
7. EXIT kimliği `>= FSM_EXIT_BASE` kuralına dayanır; ID üretimini bu aralığı bozacak şekilde değiştirme.
8. Yerel FSM ID'si ile borsa emir ID'sini karıştırma.
9. KZ-P0-02 düzeltilmeden ilk POLL davranışının güvenli olduğu varsayılmaz.
10. FSM değişikliklerinde test yalnızca geçiş tablosunu değil, gerçek SEND/POLL/CANCEL görev sırasını da kapsamalıdır.
