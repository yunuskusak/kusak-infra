// FILE: include/order_guard.hpp
// [PHASE0-GUARD] Bagimliliksiz (STL disi hicbir include yok), tahsissiz, noexcept guvenlik mantigi.
// strategy_core.hpp / main.cpp bu fonksiyonlari CAGIRIR; mantik burada oldugu icin borsaya/ag'a
// dokunmadan birim testle kanitlanabilir (bkz. tools/order_guard_test.cpp).
#pragma once
#ifndef KUSAK_ORDER_GUARD_HPP
#define KUSAK_ORDER_GUARD_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace kusak::guard {

// ───────────────────────── 1) Miktar hesabi: ASLA cap'i asmayan qty ─────────────────────────
// stepSize'in tam kati, <= x. FP gurultusu (2.9999999 / 3.0000001) icin once +1e-9 tolerans, sonra
// carpim dogrulamasi ile asagi duzeltilir: sonuc DAIMA <= x.
[[nodiscard]] inline double floor_to_step(double x, double step) noexcept {
    if (!(x > 0.0) || !std::isfinite(x)) return 0.0;
    if (!(step > 1e-12)) return std::floor(x);
    int64_t k = static_cast<int64_t>(std::floor(x / step + 1e-9));
    while (k > 0 && static_cast<double>(k) * step > x) --k;
    return static_cast<double>(k) * step;
}

// En buyuk step-kati qty: qty <= desired  VE  qty*price <= cap_usd (kesin, tolerans YOK).
[[nodiscard]] inline double cap_qty(double price, double desired, double cap_usd, double step) noexcept {
    if (!(price > 0.0) || !std::isfinite(price) || !(cap_usd > 0.0) || !std::isfinite(cap_usd)) return 0.0;
    double q = floor_to_step(std::min(desired, cap_usd / price), step);
    const double s = (step > 1e-12) ? step : 1.0;
    for (int i = 0; i < 4 && q > 0.0 && q * price > cap_usd; ++i) q -= s;   // yuvarlama artigi: bir adim geri
    return (q > 0.0 && q * price <= cap_usd) ? q : 0.0;
}

// ───────────────────────── 2) Son cikis kapisi: durumsuz + durumlu kontroller ─────────────────────────
struct Filters { double step = 0, tick = 0, min_qty = 0, max_qty = 1e18, min_notional = 0; };

enum class Verdict : uint8_t {
    PASS = 0, BAD_INPUT, SYMBOL_MISMATCH, QTY_STEP, PRICE_TICK, QTY_MIN, QTY_MAX,
    NOTIONAL_MIN, ORDER_CAP, POSITION_CAP, EXPOSURE_CAP, INSUFFICIENT_BALANCE, KILL_SWITCH
};

struct Order  { const char* symbol; bool is_buy; double qty; double price; };
struct Limits {
    const char* symbol;          // cfg.symbol -- emir sembolu ile birebir ayni olmali
    Filters     f;               // borsa filtreleri (ExchangeInfo)
    double      pos_cap_usd;     // max_position_usd (0/NaN => fail-closed)
    double      order_cap_usd;   // tek emir tavani (0 => pos_cap)
    double      other_exposure_usd;  // bu emir HARIC: dolu pozisyon (mark) + canli diger emirler
    double      free_quote_usd;  // serbest quote bakiyesi (BUY icin)
    bool        kill_switch;
};

[[nodiscard]] inline bool is_multiple(double v, double unit) noexcept {
    if (!(unit > 1e-12)) return true;
    const double r = std::round(v / unit) * unit;
    return std::fabs(r - v) <= unit * 1e-6;
}

