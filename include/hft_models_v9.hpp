#pragma once
#ifndef KUSAK_HFT_MODELS_V9_HPP
#define KUSAK_HFT_MODELS_V9_HPP

// Çözülen sorunlar (orijinal):
// 1. struct PriceLevel tanımı tamamen silindi — tek otorite research_types.hpp
// 2. research_types.hpp include edilerek FileHeader, TickRecord, from_disk, now_us görünür
// 3. quantity field erişimi research_types.hpp::PriceLevel üzerinden çalışır
//
// [FIX-QUANT-4]: HFTModelV9::update() — std::clamp(NaN) UB + NaN propagation
//   std::clamp(NaN, -1, 1): C++ standardında undefined behavior.
//   NaN >= -1 ve NaN <= 1 ikisi de false → clamp içinde undefined path.
//   Düzeltme: std::clamp → std::fmin/fmax (NaN'ı non-NaN operanda bırakır).
//   ema_imb ve sig.volatility için isfinite guard; vol_adj NaN → 0.0.
//   Epsilon 1e-9 → 1e-6: tanh argümanını makul aralıkta tutar.
//
// [FIX-QUANT-DS-3]: VolatilityEstimator::estimate() negatif karekök riski.
//   Floating-point birikimi mean < 0 yapabilir → sqrt(negatif) → NaN.
//   Düzeltme: mean = std::max(mean, 0.0) → sqrt daima finite.

#include "research_types.hpp"

#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <numeric>
#include <string>
#include <stdexcept>

#ifndef FORCE_INLINE
#  ifdef __GNUC__
#    define FORCE_INLINE __attribute__((always_inline)) inline
#  else
#    define FORCE_INLINE inline
#  endif
#endif

#ifndef LIKELY
#  define LIKELY(x)   __builtin_expect(!!(x), 1)
#endif
#ifndef UNLIKELY
#  define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

#ifndef KUSAK_TIME_HELPERS_DEFINED
#define KUSAK_TIME_HELPERS_DEFINED
#include <time.h>
FORCE_INLINE uint64_t now_ns() noexcept {
    struct timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}
