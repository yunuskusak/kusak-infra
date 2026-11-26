#pragma once
#ifndef KUSAK_ENGINE_TYPES_HPP
#define KUSAK_ENGINE_TYPES_HPP

// engine/types.hpp — V13 Ghost Engine — Order/Position Tipleri
// ─────────────────────────────────────────────────────────────────────────────
// main.cpp satır 246–592'den taşındı. Orijinal kod değiştirilmeden kopyalandı.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include "infra/time.hpp"              // FORCE_INLINE, tsc_ns()
#include "engine/market_event.hpp"     // PositionSide

// ─────────────────────────────────────────────────────────────────────────────
// Position (V12'den değişmedi)
// ─────────────────────────────────────────────────────────────────────────────
struct Position {
    PositionSide side        = PositionSide::NONE;
    double       entry_price = 0.0;
    double       qty         = 0.0;
    double       notional    = 0.0;
    double       stop_loss   = 0.0;
    double       trail_delta = 0.0;
    double       trail_stop  = 0.0;
    double       high_water  = 0.0;

    bool is_open()  const noexcept { return side != PositionSide::NONE; }
    bool is_long()  const noexcept { return side == PositionSide::LONG;  }
    bool is_short() const noexcept { return side == PositionSide::SHORT; }

    void update_trail(double price) noexcept {
        if (!is_open() || trail_delta <= 0.0) return;
        if (is_long()) {
            if (price > high_water) {
                high_water = price;
                trail_stop = high_water - trail_delta;
            }
        } else {
            if (high_water <= 0.0 || price < high_water) {
                high_water = price;
                trail_stop = high_water + trail_delta;
            }
        }
    }

    bool should_exit(double price) const noexcept {
        if (!is_open()) return false;
        if (is_long()) {
            if (stop_loss  > 0.0 && price <= stop_loss)  return true;
            if (trail_stop > 0.0 && price <= trail_stop) return true;
        } else {
            if (stop_loss  > 0.0 && price >= stop_loss)  return true;
            if (trail_stop > 0.0 && price >= trail_stop) return true;
        }
        return false;
    }

    void reset() noexcept { *this = Position{}; }
};

// ─────────────────────────────────────────────────────────────────────────────
// [V13-STRUCT] FixedString<N> — yığın serbest, dalsız kopyalama
// ─────────────────────────────────────────────────────────────────────────────
template<std::size_t N>
struct FixedString {
    char    data[N];
    uint8_t len;

    constexpr FixedString() noexcept : data{}, len(0) {}

    FORCE_INLINE void set(const char* src, uint8_t l) noexcept {
        len = (l < static_cast<uint8_t>(N)) ? l : static_cast<uint8_t>(N - 1);
        std::memcpy(data, src, len);
        data[len] = '\0';
    }

    FORCE_INLINE const char* c_str() const noexcept { return data; }
    FORCE_INLINE bool        empty()  const noexcept { return len == 0; }
};

// ─────────────────────────────────────────────────────────────────────────────
// [V13-STRUCT] Enum tanımları (spec §1'e uygun)
// ─────────────────────────────────────────────────────────────────────────────
enum class OrderSide    : uint8_t { BUY, SELL };
enum class OrderType    : uint8_t { LIMIT, MARKET, LIMIT_MAKER }; // [FIX-MVP-MAKER] LIMIT_MAKER eklendi
enum class TimeInForce  : uint8_t { GTC, IOC, FOK };

// KUSAK uzantısı: task gönderim türü (OrderTask.kind alanında taşınır)
enum class TaskKind     : uint8_t { SEND, POLL, EXIT, CANCEL };

// Spec §4 OrderStatus + KUSAK uzantıları (PENDING, ERROR)
enum class OrderStatus  : uint8_t {
    NEW              = 0,
    ACK              = 1,
    PARTIALLY_FILLED = 2,
    FILLED           = 3,
    CANCELED         = 4,
    REJECTED         = 5,
    EXPIRED          = 6,
    PENDING          = 7,   // REST poll "NEW" yanıtı
    ERROR            = 8,   // HTTP/JSON hatası
    PENDING_CANCEL   = 9    // [TIER-1] timeout cancel gönderildi, borsa teyidi bekleniyor
};


