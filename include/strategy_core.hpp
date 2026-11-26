#pragma once
// ============================================================================
// KUSAK V13 — Strategy Core (paylasilan karar mantigi)
//
// [FIX-REPLAY-1] Bu dosya, main.cpp'nin canli motorunda kullanilan TAM AYNI
// karar mantigini (StrategyContext, maybe_send_order, process_book_ticker,
// process_agg_trade vb.) src/main.cpp'den DEGISTIRMEDEN buraya tasir. Amac:
// hem canli motor (main.cpp) hem de replay/backtest araclari AYNI kodu
// kullansin -- boylece "botun karar mantigi gercek veride nasil davranir"
// sorusuna, ayri/paralel bir yeniden-uygulama (eski paper_trader.hpp gibi)
// DEGIL, birebir canlida calisacak kodun kendisiyle cevap verilebilir.
//
// main.cpp bu dosyayi #include eder ve kendi kopyasini SILMISTIR -- iki
// yerde ayni kod birbirinden BAGIMSIZ SURUKLENEMEZ (drift riski ortadan
// kalkar).
// ============================================================================
#include <string>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include "research_types.hpp"
#include "config.hpp"
#include "logger.hpp"
#include "vpin_velocity_tracker.hpp"
#include "regime_detector.hpp"
#include "auto_param_adapter.hpp"
#include "risk_scorer.hpp"
#include "telemetry_sink.hpp"
#include "adverse_selection_tracker.hpp"
#include "binance_rest_client.hpp"
#include "binance_http_client.hpp"
#include "latency_monitor.hpp"
#include "infra/time.hpp"
#include "infra/spsc_ring.hpp"
#include "engine/types.hpp"
#include "core_types.hpp"
#include "order_guard.hpp"   // [PHASE0-GUARD]
#include "engine/poll_query.hpp"   // [FIX-P0-02]

// ─────────────────────────────────────────────────────────────────────────────
// [FIX-SELFCONTAINED-1] normalize_terminal_fill_state — onceden main.cpp'de
// tanimliydi, strategy_core.hpp (dispatch_order_result icinde) buna
// bagimliydi ama kendisi TANIMLAMIYORDU. main.cpp'de include sirasi
// (bu tanim, once main.cpp'de, strategy_core.hpp'nin #include'undan ONCE
// yer aldigi icin) tesadufen calisiyordu -- ama strategy_core.hpp'yi BASKA
// bir dosyadan (replay_harness.cpp gibi) dahil eden HERKES icin
// "undeclared identifier" derleme hatasi bekliyordu. Artik header kendi
// bagimliligini kendi tasiyor -- kimden #include edilirse edilsin calisir.
// ─────────────────────────────────────────────────────────────────────────────
FORCE_INLINE OrderStatus normalize_terminal_fill_state(OrderStatus st,
                                                       double remaining_qty) noexcept {
    if (st == OrderStatus::PARTIALLY_FILLED && remaining_qty <= 0.0) {
        return OrderStatus::FILLED;
    }
    return st;
}

// ─────────────────────────────────────────────────────────────────────────────
// [FIX-SELFCONTAINED-2] enforce_timeouts — Ghost Cancel Fix. Onceden
// main.cpp'de tutuluyordu; canli emir zaman-asimi/iptal mantigi da
// stratejinin TAM davranisinin bir parcasidir (bir maker emri sonsuza kadar
// kuyrukta beklememeli) -- bu yuzden strategy_core.hpp'ye tasindi. Boylece
// ileride replay/backtest araclari da (LIVE mod icin) bu davranisi
// simule edebilir; main.cpp ise bunu kendi ana donguesunden hala cagirir.
//
// Timeout'ta emir PENDING_CANCEL yapılır, slot aktif kalır. Borsa teyidi
// (FILLED veya CANCELED) gelene kadar slot kapatılmaz.
// ─────────────────────────────────────────────────────────────────────────────
struct StrategyContext;

// [FIX-CLOCK-ISOLATION] Govde, StrategyContext tam olarak tanimlandiktan
// SONRA (asagida, struct StrategyContext'in hemen ardindan) veriliyor --
// govde icinde ctx.current_time_ns() cagrildigi icin StrategyContext'in
// TAM (complete) tipi burada henuz mevcut degil. Fonksiyonun konumu/imzasi
// (deklarasyonu) burada, orijinal yerinde KALIYOR.
FORCE_INLINE void enforce_timeouts(StrategyContext& ctx,
                                   FlatOrderMap& map,
                                   OrderChannel<>& ch,
                                   const std::string& symbol) noexcept;

#include <atomic>
#include <charconv>
#include <cstring>
#include <optional>
#include <array>
#include <limits>
#include <memory>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  #include <immintrin.h>   // _mm_pause
#endif

namespace kusak::spin {
// Tek PAUSE: SMT kardesine yer acar, bellek-sirasi ihlali cekilisini azaltir; syscall YOK, thread uyumaz.
[[gnu::always_inline]] inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}
// Ussel artan PAUSE salvosu (1,2,4..64). Sinirli: tek cagri en fazla 64 PAUSE (~mikrosaniye).
// Ilerleme olunca spins=0 ile sifirlanir; ust siniri cagiranin deadline'i koyar.
inline void adaptive_spin(uint32_t& spins) noexcept {
    const uint32_t n = 1u << (spins < 6u ? spins : 6u);
    for (uint32_t i = 0; i < n; ++i) cpu_relax();
    if (spins < 6u) ++spins;
}
} // namespace kusak::spin

// ─────────────────────────────────────────────────────────────────────────────
// [V13-PROFIT-ENGINE] RecentPnLBuffer — Kelly Kriteri için son 50 işlem
// ─────────────────────────────────────────────────────────────────────────────
struct alignas(64) RecentPnLBuffer {
    // [ULL-OP2] SIZE 50 → 64 (power-of-two). Modulo % yok; bitwise & MASK.
    // Integer DIV katliamı bertaraf, predictor entropy düşürüldü.
    static constexpr int SIZE = 64;
    static constexpr int MASK = SIZE - 1;  // 63 = 0b0011'1111
    double buffer[SIZE] = {};
    int index = 0;
    int count = 0;
    double sum_win = 0.0;
    double sum_loss = 0.0;
    int win_count = 0;
    int loss_count = 0;

    void push(double pnl) noexcept {
        if (count >= SIZE) {
            // (index - SIZE) & MASK ≡ (index - SIZE + SIZE) % SIZE (power-of-two).
            // İki'nin tümleyeni aritmetiği + bitwise AND; if-else maskeleme yok.
            const int oldest_idx = (index - SIZE) & MASK;
            double old = buffer[oldest_idx];
            if (old > 0) { sum_win -= old; win_count--; }
            else if (old < 0) { sum_loss -= -old; loss_count--; }
        }
        buffer[index] = pnl;
        if (pnl > 0)       { sum_win += pnl;  win_count++; }
        else if (pnl < 0)  { sum_loss += -pnl; loss_count++; }
        index = (index + 1) & MASK;
        if (count < SIZE) count++;
    }