[[nodiscard]] inline Verdict final_check(const Order& o, const Limits& L) noexcept {
    if (L.kill_switch) return Verdict::KILL_SWITCH;
    if (!(o.qty > 0.0) || !(o.price > 0.0) || !std::isfinite(o.qty) || !std::isfinite(o.price))
        return Verdict::BAD_INPUT;
    const double notional = o.qty * o.price;
    if (!std::isfinite(notional)) return Verdict::BAD_INPUT;
    if (!o.symbol || !L.symbol || std::strcmp(o.symbol, L.symbol) != 0) return Verdict::SYMBOL_MISMATCH;

    if (!is_multiple(o.qty, L.f.step))   return Verdict::QTY_STEP;
    if (!is_multiple(o.price, L.f.tick)) return Verdict::PRICE_TICK;
    if (o.qty < L.f.min_qty)             return Verdict::QTY_MIN;
    if (o.qty > L.f.max_qty)             return Verdict::QTY_MAX;
    if (notional < L.f.min_notional)     return Verdict::NOTIONAL_MIN;

    // Tolerans YOK: cap_qty() kesin <= uretir; burada 1 ulp bile asan emir ihlaldir.
    if (!(L.pos_cap_usd > 0.0) || !std::isfinite(L.pos_cap_usd) || notional > L.pos_cap_usd)
        return Verdict::POSITION_CAP;                                   // limit yoksa fail-closed
    const double oc = (L.order_cap_usd > 0.0) ? L.order_cap_usd : L.pos_cap_usd;
    if (notional > oc) return Verdict::ORDER_CAP;
    if (!(L.other_exposure_usd >= 0.0) || L.other_exposure_usd + notional > L.pos_cap_usd)
        return Verdict::EXPOSURE_CAP;                                   // acik pozisyon/emir + bu emir
    if (o.is_buy && notional > L.free_quote_usd) return Verdict::INSUFFICIENT_BALANCE;
    return Verdict::PASS;
}

// ───────────────────────── 3) Toplam maruziyet ─────────────────────────
// Canli yolda locked_in_orders YALNIZ dolu pozisyonun maliyetidir (gonderimde eklenmez,
// [FIX-CLAUDE-CAPITAL]); pos_usd ise ayni pozisyonun mark degeridir. Bu ikisini TOPLAMAK pozisyonu
// iki kez sayar; max() ise henuz dolmamis canli emri GORMEZ. Dogru toplam: pozisyon + bekleyen emir.
[[nodiscard]] inline double pending_remainder(bool entry_in_flight, double pending_notional,
                                              double filled_notional) noexcept {
    if (!entry_in_flight || !(pending_notional > 0.0)) return 0.0;
    return std::max(0.0, pending_notional - std::max(0.0, filled_notional));
}
[[nodiscard]] inline double total_exposure(double pos_usd, double pending_usd) noexcept {
    return std::max(0.0, pos_usd) + std::max(0.0, pending_usd);
}

// ───────────────────────── 4a) Rate-limit / IP ban kapisi ─────────────────────────
// 429 = yavasla, 418 = IP BAN. Ban sirasinda istek atmak banı uzatir -> tum istemciler (IO + cold)
// istek ONCE allow() sormali. Zaman disaridan verilir (test edilebilir).
struct RateGate {
    int64_t  ban_until_ms  = 0;
    uint32_t strikes       = 0;
    int      used_weight   = 0;
    int64_t  weight_ts_ms  = 0;
    int      weight_limit  = 1200;
    double   soft_ratio    = 0.80;
    static constexpr int64_t MIN_BAN_418_MS = 120'000;   // Binance 418 en az 2 dk
    static constexpr int64_t MAX_BACKOFF_MS = 60'000;