// ─────────────────────────────────────────────────────────────────────────────
// [PHASE2-FSM] OrderState — emir yasam dongusu, TEK DOGRU KAYNAK (FlatOrderMap slotu)
// Basari : SIGNAL→CREATED→PREFLIGHT→SUBMITTED→ACK→NEW→PARTIAL→FILLED
// Iptal  : NEW/PARTIAL→CANCEL_REQUEST→CANCEL_PENDING→CANCELED
// Hata   : REJECTED / EXPIRED / UNKNOWN (UNKNOWN = reconcile bekler, terminal DEGIL)
// OrderStatus (borsa/REST kaynakli ham durum) korunur; FSM onu OrderState'e cevirir.
// ─────────────────────────────────────────────────────────────────────────────
enum class OrderState : uint8_t {
    SIGNAL = 0, CREATED, PREFLIGHT, SUBMITTED, ACK, NEW, PARTIAL, FILLED,
    CANCEL_REQUEST, CANCEL_PENDING, CANCELED, REJECTED, EXPIRED, UNKNOWN
};

constexpr const char* order_state_name(OrderState s) noexcept {
    constexpr const char* N[] = {"SIGNAL","CREATED","PREFLIGHT","SUBMITTED","ACK","NEW","PARTIAL",
        "FILLED","CANCEL_REQUEST","CANCEL_PENDING","CANCELED","REJECTED","EXPIRED","UNKNOWN"};
    return N[static_cast<uint8_t>(s)];
}
constexpr bool is_terminal_state(OrderState s) noexcept {
    return s == OrderState::FILLED || s == OrderState::CANCELED ||
           s == OrderState::REJECTED || s == OrderState::EXPIRED;
}

namespace fsm_detail {
constexpr uint16_t bit(OrderState s) noexcept { return static_cast<uint16_t>(1u << static_cast<uint8_t>(s)); }
template<class... S> constexpr uint16_t mask(S... s) noexcept { return static_cast<uint16_t>((uint16_t{0} | ... | bit(s))); }
using OS = OrderState;
// Satir = mevcut durum, bit maskesi = izinli sonraki durumlar. Terminaller: 0 (cikis yok).
constexpr uint16_t ALLOWED[14] = {
    /*SIGNAL        */ mask(OS::CREATED, OS::REJECTED),
    /*CREATED       */ mask(OS::PREFLIGHT, OS::REJECTED),
    /*PREFLIGHT     */ mask(OS::SUBMITTED, OS::REJECTED),
    /*SUBMITTED     */ mask(OS::ACK, OS::REJECTED, OS::EXPIRED, OS::UNKNOWN),
    /*ACK           */ mask(OS::NEW, OS::PARTIAL, OS::FILLED, OS::CANCEL_REQUEST, OS::REJECTED, OS::EXPIRED, OS::UNKNOWN),
    /*NEW           */ mask(OS::PARTIAL, OS::FILLED, OS::CANCEL_REQUEST, OS::CANCELED, OS::REJECTED, OS::EXPIRED, OS::UNKNOWN),
    /*PARTIAL       */ mask(OS::PARTIAL, OS::FILLED, OS::CANCEL_REQUEST, OS::CANCELED, OS::EXPIRED, OS::UNKNOWN),
    /*FILLED        */ 0,
    /*CANCEL_REQUEST*/ mask(OS::CANCEL_PENDING, OS::CANCELED, OS::PARTIAL, OS::FILLED, OS::REJECTED, OS::EXPIRED, OS::UNKNOWN),
    /*CANCEL_PENDING*/ mask(OS::CANCELED, OS::PARTIAL, OS::FILLED, OS::REJECTED, OS::EXPIRED, OS::UNKNOWN),
    /*CANCELED      */ 0,
    /*REJECTED      */ 0,
    /*EXPIRED       */ 0,
    /*UNKNOWN       */ mask(OS::NEW, OS::PARTIAL, OS::FILLED, OS::CANCEL_REQUEST, OS::CANCELED, OS::REJECTED, OS::EXPIRED),
};
} // namespace fsm_detail