    double win_rate() const noexcept {
        int total = win_count + loss_count;
        return total > 0 ? static_cast<double>(win_count) / total : 0.0;
    }
    double avg_win() const noexcept {
        return win_count > 0 ? sum_win / win_count : 0.0;
    }
    double avg_loss() const noexcept {
        return loss_count > 0 ? sum_loss / loss_count : 0.0;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// [V13-PROFIT-ENGINE] TradeDirectionBuffer — Piyasa Momentum'u (Son 50 AggTrade Yönü)
// ─────────────────────────────────────────────────────────────────────────────
struct alignas(64) TradeDirectionBuffer {
    // [ULL-OP2] SIZE 50 → 64 (power-of-two). Modulo % yok; bitwise & MASK.
    static constexpr int SIZE = 64;
    static constexpr int MASK = SIZE - 1;  // 63
    int buffer[SIZE] = {};   // +1 = agresif alış, -1 = agresif satış
    int index = 0;
    int count = 0;
    int sum = 0;             // running sum

    void push(int direction) noexcept {
        if (count >= SIZE) {
            const int oldest_idx = (index - SIZE) & MASK;
            sum -= buffer[oldest_idx];
        }
        buffer[index] = direction;
        sum += direction;
        index = (index + 1) & MASK;
        if (count < SIZE) count++;
    }

    double momentum() const noexcept {
        return count > 0 ? static_cast<double>(sum) / count : 0.0;
    }
};

namespace kusak::notify {
struct INotifier {
    virtual ~INotifier() = default;
    virtual void send_circuit_breaker(const char* reason) noexcept = 0;
};
} // namespace kusak::notify
// ─────────────────────────────────────────────────────────────────────────────
// StrategyContext
// [V13]: order_map (unordered_map) → flat_order_map (FlatOrderMap)
//        OrderChannel mutex/CV  → SPSC OrderChannel
//        chrono zaman alanları  → uint64_t (mono_ns)
// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// [PHASE3-KS] KillSwitchState -- yalniz strateji (ana) thread'i yazar/okur.
// Thread'ler arasi paylasilanlar StrategyContext'te atomic'tir (kill_switch, order_rtt_ns, ws_disconnect_total).
// ─────────────────────────────────────────────────────────────────────────────
struct KillSwitchState {
    static constexpr int WS_RING = 32;
    const char* reason  = nullptr;      // literal; tetikleyen metrik
    double      value   = 0.0;          // tetiklendigi andaki deger
    double      limit   = 0.0;
    uint64_t    tripped_ns = 0;
    bool        latched     = false;    // tetik bir kez islendi (log+kilit dosyasi+bildirim)
    bool        safe_mode   = false;    // yeni emir OFF + acik emirler iptal edildi
    bool        lock_written = false;   // kilit dosyasi diske yazildi (manuel reset ancak bu durumda guvenli)
    int         consecutive_losses = 0;
    int         daily_rejects      = 0;
    double      daily_realized_pnl = 0.0;
    int64_t     day_index          = -1;    // UTC gun (epoch/86400)
    double      equity_hwm         = 0.0;   // high-water mark
    int         latency_breach_streak = 0;
    uint64_t    win_start_ns = 0, win_sent0 = 0, win_cancel0 = 0;   // iptal orani penceresi
    uint64_t    ws_ts[WS_RING] = {};  uint32_t ws_head = 0;  uint64_t ws_seen = 0;
    uint64_t    last_cancel_all_ns = 0, last_lock_poll_ns = 0, last_flatten_ns = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// [P1-OBI-TRACK] Bekleyen OBI gözlemleri — sabit boyutlu, heap'siz, kilitsiz.
// Tek thread (strateji) erişir: add() maybe_send_order'dan, poll() process_book_ticker'dan.
// Her sinyal +10/+50/+100/+250/+500/+1000/+5000 ms MID fiyatını toplar; 7/7 dolunca
// ObiEvent olarak TelemetrySink'e verilir ve slottan silinir (swap-with-last).
// ─────────────────────────────────────────────────────────────────────────────
struct PendingObiSignal {
    uint64_t signal_mono_ns = 0;
    uint64_t signal_wall_us = 0;
    uint64_t event_id       = 0;
    double   signal_price   = 0.0;
    double   bid            = 0.0;
    double   ask            = 0.0;
    double   price_after[kusak::telemetry::OBI_N_HORIZONS] = {};
    float    obi            = 0.0f;
    float    spread_bps     = 0.0f;
    float    threshold      = 0.0f;
    uint32_t max_lag_us     = 0;
    int8_t   side           = 0;
    uint8_t  next_h         = 0;    // sıradaki bekleyen horizon indeksi (0..7)
};

struct ObiSignalTracker {
    static constexpr size_t   CAPACITY   = 128;
    // Aynı yönde art arda sinyal üretimini seyrelt: 5s pencerede yön başına en fazla 50 kayıt
    // → iki yön toplamı en kötü 100 < 128 (taşma pratikte olmaz). Python tarafı --min-gap-ms ile ayrıca seyreltebilir.
    static constexpr uint64_t MIN_GAP_NS = 100'000'000ULL;

    static constexpr uint64_t horizon_ns(size_t i) noexcept {
        return kusak::telemetry::OBI_HORIZON_MS[i] * 1'000'000ULL;
    }

    std::array<PendingObiSignal, CAPACITY> slots{};
    uint32_t count        = 0;
    uint64_t min_due_ns   = std::numeric_limits<uint64_t>::max();   // en yakın bekleyen horizon (erken çıkış)
    uint64_t next_event_id = 1;
    uint64_t last_add_ns[2] = {0, 0};     // [0]=SAT, [1]=AL
    uint64_t dropped_full = 0;            // tracker doluyken düşen sinyal
    uint64_t throttled    = 0;            // MIN_GAP nedeniyle atlanan

    // Throttle kontrolü (wall saat okumadan önce ucuz ön-kapı).
    inline bool want(uint64_t mono_ns, int side) noexcept {
        const uint64_t last = last_add_ns[side > 0 ? 1 : 0];
        if (last != 0 && mono_ns - last < MIN_GAP_NS) { ++throttled; return false; }
        return true;
    }

    inline bool add(uint64_t mono_ns, uint64_t wall_us, double bid, double ask, double mid,
                    double obi, double thr, int side, double spread_bps) noexcept {
        if (count >= CAPACITY) { ++dropped_full; return false; }   // yeni sinyal düşer (yarım kalmış gözlem korunur)
        PendingObiSignal& s = slots[count++];
        s = PendingObiSignal{};
        s.signal_mono_ns = mono_ns;
        s.signal_wall_us = wall_us;
        s.event_id       = next_event_id++;
        s.signal_price   = mid;
        s.bid            = bid;
        s.ask            = ask;
        s.obi            = static_cast<float>(obi);
        s.spread_bps     = static_cast<float>(spread_bps);
        s.threshold      = static_cast<float>(thr);
        s.side           = static_cast<int8_t>(side > 0 ? 1 : -1);
        last_add_ns[side > 0 ? 1 : 0] = mono_ns;
        const uint64_t due0 = mono_ns + horizon_ns(0);
        if (due0 < min_due_ns) min_due_ns = due0;
        return true;
    }

    // Her yeni tick'te çağrılır. Hot-path: bekleyen yoksa / hiçbir eşik dolmadıysa 2 karşılaştırma.
    inline void poll(uint64_t now_ns, double mid, kusak::telemetry::TelemetrySink& sink) noexcept {
        if (count == 0 || now_ns < min_due_ns || mid <= 0.0) return;
        constexpr size_t NH = kusak::telemetry::OBI_N_HORIZONS;
        uint64_t new_min = std::numeric_limits<uint64_t>::max();
        for (uint32_t i = 0; i < count; ) {
            PendingObiSignal& s = slots[i];
            while (s.next_h < NH && now_ns >= s.signal_mono_ns + horizon_ns(s.next_h)) {
                s.price_after[s.next_h] = mid;
                const uint64_t lag_us = (now_ns - (s.signal_mono_ns + horizon_ns(s.next_h))) / 1000ULL;
                const uint32_t lag32  = lag_us > 0xFFFFFFFFULL ? 0xFFFFFFFFu : static_cast<uint32_t>(lag_us);
                if (lag32 > s.max_lag_us) s.max_lag_us = lag32;
                ++s.next_h;
            }
            if (s.next_h >= NH) {                       // +5000ms doldu → paketle, gönder, sil
                kusak::telemetry::ObiEvent ev;
                ev.event_id = s.event_id;  ev.signal_wall_us = s.signal_wall_us;  ev.signal_mono_ns = s.signal_mono_ns;
                ev.signal_price = s.signal_price;  ev.bid = s.bid;  ev.ask = s.ask;
                for (size_t h = 0; h < NH; ++h) ev.price_after[h] = s.price_after[h];
                ev.obi = s.obi;  ev.spread_bps = s.spread_bps;  ev.threshold = s.threshold;
                ev.max_lag_us = s.max_lag_us;  ev.side = s.side;  ev.n_filled = static_cast<uint8_t>(NH);
                sink.record_obi_event(ev);
                if (i != count - 1) slots[i] = slots[count - 1];   // swap-with-last
                --count;
                continue;                                // i'deki yeni elemanı da işle
            }
            const uint64_t due = s.signal_mono_ns + horizon_ns(s.next_h);
            if (due < new_min) new_min = due;
            ++i;
        }
        min_due_ns = new_min;
    }
};

struct alignas(64) StrategyContext {
    kusak::config::KusakConfig cfg;

    kusak::research::VPINVelocityTracker      vpin;
    bool                                      vpin_calib_logged = false;   // [PHASE5] kova hedefi bir kez loglanir
    kusak::research::RegimeDetectorV2         regime;

    // [FIX-QUANT-6] RegimeSnapshot besleme durumu -- ATR=0 bug fix'inin parcasi.
    // 1s'lik yuvarlanan gercek H/L bar (sentetik +-0.05% bandin yerine).
    double     regime_bar_high{0.0};
    double     regime_bar_low{0.0};
    uint64_t   regime_bar_start_ns{0};
    // ~60 ornekli hacim EMA'si (volume_1m proxy'si, heap yok, ATR ile ayni desen).
    double     regime_vol_ema{0.0};
    kusak::research::RiskScorer               risk_scorer;
    kusak::monitoring::LatencyMonitor         latency;
    kusak::telemetry::TelemetrySink           telemetry;
    kusak::research::AdverseSelectionTracker  adverse;
    kusak::exchange::FilterValidator          filter_validator;
    kusak::log::Logger                        logger;

    std::shared_ptr<kusak::net::BinanceHttpClient>  http_client;
    std::optional<kusak::exchange::RestOrderClient> live_order_client;
    std::shared_ptr<kusak::notify::INotifier>       notifier;   // [KD-9] opsiyonel, null ise sessiz no-op

    SpscRing<131072>   market_ring;
    OrderChannel<1024> order_channel;     // [V13-SPSC] mutex → SPSC
    OrderSpscRing<TelemetryCmd, 256> telemetry_cmd_ring;
    std::atomic<uint64_t> telemetry_cmd_drops{0};

    // Strateji durumu — YALNIZCA strateji (ana) thread'inden erişilir
    Position                              position;
    kusak::research::AutoParamAdapter     param_adapter;
    FlatOrderMap                          flat_order_map{};   // [V13-MAP] [FIX-P35] value-init
    uint64_t                              active_order_id = 0;   // [FIX-P0-02] yerel FSM ID (borsa ID DEGIL; POLL/CANCEL slottaki exchange_order_id kullanir)
    FixedString<32>                       active_client_oid{};   // [FIX-P0-02] ucustaki emrin client_oid kopyasi (UNKNOWN + borsa ID yok POLL kimligi)
    // [FIX-P37-C19B] UNKNOWN cozum durumu (yalniz strateji thread'i yazar/okur; tek ucusta emir modeli)
    uint64_t                              unknown_key{0};          // UNKNOWN'a giren slotun yerel ID'si
    uint64_t                              unknown_since_ns{0};     // UNKNOWN'a giris zamani (sonuc ts_recv_ns / saat)
    uint32_t                              unknown_notfound_streak{0}; // art arda POLL -2013 sayisi
    uint64_t                              fsm_key         = 0;   // [PHASE2-FSM] ucustaki emrin YEREL ID'si = FlatOrderMap anahtari
    uint64_t                              fsm_exit_seq    = FSM_EXIT_BASE; // [PHASE2-FSM] EXIT emirleri icin yerel ID ureteci
    uint64_t                              fsm_entry_seq   = 0;    // [PATCH-P5] GIRIS yerel ID ureteci: ASLA sifirlanmaz (active_order_id borsa ID'si ile ezilir/0'lanir)

    bool     awaiting_fill  = false;

    // [FIX-BACKPRESSURE] Ayni anda birden fazla POLL isteginin kuyruga
    // yigilmasini engelleyen "trafik isigi". Uretici (main loop) bunu
    // kontrol eder, tuketici (order_thread) HTTP cevabi donunce sifirlar.
    std::atomic<bool> poll_in_flight{false};

    // [FIX-TTL] Emrin GONDERILDIGI an damgalanir -- enforce_timeouts'un
    // aksine, her basarili POLL cevabinda SIFIRLANMAZ. "Emir ne zamandir
    // acik" sorusuna cevap verir, "borsadan ne zamandir haber alamadik"
    // sorusuna degil.
    uint64_t awaiting_fill_since_ns = 0;
    double   current_capital  = 0.0;
    double   locked_in_orders = 0.0;
    double   pending_entry_notional = 0.0;   // [PHASE0-GUARD] canli GIRIS emrinin gonderim notional'i (yalniz awaiting_fill iken gecerli)

    alignas(64) std::atomic<bool> kill_switch{false};
    // [PHASE3-KS] kill_switch_active = kill_switch'in adi (cift durum yok; order thread de ayni bayragi yazar)
    std::atomic<bool>&            kill_switch_active = kill_switch;
    alignas(64) std::atomic<double> fw_max_position_usd{0.0};        // [PHASE1-FW] order thread okur
    alignas(64) std::atomic<double> fw_max_order_notional_usd{0.0};  // [PHASE1-FW]
    std::atomic<double> fw_position_usd{0.0};     // [PHASE0-GUARD] bu emir HARIC acik maruziyet (strateji thread yazar)
    std::atomic<double> fw_free_quote_usd{0.0};   // [PHASE0-GUARD] serbest quote (current_capital - locked)
    // [FIX-P0-03] Pozisyon kopyasi (strateji thread YAZAR, order thread OKUR): azaltici EXIT kaniti.
    std::atomic<int8_t> fw_pos_side{0};           // [FIX-P0-03] -1 SHORT / 0 yok / +1 LONG
    std::atomic<double> fw_pos_qty{0.0};          // [FIX-P0-03] yerel envanter (dolum + uzlastirma tabanli)
    std::atomic<bool>   fw_pos_verified{false};   // [FIX-P0-03] yalniz onayli dolum / baslangic uzlastirmasi ile true
    std::atomic<uint32_t> fw_pos_seq{0};          // [FIX-P35] seqlock: tek=yazim suruyor, cift=tutarli; yazar tek (strateji thread / startup ana thread)
    KillSwitchState               ks;
    std::atomic<uint64_t>         ws_disconnect_total{0};   // main.cpp ws sayacindan kopyalar
    std::atomic<uint64_t>         order_rtt_ns{0};          // order thread yazar (SEND RTT), strateji thread exchange(0)
    bool                          trading_paused = false;
    bool                          pending_is_buy = true;

    alignas(64) double last_bid = 0.0;
    double             last_ask = 0.0;
    double             last_mid = 0.0;
    uint64_t market_producer_seq{0};
    // [V13-PROFIT-ENGINE] Kâr Motoru alanları
    RecentPnLBuffer      recent_pnl;
    TradeDirectionBuffer trade_direction;
    double               last_bid_qty = 0.0;   // bookTicker'daki en iyi alış miktarı
    double               last_ask_qty = 0.0;   // en iyi satış miktarı
    ObiSignalTracker     obi_tracker;          // [P1-OBI-TRACK]

    std::atomic<uint64_t> last_ticker_ns{0};
    std::atomic<uint64_t> cooldown_until_ns{0};
    uint64_t              exit_retry_not_before_ns = 0;   // [HOTFIX-AMNESIA] reddedilen EXIT sonrasi yeniden deneme kisiti
    std::atomic<bool>     vpin_force_exit{false};

    kusak::research::VPINAlarm vpin_last_alarm_level{kusak::research::VPINAlarm::NONE};
    uint64_t                   vpin_last_alarm_ns{0};          // [V13-CLOCK]

    // [FIX-CLOCK-ISOLATION] Replay/backtest icin simulasyon zamani. dry_run
    // aktifken ve bu deger sifirdan farkliyken (replay/simulasyon modu),
    // current_time_ns() donanim saati (tsc_ns()) yerine bu degeri dondurur --
    // boylece replay/backtest, canli donanim saatinden IZOLE edilmis olur.
    uint64_t mock_time_ns = 0;

    inline uint64_t current_time_ns() const noexcept {
        if (cfg.dry_run && mock_time_ns != 0) {
            return mock_time_ns;
        }
        return tsc_ns();
    }

    explicit StrategyContext(const kusak::config::KusakConfig& c)
        : cfg(c)
        , vpin([&]{
            kusak::research::VPINConfig vc;
            // [ARCH-V2-FIX] bucket_size_multiplier/dynamic_k/velocity_surge_threshold
            //               -> *_scaled (int64_t) yeniden adlandırıldı; VPIN_SCALE (1e6)
            //               ile ölçeklenmeden atama derlenmez / sessizce yanlış ölçek verirdi.
            vc.bucket_size_multiplier_scaled   = static_cast<int64_t>(c.vpin_bucket_mult * static_cast<double>(kusak::research::VPIN_SCALE));
            // [PHASE5] kova hacmi: acik hacim verildiyse SABIT, yoksa (0) ortalama islem boyutundan OTOMATIK
            vc.bucket_volume_scaled            = (c.vpin_bucket_volume > 0.0)
                                               ? static_cast<int64_t>(c.vpin_bucket_volume * static_cast<double>(kusak::research::VPIN_SCALE))
                                               : 0;
            vc.calib_trades                    = c.vpin_calib_trades;
            vc.n_buckets_window                = c.vpin_n_buckets;
            vc.use_dynamic_threshold           = c.vpin_use_dynamic;
            vc.dynamic_window                  = static_cast<size_t>(c.vpin_dynamic_window);
            vc.dynamic_k_scaled                = static_cast<int64_t>(c.vpin_dynamic_k * static_cast<double>(kusak::research::VPIN_SCALE));
            vc.velocity_surge_threshold_scaled = static_cast<int64_t>(c.vpin_vel_surge_thr * static_cast<double>(kusak::research::VPIN_SCALE));
            return vc;
          }())
        , regime([&]{
            kusak::research::RegimeDetectorV2Config rc;
            rc.hmm_high_risk_prob_threshold = c.hmm_enter_threshold;
            rc.hmm_persistence              = c.hmm_persistence;
            return rc;
          }())
        , risk_scorer(c.risk_weights, c.risk_thresholds)
        , latency([&]{
            kusak::monitoring::LatencyMonitor::Config lc;
            lc.alert_threshold_us = c.latency_alert_us;
            lc.report_every_n     = c.report_every_n;
            lc.log_to_stdout      = c.verbose;
            return lc;
          }())
        , telemetry([&]{
            kusak::telemetry::TelemetrySink::Config tc;
            tc.symbol           = c.symbol;
            tc.write_csv        = c.write_csv;
            tc.write_prometheus = c.write_prometheus;
            tc.csv_path         = c.csv_path;
            tc.prom_path        = c.prom_path;
            tc.verbose          = c.verbose;
            return tc;
          }())
        , adverse([&]{
            kusak::research::AdverseSelectionConfig ac;
            ac.cancel_track_window_us = c.cancel_track_win_us;
            ac.cancel_threshold_bps   = c.cancel_thr_bps;
            return ac;
          }())
        , logger([&]{
            kusak::log::Logger::Config lc;
            lc.min_level    = c.verbose ? kusak::log::Level::DEBUG
                                        : kusak::log::Level::INFO;
            lc.to_stdout    = true;
            lc.to_file      = true;
            lc.file_path    = "kusak.log";
            lc.color_stdout = true;
            return lc;
          }())
        , param_adapter(kusak::research::ResearchParams{})
        , current_capital(c.initial_capital)
    {}
};

// [FIX-P0-02] POLL gorevi uretici: kimligi YEREL ID'den degil slottan alir.
//   * slot.exchange_order_id != 0           -> order_id = borsa ID, client_oid bos
//   * UNKNOWN + borsa ID yok + client_oid   -> order_id = 0, client_oid = active_client_oid
//                                              (origClientOrderId ile sorgulanir)
//   * diger her durum (SUBMITTED dahil)     -> false: POLL uretilmez (SEND yanitini/timeout'u bekle)
[[nodiscard]] inline bool build_poll_task(const StrategyContext& ctx, OrderTask& pt) noexcept
{
    if (ctx.fsm_key == 0) return false;
    // find() const degil; slot yalnizca okunur (strateji thread'i tek sahip).
    const OrderSlot* slot = const_cast<FlatOrderMap&>(ctx.flat_order_map).find(ctx.fsm_key);
    if (!slot) return false;

    if (slot->exchange_order_id != 0) {
        pt.order_id = slot->exchange_order_id;
        pt.client_oid = FixedString<32>{};
    } else if (slot->fsm_state == OrderState::UNKNOWN && !ctx.active_client_oid.empty()) {
        pt.order_id   = 0;
        pt.client_oid = ctx.active_client_oid;
    } else {
        return false;
    }
    pt.kind           = TaskKind::POLL;
    pt.local_order_id = ctx.fsm_key;                        // [PHASE2-FSM] yerel ID (FlatOrderMap anahtari)
    pt.is_exit        = (ctx.fsm_key >= FSM_EXIT_BASE);     // [PATCH-P5] POLL sonucu EXIT kimligini tasisin
    pt.side           = ctx.pending_is_buy ? OrderSide::BUY : OrderSide::SELL;
    pt.ts_send_ns     = ctx.current_time_ns();
    pt.symbol.set(ctx.cfg.symbol.c_str(), static_cast<uint8_t>(ctx.cfg.symbol.size()));
    return true;
}

// [FIX-SEPARATION-1] json_extract_string BURADAN KALDIRILDI --
// artik utils.hpp icinde (strateji karar mantigiyla ilgisi yok).

// [FIX-P36-C16] Ileri bildirim: govde trip_kill_switch_on_unknown ve fw_publish_exposure'dan sonra tanimli.
inline void submitted_lost_to_unknown(StrategyContext& ctx, FlatOrderMap& map, OrderSlot& s) noexcept;

// [FIX-CLOCK-ISOLATION] enforce_timeouts govdesi -- StrategyContext artik
// tam (complete) tip oldugu icin ctx.current_time_ns() burada guvenle
// cagrilabilir. Algoritmik mantik (ULL-OP3 bitmap taramasi, timeout/cancel
// akisi) BIREBIR aynidir; degisen tek sey saat kaynagidir.
FORCE_INLINE void enforce_timeouts(StrategyContext& ctx,
                                   FlatOrderMap& map,
                                   OrderChannel<>& ch,
                                   const std::string& symbol) noexcept {
    // ── [ULL-OP3] Lineer MAX_ORDERS taraması infaz edildi ──────────────────
    // Doğrusal for (size_t i = 0; i < MAX_ORDERS; ++i) yok.
    // Sadece active_bitmap üzerinden dolu word'ler taranır;
    // her word içinde __builtin_ctzll ile aktif bit indeksleri tek-çevrimde
    // çıkarılır (BMI1: TZCNT). Boş word'ler sıfır → loop body çalışmaz.
    const uint64_t now = ctx.current_time_ns();
    const uint8_t symbol_len = static_cast<uint8_t>(symbol.size());
    for (size_t w = 0; w < FlatOrderMap::BITMAP_WORDS; ++w) {
        uint64_t bits = map.active_bitmap[w];
        const size_t base = w << 6;
        while (bits) {
            const unsigned b   = static_cast<unsigned>(__builtin_ctzll(bits));
            bits &= bits - 1;  // en düşük 1-biti söndür (BLSR)

            OrderSlot& s = map.slots[base + b];
            // Bitmap teorik olarak ACTIVE ile tutarlı; defansif kontrol ucuzdur
            // ve istenmeyen yarış/penceresi durumlarında ghost cancel'ı önler.
            if (UNLIKELY(s.state != SlotState::ACTIVE)) continue;

            // [FIX-P36-C16] SUBMITTED + borsa ID yok + SUBMITTED_LOST_TIMEOUT asildi => sonuc kayip:
            // emir borsada olabilir -> UNKNOWN (ALLOWED tablosu SUBMITTED->UNKNOWN'a izin verir) + kill switch.
            // Borsa ID'si olan SUBMITTED bu dala girmez. now <= last_update_ns (saat alani karisimi) tetiklemez.
            if (s.fsm_state == OrderState::SUBMITTED) {
                if (s.exchange_order_id == 0 && now > s.last_update_ns &&
                    (now - s.last_update_ns) > SUBMITTED_LOST_TIMEOUT_NS)
                    submitted_lost_to_unknown(ctx, map, s);
                continue;
            }

            // [FIX-MVP-MAKER]: hic dolmamis (NEW) bekleyen LIMIT_MAKER emri de
            // kendi (daha toleransli) zaman asimiyla iptal edilir. Orijinal
            // kod sadece PARTIALLY_FILLED'i kontrol ediyordu -- bu, cancel/
            // replace mimarisi olmadan bir maker emrinin SONSUZA kadar
            // kuyrukta kalabilecegi anlamina geliyordu.
            // [PHASE2-FSM] Karar OrderState'e gore. CANCEL_REQUEST = onceki iptal ring'e
            // yazilamadi -> FILL_TIMEOUT sonra yeniden dene. CANCEL_PENDING/UNKNOWN: TTL/reconcile'in isi.
            uint64_t timeout_ns;
            if      (s.fsm_state == OrderState::NEW || s.fsm_state == OrderState::ACK)
                                                                timeout_ns = MAKER_REST_TIMEOUT_NS;
            else if (s.fsm_state == OrderState::PARTIAL ||
                     s.fsm_state == OrderState::CANCEL_REQUEST) timeout_ns = FILL_TIMEOUT_NS;
            else                                                 continue;
            if ((now - s.last_update_ns) <= timeout_ns) continue;
            // Borsa ID'si yoksa (ACK henuz yok) iptal gonderilemez; slot SUBMITTED/UNKNOWN akisinda cozulur.
            if (UNLIKELY(s.exchange_order_id == 0)) continue;

            if (s.fsm_state != OrderState::CANCEL_REQUEST)
                map.step(s, OrderState::CANCEL_REQUEST);

            OrderTask ct{};
            ct.kind           = TaskKind::CANCEL;
            ct.order_id       = s.exchange_order_id;   // [PHASE2-FSM] eskiden yerel ID gidiyordu
            ct.local_order_id = s.order_id;
            ct.ts_send_ns     = now;
            // [ARCH-V2-FIX] Ghost Cancel: symbol eksikti -> make_symbol_orderid_query()
            //               boş sembolle sorgu üretiyor, borsa CANCEL isteğini reddediyordu.
            ct.symbol.set(symbol.c_str(), symbol_len);
            if (UNLIKELY(!ch.push_task(ct))) {
                // Ring dolu: slot CANCEL_REQUEST'te kalir -> FILL_TIMEOUT sonra yeniden denenir.
                ctx.logger.warn("STATE_MISMATCH", "CANCEL task drop, local_id=", s.order_id);
            } else {
                map.step(s, OrderState::CANCEL_PENDING);   // [PHASE2-FSM] borsa teyidi bekleniyor
            }
            s.last_update_ns = now;   // timeout tekrar tetiklenmesin
            // [NOT] slot hala active; active_slots ve bitmap dokunulmaz.
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// İleri bildirimler ve Kill-Switch
// ─────────────────────────────────────────────────────────────────────────────
// [PHASE1-FW] Limitleri order thread'e atomic yayinla. Baslangicta + her config reload'da cagir.
inline void fw_publish_limits(StrategyContext& ctx) noexcept {
    const double pos = ctx.cfg.max_position_usd;
    const double ord = (ctx.cfg.ks.max_order_notional_usd > 0.0) ? ctx.cfg.ks.max_order_notional_usd : pos;
    ctx.fw_max_position_usd.store(pos, std::memory_order_relaxed);
    ctx.fw_max_order_notional_usd.store(ord, std::memory_order_relaxed);
}

enum class FwVerdict : uint8_t { PASS = 0, BAD_INPUT, POSITION_CAP, ORDER_CAP,
                                 SYMBOL_MISMATCH, FILTER, EXPOSURE_CAP, INSUFFICIENT_BALANCE,   // [PHASE0-GUARD]
                                 EXIT_POS_UNKNOWN, EXIT_NOT_REDUCING };                          // [FIX-P0-03]

// [FIX-P0-03] Firewall verdict adi (log icin; sabit literal, tahsis yok).
[[nodiscard]] inline const char* fw_verdict_name(FwVerdict v) noexcept {
    switch (v) {
        case FwVerdict::PASS:                 return "PASS";
        case FwVerdict::BAD_INPUT:            return "BAD_INPUT";
        case FwVerdict::POSITION_CAP:         return "POSITION_CAP";
        case FwVerdict::ORDER_CAP:            return "ORDER_CAP";
        case FwVerdict::SYMBOL_MISMATCH:      return "SYMBOL_MISMATCH";
        case FwVerdict::FILTER:               return "FILTER";
        case FwVerdict::EXPOSURE_CAP:         return "EXPOSURE_CAP";
        case FwVerdict::INSUFFICIENT_BALANCE: return "INSUFFICIENT_BALANCE";
        case FwVerdict::EXIT_POS_UNKNOWN:     return "EXIT_POS_UNKNOWN";
        case FwVerdict::EXIT_NOT_REDUCING:    return "EXIT_NOT_REDUCING";
    }
    return "?";
}

// [PHASE0-GUARD] Strateji thread'i, emri kuyruga itmeden HEMEN once cagirir.
// [FIX-P0-03] Pozisyon kopyasini yayinlar. fw_pos_verified KURALI (oncelik sirasiyla):
//   1) Pozisyon kapali (!is_open)                  -> false (side=0 zaten).
//   2) Herhangi bir aktif slot OrderState::UNKNOWN -> false (belirsiz emir: pozisyon kaniti gecersiz, fail-closed).
//      UNKNOWN sirasinda EXIT reddi (EXIT_POS_UNKNOWN) beklenen davranistir; flatten zaten !awaiting_fill ile engelli.
//   3) FwVerify::INVALIDATE                        -> false.
//   4) FwVerify::EVIDENCE (borsa onayli kanit, bkz. dispatch_order_result) -> true.
//      UNKNOWN slot kalmadiysa ve pozisyon aciksa yeniden dogrulanir (UNKNOWN cozulunce EXIT kilitlenmez).
//   5) FwVerify::KEEP                              -> onceki deger (yukseltmez; kural 1-2 yine oncelikli).
// Yapiskanlik yalniz kural 1-2 ile daralir; KEEP kanitsiz true uretmez. Yazan: yalniz strateji thread'i.
// Sira: verified=false (release) -> side/qty/usd -> verified (release); okuyucu iki kez okuyup tutarlilik dogrular.
enum class FwVerify : uint8_t { KEEP, EVIDENCE, INVALIDATE };

[[nodiscard]] inline bool fw_any_unknown_slot(const FlatOrderMap& m) noexcept {
    for (size_t w = 0; w < FlatOrderMap::BITMAP_WORDS; ++w) {
        uint64_t bits = m.active_bitmap[w];
        while (bits) {
            const size_t idx = (w << 6) + static_cast<size_t>(__builtin_ctzll(bits));
            bits &= bits - 1;
            const OrderSlot& s = m.slots[idx];
            if (s.state == SlotState::ACTIVE && s.fsm_state == OrderState::UNKNOWN) return true;
        }
    }
    return false;
}

inline void fw_publish_exposure(StrategyContext& ctx, FwVerify mode = FwVerify::KEEP) noexcept {
    const bool prev_verified = ctx.fw_pos_verified.load(std::memory_order_relaxed);
    // [FIX-P35] seqlock yazar: seq tek -> alanlar -> seq cift (release). Tek yazar oldugundan RMW gerekmez.
    const uint32_t seq0 = ctx.fw_pos_seq.load(std::memory_order_relaxed);
    ctx.fw_pos_seq.store(seq0 + 1u, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    ctx.fw_pos_verified.store(false, std::memory_order_release);
    const double mark = (ctx.last_mid > 0.0) ? ctx.last_mid : ctx.position.entry_price;
    const bool   open = ctx.position.is_open();
    const int8_t side = !open ? int8_t{0} : (ctx.position.is_long() ? int8_t{1} : int8_t{-1});
    ctx.fw_pos_side.store(side, std::memory_order_relaxed);
    ctx.fw_pos_qty.store(open ? ctx.position.qty : 0.0, std::memory_order_relaxed);
    ctx.fw_position_usd.store(open ? ctx.position.qty * mark : 0.0, std::memory_order_relaxed);
    ctx.fw_free_quote_usd.store(std::max(0.0, ctx.current_capital - ctx.locked_in_orders), std::memory_order_relaxed);
    bool verified = false;
    if (open && mode != FwVerify::INVALIDATE && !fw_any_unknown_slot(ctx.flat_order_map))
        verified = (mode == FwVerify::EVIDENCE) || prev_verified;
    ctx.fw_pos_verified.store(verified, std::memory_order_release);
    ctx.fw_pos_seq.store(seq0 + 2u, std::memory_order_release);   // [FIX-P35] yayin tamam
}

// [FIX-P0-03] EXIT'e ozel firewall dali: azaltici oldugu KANITLANMADAN gecmez (fail-closed).
// POSITION_CAP / ORDER_CAP / toplamali EXPOSURE_CAP / INSUFFICIENT_BALANCE uygulanmaz: risk azaltma kaniti yerine gecer.
[[nodiscard]] inline FwVerdict exit_firewall(const StrategyContext& ctx, const OrderTask& t) noexcept {
    if (!t.is_exit) return FwVerdict::EXIT_NOT_REDUCING;   // kind=EXIT ama azaltici isaretli degil
    const double qty = t.qty, px = t.price;
    if (!(qty > 0.0) || !(px > 0.0) || !std::isfinite(qty) || !std::isfinite(px))
        return FwVerdict::BAD_INPUT;
    const double notional = qty * px;
    if (!std::isfinite(notional)) return FwVerdict::BAD_INPUT;

    const size_t sl = ctx.cfg.symbol.size();
    if (sl == 0 || sl > 16 || std::memcmp(t.symbol.c_str(), ctx.cfg.symbol.c_str(), sl) != 0 || t.symbol.c_str()[sl] != '\0')
        return FwVerdict::SYMBOL_MISMATCH;
    if (ctx.filter_validator.validate_order(ctx.cfg.symbol, px, qty, 0.0) != kusak::exchange::FilterResult::VALID)
        return FwVerdict::FILTER;

    // Pozisyon kaniti: verified once-sonra okunur (yayin sirasinin tersi); tutarsiz/yarim yayin => bilinmiyor.
    // [FIX-P35] seqlock okuyucu: en fazla 3 deneme, spin-wait/sleep yok; tutarli okuma yoksa fail-closed.
    bool   v1 = false; int8_t side = 0; double pq = 0.0; bool got = false;
    for (int attempt = 0; attempt < 3 && !got; ++attempt) {
        const uint32_t s1 = ctx.fw_pos_seq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;   // yazim suruyor
        v1   = ctx.fw_pos_verified.load(std::memory_order_relaxed);
        side = ctx.fw_pos_side.load(std::memory_order_relaxed);
        pq   = ctx.fw_pos_qty.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t s2 = ctx.fw_pos_seq.load(std::memory_order_relaxed);
        got = (s1 == s2);
    }
    if (!got) return FwVerdict::EXIT_POS_UNKNOWN;
    if (!v1 || side == 0 || !(pq > 0.0) || !std::isfinite(pq)) return FwVerdict::EXIT_POS_UNKNOWN;

    // Yon: LONG -> yalniz SELL, SHORT -> yalniz BUY
    if (side > 0 && t.side != OrderSide::SELL) return FwVerdict::EXIT_NOT_REDUCING;
    if (side < 0 && t.side != OrderSide::BUY)  return FwVerdict::EXIT_NOT_REDUCING;

    // Miktar: pozisyonu asamaz (1 step + FP toleransi)
    const double step = ctx.filter_validator.step_size_of(ctx.cfg.symbol);
    const double tol  = (step > 1e-12) ? step * 1e-3 : pq * 1e-9;   // [FIX-P35] eski tol=step: 1 step fazla EXIT'e izin veriyordu
    if (qty > pq + tol + 1e-12) return FwVerdict::EXIT_NOT_REDUCING;
    return FwVerdict::PASS;
}

// [PHASE1-FW] Son savunma hatti. Sifir tahsis, lock yok: 2 atomic load + 1 carpma.
// [FIX-P0-03] SEND: tum kontroller (degismedi). EXIT: ayri dal (exit_firewall) -- pozisyonu azalttigi
// kanitlanirsa cap/bakiye kontrolleri uygulanmaz; kanitlanamazsa REDDEDILIR (fail-closed).
[[nodiscard]] inline FwVerdict final_order_firewall(const StrategyContext& ctx, const OrderTask& t) noexcept {
    if (t.kind == TaskKind::EXIT) return exit_firewall(ctx, t);   // [FIX-P0-03]
    const double qty = t.qty, px = t.price;
    if (!(qty > 0.0) || !(px > 0.0) || !std::isfinite(qty) || !std::isfinite(px))
        return FwVerdict::BAD_INPUT;
    const double notional = qty * px;
    if (!std::isfinite(notional)) return FwVerdict::BAD_INPUT;

    const double pos_cap = ctx.fw_max_position_usd.load(std::memory_order_relaxed);
    if (!(pos_cap > 0.0) || notional > pos_cap) return FwVerdict::POSITION_CAP;   // limit yoksa fail-closed

    const double ord_cap = ctx.fw_max_order_notional_usd.load(std::memory_order_relaxed);
    if (ord_cap > 0.0 && notional > ord_cap) return FwVerdict::ORDER_CAP;

    // [PHASE0-GUARD] sembol: emir sembolu cfg.symbol ile birebir ayni olmali
    const size_t sl = ctx.cfg.symbol.size();
    if (sl == 0 || sl > 16 || std::memcmp(t.symbol.c_str(), ctx.cfg.symbol.c_str(), sl) != 0 || t.symbol.c_str()[sl] != '\0')
        return FwVerdict::SYMBOL_MISMATCH;
    // [PHASE0-GUARD] borsa filtreleri (tickSize/stepSize/min-max qty/minNotional); atomic_load, tahsis yok
    if (ctx.filter_validator.validate_order(ctx.cfg.symbol, px, qty, 0.0) != kusak::exchange::FilterResult::VALID)
        return FwVerdict::FILTER;
    // [PHASE0-GUARD] toplam maruziyet: acik pozisyon + bu emir <= tavan (ikinci giris engeli)
    const double other = ctx.fw_position_usd.load(std::memory_order_relaxed);
    if (!(other >= 0.0) || other + notional > pos_cap) return FwVerdict::EXPOSURE_CAP;
    // [PHASE0-GUARD] BUY serbest quote bakiyesini asamaz
    if (t.side == OrderSide::BUY && notional > ctx.fw_free_quote_usd.load(std::memory_order_relaxed))
        return FwVerdict::INSUFFICIENT_BALANCE;
    return FwVerdict::PASS;
}

// [FIX-P0-03] Yalniz FUTURES EXIT emirleri reduceOnly=true gonderir (SPOT'ta parametre yok; SEND'de yok).
[[nodiscard]] inline bool exit_needs_reduce_only(const OrderTask& t, bool futures) noexcept {
    return futures && t.kind == TaskKind::EXIT && t.is_exit;
}

// [PHASE3-KS] Tetik: bir kez islenir. NEW ORDERS=OFF (kill_switch_active) hemen; CANCEL ALL +
// SAFE MODE ks_service()'te. Kilit dosyasi yeniden baslatmayla kill-switch'in atlatilmasini onler.
inline void ks_latch(StrategyContext& ctx, const char* reason, double value, double limit) noexcept {
    auto& k = ctx.ks;
    if (k.latched) return;
    k.latched = true; k.reason = reason; k.value = value; k.limit = limit;
    k.tripped_ns = ctx.current_time_ns();
    ctx.kill_switch_active.store(true, std::memory_order_release);   // (a) NEW ORDERS = OFF
    ctx.logger.critical("KILL_SWITCH", "TETIKLENDI reason=", reason, " value=", value, " limit=", limit,
        " -- NEW ORDERS=OFF, CANCEL ALL, SAFE MODE (manuel reset: '", ctx.cfg.ks.lock_file, "' dosyasini silin)");
    std::error_code ec;
    if (std::filesystem::exists(ctx.cfg.ks.lock_file, ec)) {
        k.lock_written = true;                                    // onceki kilit korunur (iz silinmez)
    } else {
        std::ofstream f(ctx.cfg.ks.lock_file, std::ios::trunc);
        if (f) { f << "reason=" << reason << " value=" << value << " limit=" << limit << "\n"; k.lock_written = f.good(); }
        if (!k.lock_written)
            ctx.logger.error("KILL_SWITCH", "kilit dosyasi YAZILAMADI -- manuel reset yalniz yeniden baslatmayla");
    }
    if (ctx.notifier) {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "KILL_SWITCH %s value=%.4f limit=%.4f", reason, value, limit);
        ctx.notifier->send_circuit_breaker(buf);
    }
}

// Gercek risk kontrolleri. Strateji thread'inde cagrilir (her 16 tick + 500ms tarama). true => tetiklendi.
inline bool check_kill_switch(StrategyContext& ctx) noexcept {
    auto& k = ctx.ks;
    const auto& c = ctx.cfg.ks;
    if (k.latched) return true;
    if (ctx.kill_switch_active.load(std::memory_order_acquire)) {     // dis tetik (API key/imza/zaman damgasi)
        ks_latch(ctx, "EXTERNAL_TRIP", 0.0, 0.0);
        return true;
    }
    if (!c.enabled) return false;

    const uint64_t now = ctx.current_time_ns();

    // UTC gun degisimi: gunluk sayaclar kendiliginden sifirlanir (kilit sifirlanmaz)
    const int64_t day = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() / 86400);
    if (k.day_index < 0) {
        k.day_index = day;                                            // ilk gozlem: SIFIRLAMA (ilk islemin zarari silinmesin)
    } else if (day != k.day_index) {
        k.day_index = day; k.daily_realized_pnl = 0.0; k.daily_rejects = 0;
    }

    const bool   open  = ctx.position.is_open();
    const double mark  = (ctx.last_mid > 0.0) ? ctx.last_mid : ctx.position.entry_price;
    const double pos_usd  = open ? ctx.position.qty * mark : 0.0;
    // [PHASE0-GUARD] max() dolmamis canli emri gormez; pos+locked ise dolu pozisyonu 2 kez sayar
    // (canlida locked_in_orders = dolu pozisyonun maliyeti). Dogru: pozisyon (mark) + bekleyen emir kalani.
    const bool   entry_in_flight = ctx.awaiting_fill && !ctx.flat_order_map.has_live_exit();
    const double pending_usd = kusak::guard::pending_remainder(entry_in_flight, ctx.pending_entry_notional, ctx.position.notional);
    const double exposure = kusak::guard::total_exposure(pos_usd, pending_usd);
    const double unrealized = (open && mark > 0.0)
        ? (ctx.position.is_long() ? (mark - ctx.position.entry_price) : (ctx.position.entry_price - mark)) * ctx.position.qty
        : 0.0;

    // MAX_POSITION_USD / MAX_TOTAL_EXPOSURE  [PHASE1-FW] ks degeri 0 ise cfg.max_position_usd'ye dus
    const double pos_cap = (c.max_position_usd > 0.0)       ? c.max_position_usd       : ctx.cfg.max_position_usd;
    const double exp_cap = (c.max_total_exposure_usd > 0.0) ? c.max_total_exposure_usd : pos_cap;
    if (pos_cap > 0.0 && pos_usd > pos_cap)
        { ks_latch(ctx, "MAX_POSITION_USD", pos_usd, pos_cap); return true; }
    if (exp_cap > 0.0 && exposure > exp_cap)
        { ks_latch(ctx, "MAX_TOTAL_EXPOSURE", exposure, exp_cap); return true; }

    // MAX_DAILY_LOSS (gerceklesen + floating)
    const double daily_loss = -(k.daily_realized_pnl + unrealized);
    if (c.max_daily_loss_usd > 0.0 && daily_loss >= c.max_daily_loss_usd)
        { ks_latch(ctx, "MAX_DAILY_LOSS", daily_loss, c.max_daily_loss_usd); return true; }

    // MAX_CONSECUTIVE_LOSSES / MAX_ORDER_REJECTS
    if (c.max_consecutive_losses > 0 && k.consecutive_losses >= c.max_consecutive_losses)
        { ks_latch(ctx, "MAX_CONSECUTIVE_LOSSES", k.consecutive_losses, c.max_consecutive_losses); return true; }
    if (c.max_order_rejects > 0 && k.daily_rejects >= c.max_order_rejects)
        { ks_latch(ctx, "MAX_ORDER_REJECTS", k.daily_rejects, c.max_order_rejects); return true; }

    // MAX_CANCEL_RATE -- kayan pencere (telemetri sayaclarinin farki)
    if (c.max_cancel_rate > 0.0) {
        const uint64_t sent = ctx.telemetry.total_sent(), canc = ctx.telemetry.total_canceled();
        const uint64_t win_ns = static_cast<uint64_t>(c.cancel_rate_window_sec * 1e9);
        if (k.win_start_ns == 0) { k.win_start_ns = now; k.win_sent0 = sent; k.win_cancel0 = canc; }
        const uint64_t ds = sent - k.win_sent0, dc = canc - k.win_cancel0;
        if (ds >= static_cast<uint64_t>(c.cancel_rate_min_orders)) {
            const double rate = static_cast<double>(dc) / static_cast<double>(ds);
            if (rate > c.max_cancel_rate) { ks_latch(ctx, "MAX_CANCEL_RATE", rate, c.max_cancel_rate); return true; }
        }
        if (now - k.win_start_ns >= win_ns) { k.win_start_ns = now; k.win_sent0 = sent; k.win_cancel0 = canc; }
    }

    // MAX_LATENCY -- emir gonderim -> borsa cevabi (order thread olcer); ardisik N olcum
    if (c.max_latency_ms > 0.0) {
        const uint64_t rtt = ctx.order_rtt_ns.exchange(0, std::memory_order_relaxed);
        if (rtt != 0) {
            const double ms = static_cast<double>(rtt) / 1e6;
            k.latency_breach_streak = (ms > c.max_latency_ms) ? k.latency_breach_streak + 1 : 0;
            if (k.latency_breach_streak >= c.latency_breach_samples)
                { ks_latch(ctx, "MAX_LATENCY", ms, c.max_latency_ms); return true; }
        }
    }

    // MAX_STALE_MARKET_DATA -- yalniz maruziyet varken (bosta iken WS kesintisi kilit gerektirmez)
    if (c.max_stale_data_ms > 0.0 && (open || ctx.awaiting_fill)) {
        const uint64_t last = ctx.last_ticker_ns.load(std::memory_order_relaxed);
        if (last != 0 && now > last) {
            const double ms = static_cast<double>(now - last) / 1e6;
            if (ms > c.max_stale_data_ms) { ks_latch(ctx, "MAX_STALE_MARKET_DATA", ms, c.max_stale_data_ms); return true; }
        }
    }

    // MAX_WS_DISCONNECT -- pencere icindeki kopma sayisi
    {
        const uint64_t total = ctx.ws_disconnect_total.load(std::memory_order_relaxed);
        while (k.ws_seen < total) { k.ws_ts[k.ws_head++ % KillSwitchState::WS_RING] = now; ++k.ws_seen; }
        if (c.max_ws_disconnects > 0) {
            const uint64_t win_ns = static_cast<uint64_t>(c.ws_window_sec * 1e9);
            int n = 0;
            for (uint64_t t : k.ws_ts) if (t != 0 && now >= t && (now - t) <= win_ns) ++n;
            const int need = std::min(c.max_ws_disconnects, KillSwitchState::WS_RING);
            if (n >= need) { ks_latch(ctx, "MAX_WS_DISCONNECT", n, need); return true; }
        }
    }

    // MAX_ACCOUNT_BALANCE_DROP -- ic ozsermaye (baslangic bakiyesi + gerceklesen PnL + floating) HWM'e gore
    {
        const double equity = ctx.current_capital + unrealized;
        if (equity > k.equity_hwm) k.equity_hwm = equity;
        if (c.max_balance_drop_pct > 0.0 && k.equity_hwm > 0.0) {
            const double drop = (k.equity_hwm - equity) / k.equity_hwm;
            if (drop >= c.max_balance_drop_pct)
                { ks_latch(ctx, "MAX_ACCOUNT_BALANCE_DROP", drop, c.max_balance_drop_pct); return true; }
        }
    }
    return false;
}

// Olay kancalari -- sayaci gunceller ve esigi ANINDA denetler (tick/500ms beklemez).
inline void ks_on_trade_closed(StrategyContext& ctx, double pnl) noexcept {
    auto& k = ctx.ks;
    k.daily_realized_pnl += pnl;
    k.consecutive_losses = (pnl < 0.0) ? k.consecutive_losses + 1 : 0;   // brut PnL (komisyon dahil degil)
    (void)check_kill_switch(ctx);
}
inline void ks_on_reject(StrategyContext& ctx) noexcept {
    ++ctx.ks.daily_rejects;
    (void)check_kill_switch(ctx);
}

// ─────────────────────────────────────────────────────────────────────────────
// Resume koşulları — [V13-CLOCK]: chrono → mono_ns
// ─────────────────────────────────────────────────────────────────────────────
inline void check_resume_conditions(StrategyContext& ctx) {
    if (!ctx.trading_paused) return;
    if (ctx.kill_switch_active.load(std::memory_order_acquire)) return;   // [PHASE3-KS] yalniz MANUEL reset
    const uint64_t now_ns = ctx.current_time_ns();
    if (now_ns - ctx.vpin_last_alarm_ns < 5'000'000'000ULL) return;  // 5s

    const double fr      = ctx.telemetry.fill_rate();
    const bool fill_ok   = (ctx.telemetry.total_sent() < 20) || (fr >= ctx.cfg.max_fill_rate_floor);
    const bool vpin_ok   = ctx.vpin.alarm() < kusak::research::VPINAlarm::CRITICAL;
    const bool regime_ok = true; // Risk kontrolü aktif edilene kadar bypass

    if (fill_ok && vpin_ok && regime_ok) {
        ctx.trading_paused = false;
        ctx.logger.info("RESUME",
            "Risk kosullari normalize oldu -- trading yeniden aktif."
            " fill_rate=", fr * 100.0,
            "% vpin_alarm=", static_cast<int>(ctx.vpin.alarm()));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// send_exit_order — [V13-STRUCT]: std::string → FixedString + char*
// ─────────────────────────────────────────────────────────────────────────────
inline void send_exit_order(StrategyContext& ctx, double price, const char* reason)
{
    if (!ctx.position.is_open()) return;
    // [HOTFIX-AMNESIA] pozisyon artik red/hata sonrasi korunuyor -> tick hizinda emir yagmuru olmasin
    if (ctx.current_time_ns() < ctx.exit_retry_not_before_ns) return;

    const bool   is_sell  = ctx.position.is_long();
    const double qty      = ctx.filter_validator.floor_qty(ctx.cfg.symbol, ctx.position.qty);   // [FIX-P35] round_qty pozisyonu asabiliyordu (0.290006->0.29001)

    ctx.logger.warn("EXIT", reason, " -> ",
        (is_sell ? "SELL" : "BUY"), " qty=", qty, " ~@", price);

    if (ctx.cfg.dry_run) {
        const double pnl = ctx.position.is_long()
            ? (price - ctx.position.entry_price) * qty
            : (ctx.position.entry_price - price) * qty;
        ctx.current_capital += pnl;
        ctx.locked_in_orders = std::max(0.0, ctx.locked_in_orders - ctx.position.notional);
        ks_on_trade_closed(ctx, pnl);   // [PHASE3-KS]
        ctx.logger.info("EXIT", "[DRY-RUN] PnL=", pnl,
            " capital_after=", ctx.current_capital);

        // [FIX-DRYRUN-ZOMBIE] maybe_send_order() dry-run kolunda
        // ctx.flat_order_map.insert(ctx.active_order_id, ...) ile eklenen
        // emir, burada haritadan hic temizlenmiyordu -- flat_order_map'te
        // sonsuza kadar ACTIVE kalan bir "zombi" emir birikiyordu. Sentetik
        // bir FILLED OrderResult uretip haritayi terminal (tombstone)
        // duruma cekiyoruz.
        // [PATCH-P1/P6] Dry-run EXIT de live yolla AYNI sekilde kendi EXIT slotunu acar ve terminale tasir.
        // Giris slotu dry-run'da girisin kendisinde terminal (FILLED) olur -> burada artik giris ID'sine dokunulmaz.
        ctx.fsm_key = ++ctx.fsm_exit_seq;
        ctx.flat_order_map.insert(ctx.fsm_key, qty);
        for (OrderState st : {OrderState::CREATED, OrderState::PREFLIGHT, OrderState::SUBMITTED})
            ctx.flat_order_map.transition_key(ctx.fsm_key, st);

        OrderResult res{};
        res.order_id          = ctx.fsm_key;
        res.local_order_id    = ctx.fsm_key;
        res.exchange_order_id = ctx.fsm_key;
        res.is_exit           = true;
        res.status            = OrderStatus::FILLED;
        res.filled_qty        = qty;
        res.remaining_qty     = 0.0;
        res.avg_price         = price;
        res.ts_recv_ns        = ctx.current_time_ns();
        const FsmResult fsm = ctx.flat_order_map.update(res);
        if (UNLIKELY(fsm.verdict != FsmVerdict::APPLIED)) {
            ctx.logger.error("DRYRUN-EXIT", "mock fill FSM'e uygulanamadi verdict=",
                static_cast<int>(fsm.verdict), " state=", order_state_name(fsm.from),
                " local_id=", res.local_order_id);
        }

        ctx.position.reset();
        ctx.awaiting_fill   = false;
        ctx.active_order_id = 0;
        ctx.active_client_oid = FixedString<32>{};   // [FIX-P0-02]
        return;
    }

    if (!ctx.live_order_client.has_value()) return;

    // [V13-CLOCK]: chrono → mono_ns
    const uint64_t now_ns = ctx.current_time_ns();

    OrderTask task{};
    task.kind            = TaskKind::EXIT;
    task.type            = OrderType::MARKET;
    task.tif             = TimeInForce::GTC;
    task.side            = is_sell ? OrderSide::SELL : OrderSide::BUY;
    task.price           = price;
    task.qty             = qty;
    // [PHASE2-FSM] EXIT de FSM'e girer: kendi yerel ID'si (entry'ninkiyle karismaz)
    ctx.fsm_key          = ++ctx.fsm_exit_seq;
    task.order_id        = ctx.active_order_id;
    task.local_order_id  = ctx.fsm_key;
    task.is_exit         = true;
    task.ts_send_ns      = now_ns;
    ctx.flat_order_map.insert(ctx.fsm_key, qty);
    ctx.flat_order_map.transition_key(ctx.fsm_key, OrderState::CREATED);
    ctx.flat_order_map.transition_key(ctx.fsm_key, OrderState::PREFLIGHT);

    {
        const uint8_t slen = static_cast<uint8_t>(ctx.cfg.symbol.size());
        task.symbol.set(ctx.cfg.symbol.c_str(), slen);
    }
    {
        char coid[32];
        std::memcpy(coid, "KUSAK_EXIT_", 11);
        auto [ptr, ec] = std::to_chars(coid + 11, coid + 32, now_ns / 1'000'000ULL);
        task.client_oid.set(coid, static_cast<uint8_t>(ptr - coid));
        ctx.active_client_oid = task.client_oid;   // [FIX-P0-02]
    }

    ctx.awaiting_fill   = true;
    ctx.awaiting_fill_since_ns = now_ns;   // [FIX-TTL]
    ctx.active_order_id = 0;

    fw_publish_exposure(ctx);   // [FIX-P0-03] order thread EXIT firewall'i taze pozisyon kopyasi gorsun (verified korunur)
    if (UNLIKELY(!ctx.order_channel.push_task(task))) {
        ctx.logger.error("EXIT", "KRITIK: EXIT task drop! Ring dolu.");
        ctx.flat_order_map.transition_key(ctx.fsm_key, OrderState::REJECTED);   // zombi slot birakma
        ctx.awaiting_fill = false;
        ctx.active_client_oid = FixedString<32>{};   // [FIX-P0-02]
        return;
    }
    ctx.flat_order_map.transition_key(ctx.fsm_key, OrderState::SUBMITTED);
}

// ─────────────────────────────────────────────────────────────────────────────
// [PHASE3-KS] CANCEL ALL -- FlatOrderMap uzerinden (Phase 2 FSM): her bekleyen emir
// CANCEL_REQUEST -> (ring'e yazildi) CANCEL_PENDING; borsa teyidiyle CANCELED. Donus: hala canli emir sayisi.
// EXIT emirleri (anahtar >= fsm_exit_seq tabani) atlanir: flatten emri iptal edilmemeli.
// ─────────────────────────────────────────────────────────────────────────────
inline uint32_t ks_cancel_all(StrategyContext& ctx) noexcept {
    uint32_t live = 0;
    auto& map = ctx.flat_order_map;
    const uint64_t now = ctx.current_time_ns();
    constexpr uint64_t EXIT_BASE = (1ULL << 56);
    for (size_t w = 0; w < FlatOrderMap::BITMAP_WORDS; ++w) {
        uint64_t bits = map.active_bitmap[w];
        const size_t base = w << 6;
        while (bits) {
            const unsigned b = static_cast<unsigned>(__builtin_ctzll(bits));
            bits &= bits - 1;
            OrderSlot& s = map.slots[base + b];
            if (s.state != SlotState::ACTIVE || s.order_id >= EXIT_BASE) continue;

            switch (s.fsm_state) {
                case OrderState::SIGNAL: case OrderState::CREATED: case OrderState::PREFLIGHT:
                    map.step(s, OrderState::REJECTED);      // borsaya hic gitmedi
                    continue;
                case OrderState::CANCEL_PENDING:            // borsa teyidi bekleniyor
                case OrderState::SUBMITTED:                 // borsa ID'si yok: ACK/UNKNOWN akisi beklenir
                    ++live; continue;
                default: break;                             // ACK, NEW, PARTIAL, CANCEL_REQUEST, UNKNOWN
            }
            if (ctx.cfg.dry_run) {                          // borsa yok: FSM'de iptal et
                if (s.fsm_state != OrderState::CANCEL_REQUEST) map.step(s, OrderState::CANCEL_REQUEST);
                map.step(s, OrderState::CANCELED);
                continue;
            }
            ++live;
            if (s.exchange_order_id == 0) continue;         // iptal edilecek borsa ID'si henuz yok
            if (s.fsm_state != OrderState::CANCEL_REQUEST) map.step(s, OrderState::CANCEL_REQUEST);
            OrderTask ct{};
            ct.kind           = TaskKind::CANCEL;
            ct.order_id       = s.exchange_order_id;
            ct.local_order_id = s.order_id;
            ct.ts_send_ns     = now;
            ct.symbol.set(ctx.cfg.symbol.c_str(), static_cast<uint8_t>(ctx.cfg.symbol.size()));
            if (ctx.order_channel.push_task(ct)) map.step(s, OrderState::CANCEL_PENDING);
            // ring doluysa slot CANCEL_REQUEST'te kalir; sonraki servis turunda yeniden denenir
        }
    }
    return live;
}

// ─────────────────────────────────────────────────────────────────────────────
// [PATCH-P2] request_entry_cancel -- TTL/timeout iptali icin TEK KAPI.
//   * EXIT emri asla iptal edilmez (flatten emri).
//   * Yalniz iptal EDILEBILIR durumlarda (NEW/ACK/PARTIAL/UNKNOWN/CANCEL_REQUEST-retry) gonderilir.
//   * CANCEL_PENDING'deyken ikinci iptal GITMEZ: terminal yalniz borsa teyidiyle (CANCELED) ya da
//     yarisi kazanan FILLED ile kapanir -> tek terminal, tek tombstone, sayac tek kez eksilir.
//   * Borsa ID'si slottan okunur (active_order_id degil); ID yoksa iptal gonderilemez.
// Donus: true => CANCEL task ring'e yazildi ve slot CANCEL_PENDING'e gecti.
// ─────────────────────────────────────────────────────────────────────────────
inline bool request_entry_cancel(StrategyContext& ctx, uint64_t key, uint64_t now_ns) noexcept {
    if (key == 0 || key >= FSM_EXIT_BASE) return false;
    OrderSlot* s = ctx.flat_order_map.find(key);
    if (!s || s->state != SlotState::ACTIVE) return false;
    switch (s->fsm_state) {
        case OrderState::NEW: case OrderState::ACK: case OrderState::PARTIAL:
        case OrderState::UNKNOWN: case OrderState::CANCEL_REQUEST: break;
        default: return false;                       // SUBMITTED / CANCEL_PENDING / on-uc durumlar
    }
    if (s->exchange_order_id == 0) return false;
    if (s->fsm_state != OrderState::CANCEL_REQUEST) ctx.flat_order_map.step(*s, OrderState::CANCEL_REQUEST);
    OrderTask ct{};
    ct.kind           = TaskKind::CANCEL;
    ct.order_id       = s->exchange_order_id;
    ct.local_order_id = key;
    ct.side           = ctx.pending_is_buy ? OrderSide::BUY : OrderSide::SELL;
    ct.is_exit        = false;
    ct.ts_send_ns     = now_ns;
    ct.symbol.set(ctx.cfg.symbol.c_str(), static_cast<uint8_t>(ctx.cfg.symbol.size()));
    if (!ctx.order_channel.push_task(ct)) return false;   // slot CANCEL_REQUEST'te kalir; sonraki turda tekrar
    ctx.flat_order_map.step(*s, OrderState::CANCEL_PENDING);
    return true;
}

// Manuel reset: YALNIZ SAFE MODE'dayken ve kilit dosyasi operator tarafindan silinmisse.
// Gunluk zarar SIFIRLANMAZ (ayni gun limit hala asiksa hemen yeniden tetiklenir). HWM mevcut ozsermayeye cekilir.
inline void ks_manual_reset(StrategyContext& ctx) noexcept {
    auto& k = ctx.ks;
    k.latched = false; k.safe_mode = false; k.lock_written = false; k.reason = nullptr;
    k.consecutive_losses = 0; k.daily_rejects = 0; k.latency_breach_streak = 0;
    k.equity_hwm = ctx.current_capital; k.win_start_ns = 0;
    for (auto& t : k.ws_ts) t = 0;
    k.ws_seen = ctx.ws_disconnect_total.load(std::memory_order_relaxed);
    ctx.order_rtt_ns.store(0, std::memory_order_relaxed);
    ctx.trading_paused = false;
    ctx.kill_switch_active.store(false, std::memory_order_release);
    ctx.logger.critical("KILL_SWITCH", "MANUEL RESET -- kilit dosyasi silindi, ticaret yeniden aktif. ",
        "Gunluk PnL korunur; limit hala asiksa yeniden tetiklenir.");
}

// Baslangicta kilit dosyasi varsa sistem KILITLI baslar (restart ile kill-switch atlatilamaz).
inline void ks_boot_check(StrategyContext& ctx) noexcept {
    std::error_code ec;
    if (std::filesystem::exists(ctx.cfg.ks.lock_file, ec))
        ks_latch(ctx, "LOCK_FILE_PRESENT", 0.0, 0.0);
}

// Ana dongu servisi (her iterasyon, kill_switch aktifken): CANCEL ALL -> SAFE MODE -> (flatten) -> manuel reset.
inline void ks_service(StrategyContext& ctx) noexcept {
    auto& k = ctx.ks;
    if (!ctx.kill_switch_active.load(std::memory_order_acquire)) return;
    if (!k.latched) ks_latch(ctx, "EXTERNAL_TRIP", 0.0, 0.0);
    const uint64_t now = ctx.current_time_ns();

    if (k.safe_mode) {
        if (k.lock_written && now - k.last_lock_poll_ns >= 1'000'000'000ULL) {
            k.last_lock_poll_ns = now;
            std::error_code ec;
            if (!std::filesystem::exists(ctx.cfg.ks.lock_file, ec) && !ec) ks_manual_reset(ctx);
        }
        // SAFE MODE'da bile acik pozisyon varsa (flatten_on_trip) kapatma denemesi surer
    } else if (k.last_cancel_all_ns == 0 || now - k.last_cancel_all_ns >= 250'000'000ULL) {
        k.last_cancel_all_ns = now;
        if (ks_cancel_all(ctx) == 0) {                               // (b) CANCEL ALL tamam
            k.safe_mode = true;                                      // (c) SAFE MODE
            ctx.trading_paused = true;
            ctx.logger.critical("KILL_SWITCH", "SAFE MODE -- acik emir kalmadi. reason=",
                k.reason ? k.reason : "?", " | manuel reset: '", ctx.cfg.ks.lock_file, "' silinmeli");
        }
    }

    if (ctx.cfg.ks.flatten_on_trip && ctx.position.is_open() && !ctx.awaiting_fill &&
        (k.last_flatten_ns == 0 || now - k.last_flatten_ns >= 5'000'000'000ULL)) {
        k.last_flatten_ns = now;
        const double px = ctx.position.is_long() ? ctx.last_bid : ctx.last_ask;
        send_exit_order(ctx, px > 0.0 ? px : ctx.last_mid, "KILL-SWITCH acil cikis");
    }
}


// ─────────────────────────────────────────────────────────────────────────────
// [V13-PROFIT-ENGINE] VPIN Toksisite Filtresi
// ─────────────────────────────────────────────────────────────────────────────
inline bool vpin_allows_trade(const StrategyContext& ctx) noexcept {
    // [ARCH-V2-FIX] vpin() -> vpin_x1e6(); eşik de 0.75 -> 750'000 (VPIN_SCALE) olarak ölçeklendi.
    return ctx.vpin.vpin_x1e6() <= 750'000;  // if >0.75, informed traders are active → BLOCK
}

// ─────────────────────────────────────────────────────────────────────────────
// [V13-PROFIT-ENGINE] Top-of-Book OBI (Emir Defteri Dengesizliği)
// ─────────────────────────────────────────────────────────────────────────────
inline double compute_top_of_book_obi(const StrategyContext& ctx) noexcept {
    double bid_qty = ctx.last_bid_qty;    // from bookTicker
    double ask_qty = ctx.last_ask_qty;
    double denom = bid_qty + ask_qty;
    if (denom < 1e-12) return 0.0;
    return (bid_qty - ask_qty) / denom;
}

// ─────────────────────────────────────────────────────────────────────────────
// [V13-PROFIT-ENGINE] Yarım Kelly Pozisyon Büyüklüğü (O(1))
// ─────────────────────────────────────────────────────────────────────────────
inline double compute_half_kelly_qty(const StrategyContext& ctx,
                                     double usable_capital,
                                     double price) noexcept {
    if (ctx.cfg.kelly_fraction <= 0.0) {
        // Fallback to buy_pct if Kelly disabled
        return usable_capital * ctx.cfg.buy_pct / price;
    }

    const auto& pnl = ctx.recent_pnl;
    double wr = pnl.win_rate();
    double avgW = pnl.avg_win();
    double avgL = pnl.avg_loss();

    if (wr < 0.01 || avgL < 1e-12) return 0.0;

    double f = wr - (1.0 - wr) / (avgW / avgL);
    if (f <= 0.0) return 0.0;

    double half_kelly = f * 0.5 * ctx.cfg.kelly_fraction;    // half-Kelly scaled by config
    double notional = usable_capital * std::min(half_kelly, 0.25); // max 25% capital per trade
    return notional / price;
}

// ─────────────────────────────────────────────────────────────────────────────
// [V13-PROFIT-ENGINE] Dinamik Momentum Çıkışı (Maçı Bitiren Düdük)
// ─────────────────────────────────────────────────────────────────────────────
inline void dynamic_momentum_exit(StrategyContext& ctx) {
    if (!ctx.position.is_open() || ctx.awaiting_fill) return;

    double momentum = ctx.trade_direction.momentum();

    // LONG pozisyonda momentum satışa dönerse erkenden çık
    if (ctx.position.is_long()) {
        bool profitable = ctx.last_bid > ctx.position.entry_price * 1.002;
        if (profitable && momentum < -0.3) {
            send_exit_order(ctx, ctx.last_bid, "momentum_reversal_LONG");
            return;
        }
    }
    // SHORT pozisyonda momentum alışa dönerse erkenden çık
    else if (ctx.position.is_short()) {
        bool profitable = ctx.last_ask < ctx.position.entry_price * 0.998;
        if (profitable && momentum > 0.3) {
            send_exit_order(ctx, ctx.last_ask, "momentum_reversal_SHORT");
            return;
        }
    }

    // VPIN kritik seviye → acil çıkış
    if (ctx.vpin.alarm() >= kusak::research::VPINAlarm::CRITICAL) {
        double exit_px = ctx.position.is_long() ? ctx.last_bid : ctx.last_ask;
        send_exit_order(ctx, exit_px, "vpin_critical");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// maybe_send_order — [V13-STRUCT]: std::string → FixedString
//                    [V13-MAP]:    order_map → flat_order_map
//                    [V13-CLOCK]:  chrono → mono_ns
// V12 fix etiketleri ([FIX-STALE], [FIX-OPT], [FIX-CLAUDE-CAPITAL], vb.) geçerli.
// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// [FIX-LOGSPAM-3] Genel amaçlı throttle makrosu. Her çağrı noktası kendi
// BAĞIMSIZ zamanlayıcısına sahiptir (makro her genişletildiğinde ayrı bir
// static değişken üretir) -- bu yüzden bir ret koşulunun sık tetiklenmesi
// diğerinin loglarını bastırmaz. steady_clock kullanır, TSC kalibrasyonuna
// bağlı DEĞİLDİR.
// ─────────────────────────────────────────────────────────────────────────────
#define KUSAK_THROTTLE_MS 2000

#define LOG_WARN_THROTTLED(LOGGER, TAG, ...)                                     \
    do {                                                                          \
        static std::atomic<int64_t> _thr_last_ms{0};                              \
        const int64_t _thr_now_ms = std::chrono::duration_cast<std::chrono::milliseconds>( \
            std::chrono::steady_clock::now().time_since_epoch()).count();         \
        int64_t _thr_prev_ms = _thr_last_ms.load(std::memory_order_relaxed);      \
        if (_thr_now_ms - _thr_prev_ms > KUSAK_THROTTLE_MS &&                     \
            _thr_last_ms.compare_exchange_strong(_thr_prev_ms, _thr_now_ms,       \
                                                  std::memory_order_relaxed)) {    \
            (LOGGER).warn(TAG, __VA_ARGS__);                                      \
        }                                                                         \
    } while (0)

inline bool maybe_send_order(StrategyContext& ctx)
{
    if (!ctx.filter_validator.has_symbol(ctx.cfg.symbol)) {
        LOG_WARN_THROTTLED(ctx.logger, "STRATEGY", "[DEBUG] RET: has_symbol=false sym=", ctx.cfg.symbol);
        return false;
    }
    if (ctx.trading_paused) {
        LOG_WARN_THROTTLED(ctx.logger, "STRATEGY", "[DEBUG] RET: trading_paused=true");
        return false;
    }
    if (ctx.kill_switch.load(std::memory_order_acquire)) {
        LOG_WARN_THROTTLED(ctx.logger, "STRATEGY", "[DEBUG] RET: kill_switch=true");
        return false;
    }
    if (ctx.position.is_open()) {
        LOG_WARN_THROTTLED(ctx.logger, "STRATEGY", "[DEBUG] RET: position.is_open=true");
        return false;
    }
    if (ctx.awaiting_fill) {
        LOG_WARN_THROTTLED(ctx.logger, "STRATEGY", "[DEBUG] RET: awaiting_fill=true");
        return false;
    }

    const uint64_t now_ns = ctx.current_time_ns();
    if (now_ns < ctx.cooldown_until_ns.load(std::memory_order_relaxed)) {
        LOG_WARN_THROTTLED(ctx.logger, "STRATEGY", "[DEBUG] RET: cooldown aktif, kalan_ns=",
            (ctx.cooldown_until_ns.load(std::memory_order_relaxed) - now_ns));
        return false;
    }

    constexpr uint64_t MAX_STALE_NS = 5'000'000'000ULL;
    const uint64_t last_tick = ctx.last_ticker_ns.load(std::memory_order_relaxed);
    if (last_tick == 0 || (now_ns - last_tick) > MAX_STALE_NS) {
        LOG_WARN_THROTTLED(ctx.logger, "STRATEGY",
            "Fiyat verisi bayat (>5s) veya hic alinmadi -- emir gonderilmiyor");
        return false;
    }

    const double bid = ctx.last_bid;
    const double ask = ctx.last_ask;
    const double mid = ctx.last_mid;
    if (mid <= 0.0 || bid <= 0.0 || ask <= 0.0) {
        LOG_WARN_THROTTLED(ctx.logger, "STRATEGY", "[DEBUG] RET: gecersiz fiyat bid=", bid,
            " ask=", ask, " mid=", mid);
        return false;
    }

    static uint64_t last_volatile_log_ns = 0;
    // [MVP-MAKER] Bu devre kesici onceden `if (UNLIKELY(false))` ile KALICI
    // OLARAK DEVRE DISIYDI -- muhtemelen ATR/rejim verisi hep bozukken (bkz.
    // ATR=0 bug) HERHANGI bir islem test edebilmek icin. Rejim dedektoru artik
    // gercek veriyle besleniyor (Patch 2), bu yuzden yeniden etkinlestirildi:
    // "strict MVP, conservative" talebinizin dogrudan karsiligi budur.
    if (UNLIKELY(ctx.regime.isHighRisk())) {
        if (now_ns - last_volatile_log_ns >= 1'000'000'000ULL) {
            last_volatile_log_ns = now_ns;
            ctx.logger.warn("STRATEGY", "VOLATILE rejim -- emir durduruldu (devre kesici aktif)");
        }
        return false;
    }

    kusak::research::OrderLifecycle lifecycle;
    lifecycle.tickRecvNS = now_ns;

    kusak::research::RiskContext rctx;
    // [ARCH-V2-FIX] vpin() -> vpin_x1e6(); int64_t sonuç /VPIN_SCALE ile double [0,1] aralığına geri ölçeklendi
    //               (aksi halde 0.55 yerine 550000 gibi 1e6 kat yanlış bir değer risk motoruna sızardı).
    rctx.vpin          = static_cast<double>(ctx.vpin.vpin_x1e6()) / static_cast<double>(kusak::research::VPIN_SCALE);
    rctx.vpin_mean     = 0.0;
    rctx.vpin_sigma    = 1.0;
    rctx.hmm_prob_high = ctx.regime.regime_probability(kusak::research::MarketRegime::VOLATILE);
    rctx.spread_bps    = (ask - bid) / mid * 10000.0;
    rctx.depth_bid_usd = bid;
    rctx.depth_ask_usd = ask;
    rctx.refresh_inverses();

    const auto risk = ctx.risk_scorer.evaluate(rctx);
    lifecycle.decisionNS = ctx.current_time_ns();

    if (!risk.can_trade()) {
        ctx.telemetry.record_risk_veto(risk.score, lifecycle.decisionNS);
        LOG_WARN_THROTTLED(ctx.logger, "STRATEGY", "[DEBUG] RET: risk_scorer can_trade=false score=", risk.score);
        return false;
    }

    // ─── KÂR MOTORU: VPIN Toksisite Kapısı ───
    if (!vpin_allows_trade(ctx)) {
        LOG_WARN_THROTTLED(ctx.logger, "STRATEGY", "[DEBUG] RET: vpin_allows_trade=false");
        return false;
    }

    // ─── KÂR MOTORU: Top-of-Book OBI Sinyali ───
    // [FIX-CONFIG-BRIDGE] Sabit +-0.35 yerine artik config'ten okunuyor.
    const double obi_thr = ctx.cfg.obi_threshold;
    double obi = compute_top_of_book_obi(ctx);
    if (obi > obi_thr)
        ctx.pending_is_buy = true;
    else if (obi < -obi_thr)
        ctx.pending_is_buy = false;
    else {
        LOG_WARN_THROTTLED(ctx.logger, "STRATEGY", "[DEBUG] RET: OBI sinyal yok, obi=", obi, " (esik +-", obi_thr, ")");
        return false;   // No clear signal
    }

    const bool is_buy = ctx.pending_is_buy;

    // [MVP-MAKER] KUSAK-INFRA MVP Yürütme Modu.
    // true  -> LIMIT_MAKER (post-only): borsa asla cross ettirmez, sadece
    //          maker fee + spread yakalama; doldurulmama riski var (bkz.
    //          MAKER_REST_TIMEOUT_NS ile otomatik iptal).
    // false -> MARKET (taker): anlik dolar, spread+taker fee'yi hemen oder.
    // "strict MVP hard-coded strategy" talebine uygun: tek satır, derleme
    // zamanında sabit. Bkz. yanıttaki "MVP Quant Strategy" bölümü.
    constexpr bool is_passive = true;

    if ( is_buy && ctx.param_adapter.buys_disabled()) {
        LOG_WARN_THROTTLED(ctx.logger, "STRATEGY", "[DEBUG] RET: buys_disabled=true");
        return false;
    }
    if (!is_buy && ctx.param_adapter.sells_disabled()) {
        LOG_WARN_THROTTLED(ctx.logger, "STRATEGY", "[DEBUG] RET: sells_disabled=true");
        return false;
    }

    // ─── KÂR MOTORU: Yarım Kelly Pozisyon Boyutu ───
    const double usable = ctx.current_capital - ctx.locked_in_orders;
    if (usable <= 0.0) {
        LOG_WARN_THROTTLED(ctx.logger, "STRATEGY", "[DEBUG] RET: Yetersiz serbest sermaye: ", usable);
        return false;
    }

    // [FIX-MVP-MAKER]: onceki kod pasif modda bile order_price = is_buy ? ask
    // : bid kullaniyordu -- bu, bir BUY LIMIT emrini dogrudan mevcut en iyi
    // ASK fiyatina koymak demektir, yani ANINDA cross eder (taker olur).
    // is_passive=true iken artik kitabin KENDI tarafina (BUY->bid, SELL->ask)
    // katiliyoruz -- boylece emir asla spread'i gecmiyor.
    double order_price = is_passive
        ? (is_buy ? bid : ask)   // pasif: kuyruga katil, cross ETME
        : (is_buy ? ask : bid);  // agresif (taker) -- referans/fallback yolu
    order_price = ctx.filter_validator.round_price(ctx.cfg.symbol, order_price);

    double kelly_qty = compute_half_kelly_qty(ctx, usable, order_price);
    kelly_qty = ctx.filter_validator.round_qty(ctx.cfg.symbol, kelly_qty);
    if (kelly_qty <= 0.0) {
        LOG_WARN_THROTTLED(ctx.logger, "STRATEGY", "[DEBUG] RET: kelly_qty<=0, order_price=", order_price,
            " usable=", usable);
        return false;
    }

    const double min_qty = ctx.filter_validator.min_qty_for_notional(ctx.cfg.symbol, order_price);
    if (kelly_qty < min_qty) {
        LOG_WARN_THROTTLED(ctx.logger, "STRATEGY", "[DEBUG] RET: kelly_qty(", kelly_qty,
            ") < min_qty(", min_qty, ") -- ISLEM IPTAL");
        return false;
    }

    double order_qty = kelly_qty;
    {   // [PHASE1-FW] Madde 1: max_position_usd sert tavan; canlida limit yoksa fail-closed
        const double cap_usd = ctx.cfg.max_position_usd;
        if (!(cap_usd > 0.0) || !std::isfinite(cap_usd)) {
            if (!ctx.cfg.dry_run) {
                ctx.logger.error("FIREWALL", "max_position_usd tanimsiz/gecersiz -- emir reddedildi");
                return false;
            }
        } else {
            if (!(order_price > 0.0) || !std::isfinite(order_price)) {
                ctx.logger.error("FIREWALL", "gecersiz order_price=", order_price, " -- emir reddedildi");
                return false;
            }
            // (1 - 1e-12): floor sonrasi qty*price <= cap garantisi (FP tasmasina karsi)
            order_qty = std::min(order_qty, (cap_usd / order_price) * (1.0 - 1e-12));
            order_qty = ctx.filter_validator.floor_qty(ctx.cfg.symbol, order_qty);   // daima asagi
            if (!(order_qty > 0.0) || !std::isfinite(order_qty)) { return false; }
            if (order_qty < min_qty)                              { return false; }
        }
    }

    const auto fres = ctx.filter_validator.validate_order(
        ctx.cfg.symbol, order_price, order_qty, mid);
    if (fres != kusak::exchange::FilterResult::VALID) {
        ctx.logger.warn("STRATEGY", "Filtre hatasi: ",
            kusak::exchange::filter_result_name(fres));
        return false;
    }

    {
        const double notional = order_price * order_qty;
        const double min_not  = min_qty * order_price;
        ctx.logger.debug("PREFLIGHT",
            "sym=", ctx.cfg.symbol,
            " side=", (is_buy ? "BUY" : "SELL"),
            " type=", (is_passive ? "LIMIT" : "MARKET"),
            " price=", order_price, " qty=", order_qty,
            " notional=", notional, " min=", min_not,
            " obi=", obi, " kelly_qty=", kelly_qty);
        if (notional < min_not) {
            ctx.logger.error("PREFLIGHT", "NOTIONAL ALTINDA -- emir reddedilecek");
            return false;
        }
    }
    if (ctx.cfg.max_position_usd > 0.0 &&
        (order_qty * order_price) > ctx.cfg.max_position_usd * (1.0 + 1e-12)) {
        ctx.logger.error("FIREWALL", "REJECT: notional=", order_qty * order_price,
            " > max_position_usd=", ctx.cfg.max_position_usd);
        return false;
    }
    // [PATCH-P3] REVERSAL KAPISI: eski pozisyonun EXIT emri hala canli (SUBMITTED/NEW/PARTIAL/UNKNOWN/CANCEL_*) iken
    // ters/yeni giris ASLA gonderilmez. Normalde position.is_open()/awaiting_fill bunu zaten engeller;
    // bu kapi, o bayraklardan biri yanlis temizlenirse bile cift-pozisyon (long+short) riskini kapatir.
    if (UNLIKELY(ctx.flat_order_map.has_live_exit())) {
        LOG_WARN_THROTTLED(ctx.logger, "STRATEGY", "[REVERSAL-GUARD] canli EXIT emri var -- yeni giris bekletiliyor");
        return false;
    }
    lifecycle.sendOrderNS = ctx.current_time_ns();
    ctx.adverse.record_order_sent(lifecycle.sendOrderNS, order_price, is_buy);
    ctx.active_order_id = ++ctx.fsm_entry_seq;   // [PATCH-P5] monoton yerel ID: eski emrin gec gelen POLL'u yeni emre ALIAS olamaz
    ctx.telemetry.record_order_sent(
        ctx.active_order_id, order_price, order_qty, lifecycle.sendOrderNS);

    // [MVP-MAKER] Sabit 0.002/0.0005 yerine ATR-turevi trailing_params() --
    // bu deger onceden HESAPLANIYORDU ama hicbir yerde KULLANILMIYORDU.
    // ATR=0 iken taban degerlere (5bps/2bps) kilitlenir; ATR akmaya basladiktan
    // sonra gercek volatiliteyle olcekli hale gelir.
    const auto  tp        = ctx.regime.trailing_params();
    const double stop_pct  = tp.activation_bps / 10000.0;
    const double trail_pct = tp.callback_bps   / 10000.0;

    if (ctx.cfg.dry_run) {
        ctx.logger.info("STRATEGY", "[DRY-RUN STRESS] ",
            (is_buy ? "BUY" : "SELL"),
            " @", order_price, " qty=", order_qty,
            " notional=", order_price * order_qty);

        ctx.position.side        = is_buy ? PositionSide::LONG : PositionSide::SHORT;
        ctx.position.entry_price = order_price;
        ctx.position.qty         = order_qty;
        ctx.position.notional    = order_price * order_qty;
        ctx.position.stop_loss   = order_price * (is_buy ? (1.0 - stop_pct) : (1.0 + stop_pct));
        ctx.position.trail_delta = order_price * trail_pct;
        ctx.position.high_water  = order_price;
        ctx.position.trail_stop  = is_buy
            ? (order_price - ctx.position.trail_delta)
            : (order_price + ctx.position.trail_delta);
        ctx.locked_in_orders += ctx.position.notional;

        ctx.flat_order_map.insert(ctx.active_order_id, order_qty);  // [V13-MAP]
        ctx.fsm_key = ctx.active_order_id;
        for (OrderState st : {OrderState::CREATED, OrderState::PREFLIGHT, OrderState::SUBMITTED,
                              OrderState::NEW})   // dry-run: borsa yok -> ortuk ACK ile NEW'e kadar
            ctx.flat_order_map.transition_key(ctx.fsm_key, st);
        {   // [PATCH-P1/P6] live yolla simetri: dry-run giris de ANINDA dolar -> slot terminal (tombstone), active_orders=0.
            OrderResult fill{};
            fill.order_id          = ctx.fsm_key;
            fill.local_order_id    = ctx.fsm_key;
            fill.exchange_order_id = ctx.fsm_key;
            fill.is_buy            = is_buy;
            fill.status            = OrderStatus::FILLED;
            fill.filled_qty        = order_qty;
            fill.remaining_qty     = 0.0;
            fill.avg_price         = order_price;
            fill.ts_recv_ns        = ctx.current_time_ns();
            const FsmResult fr = ctx.flat_order_map.update(fill);
            if (UNLIKELY(fr.verdict != FsmVerdict::APPLIED))
                ctx.logger.error("DRYRUN-ENTRY", "mock fill FSM'e uygulanamadi verdict=",
                    static_cast<int>(fr.verdict), " state=", order_state_name(fr.from), " local_id=", fill.local_order_id);
        }
        ctx.awaiting_fill = false;

        kusak::monitoring::TickToTradeTimer ttt;
        ttt.mark_tick_received();
        ttt.mark_order_sent();
        ctx.latency.record_tick_to_trade(ttt);
        ctx.telemetry.record_latency(
            static_cast<uint64_t>(ttt.tick_to_trade_us() * 1000.0),
            lifecycle.sendOrderNS);
        return true;
    }

    if (!ctx.live_order_client.has_value()) {
        ctx.logger.error("STRATEGY", "live_order_client yok");
        return false;
    }

    // [FIX-OPT]: Optimistik pozisyon kurulumu YOK — yalnızca FILLED teyidiyle
    // [FIX-CLAUDE-CAPITAL]: locked_in_orders burada EKLENMİYOR
    ctx.awaiting_fill = true;
    ctx.pending_entry_notional = order_qty * order_price;   // [PHASE0-GUARD]
    ctx.awaiting_fill_since_ns = tsc_ns();   // [FIX-TTL]
    ctx.flat_order_map.insert(ctx.active_order_id, order_qty);  // [V13-MAP]
    ctx.fsm_key = ctx.active_order_id;                          // [PHASE2-FSM] SIGNAL
    ctx.flat_order_map.transition_key(ctx.fsm_key, OrderState::CREATED);
    ctx.flat_order_map.transition_key(ctx.fsm_key, OrderState::PREFLIGHT);   // firewall kontrolleri yukarida gecti

    // [V13-STRUCT]: FixedString doldurma
    OrderTask task{};
    task.kind           = TaskKind::SEND;
    task.type           = is_passive ? OrderType::LIMIT_MAKER : OrderType::MARKET;
    task.tif            = TimeInForce::GTC;
    task.side           = is_buy ? OrderSide::BUY : OrderSide::SELL;
    task.price          = order_price;
    task.qty            = order_qty;
    task.order_id       = ctx.active_order_id;
    task.local_order_id = ctx.active_order_id;
    task.is_exit        = false;
    task.ts_send_ns     = lifecycle.sendOrderNS;

    {
        const uint8_t sl = static_cast<uint8_t>(ctx.cfg.symbol.size());
        task.symbol.set(ctx.cfg.symbol.c_str(), sl);
    }
    {
        char coid[32];
        std::memcpy(coid, "KUSAK_", 6);
        auto [ptr, ec] = std::to_chars(coid + 6, coid + 32, lifecycle.sendOrderNS / 1'000'000ULL);
        task.client_oid.set(coid, static_cast<uint8_t>(ptr - coid));
        ctx.active_client_oid = task.client_oid;   // [FIX-P0-02]
    }

    fw_publish_exposure(ctx);   // [PHASE0-GUARD] order thread firewall'i taze maruziyet gorsun
    if (UNLIKELY(!ctx.order_channel.push_task(task))) {
        ctx.logger.error("ORDER", "KRITIK: SEND task drop! Ring dolu.");
        ctx.flat_order_map.transition_key(ctx.fsm_key, OrderState::REJECTED);   // zombi slot birakma
        ctx.awaiting_fill = false;
        ctx.active_order_id = 0;
        ctx.active_client_oid = FixedString<32>{};   // [FIX-P0-02]
        return false;
    }
    ctx.flat_order_map.transition_key(ctx.fsm_key, OrderState::SUBMITTED);      // [PHASE2-FSM]

    kusak::monitoring::TickToTradeTimer ttt;
    ttt.mark_tick_received();
    ttt.mark_order_sent();
    ctx.latency.record_tick_to_trade(ttt);
    ctx.telemetry.record_latency(
        static_cast<uint64_t>(ttt.tick_to_trade_us() * 1000.0),
        lifecycle.sendOrderNS);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// [HOTFIX-AMNESIA] Iptal/red/hata sonrasi pozisyon muhasebesi.
// Eski kod bu uc durumda KOSULSUZ position.reset() cagiriyordu:
//   * kismen dolmus GIRIS emri iptal edilirse (FILL_TIMEOUT, TTL, kill-switch CANCEL ALL) dolan miktar siliniyordu,
//   * reddedilen/iptal edilen EXIT emri GERCEK acik pozisyonu siliyordu.
// Canli yolda position gonderimde degil, ilk dolumda kurulur; bu yuzden cancel aninda GIRIS icin
// position.is_open() == "kismen doldu" demektir.
// Donus: true  => pozisyon KORUNDU (kilitli sermaye de korunur; cagiran reset/serbest birakma YAPMAZ)
//        false => hic dolum yok; eski davranis (kilit serbest, reset)
// ─────────────────────────────────────────────────────────────────────────────
inline bool settle_position_after_terminal_order(StrategyContext& ctx, const OrderResult& eff) noexcept
{
    if (eff.is_exit) return true;     // EXIT: pozisyon borsada hala acik, dokunma

    // GIRIS: dolan miktar = max(POLL'un bildirdigi executedQty, onceki PARTIAL'larla izlenen miktar)
    const double tracked = ctx.position.is_open() ? ctx.position.qty : 0.0;
    const double filled  = std::max(eff.filled_qty, tracked);
    if (!(filled > 0.0)) return false;

    const double px = (eff.avg_price > 0.0) ? eff.avg_price
                    : (ctx.position.is_open() ? ctx.position.entry_price : ctx.last_mid);

    if (!ctx.position.is_open()) {
        // Iptalden ONCE hic PARTIAL gorulmedi ama borsada dolum var (poll'lar arasi dolum): pozisyonu simdi kur.
        const auto   tp        = ctx.regime.trailing_params();
        const double stop_pct  = tp.activation_bps / 10000.0;
        const double trail_pct = tp.callback_bps   / 10000.0;
        const bool   is_buy    = ctx.pending_is_buy;
        ctx.position.side        = is_buy ? PositionSide::LONG : PositionSide::SHORT;
        ctx.position.entry_price = px;
        ctx.position.high_water  = px;
        ctx.position.stop_loss   = px * (is_buy ? (1.0 - stop_pct) : (1.0 + stop_pct));
        ctx.position.trail_delta = px * trail_pct;
        ctx.position.trail_stop  = is_buy ? (px - ctx.position.trail_delta) : (px + ctx.position.trail_delta);
    }
    if (filled > ctx.position.qty || ctx.position.notional <= 0.0) {
        const double old_notional = ctx.position.notional;
        ctx.position.qty      = filled;
        ctx.position.notional = px * filled;
        ctx.locked_in_orders  = std::max(0.0, ctx.locked_in_orders + (ctx.position.notional - old_notional));
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// [PATCH-P0-EXIT] EXIT kismi dolum + terminal iptal/red/sona erme uzlastirmasi.
// 10 birimlik EXIT'te 4 dolup kalan 6 iptal/EXPIRED olursa borsadaki gercek pozisyon 6'dir.
// Dolan kisim icin PnL gerceklesir, pozisyon kalan miktara iner; tamami dolduysa kapanir.
// filled = max(terminal sonucun filled_qty'si, FSM'in onceki PARTIAL'lardan izledigi miktar).
// Belirsiz hatalar (UNKNOWN) buraya GELMEZ: pozisyon oldugu gibi korunur, POLL reconcile eder.
// ─────────────────────────────────────────────────────────────────────────────
inline void reconcile_exit_partial(StrategyContext& ctx, double filled, double px) noexcept
{
    if (!ctx.position.is_open() || !(filled > 0.0)) return;
    const double q = ctx.position.qty;
    const double f = std::min(filled, q);
    const double exit_px = (px > 0.0) ? px : ctx.last_mid;
    const double pnl = ctx.position.is_long()
        ? (exit_px - ctx.position.entry_price) * f
        : (ctx.position.entry_price - exit_px) * f;
    ctx.current_capital += pnl;
    const double old_notional = ctx.position.notional;
    const double rem = q - f;
    if (rem <= q * 1e-9) {                       // fiilen tamami dolmus
        ctx.locked_in_orders = std::max(0.0, ctx.locked_in_orders - old_notional);
        ctx.recent_pnl.push(pnl);
        ks_on_trade_closed(ctx, pnl);
        ctx.position.reset();
        ctx.logger.warn("POSITION", "[EXIT-PARTIAL] terminal oncesi tam dolum tespit -> pozisyon kapandi pnl=", pnl);
        return;
    }
    ctx.position.qty      = rem;
    ctx.position.notional = ctx.position.entry_price * rem;
    ctx.locked_in_orders  = std::max(0.0, ctx.locked_in_orders - (old_notional - ctx.position.notional));
    ctx.logger.warn("POSITION", "[EXIT-PARTIAL] kismi EXIT sonrasi pozisyon kalan miktara indi: filled=", f,
        " kalan=", rem, " realized_pnl=", pnl);
}

// ─────────────────────────────────────────────────────────────────────────────
// [FIX-P35] C13: ADOPT edilen borsa pozisyonunu yerel kopyaya kur ve firewall'a KANITLI yayinla.
// UYARI: entry_price gercek giris fiyati DEGIL, ADOPT anindaki referans fiyattir (bid). PnL/stop buna gore.
// Alan atamalari FILLED giris dali ile ayni (stop/trail rejim parametreleriyle). locked_in_orders'a
// dokunulmaz (ADOPT'ta base envanteri quote kilidi degildir). Yalniz startup ana thread'i
// (strateji/order thread'leri baslamadan) cagirir -> fw_* yazari tek kalir.
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]] inline bool adopt_position(StrategyContext& ctx, PositionSide side, double qty, double ref_px) noexcept
{
    if (side == PositionSide::NONE) return false;
    if (!(qty > 0.0) || !(ref_px > 0.0) || !std::isfinite(qty) || !std::isfinite(ref_px)) return false;
    const double notional = ref_px * qty;
    if (!std::isfinite(notional)) return false;
    const auto   tp        = ctx.regime.trailing_params();
    const double stop_pct  = tp.activation_bps / 10000.0;
    const double trail_pct = tp.callback_bps   / 10000.0;
    const bool   is_buy    = (side == PositionSide::LONG);
    ctx.position.side        = side;
    ctx.position.entry_price = ref_px;
    ctx.position.qty         = qty;
    ctx.position.notional    = notional;
    ctx.position.stop_loss   = ref_px * (is_buy ? (1.0 - stop_pct) : (1.0 + stop_pct));
    ctx.position.trail_delta = ref_px * trail_pct;
    ctx.position.high_water  = ref_px;
    ctx.position.trail_stop  = is_buy ? (ref_px - ctx.position.trail_delta) : (ref_px + ctx.position.trail_delta);
    fw_publish_exposure(ctx, FwVerify::EVIDENCE);
    return true;
}

// [FIX-P35] ADOPT karar mantigi (saf, main.cpp ADOPT dali kullanir; testlenebilir).
enum class AdoptDecision : uint8_t { OK, REFUSE_FUTURES, REFUSE_LOCKED };
[[nodiscard]] inline AdoptDecision adopt_decision(bool futures, double /*base_free*/, double base_locked, double step) noexcept
{
    if (futures) return AdoptDecision::REFUSE_FUTURES;   // yon/miktar bilinmiyor -> fail-closed
    const bool locked = (step > 0.0) ? (base_locked >= step) : (base_locked > 0.0);
    if (locked || !std::isfinite(base_locked)) return AdoptDecision::REFUSE_LOCKED;
    return AdoptDecision::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// dispatch_order_result
// [V13-STRUCT]: fill_qty→filled_qty, fill_price→avg_price, fill_ns→ts_recv_ns
//               raw_msg→error_msg (FixedString), OrderResult::Status→OrderStatus
// [V13-MAP]:    order_map.find() → flat_order_map.get()
// V12 iş mantığı fix etiketleri ([FIX-OPT], [FIX-PFILL], vb.) korunuyor.
// ─────────────────────────────────────────────────────────────────────────────
// [FIX-UNKNOWN-SPAM] UNKNOWN = emir borsada olabilir, durumu bilinmiyor -> yeni emir KESIN yasak.
// Atama ks_latch() yolunu atlar: check_kill_switch() bayragi EXTERNAL_TRIP olarak yakalar ve
// kilit dosyasi + bildirim + CANCEL ALL oradan tetiklenir.
inline void trip_kill_switch_on_unknown(StrategyContext& ctx, const OrderResult& eff,
                                        const char* where) noexcept
{
    const bool was_active = ctx.kill_switch.load(std::memory_order_relaxed);
    ctx.kill_switch.store(true, std::memory_order_seq_cst);          // NEW ORDERS = OFF (tum thread'ler gorur)
    if (!was_active)
        ctx.logger.critical("UNKNOWN_STATE", "KILL_SWITCH ARMED (", where, ") local_id=", eff.local_order_id,
            " ex_id=", eff.exchange_order_id, " -- yeni emir engellendi, CANCEL ALL + reconcile bekleniyor");
    else
        ctx.logger.error("UNKNOWN_STATE", "UNKNOWN tekrar (", where, ") local_id=", eff.local_order_id,
            " -- kill_switch zaten aktif");
}

// [FIX-P36-C16] enforce_timeouts'tan cagrilir (strateji thread'i; slot tek yazar kurali korunur).
// active_client_oid terminal olmadigi icin dolu kalir -> build_poll_task UNKNOWN dali origClientOrderId POLL uretir.
// [FIX-P37-C19B] UNKNOWN'a giris kaydi: C19b -2013 cozum penceresi bu zamandan olculur. Seri sifirlanir.
inline void note_unknown_entry(StrategyContext& ctx, uint64_t local_id, uint64_t ts_ns) noexcept {
    ctx.unknown_key             = local_id;
    ctx.unknown_since_ns        = ts_ns ? ts_ns : ctx.current_time_ns();
    ctx.unknown_notfound_streak = 0;
}

inline void submitted_lost_to_unknown(StrategyContext& ctx, FlatOrderMap& map, OrderSlot& s) noexcept
{
    map.step(s, OrderState::UNKNOWN);   // last_update_ns guncellenir
    note_unknown_entry(ctx, s.order_id, ctx.current_time_ns());   // [FIX-P37-C19B]
    OrderResult r{};
    r.local_order_id    = s.order_id;
    r.order_id          = s.order_id;
    r.exchange_order_id = s.exchange_order_id;
    ctx.logger.critical("UNKNOWN_STATE", "SUBMITTED sonucu kayip (> ", SUBMITTED_LOST_TIMEOUT_NS / 1'000'000'000ULL,
                        " s) local_id=", s.order_id, " -> UNKNOWN; emir borsada olabilir");
    trip_kill_switch_on_unknown(ctx, r, "SUBMITTED_LOST");
    fw_publish_exposure(ctx);   // UNKNOWN slot var -> fw_pos_verified=false (kural 2)
}

inline void dispatch_order_result_impl(StrategyContext& ctx, const OrderResult& res)
{
    OrderResult eff = res;
    eff.status = normalize_terminal_fill_state(eff.status, eff.remaining_qty);
    // [PATCH-P5] EXIT kimligi ID araligindan turetilir: main.cpp POLL/CANCEL gorevleri is_exit set etmiyordu ->
    // bir EXIT'in POLL'dan gelen FILLED'i GIRIS dolumu gibi islenip pozisyonu KAPATMADAN uzerine yaziyordu.
    if (eff.local_order_id >= FSM_EXIT_BASE) eff.is_exit = true;
    // [PATCH-P0-EXIT] update() slotu tombstone'lamadan once FSM'in izledigi kumulatif dolum
    double exit_filled = eff.filled_qty, exit_avg = eff.avg_price;
    if (eff.is_exit) {
        const OrderSlot* ps = ctx.flat_order_map.find(eff.local_order_id ? eff.local_order_id : eff.order_id);
        if (ps) {
            exit_filled = std::max(exit_filled, ps->filled_qty);
            if (!(exit_avg > 0.0)) exit_avg = ps->avg_price;
        }
    }
    // [FIX-P37-C19B] UNKNOWN + borsa ID yok + POLL(origClientOrderId) "-2013 emir yok" cozumu.
    // Kosul: slot UNKNOWN, exchange_order_id==0, art arda >= MIN_STREAK "HTTP400_2013" ve
    // (bu sonucun ts_recv_ns - UNKNOWN'a giris) >= recvWindow + PAY. Saglanirsa sonuc REJECTED "NOTFOUND_RSLV"
    // olarak FSM'e verilir (UNKNOWN->REJECTED ALLOWED tablosunda izinli). Baska her sonuc seriyi sifirlar.
    // Kill switch'e dokunulmaz (kilitli kalir; manuel reset).
    {
        const uint64_t lid = eff.local_order_id ? eff.local_order_id : eff.order_id;
        const bool is_2013 = (eff.status == OrderStatus::ERROR) &&
                             (std::strcmp(eff.error_msg.c_str(), "HTTP400_2013") == 0);
        if (lid != 0 && lid == ctx.unknown_key) {
            const OrderSlot* us = ctx.flat_order_map.find(lid);
            const bool eligible = us && us->fsm_state == OrderState::UNKNOWN && us->exchange_order_id == 0 &&
                                  eff.exchange_order_id == 0;
            if (is_2013 && eligible) {
                ++ctx.unknown_notfound_streak;
                const uint64_t win_ns = ctx.cfg.recvWindow_ms * 1'000'000ULL + UNKNOWN_NOTFOUND_MARGIN_NS;
                const uint64_t ts     = eff.ts_recv_ns ? eff.ts_recv_ns : ctx.current_time_ns();
                const uint64_t since  = ctx.unknown_since_ns;
                const uint64_t elapsed = (since != 0 && ts > since) ? (ts - since) : 0;
                ctx.logger.warn("UNKNOWN_STATE", "UNKNOWN_NOTFOUND streak=", ctx.unknown_notfound_streak, "/",
                                UNKNOWN_NOTFOUND_MIN_STREAK, " elapsed_ms=", elapsed / 1'000'000ULL,
                                " window_ms=", win_ns / 1'000'000ULL, " local_id=", lid);   // [FIX-P37-C20]
                if (ctx.unknown_notfound_streak >= UNKNOWN_NOTFOUND_MIN_STREAK && since != 0 && elapsed >= win_ns) {
                    eff.status = OrderStatus::REJECTED;
                    constexpr char rm[] = "NOTFOUND_RSLV";
                    eff.error_msg.set(rm, static_cast<uint8_t>(sizeof(rm) - 1));
                    ctx.logger.critical("UNKNOWN_STATE", "NOTFOUND_RSLV: UNKNOWN emir borsada YOK (", ctx.unknown_notfound_streak,
                                        "x -2013, ", elapsed / 1'000'000ULL, " ms >= recvWindow+pay) -> REJECTED local_id=", lid,
                                        " -- kill switch KILITLI KALIR (manuel reset)");
                    ctx.unknown_key = 0; ctx.unknown_since_ns = 0; ctx.unknown_notfound_streak = 0;
                }
            } else {
                ctx.unknown_notfound_streak = 0;   // seri kesildi
            }
        }
    }
    // [PHASE2-FSM] Tek kapi: her sonuc once FSM'den gecer. Yalniz APPLIED yan etki uretir;
    // tekrar/eski/hayalet olaylar idempotent duser, gecersiz atlama emri UNKNOWN yapar.
    const FsmResult fsm = ctx.flat_order_map.update(eff);
    switch (fsm.verdict) {
    case FsmVerdict::GHOST:
        ctx.logger.warn("STATE_MISMATCH", "GHOST olay (slot yok/terminal): local_id=",
            eff.local_order_id, " ex_id=", eff.exchange_order_id, " status=", static_cast<int>(eff.status));
        return;
    case FsmVerdict::DUPLICATE:
        ctx.logger.debug("FSM", "DUPLICATE: local_id=", eff.local_order_id, " state=", order_state_name(fsm.from));
        return;
    case FsmVerdict::STALE:
        ctx.logger.warn("STATE_MISMATCH", "STALE olay yok sayildi: local_id=", eff.local_order_id,
            " state=", order_state_name(fsm.from), " event->", order_state_name(fsm.attempted));
        return;
    case FsmVerdict::INVALID:
        ctx.logger.error("UNKNOWN_STATE", "gecersiz gecis ", order_state_name(fsm.from), "->",
            order_state_name(fsm.attempted), " local_id=", eff.local_order_id,
            " -- emir UNKNOWN, POLL ile reconcile");
        if (fsm.from != OrderState::UNKNOWN)   // [FIX-P37-C19B] yeni UNKNOWN girisi
            note_unknown_entry(ctx, eff.local_order_id ? eff.local_order_id : eff.order_id, eff.ts_recv_ns);
        trip_kill_switch_on_unknown(ctx, eff, "INVALID_TRANSITION");
        return;   // awaiting_fill acik kalir; mevcut POLL dongusu UNKNOWN'dan cikarir
    case FsmVerdict::APPLIED:
        break;
    }
    if (fsm.to == OrderState::UNKNOWN) {   // belirsiz hata (timeout/HTTP/cancel hatasi): emir borsada olabilir
        ctx.logger.error("UNKNOWN_STATE", "belirsiz sonuc ", order_state_name(fsm.from), "->UNKNOWN local_id=",
            eff.local_order_id, " err=", eff.error_msg.c_str(), " -- pozisyon/kilit KORUNDU, reconcile bekleniyor");
        note_unknown_entry(ctx, eff.local_order_id ? eff.local_order_id : eff.order_id, eff.ts_recv_ns);   // [FIX-P37-C19B]
        trip_kill_switch_on_unknown(ctx, eff, "UNCERTAIN_RESULT");
        return;
    }

    switch (eff.status) {

    // ── FILLED ────────────────────────────────────────────────────────────────
    case OrderStatus::FILLED: {
        ctx.active_client_oid = FixedString<32>{};   // [FIX-P0-02] emir terminal
        ctx.awaiting_fill = false;
        const uint64_t fill_ns = eff.ts_recv_ns ? eff.ts_recv_ns : ctx.current_time_ns();

        if (eff.exchange_order_id != 0)
            ctx.active_order_id = eff.exchange_order_id;

        // [FIX-CLAUDE-TELEMETRY]: Telemetri fill öncesi res değerleriyle yapılır
        const double telemetry_price = (eff.avg_price  > 0.0) ? eff.avg_price  : ctx.last_mid;
        const double telemetry_qty   = (eff.filled_qty > 0.0) ? eff.filled_qty
                                     : (ctx.position.qty > 0.0 ? ctx.position.qty : 0.0);
        ctx.telemetry.record_order_filled(ctx.active_order_id,
            telemetry_price, telemetry_qty, fill_ns);
        ctx.adverse.record_fill_confirmed(fill_ns);

        // [V13-MAP]: lifecycle yerine flat_order_map'tan log
        {
            const OrderSlot& slot = ctx.flat_order_map.get(eff.local_order_id);
            if (slot.order_id == eff.local_order_id)
                ctx.logger.debug("LIFECYCLE",
                    "FILLED: exchange_id=", eff.exchange_order_id,
                    " local_id=", eff.local_order_id,
                    " fill_ns=", fill_ns);
        }

        // ── EXIT emri doldu: PnL hesapla, pozisyonu kapat ─────────────────────
        if (eff.is_exit && ctx.position.is_open()) {
            const double exit_price = (eff.avg_price > 0.0) ? eff.avg_price : ctx.last_mid;
            const double pnl = ctx.position.is_long()
                ? (exit_price - ctx.position.entry_price) * ctx.position.qty
                : (ctx.position.entry_price - exit_price) * ctx.position.qty;
            ctx.current_capital  += pnl;
            ctx.locked_in_orders  = std::max(0.0, ctx.locked_in_orders - ctx.position.notional);
            // [V13-PROFIT-ENGINE] PnL'i Kelly buffer'ına kaydet
            ctx.recent_pnl.push(pnl);
            ks_on_trade_closed(ctx, pnl);   // [PHASE3-KS]
            ctx.logger.debug("PNL",
                "EXIT FILL -- ", (ctx.position.is_long() ? "LONG" : "SHORT"),
                " entry=",  ctx.position.entry_price,
                " exit=",   exit_price,
                " qty=",    ctx.position.qty,
                " pnl=",    pnl,
                " capital_after=", ctx.current_capital);
            ctx.position.reset();
            ctx.active_order_id = 0;
            ctx.active_client_oid = FixedString<32>{};   // [FIX-P0-02]
            break;
        }

        // ── Giriş emri doldu ──────────────────────────────────────────────────
        if (!eff.is_exit) {
            const double fill_price = (eff.avg_price  > 0.0) ? eff.avg_price  : ctx.last_mid;
            const double fill_qty   = (eff.filled_qty > 0.0) ? eff.filled_qty
                                    : (ctx.position.qty > 0.0 ? ctx.position.qty : 0.0);

            if (fill_qty <= 0.0) {
                ctx.logger.error("FILLED",
                    "[V13] fill_qty sifir -- pozisyon guncelleme atlaniyor");
                ctx.active_order_id = 0;
                ctx.active_client_oid = FixedString<32>{};   // [FIX-P0-02]
                break;
            }

            // [MVP-MAKER] bkz. maybe_send_order() -- ayni ATR-turevi trailing_params() deseni
            const auto  tp        = ctx.regime.trailing_params();
            const double stop_pct  = tp.activation_bps / 10000.0;
            const double trail_pct = tp.callback_bps   / 10000.0;
            const bool is_buy = eff.is_buy;

            if (!ctx.position.is_open()) {
                // [FIX-OPT] + [FIX-CLAUDE-CAPITAL]: İlk dolum — pozisyon burada kurulur
                ctx.position.side        = is_buy ? PositionSide::LONG : PositionSide::SHORT;
                ctx.position.entry_price = fill_price;
                ctx.position.qty         = fill_qty;
                ctx.position.notional    = fill_price * fill_qty;
                ctx.position.stop_loss   = fill_price * (is_buy ? (1.0 - stop_pct) : (1.0 + stop_pct));
                ctx.position.trail_delta = fill_price * trail_pct;
                ctx.position.high_water  = fill_price;
                ctx.position.trail_stop  = is_buy
                    ? (fill_price - ctx.position.trail_delta)
                    : (fill_price + ctx.position.trail_delta);
                ctx.locked_in_orders += ctx.position.notional;

                ctx.logger.debug("POSITION",
                    "[FIX-OPT][FIX-CLAUDE-CAPITAL] FILLED pozisyon kuruldu: ",
                    (is_buy ? "LONG" : "SHORT"),
                    " entry=", fill_price, " qty=", fill_qty,
                    " notional=", ctx.position.notional,
                    " locked_in_orders=", ctx.locked_in_orders);
            } else {
                // [FIX-CLAUDE-PFILL-FINAL]: PARTIALLY_FILLED zincirinin son fill'i
                const double old_notional = ctx.position.notional;
                const double new_notional = fill_price * fill_qty;
                ctx.position.entry_price  = fill_price;
                ctx.position.qty          = fill_qty;
                ctx.position.notional     = new_notional;
                ctx.locked_in_orders = std::max(0.0,
                    ctx.locked_in_orders + (new_notional - old_notional));

                ctx.logger.debug("POSITION",
                    "[FIX-CLAUDE-PFILL-FINAL] Son dolum guncellendi: ",
                    " entry=", fill_price, " qty=", fill_qty,
                    " notional=", new_notional,
                    " locked_in_orders=", ctx.locked_in_orders);
            }
        }

        ctx.logger.debug("POLL", "Emir DOLDU: exchange_id=", eff.exchange_order_id,
            " local_id=", eff.local_order_id);
        break;
    }

    // ── [FIX-PFILL] PARTIALLY_FILLED ─────────────────────────────────────────
    case OrderStatus::PARTIALLY_FILLED: {
        if (eff.is_exit) {   // [PATCH-P3] EXIT kismi dolumu: pozisyon FILLED'a kadar ACIK ve ayni boyutta kalir (PnL tam miktar/avg ile)
            ctx.logger.debug("POLL", "EXIT PARTIALLY_FILLED: filled=", res.filled_qty,
                " -- pozisyon degismedi, FILLED bekleniyor");
            break;
        }
        if (res.filled_qty > 0.0) {
            const double actual_price = (res.avg_price  > 0.0) ? res.avg_price  : ctx.last_mid;
            const double new_qty      = res.filled_qty;
            const double new_notional = actual_price * new_qty;

            if (!ctx.position.is_open()) {
                // [MVP-MAKER] bkz. maybe_send_order() -- ayni ATR-turevi trailing_params() deseni
                const auto  tp        = ctx.regime.trailing_params();
                const double stop_pct  = tp.activation_bps / 10000.0;
                const double trail_pct = tp.callback_bps   / 10000.0;
                const bool is_buy = res.is_buy;

                ctx.position.side        = is_buy ? PositionSide::LONG : PositionSide::SHORT;
                ctx.position.entry_price = actual_price;
                ctx.position.high_water  = actual_price;
                ctx.position.stop_loss   = actual_price * (is_buy ? (1.0 - stop_pct) : (1.0 + stop_pct));
                ctx.position.trail_delta = actual_price * trail_pct;
                ctx.position.trail_stop  = is_buy
                    ? (actual_price - ctx.position.trail_delta)
                    : (actual_price + ctx.position.trail_delta);
            }

            const double old_notional = ctx.position.notional;
            ctx.position.qty          = new_qty;
            ctx.position.notional     = new_notional;
            ctx.locked_in_orders = std::max(0.0,
                ctx.locked_in_orders + (new_notional - old_notional));

            ctx.logger.debug("POLL",
                "PARTIALLY_FILLED: fill_qty=", new_qty,
                " fill_price=", actual_price,
                " notional=", new_notional,
                " locked_in_orders=", ctx.locked_in_orders);
        }
        // awaiting_fill = true → ana döngü 1s sonra tekrar POLL gönderir
        break;
    }

    // ── PENDING ───────────────────────────────────────────────────────────────
    case OrderStatus::PENDING:
        if (res.exchange_order_id != 0)
            ctx.active_order_id = res.exchange_order_id;
        ctx.logger.debug("POLL", "Emir beklemede (NEW) -- id=", ctx.active_order_id);
        break;

    // ── CANCELED / REJECTED / EXPIRED ─────────────────────────────────────────
    case OrderStatus::CANCELED:
    case OrderStatus::REJECTED:
    case OrderStatus::EXPIRED: {
        ctx.logger.warn("POLL",
            "Emir IPTAL/RED/SURESI_DOLDU: id=", res.order_id,
            eff.is_exit ? " (EXIT)" : " (ENTRY)");
        const uint64_t now_ns = ctx.current_time_ns();
        if (eff.status == OrderStatus::CANCELED) {
            ctx.telemetry.record_order_canceled(res.order_id, now_ns);
        } else {                                   // [PHASE3-KS] RED/SURE_DOLDU iptal orani sayacini kirletmesin
            ctx.telemetry.record_order_rejected(res.order_id, now_ns);
            ks_on_reject(ctx);
        }
        auto cr = ctx.adverse.record_cancel_with_latency(
            now_ns, ctx.position.entry_price, true);
        if (cr.pre_market)
            ctx.logger.error("ADVERSE",
                "PRE-MARKET CANCEL (<1ms) lat_us=", cr.latency_us);

        // [FIX-AMNESIA-2] Dolum kaniti, muhasebeden ONCE yakalanir (eff, FSM'in izledigi miktar, acik pozisyon).
        const bool fill_evidence = (eff.filled_qty > 0.0) || (exit_filled > 0.0) || ctx.position.is_open();

        if (eff.is_exit) reconcile_exit_partial(ctx, exit_filled, exit_avg);   // [PATCH-P0-EXIT] kalan pozisyona iner / tam doluysa kapanir
        const bool preserved = settle_position_after_terminal_order(ctx, eff); // dolan kisim KALICI pozisyon

        if (preserved || fill_evidence) {
            // reset() YOK. SERMAYE INVARIANTI ([FIX-CLAUDE-CAPITAL]): canli yolda locked_in_orders YALNIZ
            // dolu maliyettir; dolmayan kisim (pending_entry_notional - position.notional) HIC kilitlenmedi.
            // Onu locked_in_orders'tan DUSMEK dolu pozisyonun kilidini eksiltir (cift serbest birakma).
            // Dolmayan kisim yalniz asagida pending_entry_notional=0 ile "serbest" kalir.
            const double unfilled_never_locked = eff.is_exit
                ? 0.0
                : std::max(0.0, ctx.pending_entry_notional - ctx.position.notional);

            // Sapma dedektoru (degistirmez, yalniz bildirir): tek pozisyonlu ctx'te kilit == dolu notional.
            const double expect_locked = ctx.position.is_open() ? ctx.position.notional : 0.0;
            if (std::fabs(ctx.locked_in_orders - expect_locked) > 1e-6 * std::max(1.0, expect_locked))
                ctx.logger.error("CAPITAL", "[LEDGER-DRIFT] locked_in_orders=", ctx.locked_in_orders,
                    " beklenen=", expect_locked, " -- muhasebe sapmasi, manuel inceleme");

            ctx.logger.warn("POSITION", "[AMNESIA-FIX] emir sonlandi, pozisyon KORUNDU: ",
                ctx.position.is_open() ? (ctx.position.is_long() ? "LONG" : "SHORT") : "NONE",
                " qty=", ctx.position.qty, " entry=", ctx.position.entry_price,
                " notional=", ctx.position.notional, " locked_in_orders=", ctx.locked_in_orders,
                " unfilled_never_locked=", unfilled_never_locked);
            // Reddedilen/suresi dolan EXIT: pozisyon HALA aciksa yeniden gonderim firtinasi olmasin
            if (eff.is_exit && eff.status != OrderStatus::CANCELED && ctx.position.is_open())
                ctx.exit_retry_not_before_ns = ctx.current_time_ns() + 1'000'000'000ULL;
        } else {
            const double released = ctx.position.notional;      // dolum kaniti yok: yalniz burada kilit cozulur
            ctx.locked_in_orders  = std::max(0.0, ctx.locked_in_orders - released);
            ctx.position.reset();
            ctx.logger.info("CAPITAL", "IPTAL/RED (dolum yok) -- kilit serbest: notional=", released,
                " locked_in_orders=", ctx.locked_in_orders);
        }
        ctx.pending_entry_notional = 0.0;
        ctx.awaiting_fill   = false;
        ctx.active_order_id = 0;
        ctx.active_client_oid = FixedString<32>{};   // [FIX-P0-02]
        break;
    }

    // ── ERROR ─────────────────────────────────────────────────────────────────
    case OrderStatus::ERROR:
        if (!res.error_msg.empty())
            ctx.logger.error("ORDER_RESULT", "Hata: ", res.error_msg.c_str());
        ctx.telemetry.record_order_rejected(res.order_id, ctx.current_time_ns());   // [PHASE3-KS] (belirsiz hatalar buraya inmez)
        ks_on_reject(ctx);
        if (eff.is_exit) reconcile_exit_partial(ctx, exit_filled, exit_avg);   // [PATCH-P0-EXIT] (kesin red: belirsizler UNKNOWN'da erken doner)
        // [HOTFIX-AMNESIA] ERROR de pozisyonu kosulsuz silmez (reddedilen EXIT / kismen dolmus giris)
        if (settle_position_after_terminal_order(ctx, eff)) {
            ctx.logger.warn("POSITION", "[AMNESIA-FIX] hata sonrasi pozisyon KORUNDU: qty=", ctx.position.qty,
                " entry=", ctx.position.entry_price, " locked_in_orders=", ctx.locked_in_orders);
            if (eff.is_exit)
                ctx.exit_retry_not_before_ns = ctx.current_time_ns() + 1'000'000'000ULL;
        } else {
            const double released = ctx.position.notional;
            ctx.locked_in_orders = std::max(0.0, ctx.locked_in_orders - released);
            ctx.position.reset();
        }
        ctx.awaiting_fill   = false;
        ctx.active_order_id = 0;
        ctx.active_client_oid = FixedString<32>{};   // [FIX-P0-02]
        ctx.cooldown_until_ns.store(
            ctx.current_time_ns() + 200'000'000ULL,
            std::memory_order_relaxed);
        break;

    default:
        break;
    }
}

// [FIX-P0-03] Pozisyon her sonucla degisebilir: sonuc islendikten sonra kopya yayinlanir.
// EVIDENCE (borsa onayli kanit) yalniz sonuc FSM'de var olan bir slota aitse (hayalet/slotsuz olay kanit degildir):
//   - FILLED / PARTIALLY_FILLED / CANCELED / EXPIRED, veya
//   - NEW / PENDING / REJECTED ve exchange_order_id != 0 (borsa yaniti), veya
//   - terminal sonucta filled_qty > 0 (kismi dolum kaniti; settle_position_after_terminal_order pozisyonu korur).
// Yerel sonuc (FW_REJECT, gate redleri: REJECTED + exchange_order_id==0) ve ERROR: kanit degil -> KEEP.
// Nihai verified, fw_publish_exposure kurali ile belirlenir (UNKNOWN slot / kapali pozisyon -> false).
inline void dispatch_order_result(StrategyContext& ctx, const OrderResult& res)
{
    const uint64_t key = res.local_order_id ? res.local_order_id : res.order_id;
    const bool had_slot = (ctx.flat_order_map.find(key) != nullptr);   // impl slotu tombstone'layabilir: once bak
    dispatch_order_result_impl(ctx, res);
    bool evidence = false;
    switch (res.status) {
    case OrderStatus::FILLED: case OrderStatus::PARTIALLY_FILLED:
    case OrderStatus::CANCELED: case OrderStatus::EXPIRED:
        evidence = true; break;
    case OrderStatus::NEW: case OrderStatus::PENDING: case OrderStatus::REJECTED:
        evidence = (res.exchange_order_id != 0); break;
    default: break;
    }
    if (res.filled_qty > 0.0 && (res.status == OrderStatus::REJECTED || res.status == OrderStatus::ERROR))
        evidence = true;   // terminal + dolum kaniti
    fw_publish_exposure(ctx, (had_slot && evidence) ? FwVerify::EVIDENCE : FwVerify::KEEP);
}

// ─────────────────────────────────────────────────────────────────────────────
// [PATCH-P7] drain_orders_on_shutdown -- while(active_orders>0) kapanis dongusu.
// order_thread HALA CANLI iken cagrilmali (CANCEL/POLL task'larini o isler). Yeni giris engellenir
// (trading_paused). Her ~250ms: ks_cancel_all (EXIT hariz bekleyen girisleri iptal eder) + POLL.
// Dongu SINIRLIDIR (deadline): sonsuz bekleme Ctrl+C'yi asardi ([FIX-SHUTDOWN-DEADLOCK] ruhu).
// Haritayi ZORLA temizlemez: sure dolarsa kalan sayiyi dondurur -> cagiran CRITICAL loglar (borsada elle kontrol).
// ─────────────────────────────────────────────────────────────────────────────
inline size_t drain_orders_on_shutdown(StrategyContext& ctx, uint32_t timeout_ms) noexcept
{
    using clk = std::chrono::steady_clock;
    ctx.trading_paused = true;                                   // yeni GIRIS yok
    const auto deadline = clk::now() + std::chrono::milliseconds(timeout_ms);
    auto next_service   = clk::now();                            // ilk turda hemen
    size_t   live  = ctx.flat_order_map.active_count();
    uint32_t spins = 0;                                          // adaptif bekleme durumu

    while (live > 0 && clk::now() < deadline) {
        bool progressed = false;
        {
            OrderResult r{};
            while (ctx.order_channel.pop_result(r)) { dispatch_order_result(ctx, r); progressed = true; }
        }
        live = ctx.flat_order_map.active_count();
        if (live == 0) break;

        if (clk::now() >= next_service) {
            next_service = clk::now() + std::chrono::milliseconds(250);
            (void)ks_cancel_all(ctx);                            // girisleri iptal et (FSM: REQUEST->PENDING->CANCELED)
            if (!ctx.cfg.dry_run && ctx.awaiting_fill && ctx.fsm_key != 0 &&
                !ctx.poll_in_flight.load(std::memory_order_acquire)) {
                OrderTask pt{};                                  // EXIT + SUBMITTED/UNKNOWN emirleri POLL ile reconcile
                // [FIX-P0-02] kimlik slottan (borsa ID) veya UNKNOWN+client_oid; aksi halde POLL uretilmez
                if (build_poll_task(ctx, pt) && ctx.order_channel.push_task(pt))
                    ctx.poll_in_flight.store(true, std::memory_order_release);
            }
            progressed = true;
        }
        if (progressed) spins = 0;
        kusak::spin::adaptive_spin(spins);                       // [FIX-SHUTDOWN-DEADLOCK] OS'a donmeden, SINIRLI bekleme
    }
    return ctx.flat_order_map.active_count();
}

// ─────────────────────────────────────────────────────────────────────────────
// process_agg_trade (V12'den değişmedi)
// ─────────────────────────────────────────────────────────────────────────────
inline void process_agg_trade(StrategyContext& ctx, const kusak::ws::AggTrade& t)
{
    const uint64_t ts_us = t.ts_exchange / 1000;
    // [ARCH-V2-FIX] update_trade artık (int64_t qty_scaled, int64_t price_scaled,...) alıyor.
    //               t.qty/t.price hâlâ double (feed) -- ölçeklemeden geçilirse double->int64_t
    //               daraltıcı dönüşüm DERLEME HATASI VERMEDEN kesirli kısmı atardı (0.02 -> 0),
    //               ve update_trade'in qty_scaled<=0 erken dönüşü yüzünden neredeyse tüm
    //               kripto trade'leri VPIN hesabına hiç girmezdi. En kritik bulgu buydu.
    const double real_price = static_cast<double>(t.price) / 1e8;
    static_assert(kusak::research::VPIN_SCALE * 100 == 100'000'000LL, "qty x1e8 -> VPIN x1e6");
    const int64_t qty_scaled = t.qty / 100;   // 1e8 -> 1e6, pure integer
    const int64_t price_scaled = static_cast<int64_t>(real_price * static_cast<double>(kusak::research::VPIN_SCALE));
    const auto vs = ctx.vpin.update_trade(qty_scaled, price_scaled, t.buyer_is_maker, ts_us);

    // [PHASE5] Kova hedefi belirlendiginde BIR KEZ logla (kalibrasyon bitti / sabit hedef kullanimda)
    if (UNLIKELY(!ctx.vpin_calib_logged) && ctx.vpin.calibrated()) {
        ctx.vpin_calib_logged = true;
        const double S    = static_cast<double>(kusak::research::VPIN_SCALE);
        const double eff  = static_cast<double>(ctx.vpin.effective_bucket_volume_scaled()) / S;
        if (ctx.cfg.vpin_bucket_volume > 0.0) {
            ctx.logger.info("VPIN_CALIB", "SABIT kova hacmi=", eff, " birim (vpin_bucket_volume)");
        } else {
            const double mean = static_cast<double>(ctx.vpin.calib_mean_qty_scaled()) / S;
            ctx.logger.info("VPIN_CALIB", "OTOMATIK kova hacmi=", eff, " birim | ort_islem=", mean,
                " | T_cfg=", ctx.cfg.vpin_bucket_mult, " T_etkin=", (mean > 0.0 ? eff / mean : 0.0),
                " | kalibrasyon_islem=", ctx.cfg.vpin_calib_trades);
            if (ctx.cfg.vpin_bucket_mult < 10.0)
                ctx.logger.warn("VPIN_CALIB", "vpin_bucket_mult=", ctx.cfg.vpin_bucket_mult,
                    " < 10: islem/kova alt siniri 10'a yukseltildi (kova=1 islem olmasin)");
        }
    }

    kusak::research::RegimeSnapshot snap;
    snap.mid_price_fp = static_cast<int64_t>(
                        real_price * static_cast<double>(
                            kusak::research::kPriceScale));
    snap.ts_us        = ts_us;
    ctx.regime.update(snap);
    if (ctx.regime.regime_changed())
        ctx.param_adapter.set_regime(ctx.regime.current_regime());

    if (vs.alarm >= kusak::research::VPINAlarm::HIGH)
        ctx.telemetry.record_vpin_alarm(vs.vpin, vs.dynamic_threshold, ts_us * 1000ULL);

    // [FIX-SCALE-3] Onceden ham (x1e8, olceklenmemis) t.price gonderiliyordu --
    // real_price zaten yukarida dogru hesaplanmisti, sadece kullanilmamisti.
    ctx.adverse.update_mid(ts_us, real_price);

    // [V13-PROFIT-ENGINE] Trade Direction Kaydı
    int dir = t.buyer_is_maker ? -1 : 1;   // true = pasif alıcı → satış agresif
    ctx.trade_direction.push(dir);
}

// ─────────────────────────────────────────────────────────────────────────────
// [P1-OBI-TRACK] Tarafsız OBI örnekleme. process_book_ticker'ın başında, pozisyon /
// cooldown / VPIN / risk / kill-switch filtrelerinden ÖNCE çağrılır -> "işlem yapılabilir"
// anlarla sınırlı seçim yanlılığı yok. Maliyet: 1 bölme + 2 karşılaştırma (+ nadiren add).
// ─────────────────────────────────────────────────────────────────────────────
inline void track_obi_signal(StrategyContext& ctx, uint64_t tick_ns) noexcept
{
    const double bid = ctx.last_bid, ask = ctx.last_ask, mid = ctx.last_mid;
    if (bid <= 0.0 || ask <= 0.0 || mid <= 0.0 || ask < bid) return;

    const double obi     = compute_top_of_book_obi(ctx);
    const double obi_thr = ctx.cfg.obi_threshold;
    int side;
    if      (obi >  obi_thr) side = +1;
    else if (obi < -obi_thr) side = -1;
    else return;

    if (!ctx.obi_tracker.want(tick_ns, side)) return;   // ucuz throttle ön-kapısı (wall saat okumadan önce)
    const uint64_t wall_us = (ctx.cfg.dry_run && ctx.mock_time_ns != 0)
        ? tick_ns / 1000ULL                              // replay: simülasyon saati
        : kusak::telemetry::wall_clock_us();
    ctx.obi_tracker.add(tick_ns, wall_us, bid, ask, mid, obi, obi_thr, side,
                        (ask - bid) / mid * 10000.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// process_book_ticker
// [FIX-STALE][FIX-CLAUDE-GHOST] V12'den taşındı — değişmedi
// ─────────────────────────────────────────────────────────────────────────────
inline void process_book_ticker(StrategyContext& ctx, const kusak::ws::BookTicker& bk)
{
    // [V13-PROFIT-ENGINE] Momentum Exit — kitap güncellemesi öncesi kontrol
    dynamic_momentum_exit(ctx);

    // [FIX-SCALE-1] bk.best_bid/best_ask, WebSocket parser'dan int64_t,
    // x1e8 sabit-nokta (fixed-point) olarak gelir (orn. gercek fiyat 1.29 ->
    // 129000000). ctx.last_bid/ask/mid ise GERCEK ondalikli fiyat bekler.
    // Bolme UNUTULURSA order_price 100 milyon kat buyuk cikar, kelly_qty
    // hesaplamasi sifira yuvarlanir ve emirler sessizce iptal olur.
    ctx.last_bid = static_cast<double>(bk.best_bid) / 1e8;
    ctx.last_ask = static_cast<double>(bk.best_ask) / 1e8;
    ctx.last_mid = (ctx.last_bid + ctx.last_ask) * 0.5;

    // [V13-PROFIT-ENGINE] BookTicker miktarlarını güncelle
    ctx.last_bid_qty = bk.best_bid_qty;
    ctx.last_ask_qty = bk.best_ask_qty;

    // [FIX-STALE]: [V13-CLOCK] mono_ns
    const uint64_t tick_ns = ctx.current_time_ns();
    ctx.last_ticker_ns.store(tick_ns, std::memory_order_relaxed);
    ctx.obi_tracker.poll(tick_ns, ctx.last_mid, ctx.telemetry);   // [P1-OBI-TRACK] önce eski gözlemleri ilerlet
    track_obi_signal(ctx, tick_ns);                               // [P1-OBI-TRACK] sonra yeni sinyal (filtresiz)

    if (ctx.vpin_force_exit.exchange(false, std::memory_order_acq_rel)) {
        if (ctx.position.is_open() && !ctx.awaiting_fill) {
            const double vpin_exit_price = ctx.position.is_long()
                ? ctx.last_bid : ctx.last_ask;
            send_exit_order(ctx, vpin_exit_price, "VPIN stress acil cikis");
            return;
        }
    }

    // [FIX-CLAUDE-GHOST]: !ctx.awaiting_fill guard
    if (ctx.position.is_open() && !ctx.awaiting_fill) {
        const double exit_ref_price = ctx.position.is_long()
            ? ctx.last_bid : ctx.last_ask;
        ctx.position.update_trail(exit_ref_price);
        if (ctx.position.should_exit(exit_ref_price)) {
            send_exit_order(ctx, exit_ref_price, "Stop/TrailingStop tetiklendi");
            return;
        }
    }

    static uint64_t cnt = 0;
    if ((++cnt & 0xF) == 0) {
        if (!ctx.kill_switch.load(std::memory_order_acquire) && check_kill_switch(ctx))
            ctx.kill_switch.store(true, std::memory_order_release);
    }

    if (!ctx.kill_switch.load(std::memory_order_acquire))
        maybe_send_order(ctx);
}