    void on_response(int status, int retry_after_s, int used_w, int64_t now_ms) noexcept {
        if (used_w >= 0) { used_weight = used_w; weight_ts_ms = now_ms; }
        if (status == 418) {
            ++strikes;
            int64_t w = std::max<int64_t>(MIN_BAN_418_MS, retry_after_s > 0 ? retry_after_s * 1000LL : 0);
            ban_until_ms = std::max(ban_until_ms, now_ms + w);
        } else if (status == 429) {
            const int64_t exp = std::min<int64_t>(MAX_BACKOFF_MS, 2000LL << std::min<uint32_t>(strikes, 5));
            ++strikes;
            const int64_t w = retry_after_s > 0 ? retry_after_s * 1000LL : exp;
            ban_until_ms = std::max(ban_until_ms, now_ms + w);
        } else if (status >= 200 && status < 300) {
            strikes = 0;
        }
    }
    [[nodiscard]] bool banned(int64_t now_ms) const noexcept { return now_ms < ban_until_ms; }
    // Emir endpoint'i yalniz ban ile engellenir; okuma (poll/account) ayrica agirlik tavaninda durur.
    [[nodiscard]] bool allow(int64_t now_ms, bool is_order) const noexcept {
        if (banned(now_ms)) return false;
        if (is_order) return true;
        const int w = (now_ms - weight_ts_ms > 60'000) ? 0 : used_weight;
        return w < static_cast<int>(weight_limit * soft_ratio);
    }
    [[nodiscard]] int64_t wait_ms(int64_t now_ms) const noexcept { return banned(now_ms) ? ban_until_ms - now_ms : 0; }
};

// ───────────────────────── 4b) Soguk baslangic: STATE-BAZLI KAPI ─────────────────────────
// Kapi sirasi SABIT ve atlanamaz:  LOCK -> ORDER GATE (open_orders==0 KANITLI) -> BALANCE GATE.
// Dogrulanmamis (sorgu basarisiz / bilinmiyor) her girdi FAIL-CLOSED'dur; varsayilanlar bunu zorlar.
enum class StartAction : uint8_t {
    CLEAN_START = 0, REFUSE_LOCKED, CANCEL_OPEN_ORDERS_THEN_RECHECK, ADOPT_POSITION,
    FLATTEN_REDUCE_ONLY, IGNORE_DUST,
    REFUSE_ORDERS_UNVERIFIED,    // open-orders sorgusu yok/basarisiz -> bakiyeye GECME
    REFUSE_CANCEL_STUCK,         // iptal denemeleri tukendi, hala yetim emir var -> dur
    REFUSE_BALANCE_UNVERIFIED    // emirler temiz ama bakiye okunamadi/gecersiz
};

struct StartupProbe {
    bool     lock_file_exists      = true;    // bilinmiyorsa kilitli say
    bool     open_orders_verified  = false;   // REST sorgusu BASARIYLA ve TAZE dondu mu
    int      open_orders           = -1;      // <0 = bilinmiyor
    uint32_t cancel_attempts       = 0;       // cagiran her cancel-all turunda artirir
    bool     balance_verified      = false;
    double   base_value_usd        = std::numeric_limits<double>::quiet_NaN();
    double   min_notional          = 0.0;
    double   pos_cap_usd           = 0.0;
};
inline constexpr uint32_t MAX_STARTUP_CANCEL_ATTEMPTS = 3;

// ORDER GATE gecildi mi? Bakiye okumasi YALNIZ bu true iken yapilabilir (cagiran da buna bakmali).
[[nodiscard]] inline bool startup_orders_clear(const StartupProbe& p) noexcept {
    return !p.lock_file_exists && p.open_orders_verified && p.open_orders == 0;
}
[[nodiscard]] inline bool startup_may_read_balance(const StartupProbe& p) noexcept {
    return startup_orders_clear(p);
}

[[nodiscard]] inline StartAction startup_decision(const StartupProbe& p) noexcept {
    // GATE 0: kilit
    if (p.lock_file_exists) return StartAction::REFUSE_LOCKED;

    // GATE 1: yetim emir. open_orders==0 KANITLANMADAN asagi inilmez.
    if (!p.open_orders_verified || p.open_orders < 0) return StartAction::REFUSE_ORDERS_UNVERIFIED;
    if (p.open_orders > 0)
        return (p.cancel_attempts >= MAX_STARTUP_CANCEL_ATTEMPTS)
                   ? StartAction::REFUSE_CANCEL_STUCK
                   : StartAction::CANCEL_OPEN_ORDERS_THEN_RECHECK;   // cagiran iptal eder, SONRA taze sorgu ile tekrar cagirir

    // --- buradan sonrasi: open_orders == 0 KESIN ---
    // GATE 2: bakiye (NaN/Inf/negatif => CLEAN_START'a dusmez)
    if (!p.balance_verified || !std::isfinite(p.base_value_usd) || p.base_value_usd < 0.0)
        return StartAction::REFUSE_BALANCE_UNVERIFIED;
    if (p.base_value_usd == 0.0) return StartAction::CLEAN_START;
    if (p.base_value_usd < p.min_notional) return StartAction::IGNORE_DUST;
    return (p.pos_cap_usd > 0.0 && p.base_value_usd <= p.pos_cap_usd) ? StartAction::ADOPT_POSITION
                                                                      : StartAction::FLATTEN_REDUCE_ONLY;
}

} // namespace kusak::guard
#endif