// [PHASE2-FSM] Gecersiz state atlamasi kalkani
constexpr bool is_valid_transition(OrderState from, OrderState to) noexcept {
    return (fsm_detail::ALLOWED[static_cast<uint8_t>(from)] & fsm_detail::bit(to)) != 0;
}

enum class FsmVerdict : uint8_t { APPLIED, DUPLICATE, STALE, INVALID, GHOST };
struct FsmResult { FsmVerdict verdict; OrderState from; OrderState to; OrderState attempted; };

// ─────────────────────────────────────────────────────────────────────────────
// [V13-STRUCT] OrderTask — 128B, sıfır yığın
//
// NOT (Spec §1 sapması): Spec 64B hedefler, fakat
//   uint64_t*2(16) + double*2(16) + enum*3(3) + FixedString<16>(17) +
//   FixedString<32>(33) = 85B > 64B  →  static_assert(== 64) derlenmez.
// Çözüm: 2 cache line = 128B, alignas(64) korundu.
// ─────────────────────────────────────────────────────────────────────────────
struct alignas(64) OrderTask {
    // ── Spec alanları ─────────────────────────────────────────────────────────
    uint64_t        order_id;          //  8 → 8
    uint64_t        ts_send_ns;        //  8 → 16
    uint64_t        local_order_id;    //  8 → 24   (KUSAK-ext)
    double          price;             //  8 → 32
    double          qty;               //  8 → 40
    OrderSide       side;              //  1 → 41
    OrderType       type;              //  1 → 42
    TimeInForce     tif;               //  1 → 43
    TaskKind        kind;              //  1 → 44   (KUSAK-ext: SEND/POLL/EXIT/CANCEL)
    bool            is_exit;           //  1 → 45   (KUSAK-ext)
    uint8_t         _rsv[3];           //  3 → 48
    FixedString<16> symbol;            // 17 → 65
    uint8_t         _rsv2[15];         // 15 → 80   (cache sub-line hizalama)
    FixedString<32> client_oid;        // 33 → 113
    uint8_t         pad[15];           // 15 → 128
};
static_assert(sizeof(OrderTask)  == 128,
    "[V13] OrderTask 128B olmali (spec 85B > 64B; cozum: 2 cache line)");
static_assert(alignof(OrderTask) == 64);

// ─────────────────────────────────────────────────────────────────────────────
// [V13-STRUCT] OrderResult — 128B, sıfır yığın
// ─────────────────────────────────────────────────────────────────────────────
struct alignas(64) OrderResult {
    // ── Spec alanları ─────────────────────────────────────────────────────────
    uint64_t        order_id;          //  8 → 8
    uint64_t        ts_exchange_ns;    //  8 → 16
    uint64_t        ts_recv_ns;        //  8 → 24
    uint64_t        exchange_order_id; //  8 → 32   (KUSAK-ext)
    uint64_t        local_order_id;    //  8 → 40   (KUSAK-ext)
    double          filled_qty;        //  8 → 48
    double          remaining_qty;     //  8 → 56
    double          avg_price;         //  8 → 64
    OrderStatus     status;            //  1 → 65
    bool            is_exit;           //  1 → 66   (KUSAK-ext)
    bool            is_buy;            //  1 → 67   (KUSAK-ext)
    uint8_t         _rsv[5];           //  5 → 72
    FixedString<32> client_oid;        // 33 → 105
    FixedString<16> error_msg;         // 17 → 122  (ERROR durumu için)
    uint8_t         pad[6];            //  6 → 128
};
static_assert(sizeof(OrderResult)  == 128,
    "[V13] OrderResult 128B olmali");
static_assert(alignof(OrderResult) == 64);