FORCE_INLINE uint64_t now_us() noexcept { return now_ns() / 1'000ULL; }
#endif

namespace kusak::research {

// ─────────────────────────────────────────────────────────────────────────────
// OrderBook — emir defteri modellemesi
// ─────────────────────────────────────────────────────────────────────────────
class OrderBook {
public:
    static constexpr int MAX_LEVELS = MAX_DEPTH;

    void reset() noexcept {
        bid_count_ = 0;
        ask_count_ = 0;
    }

    void update(const TickRecord& rec) noexcept {
        bid_count_ = static_cast<int>(rec.bid_count);
        ask_count_ = static_cast<int>(rec.ask_count);
        for (int i = 0; i < bid_count_ && i < MAX_LEVELS; ++i)
            bids_[i] = rec.bids[i];
        for (int i = 0; i < ask_count_ && i < MAX_LEVELS; ++i)
            asks_[i] = rec.asks[i];
    }

    FORCE_INLINE std::vector<std::pair<double,double>>
    getBidsForModel(int cnt) const noexcept {
        std::vector<std::pair<double,double>> v;
        cnt = std::min(cnt, bid_count_);
        v.reserve(cnt);
        for (int i = 0; i < cnt; ++i)
            v.emplace_back(bids_[i].price, bids_[i].quantity);
        return v;
    }

    FORCE_INLINE std::vector<std::pair<double,double>>
    getAsksForModel(int cnt) const noexcept {
        std::vector<std::pair<double,double>> v;
        cnt = std::min(cnt, ask_count_);
        v.reserve(cnt);
        for (int i = 0; i < cnt; ++i)
            v.emplace_back(asks_[i].price, asks_[i].quantity);
        return v;
    }

    FORCE_INLINE double totalBidDepth(int cnt) const noexcept {
        double s = 0.0;
        cnt = std::min(cnt, bid_count_);
        for (int i = 0; i < cnt; ++i)
            s += bids_[i].quantity;
        return s;
    }

    FORCE_INLINE double totalAskDepth(int cnt) const noexcept {
        double s = 0.0;
        cnt = std::min(cnt, ask_count_);
        for (int i = 0; i < cnt; ++i)
            s += asks_[i].quantity;
        return s;
    }

    FORCE_INLINE double midPrice() const noexcept {
        if (bid_count_ == 0 || ask_count_ == 0) return 0.0;
        return (bids_[0].price + asks_[0].price) * 0.5;
    }

    FORCE_INLINE double spread() const noexcept {
        if (bid_count_ == 0 || ask_count_ == 0) return 0.0;
        return asks_[0].price - bids_[0].price;
    }

    FORCE_INLINE double spreadBps() const noexcept {
        const double mid = midPrice();
        if (mid <= 0.0) return 0.0;
        return spread() / mid * 10'000.0;
    }

    FORCE_INLINE double imbalance(int levels = 5) const noexcept {
        const double bd = totalBidDepth(levels);
        const double ad = totalAskDepth(levels);
        const double total = bd + ad;
        if (total <= 0.0) return 0.0;
        return (bd - ad) / total;
    }

    FORCE_INLINE double weightedMid() const noexcept {
        if (bid_count_ == 0 || ask_count_ == 0) return midPrice();
        const double bq = bids_[0].quantity;
        const double aq = asks_[0].quantity;
        const double total = bq + aq;
        if (total <= 0.0) return midPrice();
        return (bids_[0].price * aq + asks_[0].price * bq) / total;
    }

    int bidCount() const noexcept { return bid_count_; }
    int askCount() const noexcept { return ask_count_; }
    const PriceLevel& bid(int i) const noexcept { return bids_[i]; }
    const PriceLevel& ask(int i) const noexcept { return asks_[i]; }

private:
    PriceLevel bids_[MAX_LEVELS];
    PriceLevel asks_[MAX_LEVELS];
    int        bid_count_ = 0;
    int        ask_count_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// MicroPrice model — Stoikov ağırlıklı mid
// ─────────────────────────────────────────────────────────────────────────────
struct MicroPrice {
    static FORCE_INLINE double compute(const OrderBook& ob) noexcept {
        return ob.weightedMid();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// DepthPressure — N seviyeli normalize edilmiş baskı sinyali [-1, +1]
// ─────────────────────────────────────────────────────────────────────────────
struct DepthPressure {
    static FORCE_INLINE double compute(const OrderBook& ob, int levels = 10) noexcept {
        return ob.imbalance(levels);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// VWAP seviye hesabı — tek taraf için
// ─────────────────────────────────────────────────────────────────────────────
FORCE_INLINE double side_vwap(const std::vector<std::pair<double,double>>& levels) noexcept {
    double num = 0.0, den = 0.0;
    for (const auto& [p, q] : levels) { num += p * q; den += q; }
    return (den > 0.0) ? num / den : 0.0;
}

// ─────────────────────────────────────────────────────────────────────────────
// VolatilityEstimator — Parkinson high-low volatilite tahmini
// ─────────────────────────────────────────────────────────────────────────────
class VolatilityEstimator {
public:
    explicit VolatilityEstimator(int window = 20) : window_(window) {
        samples_.reserve(window);
    }

    void update(double high, double low) noexcept {
        if (high <= 0.0 || low <= 0.0 || high < low) return;
        const double log_ratio = std::log(high / low);
        samples_.push_back(log_ratio * log_ratio);
        if (static_cast<int>(samples_.size()) > window_)
            samples_.erase(samples_.begin());
    }

    FORCE_INLINE double estimate() const noexcept {
        if (samples_.empty()) return 0.0;
        const double mean = std::accumulate(samples_.begin(), samples_.end(), 0.0)
                          / static_cast<double>(samples_.size());
        // [FIX-QUANT-DS-3]: floating-point birikimi mean < 0 yapabilir →
        // sqrt(negatif) → NaN. std::max(mean, 0.0) ile NaN riski sıfırlanır.
        const double variance = std::fmax(mean, 0.0); // [FIX-QUANT-DS-3]
        return std::sqrt(variance / (4.0 * std::log(2.0)));
    }

    bool ready() const noexcept { return static_cast<int>(samples_.size()) >= window_; }

private:
    int                  window_;
    std::vector<double>  samples_;
};

// ─────────────────────────────────────────────────────────────────────────────
// EMA — Exponential Moving Average, O(1) update
// ─────────────────────────────────────────────────────────────────────────────
class EMA {
public:
    explicit EMA(double alpha) noexcept : alpha_(alpha) {}

    FORCE_INLINE double update(double x) noexcept {
        if (!initialized_) { value_ = x; initialized_ = true; return value_; }
        value_ = alpha_ * x + (1.0 - alpha_) * value_;
        return value_;
    }

    FORCE_INLINE double value()       const noexcept { return value_; }
    FORCE_INLINE bool   initialized() const noexcept { return initialized_; }
    void reset() noexcept { initialized_ = false; value_ = 0.0; }

private:
    double alpha_;
    double value_       = 0.0;
    bool   initialized_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// ModelSignal — aggregate çıktı
// ─────────────────────────────────────────────────────────────────────────────
struct ModelSignal {
    double  score       = 0.0;   // [-1, +1] normalize sinyal
    double  confidence  = 0.0;   // [0, 1]
    double  micro_price = 0.0;
    double  imbalance   = 0.0;
    double  volatility  = 0.0;
    bool    valid       = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// HFTModelV9 — ana model sınıfı
// ─────────────────────────────────────────────────────────────────────────────
class HFTModelV9 {
public:
    struct Config {
        int    depth_levels       = 10;
        int    vol_window         = 20;
        double ema_alpha          = 0.1;
        double imbalance_thresh   = 0.35;
        double spread_bps_max     = 10.0;
    };

    explicit HFTModelV9(Config cfg = {}) noexcept
        : cfg_(cfg)
        , vol_(cfg.vol_window)
        , ema_imb_(cfg.ema_alpha)
    {}

    ModelSignal update(const TickRecord& rec) noexcept {
        book_.update(rec);
        ModelSignal sig;

        const double spread_bps = book_.spreadBps();
        if (spread_bps > cfg_.spread_bps_max) return sig;

        sig.micro_price = MicroPrice::compute(book_);
        sig.imbalance   = DepthPressure::compute(book_, cfg_.depth_levels);
        sig.valid       = true;

        const double ema_imb = ema_imb_.update(sig.imbalance);

        if (book_.bidCount() > 0 && book_.askCount() > 0) {
            vol_.update(book_.ask(0).price, book_.bid(0).price);
        }
        sig.volatility = vol_.estimate();

        // [FIX-QUANT-4]: NaN propagation kırma — isfinite guard.
        // ema_imb veya sig.volatility NaN/Inf ise güvenli sıfır kullan.
        const double safe_ema_imb = std::isfinite(ema_imb) ? ema_imb : 0.0;
        const double safe_vol     = std::isfinite(sig.volatility) ? sig.volatility : 0.0;

        double vol_adj;
        if (safe_vol > 1e-6) {
            // [FIX-QUANT-4]: epsilon 1e-9 → 1e-6 (tanh argümanı makul aralıkta)
            // safe_vol > 1e-6 ile bölme güvenli; tanh(±∞) = ±1.0 matematik olarak
            // doğru ama çok küçük epsilon ile gereksiz büyük argüman üretiyordu.
            vol_adj = std::tanh(safe_ema_imb / safe_vol);
        } else {
            vol_adj = safe_ema_imb;
        }

        // [FIX-QUANT-4]: std::clamp(NaN, -1, 1) → C++ UB.
        // NaN >= -1 ve NaN <= 1 ikisi de false → undefined path.
        // fmax/fmin IEEE 754'te NaN'ı non-NaN operanda bırakır → güvenli clamp.
        const double clamped = std::fmin(std::fmax(vol_adj, -1.0), 1.0);
        // [FIX-QUANT-4]: fmin/fmax sonucu hâlâ NaN ise (her iki operand NaN ise)
        // isfinite ile son savunma hattı — score = 0.0 (nötr sinyal)
        sig.score      = std::isfinite(clamped) ? clamped : 0.0;
        sig.confidence = std::fabs(sig.score);
        return sig;
    }

    void reset() noexcept {
        book_.reset();
        vol_ = VolatilityEstimator(cfg_.vol_window);
        ema_imb_.reset();
    }

private:
    Config              cfg_;
    OrderBook           book_;
    VolatilityEstimator vol_;
    EMA                 ema_imb_;
};

} // namespace kusak::research

#endif // KUSAK_HFT_MODELS_V9_HPP