// ─────────────────────────────────────────────────────────────────────────────
// [V13-MAP] OrderSlot — 64B (spec §3 birebir)
// ─────────────────────────────────────────────────────────────────────────────
constexpr size_t   MAX_ORDERS      = 4096;
constexpr uint64_t FILL_TIMEOUT_NS = 2'000'000'000ULL; // 2s (spec §4 — API rate limit koruması)
// [PATCH-P1] EXIT emirlerinin yerel ID tabani (StrategyContext::fsm_exit_seq ile ayni deger).
// local_order_id >= FSM_EXIT_BASE  <=>  emir bir EXIT emridir (POLL/CANCEL sonuclarinda is_exit'e guvenme).
constexpr uint64_t FSM_EXIT_BASE   = (1ULL << 56);
// [FIX-MVP-MAKER] LIMIT_MAKER hic dolmadan (status=NEW) kuyrukta bekliyorsa
// ne kadar sure sonra iptal edilecegi. FILL_TIMEOUT_NS'ten kasitli olarak
// ayri/daha toleransli -- o, PARTIALLY_FILLED icin API rate-limit korumasi;
// bu ise "cancel/replace mimarisi yok" MVP kisitiyla basa cikan tek mekanizma.
constexpr uint64_t MAKER_REST_TIMEOUT_NS = 6'000'000'000ULL; // 6s
// [FIX-P36-C16] SEND/EXIT sonucu hic donmezse (kayip/halka dolu) slot SUBMITTED'da sonsuza kadar kalmasin:
// bu sureden sonra borsa ID'siz SUBMITTED slot UNKNOWN'a cekilir (kill switch + origClientOrderId POLL).
// Gerekce: eski cold_post en kotu 4 x 8 s curl + 2.8 s backoff = 34.8 s; +5 s pay => 40 s.
// [FIX-P36-RETRY] sonrasi emir POST'u tek deneme; deger bilerek DUSURULMEDI (proje sahibi karari, ayri madde).
constexpr uint64_t SUBMITTED_LOST_TIMEOUT_NS = 40'000'000'000ULL; // 40s
// [FIX-P37-C19B] UNKNOWN + borsa ID yok + POLL(origClientOrderId) "-2013 emir yok" cozumu:
// borsa, timestamp'i recvWindow'dan eski istegi kabul etmez; UNKNOWN'a giristen bu yana
// recvWindow + PAY gecmis ve art arda MIN_STREAK kez -2013 alinmissa emir "gonderilmedi" sayilir (REJECTED).
// Kill switch bu kararla SIFIRLANMAZ (SAFETY_POLICY §5.4: otomatik reset yok).
constexpr uint64_t UNKNOWN_NOTFOUND_MARGIN_NS  = 10'000'000'000ULL; // 10s (saat ofseti + borsa tutarlilik payi)
constexpr uint32_t UNKNOWN_NOTFOUND_MIN_STREAK = 3;

// E4 prerequisite
enum class SlotState : uint8_t { EMPTY, ACTIVE, DELETED };

// E2 fix — exact layout (64 bytes)
// [FIX-P35] C12: tum alanlar varsayilan ilklendirilir (ilklenmemis bellekte state/gen cop olmasin).
// fsm_state = SIGNAL (enum baslangic degeri 0, en az ilerlemis durum; EMPTY slot icin anlamsiz
// ama zararsiz -- find() state==EMPTY ile eler); status = NEW (0, legacy baslangic).
struct alignas(64) OrderSlot {
    uint64_t    order_id{0};        // 8
    double      filled_qty{0.0};      // 8
    double      remaining_qty{0.0};   // 8
    uint64_t    last_update_ns{0};  // 8

    uint32_t    gen{0};             // 4
    OrderStatus status{OrderStatus::NEW};          // 1  (legacy ayna -- FSM durumundan turetilir)
    SlotState   state{SlotState::EMPTY};           // 1

    OrderState  fsm_state{OrderState::SIGNAL};     // 1  [PHASE2-FSM] tek dogru kaynak
    uint8_t     fsm_flags{0};       // 1  (rezerve)
    uint64_t    exchange_order_id{0}; // 8 [PHASE2-FSM] CANCEL/POLL icin borsa ID (anahtar = yerel ID)
    double      avg_price{0.0};       // 8 [PATCH-P0-EXIT] son bildirilen kumulatif ort. dolum fiyati
    char        pad[8]{};          // total = 64
};
static_assert(sizeof(OrderSlot) == 64, "OrderSlot must be 64 bytes");
static_assert(std::is_trivially_copyable_v<OrderSlot>, "[FIX-P35] OrderSlot trivially copyable olmali");
static_assert(alignof(OrderSlot) == 64, "[FIX-P35] OrderSlot 64B hizali olmali");

// [PHASE2-FSM] Belirsiz hata: emir borsada olabilir (timeout/ag/HTTP/JSON/cancel hatasi).
// Bu durumda REJECTED denmez -> UNKNOWN (reconcile). Kesin borsa kodlari REJECTED'dir.
inline bool is_ambiguous_error(const OrderResult& r) noexcept {
    const char* m = r.error_msg.c_str();
    return std::strncmp(m, "NET_", 4) == 0 || std::strncmp(m, "HTTP", 4) == 0 ||
           std::strncmp(m, "JSON", 4) == 0 || std::strncmp(m, "CANC_", 5) == 0;
}

// E1 fix — forward declaration (exact signature)
inline OrderStatus normalize_terminal_fill_state(OrderStatus status, double remaining_qty) noexcept;

// ─────────────────────────────────────────────────────────────────────────────
// [V13-MAP → TIER-1] FlatOrderMap — Linear Probing + Tombstone (E4)
// SlotState::EMPTY = boş, ACTIVE = dolu, DELETED = tombstone
// ─────────────────────────────────────────────────────────────────────────────
struct FlatOrderMap {
    // ── [ULL-OP3] Active bitmap — sıfır-gecikme dolu-slot taraması ─────────
    // MAX_ORDERS = 4096 → 64 adet 64-bit word (4096 / 64).
    // Slot i aktif ⇔ bitmap[i >> 6] bit (i & 63) = 1.
    // Bit-set: OR (|=)   |   Bit-clear: AND NOT (&= ~)
    // Tarama: __builtin_ctzll → tek çevrimde sıradaki dolu slot indeksi.
    // std::bitset KULLANILMIYOR; ham uint64_t + bitwise operatörler.
    static constexpr size_t   BITMAP_WORDS = MAX_ORDERS / 64;  // 64
    static_assert(MAX_ORDERS % 64 == 0, "MAX_ORDERS must be multiple of 64");

    OrderSlot slots[MAX_ORDERS]{};   // [FIX-P35] value-init
    alignas(64) uint64_t active_bitmap[BITMAP_WORDS]{};
    size_t    active_slots{0};
    static constexpr size_t MASK = MAX_ORDERS - 1;

    // ── Bitmap helpers (FORCE_INLINE; iki'nin gücü shift/and) ──────────────
    FORCE_INLINE void set_active(size_t idx) noexcept {
        active_bitmap[idx >> 6] |= (uint64_t{1} << (idx & 63));
    }
    FORCE_INLINE void clear_active(size_t idx) noexcept {
        active_bitmap[idx >> 6] &= ~(uint64_t{1} << (idx & 63));
    }

    // ── find — EMPTY break, DELETED skip, scan limiti ile sonsuz döngü koruması
    FORCE_INLINE OrderSlot* find(uint64_t order_id) noexcept {
        size_t idx = static_cast<size_t>(order_id & MASK);
        for (size_t scanned = 0; scanned < MAX_ORDERS; ++scanned) {
            OrderSlot& s = slots[idx];
            if (s.state == SlotState::EMPTY) return nullptr;
            if (s.state == SlotState::ACTIVE && s.order_id == order_id) return &s;
            idx = (idx + 1) & MASK;
        }
        return nullptr;  // tüm slotlar DELETED → bulunamadı
    }

    // ── get (uyumluluk katmanı) ──────────────────────────────────────────────
    FORCE_INLINE OrderSlot& get(uint64_t order_id) noexcept {
        OrderSlot* s = find(order_id);
        if (s) return *s;
        static OrderSlot dummy{};
        return dummy;
    }
    FORCE_INLINE const OrderSlot& get(uint64_t order_id) const noexcept {
        size_t idx = static_cast<size_t>(order_id & MASK);
        for (size_t scanned = 0; scanned < MAX_ORDERS; ++scanned) {
            const OrderSlot& s = slots[idx];
            if (s.state == SlotState::EMPTY) break;
            if (s.state == SlotState::ACTIVE && s.order_id == order_id) return s;
            idx = (idx + 1) & MASK;
        }
        static const OrderSlot dummy{};
        return dummy;
    }

    // ── insert — DELETED reuse, EMPTY stop, scan limiti ile sonsuz döngü koruması (E4)
    FORCE_INLINE bool insert(uint64_t order_id, double qty) noexcept {
        if (active_slots >= MAX_ORDERS) return false;

        size_t idx = static_cast<size_t>(order_id & MASK);
        size_t first_deleted = SIZE_MAX;

        for (size_t scanned = 0; scanned < MAX_ORDERS; ++scanned) {
            OrderSlot& slot = slots[idx];

            if (slot.state == SlotState::EMPTY) {
                size_t target = (first_deleted != SIZE_MAX) ? first_deleted : idx;
                OrderSlot& s = slots[target];
                s.order_id       = order_id;
                s.gen++;
                s.status         = OrderStatus::NEW;
                s.fsm_state      = OrderState::SIGNAL;   // [PHASE2-FSM]
                s.exchange_order_id = 0;
                s.filled_qty     = 0.0;
                s.avg_price      = 0.0;
                s.remaining_qty  = qty;
                s.last_update_ns = tsc_ns();
                s.state          = SlotState::ACTIVE;
                ++active_slots;
                set_active(target);  // [ULL-OP3] bitmap → ACTIVE
                return true;
            }

            if (slot.state == SlotState::DELETED && first_deleted == SIZE_MAX) {
                first_deleted = idx;
            }

            if (slot.state == SlotState::ACTIVE && slot.order_id == order_id) {
                // aynı order_id zaten var → güncelle (savunma)
                slot.remaining_qty  = qty;
                slot.status         = OrderStatus::NEW;
                slot.last_update_ns = tsc_ns();
                return true;
            }

            idx = (idx + 1) & MASK;
        }

        // Tüm slotlar DELETED — first_deleted varsa oraya insert et
        if (first_deleted != SIZE_MAX) {
            OrderSlot& s = slots[first_deleted];
            s.order_id       = order_id;
            s.gen++;
            s.status         = OrderStatus::NEW;
            s.fsm_state      = OrderState::SIGNAL;       // [PHASE2-FSM]
            s.exchange_order_id = 0;
            s.filled_qty     = 0.0;
            s.avg_price      = 0.0;
            s.remaining_qty  = qty;
            s.last_update_ns = tsc_ns();
            s.state          = SlotState::ACTIVE;
            ++active_slots;
            set_active(first_deleted);  // [ULL-OP3] bitmap → ACTIVE
            return true;
        }

        return false;  // harita dolu, insert başarısız
    }

    // ── [PHASE2-FSM] State uygulama (tek giris noktasi): fsm_state + legacy status + tombstone ──
    FORCE_INLINE void apply_state(OrderSlot& s, OrderState to) noexcept {
        s.fsm_state = to;
        switch (to) {
            case OrderState::ACK:            s.status = OrderStatus::ACK;              break;
            case OrderState::PARTIAL:        s.status = OrderStatus::PARTIALLY_FILLED; break;
            case OrderState::FILLED:         s.status = OrderStatus::FILLED;           break;
            case OrderState::CANCEL_REQUEST:
            case OrderState::CANCEL_PENDING: s.status = OrderStatus::PENDING_CANCEL;   break;
            case OrderState::CANCELED:       s.status = OrderStatus::CANCELED;         break;
            case OrderState::REJECTED:       s.status = OrderStatus::REJECTED;         break;
            case OrderState::EXPIRED:        s.status = OrderStatus::EXPIRED;          break;
            case OrderState::UNKNOWN:        s.status = OrderStatus::PENDING;          break;
            default:                         s.status = OrderStatus::NEW;              break;
        }
        if (is_terminal_state(to) && s.state == SlotState::ACTIVE) {   // tombstone (E4 erase)
            if (active_slots > 0) --active_slots;
            s.state = SlotState::DELETED;
            clear_active(static_cast<size_t>(&s - slots));
        }
    }

    // Tek adim: ayni state = DUPLICATE; SUBMITTED->NEW/PARTIAL/FILLED = ACK ortuk (fill-before-ACK
    // yarisi); gecersiz atlama = UNKNOWN'a dus + INVALID.
    FORCE_INLINE FsmResult step(OrderSlot& s, OrderState to) noexcept {
        const OrderState from = s.fsm_state;
        if (to == from) return {FsmVerdict::DUPLICATE, from, from, to};
        if (from == OrderState::SUBMITTED &&
            (to == OrderState::NEW || to == OrderState::PARTIAL || to == OrderState::FILLED)) {
            apply_state(s, OrderState::ACK);   // REST/WS ACK'i atlandi veya gecikti: ortuk ACK
        }
        if (!is_valid_transition(s.fsm_state, to)) {
            apply_state(s, OrderState::UNKNOWN);
            s.last_update_ns = tsc_ns();
            return {FsmVerdict::INVALID, from, OrderState::UNKNOWN, to};
        }
        apply_state(s, to);
        s.last_update_ns = tsc_ns();
        return {FsmVerdict::APPLIED, from, to, to};
    }

    // Strateji tarafi gecisleri (SUBMITTED, CANCEL_REQUEST, ...) icin: anahtar = yerel ID
    FORCE_INLINE FsmResult transition_key(uint64_t key, OrderState to) noexcept {
        OrderSlot* s = find(key);
        if (!s) return {FsmVerdict::GHOST, OrderState::UNKNOWN, OrderState::UNKNOWN, to};
        return step(*s, to);
    }

    // ── update — borsa/REST sonucunu FSM olayina cevirir ─────────────────────
    // Anahtar: local_order_id (insert() yerel ID ile yapilir; r.order_id borsa ID'sidir).
    FORCE_INLINE FsmResult update(const OrderResult& r) noexcept {
        const uint64_t key = r.local_order_id ? r.local_order_id : r.order_id;
        OrderSlot* s = find(key);
        if (!s) return {FsmVerdict::GHOST, OrderState::UNKNOWN, OrderState::UNKNOWN, OrderState::UNKNOWN};
        const OrderState from = s->fsm_state;

        OrderState to = from;
        switch (r.status) {
            case OrderStatus::NEW:
            case OrderStatus::PENDING:
                // Emir hala acik: iptal/kismi dolum surerken gelen eski "NEW" poll'u olay degildir.
                if (from == OrderState::PARTIAL || from == OrderState::CANCEL_REQUEST ||
                    from == OrderState::CANCEL_PENDING)
                    return {FsmVerdict::STALE, from, from, OrderState::NEW};
                to = OrderState::NEW; break;
            case OrderStatus::ACK:              to = OrderState::ACK;            break;
            case OrderStatus::PARTIALLY_FILLED: to = OrderState::PARTIAL;        break;
            case OrderStatus::FILLED:           to = OrderState::FILLED;         break;
            case OrderStatus::CANCELED:         to = OrderState::CANCELED;       break;
            case OrderStatus::REJECTED:         to = OrderState::REJECTED;       break;
            case OrderStatus::EXPIRED:          to = OrderState::EXPIRED;        break;
            case OrderStatus::PENDING_CANCEL:   to = OrderState::CANCEL_PENDING; break;
            case OrderStatus::ERROR:
                to = is_ambiguous_error(r) ? OrderState::UNKNOWN : OrderState::REJECTED; break;
        }

        // Sira disi / eski rapor: dolum miktari geriye gidemez.
        const bool fill_report = (r.status == OrderStatus::PENDING || r.status == OrderStatus::PARTIALLY_FILLED);
        if (fill_report && r.filled_qty + 1e-12 < s->filled_qty)
            return {FsmVerdict::STALE, from, from, to};

        // Ayni durum tekrari: yalniz YENI dolum varsa uygulanir (ardisik PARTIAL), aksi DUPLICATE.
        if (to == from) {
            if (to == OrderState::PARTIAL && r.filled_qty > s->filled_qty + 1e-12) {
                s->filled_qty = r.filled_qty; s->remaining_qty = r.remaining_qty;
                if (r.avg_price > 0.0) s->avg_price = r.avg_price;
                s->last_update_ns = r.ts_recv_ns ? r.ts_recv_ns : tsc_ns();
                return {FsmVerdict::APPLIED, from, to, to};
            }
            return {FsmVerdict::DUPLICATE, from, from, to};
        }

        const FsmResult fr = step(*s, to);
        if (fr.verdict == FsmVerdict::APPLIED) {
            if (r.filled_qty >= s->filled_qty) { s->filled_qty = r.filled_qty; s->remaining_qty = r.remaining_qty; }
            if (r.avg_price > 0.0) s->avg_price = r.avg_price;
            if (r.exchange_order_id != 0) s->exchange_order_id = r.exchange_order_id;
        }
        return fr;
    }

    // [PATCH-P3] Haritada canli (ACTIVE) bir EXIT slotu var mi? Reversal/yeni giris oncesi son kapi.
    // Yalniz strateji thread'i okur/yazar (tek-yazar); bitmap taramasi <= 64 word.
    FORCE_INLINE bool has_live_exit() const noexcept {
        for (size_t w = 0; w < BITMAP_WORDS; ++w) {
            uint64_t bits = active_bitmap[w];
            const size_t base = w << 6;
            while (bits) {
                const unsigned b = static_cast<unsigned>(__builtin_ctzll(bits));
                bits &= bits - 1;
                const OrderSlot& s = slots[base + b];
                if (s.state == SlotState::ACTIVE && s.order_id >= FSM_EXIT_BASE) return true;
            }
        }
        return false;
    }

    // [PATCH-P4] Iki temsil (active_slots sayaci <-> bitmap popcount) ayrismasin: kapanista dogrulanir.
    bool consistent() const noexcept { return active_slots == active_count(); }

    // [ULL-OP3] Linear scan infazı — popcount over bitmap words.
    size_t active_count() const noexcept {
        size_t n = 0;
        for (size_t w = 0; w < BITMAP_WORDS; ++w) {
            n += static_cast<size_t>(__builtin_popcountll(active_bitmap[w]));
        }
        return n;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// [V13-SPSC] OrderSpscRing<T,N> — emir kanalı için genel SPSC ring
// ─────────────────────────────────────────────────────────────────────────────
template<typename T, size_t N = 1024>
class OrderSpscRing {
    static_assert((N & (N - 1)) == 0, "N power of 2");
    alignas(64) std::atomic<uint32_t> head_{0};
    alignas(64) std::atomic<uint32_t> tail_{0};
    alignas(64) std::atomic<uint64_t> drop_count_{0};
    alignas(64) T                     buf_[N];
public:
    FORCE_INLINE bool push(const T& e) noexcept {
        const uint32_t h    = head_.load(std::memory_order_relaxed);
        const uint32_t next = (h + 1) & (N - 1);
        if (next == tail_.load(std::memory_order_acquire)) {
            drop_count_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        buf_[h] = e;
        std::atomic_thread_fence(std::memory_order_release);
        head_.store(next, std::memory_order_relaxed);
        return true;
    }
    FORCE_INLINE bool pop(T& out) noexcept {
        const uint32_t h = head_.load(std::memory_order_acquire);
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        if (t == h) return false;
        out = buf_[t];
        tail_.store((t + 1) & (N - 1), std::memory_order_release);
        return true;
    }
    FORCE_INLINE uint64_t dropped() const noexcept {
        return drop_count_.load(std::memory_order_relaxed);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// [V13-SPSC] OrderChannel — lock-free, syscall yok (spec §2)
// ─────────────────────────────────────────────────────────────────────────────
template<size_t CAP = 1024>
struct OrderChannel {
    OrderSpscRing<OrderTask,   CAP> tasks;
    OrderSpscRing<OrderResult, CAP> results;
    std::atomic<bool>               stop{false};

    FORCE_INLINE bool push_task(const OrderTask& t) noexcept   { return tasks.push(t); }
    FORCE_INLINE bool pop_task(OrderTask& t)         noexcept  { return tasks.pop(t);  }
    FORCE_INLINE bool push_result(const OrderResult& r) noexcept { return results.push(r); }
    FORCE_INLINE bool pop_result(OrderResult& r)     noexcept  { return results.pop(r); }
    FORCE_INLINE uint64_t task_drops() const noexcept { return tasks.dropped(); }
    FORCE_INLINE uint64_t result_drops() const noexcept { return results.dropped(); }
};

enum class TelemetryCmd : uint8_t { FLUSH_CSV, STOP };

#endif // KUSAK_ENGINE_TYPES_HPP
