// ============================================================================
// KUSAK V12 — RESEARCH FRAMEWORK
// regime_detector.hpp — Piyasa Rejimi Dedektörü (V2 — V12 Revizyonu)
//
// Çözülen (orijinal):
// 1. MarketRegime: TREND→TRENDING, RANGE→RANGING, TOXIC→VOLATILE, DEAD→THIN
// 2. names[] dizileri: {"UNKNOWN","TRENDING","RANGING","VOLATILE","THIN"}
//
// [FIX-QUANT-3]: RegimeSnapshot'a vpin_x1e6 ve mid_price_fp alanları eklendi.
//   main.cpp process_agg_trade() fixed-point alanları dolduruyordu ama
//   RegimeSnapshot struct'ında tanımsızdı → snap.vpin her zaman 0.0 kalıyordu
//   → VOLATILE rejim hiçbir zaman tetiklenemiyordu.
//   classify_raw(): vpin_x1e6'dan double recover + uninitialised snapshot guard.
//   Tüm V1 ve V2 sınıfları aynı düzeltmeyi paylaşır.
//
// [FIX-QUANT-DS-2]: HMMRegimeFilter::update() — NaN/Inf confidence + bozuk
//   probs_ vektörü koruması. norm ≈ 0 → RANGING'e resetle (eski: NaN → lock-up).
//   T_ ve probs_ NaN için isfinite filtresi eklendi.
// ============================================================================

#pragma once

#include "research_types.hpp"
#include <cmath>
#include <array>
#include <deque>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <numeric>
#include <string>

namespace kusak::research {

// ── kPriceScale sabit tanımı — main.cpp ile paylaşılır ──────────────────────
static constexpr int64_t kPriceScale = 1'000'000LL; // [FIX-QUANT-3]

// ============================================================================
// §1  Ortak veri yapıları
// ============================================================================

struct RegimeDetectorConfig {
    double vpin_toxic_threshold   = 0.70;
    double volume_dead_threshold  = 100.0;
    double spread_dead_max_bps    = 2.5;
    double atr_trend_ratio        = 0.0008;
    double trend_imbalance_min    = 0.25;
    double smoothing_alpha        = 0.15;
    int    min_confirmations      = 3;
};

// [FIX-QUANT-3]: vpin_x1e6 ve mid_price_fp alanları eklendi.
// main.cpp bu alanları dolduruyor ama orijinal struct'ta yoktu;
// snap.vpin = 0.0 kalıyordu → VOLATILE hiç tetiklenemiyordu.
struct RegimeSnapshot {
    double   vpin       = 0.0;
    double   atr        = 0.0;
    double   spread_bps = 0.0;
    double   imbalance  = 0.0;
    double   volume_1m  = 0.0;
    double   mid_price  = 0.0;
    uint64_t ts_us      = 0;
    int32_t  vpin_x1e6    = 0;   // [FIX-QUANT-3]: fixed-point VPIN
    int64_t  mid_price_fp = 0;   // [FIX-QUANT-3]: fixed-point mid price
};

// ============================================================================
// §2  V1 RegimeDetector (geriye dönük uyumluluk)
// ============================================================================

class RegimeDetector {
public:
    explicit RegimeDetector(RegimeDetectorConfig cfg = {})
        : cfg_(cfg)
    {
        scores_.fill(0.0);
    }

    MarketRegime update(const RegimeSnapshot& snap) noexcept {
        MarketRegime raw = classify_raw(snap);
        return apply_smoothing(raw);
    }

    MarketRegime current_regime() const noexcept { return current_; }

    // [KUSAK V13]: Branchless classify_raw — predicate bits + LUT dispatch.
    // Hiçbir if/else/switch içermez. Yalnızca recovery hesapları için ternary
    // (CMOV) kullanılır. 4-bit state index → static constexpr 16-entry LUT.
    MarketRegime classify_raw(const RegimeSnapshot& snap) const noexcept {
        // ── Recovery (CMOV-only, branchless) ────────────────────────────────
        const double effective_vpin = (snap.vpin > 0.0)
            ? snap.vpin
            : static_cast<double>(snap.vpin_x1e6) / 1'000'000.0;

        const double mid = (snap.mid_price > 0.0)
            ? snap.mid_price
            : static_cast<double>(snap.mid_price_fp)
              / static_cast<double>(kPriceScale);

        const double atr_ratio = (mid > 0.0) ? snap.atr / mid : 0.0;

        // ── Predicate bit üretimi (SETcc/CMOV hedefli, dalsız) ──────────────
        const uint8_t volatile_bit =
            static_cast<uint8_t>(effective_vpin > cfg_.vpin_toxic_threshold);

        const uint8_t has_data_bit = static_cast<uint8_t>(
            (snap.volume_1m  > 0.0) |
            (snap.spread_bps > 0.0) |
            (snap.mid_price  > 0.0) |
            (snap.mid_price_fp != 0));

        const uint8_t thin_bit = static_cast<uint8_t>(
            (snap.volume_1m  < cfg_.volume_dead_threshold) &
            (snap.spread_bps < cfg_.spread_dead_max_bps));

        const uint8_t trending_bit = static_cast<uint8_t>(
            (atr_ratio > cfg_.atr_trend_ratio) &
            (std::fabs(snap.imbalance) > cfg_.trend_imbalance_min));

        // ── 4-bit State Index (V,H,T,Tr) ────────────────────────────────────
        const uint8_t state_index =
            static_cast<uint8_t>((volatile_bit << 3) |
                                 (has_data_bit << 2) |
                                 (thin_bit     << 1) |
                                  trending_bit);

        // ── Static constexpr 16-entry LUT (compile-time topology) ──────────
        static constexpr std::array<MarketRegime, 16> kRegimeLUT = {{
            /* 0 0000 */ MarketRegime::RANGING,
            /* 1 0001 */ MarketRegime::RANGING,
            /* 2 0010 */ MarketRegime::RANGING,
            /* 3 0011 */ MarketRegime::RANGING,
            /* 4 0100 */ MarketRegime::RANGING,
            /* 5 0101 */ MarketRegime::TRENDING,
            /* 6 0110 */ MarketRegime::THIN,
            /* 7 0111 */ MarketRegime::THIN,
            /* 8 1000 */ MarketRegime::VOLATILE,
            /* 9 1001 */ MarketRegime::VOLATILE,
            /*10 1010 */ MarketRegime::VOLATILE,
            /*11 1011 */ MarketRegime::VOLATILE,
            /*12 1100 */ MarketRegime::VOLATILE,
            /*13 1101 */ MarketRegime::VOLATILE,
            /*14 1110 */ MarketRegime::VOLATILE,
            /*15 1111 */ MarketRegime::VOLATILE,
        }};

        return kRegimeLUT[state_index];
    }

    struct RegimeParamOverrides {
        bool   disable_buys         = false;
        bool   disable_sells        = false;
        double min_edge_multiplier  = 1.0;
        double score_threshold_mult = 1.0;
        bool   sniper_mode          = false;
        bool   force_passive        = false;
    };

    RegimeParamOverrides get_overrides() const noexcept {
        RegimeParamOverrides ov{};
        switch (current_) {
            case MarketRegime::VOLATILE:
                ov.disable_buys        = true;
                ov.min_edge_multiplier = 2.0;
                ov.force_passive       = true;
                break;
            case MarketRegime::THIN:
                ov.min_edge_multiplier  = 1.5;
                ov.score_threshold_mult = 1.3;
                break;
            case MarketRegime::TRENDING:
                ov.sniper_mode         = true;
                ov.min_edge_multiplier = 0.9;
                break;
            case MarketRegime::RANGING:
            default:
                break;
        }
        return ov;
    }

    bool regime_changed()    const noexcept { return regime_changed_; }
    int  ticks_in_regime()   const noexcept { return ticks_in_regime_; }

    void reset() noexcept {
        current_         = MarketRegime::UNKNOWN;
        scores_.fill(0.0);
        ticks_in_regime_ = 0;
        regime_changed_  = false;
        confirm_count_   = 0;
        pending_regime_  = MarketRegime::UNKNOWN;
    }

private:
    MarketRegime apply_smoothing(MarketRegime raw) noexcept {
        for (int i = 0; i < 5; ++i) {
            auto r      = static_cast<MarketRegime>(i);
            double tgt  = (r == raw) ? 1.0 : 0.0;
            scores_[i]  = scores_[i] * (1.0 - cfg_.smoothing_alpha)
                        + tgt        *  cfg_.smoothing_alpha;
        }
        int best = 0;
        for (int i = 1; i < 5; ++i)
            if (scores_[i] > scores_[best]) best = i;
        MarketRegime dominant = static_cast<MarketRegime>(best);

        if (dominant != current_) {
            if (dominant == pending_regime_) ++confirm_count_;
            else { pending_regime_ = dominant; confirm_count_ = 1; }

            if (confirm_count_ >= cfg_.min_confirmations) {
                current_         = dominant;
                regime_changed_  = true;
                ticks_in_regime_ = 1;
                confirm_count_   = 0;
                pending_regime_  = MarketRegime::UNKNOWN;
            } else {
                regime_changed_ = false;
            }
        } else {
            regime_changed_ = false;
            ticks_in_regime_++;
            confirm_count_ = 0;
        }
        return current_;
    }

    RegimeDetectorConfig cfg_;
    MarketRegime         current_         = MarketRegime::UNKNOWN;
    MarketRegime         pending_regime_  = MarketRegime::UNKNOWN;
    std::array<double,5> scores_{};
    int                  ticks_in_regime_ = 0;
    int                  confirm_count_   = 0;
    bool                 regime_changed_  = false;
};

}  // namespace kusak::research

// ============================================================================
// §3  V2 Yeni Sınıflar — ATRTrailingStop, HMMRegimeFilter, RegimeDetectorV2
// ============================================================================

namespace kusak::research {

// ============================================================================
// §3.1  ATR-Tabanlı Trailing Stop
// ============================================================================

class ATRTrailingStop {
public:
    struct Config {
        int    atr_period         = 14;
        double trending_atr_mult  = 1.5;
        double ranging_atr_mult   = 2.5;
        double volatile_atr_mult  = 1.0;
        double thin_atr_mult      = 3.0;
        double min_activation_atr = 0.5;
        double max_callback_pct   = 0.005;
    };

    struct TrailingParams {
        double activation_bps = 0.0;
        double callback_bps   = 0.0;
        double atr_value      = 0.0;
        MarketRegime regime   = MarketRegime::UNKNOWN;
    };

    ATRTrailingStop() = default;
    explicit ATRTrailingStop(Config cfg) : cfg_(cfg) {}

    void update(double high, double low, double close) noexcept {
        if (prev_close_ > 0.0) {
            double tr = std::max({high - low,
                                  std::fabs(high - prev_close_),
                                  std::fabs(low  - prev_close_)});
            atr_ = (atr_ == 0.0) ? tr : atr_ * (1.0 - alpha_) + tr * alpha_;
        }
        prev_close_ = close;
    }

    TrailingParams compute(MarketRegime regime, double mid_price) const noexcept {
        TrailingParams p;
        p.atr_value = atr_;
        p.regime    = regime;

        double mult          = get_atr_mult(regime);
        double stop_distance = atr_ * mult;
        double bps_per_unit  = (mid_price > 0.0) ? 10000.0 / mid_price : 1.0;

        p.callback_bps   = stop_distance * bps_per_unit;
        p.activation_bps = atr_ * cfg_.min_activation_atr * bps_per_unit;

        p.callback_bps   = std::max(2.0, std::min(p.callback_bps,
                                    mid_price * cfg_.max_callback_pct * 10000.0));
        p.activation_bps = std::max(5.0, p.activation_bps);
        return p;
    }

    double atr() const noexcept { return atr_; }

private:
    double get_atr_mult(MarketRegime r) const noexcept {
        switch (r) {
            case MarketRegime::TRENDING:  return cfg_.trending_atr_mult;
            case MarketRegime::RANGING:   return cfg_.ranging_atr_mult;
            case MarketRegime::VOLATILE:  return cfg_.volatile_atr_mult;
            case MarketRegime::THIN:      return cfg_.thin_atr_mult;
            default:                      return cfg_.ranging_atr_mult;
        }
    }

    Config   cfg_;
    double   atr_        = 0.0;
    double   prev_close_ = 0.0;
    double   alpha_      = 2.0 / (14.0 + 1.0);
};

// ============================================================================
// §3.2  Rejim Doğruluk Takipçisi
// ============================================================================

struct RegimeAccuracyStats {
    struct PerRegime {
        int tp = 0, fp = 0, fn = 0;
        double precision() const {
            return (tp + fp > 0) ? static_cast<double>(tp) / (tp + fp) : 0.0;
        }
        double recall() const {
            return (tp + fn > 0) ? static_cast<double>(tp) / (tp + fn) : 0.0;
        }
        double f1() const {
            double p = precision(), r = recall();
            return (p + r > 0.0) ? 2.0 * p * r / (p + r) : 0.0;
        }
    };
    std::array<PerRegime, 5> per_regime{};
    std::array<std::array<int,5>, 5> confusion{};
    int total_labels = 0;

    void record(MarketRegime predicted, MarketRegime actual) {
        int p = static_cast<int>(predicted);
        int a = static_cast<int>(actual);
        if (p < 5 && a < 5) {
            confusion[p][a]++;
            total_labels++;
            if (p == a) per_regime[p].tp++;
            else        { per_regime[p].fp++; per_regime[a].fn++; }
        }
    }

    void print() const {
        static const char* names[] = {"UNKNOWN","TRENDING","RANGING","VOLATILE","THIN"};
        std::cout << "\n─── Rejim Doğruluk Metrikleri ─────────────────\n";
        std::cout << std::left << std::setw(10) << "Rejim"
                  << std::right
                  << std::setw(8) << "Prec."
                  << std::setw(8) << "Recall"
                  << std::setw(8) << "F1\n";
        for (int i = 1; i < 5; ++i) {
            std::cout << std::left  << std::setw(10) << names[i]
                      << std::right << std::fixed << std::setprecision(3)
                      << std::setw(8) << per_regime[i].precision()
                      << std::setw(8) << per_regime[i].recall()
                      << std::setw(8) << per_regime[i].f1() << "\n";
        }
        std::cout << "  Toplam etiket: " << total_labels << "\n"
                  << "──────────────────────────────────────────────\n";
    }
};

// ============================================================================
// §3.3  HMMRegimeFilter
// ============================================================================

struct HMMRegimeFilterConfig {
    int    min_persistence_ticks    = 2;
    double high_risk_prob_threshold = 0.80;
    double self_transition_prob     = 0.90;
};

class HMMRegimeFilter {
public:
    using Config = HMMRegimeFilterConfig;

    explicit HMMRegimeFilter(Config cfg = Config{})
        : cfg_(cfg)
    {
        probs_.fill(0.25);
        probs_[0] = 0.0;
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 5; ++j)
                T_[i][j] = (i == j) ? cfg_.self_transition_prob
                                     : (1.0 - cfg_.self_transition_prob) / 4.0;
    }

    // [FIX-QUANT-DS-2]: NaN/Inf confidence + bozuk probs_ vektörü koruması.
    // Önceki kod: norm ≈ 0 → tüm probs_ NaN → max_element yanlış seçim →
    // rejim UNKNOWN'a kilitlenme. Yeni kod: bozuk giriş durumunda RANGING'e reset.
    MarketRegime update(MarketRegime raw_obs, double confidence = 0.8) noexcept {
        // [FIX-QUANT-DS-2]: raw_obs sınır kontrolü
        const int raw_idx = static_cast<int>(raw_obs);
        if (raw_idx < 0 || raw_idx >= 5) {
            raw_obs = MarketRegime::RANGING;
        }

        // [FIX-QUANT-DS-2]: confidence NaN/Inf koruması
        if (!std::isfinite(confidence)) confidence = 0.8;
        confidence = std::clamp(confidence, 0.0, 1.0);

        std::array<double, 5> likelihood{};
        likelihood.fill((1.0 - confidence) / 4.0);
        likelihood[static_cast<int>(raw_obs)] = confidence;

        std::array<double, 5> predicted{};
        predicted.fill(0.0);

        // [FIX-QUANT-DS-2]: T_ ve probs_ NaN/Inf filtresi
        for (int j = 0; j < 5; ++j) {
            for (int i = 0; i < 5; ++i) {
                const double tij = std::isfinite(T_[i][j]) ? T_[i][j] : 0.0;
                const double pi  = std::isfinite(probs_[i]) ? probs_[i] : 0.0;
                predicted[j] += tij * pi;
            }
            if (!std::isfinite(predicted[j]) || predicted[j] < 0.0)
                predicted[j] = 0.0;
        }

        double norm = 0.0;
        for (int i = 0; i < 5; ++i) {
            probs_[i] = predicted[i] * likelihood[i];
            // [FIX-QUANT-DS-2]: NaN/negatif probs_ → 0.0
            if (!std::isfinite(probs_[i]) || probs_[i] < 0.0) probs_[i] = 0.0;
            norm += probs_[i];
        }

        if (std::isfinite(norm) && norm > 1e-12) {
            const double inv_norm = 1.0 / norm;
            for (auto& p : probs_) p *= inv_norm;
        } else {
            // [FIX-QUANT-DS-2]: norm ≈ 0 veya NaN → güvenli RANGING'e resetle.
            // Eski kod: bölme yoktu ama probs_ NaN kalıyordu → max_element UB.
            probs_.fill(0.0);
            probs_[static_cast<int>(MarketRegime::RANGING)] = 1.0;
            volatile_tick_count_ = 0;
        }

        // [FIX-QUANT-DS-2]: max_element NaN ile UNKNOWN seçebilir; UNKNOWN (0)
        // dışı rejimler arasında seç — en az RANGING (canlı rejim) garantisi.
        int best = static_cast<int>(MarketRegime::RANGING); // varsayılan güvenli
        for (int i = 1; i < 5; ++i) {
            const double lhs = std::isfinite(probs_[i]) ? probs_[i] : -1.0;
            const double rhs = std::isfinite(probs_[best]) ? probs_[best] : -1.0;
            if (lhs > rhs) best = i;
        }

        const MarketRegime dominant = static_cast<MarketRegime>(best);

        if (dominant == MarketRegime::VOLATILE)
            volatile_tick_count_++;
        else
            volatile_tick_count_ = 0;

        return dominant;
    }

    bool isHighRisk() const noexcept {
        return probs_[static_cast<int>(MarketRegime::VOLATILE)]
               > cfg_.high_risk_prob_threshold
            && volatile_tick_count_ >= cfg_.min_persistence_ticks;
    }

    double regime_probability(MarketRegime r) const noexcept {
        const int idx = static_cast<int>(r);
        if (idx < 0 || idx >= 5) return 0.0;
        return probs_[idx];
    }

    const std::array<double,5>& probabilities() const noexcept { return probs_; }

    void reset() noexcept {
        probs_.fill(0.25);
        probs_[0]             = 0.0;
        volatile_tick_count_  = 0;
    }

private:
    Config                              cfg_;
    std::array<double, 5>               probs_{};
    std::array<std::array<double,5>, 5> T_{};
    int                                 volatile_tick_count_ = 0;
};

// ============================================================================
// §4  RegimeDetectorV2Config
// ============================================================================

struct RegimeDetectorV2Config {
    double vpin_toxic_threshold             = 0.70;
    double volume_dead_threshold            = 100.0;
    double spread_dead_max_bps              = 2.5;
    double atr_trend_ratio                  = 0.0008;
    double trend_imbalance_min              = 0.25;
    double hmm_confidence                   = 0.80;
    double hmm_high_risk_prob_threshold     = 0.80;
    int    hmm_persistence                  = 2;
    bool   track_accuracy                   = true;
    ATRTrailingStop::Config atr_stop_cfg;
};

// ============================================================================
// §5  RegimeDetectorV2
// ============================================================================

class RegimeDetectorV2 {
public:
    static HMMRegimeFilter::Config make_hmm_cfg(const RegimeDetectorV2Config& cfg) {
        HMMRegimeFilter::Config hcfg;
        hcfg.high_risk_prob_threshold = cfg.hmm_high_risk_prob_threshold;
        hcfg.min_persistence_ticks    = cfg.hmm_persistence;
        return hcfg;
    }

    explicit RegimeDetectorV2(RegimeDetectorV2Config cfg = {})
        : cfg_(cfg)
        , atr_stop_(cfg.atr_stop_cfg)
        , hmm_(make_hmm_cfg(cfg))
    {}

    MarketRegime update(const RegimeSnapshot& snap,
                        double high = 0.0, double low = 0.0) noexcept
    {
        // [FIX-QUANT-6] snap.mid_price cogu canli caller'da (bkz. main.cpp
        // process_agg_trade) hic doldurulmuyor -- sadece mid_price_fp
        // dolduruluyor. classify_raw() bunu zaten kurtariyordu (FIX-QUANT-3,
        // VPIN icin); ATR/trailing-stop tarafi ayni kurtarmadan yoksundu, bu
        // yuzden h=l=close=0.0 -> ATRTrailingStop::update()'teki
        // `prev_close_ > 0.0` sarti hicbir zaman saglanmiyor ve atr_ sonsuza
        // kadar 0.0'da kilitli kaliyordu (ATR=0 bug'inin kok nedeni).
        const double recovered_mid = (snap.mid_price > 0.0)
            ? snap.mid_price
            : static_cast<double>(snap.mid_price_fp)
              / static_cast<double>(kPriceScale);

        double h = (high > 0.0) ? high : recovered_mid * 1.0005;
        double l = (low  > 0.0) ? low  : recovered_mid * 0.9995;
        atr_stop_.update(h, l, recovered_mid);

        MarketRegime raw      = classify_raw(snap);
        MarketRegime filtered = hmm_.update(raw, cfg_.hmm_confidence);

        if (filtered != current_) {
            transitions_[static_cast<int>(current_)][static_cast<int>(filtered)]++;
            prev_regime_     = current_;
            current_         = filtered;
            ticks_in_regime_ = 0;
            regime_changed_  = true;
        } else {
            regime_changed_ = false;
            ticks_in_regime_++;
        }

        last_snap_ = snap;
        return current_;
    }

    bool isHighRisk() const noexcept { return hmm_.isHighRisk(); }

    double regime_probability(MarketRegime r) const noexcept {
        return hmm_.regime_probability(r);
    }

    ATRTrailingStop::TrailingParams trailing_params() const noexcept {
        return atr_stop_.compute(current_, last_snap_.mid_price);
    }

    void record_accuracy(MarketRegime predicted, MarketRegime gold_label) {
        if (cfg_.track_accuracy) accuracy_.record(predicted, gold_label);
    }

    MarketRegime current_regime()  const noexcept { return current_; }
    MarketRegime previous_regime() const noexcept { return prev_regime_; }
    bool         regime_changed()  const noexcept { return regime_changed_; }
    int          ticks_in_regime() const noexcept { return ticks_in_regime_; }
    double       atr()             const noexcept { return atr_stop_.atr(); }

    int transition_count(MarketRegime from, MarketRegime to) const noexcept {
        return transitions_[static_cast<int>(from)][static_cast<int>(to)];
    }

    const RegimeAccuracyStats& accuracy_stats() const noexcept { return accuracy_; }

    void print_diagnostics() const {
        static const char* names[] = {"UNKNOWN","TRENDING","RANGING","VOLATILE","THIN"};
        std::cout << "\n─── Rejim Dedektörü V2 Teşhis (V12) ───────────\n"
                  << "  Mevcut rejim    : " << names[static_cast<int>(current_)] << "\n"
                  << "  Tick'ler içinde : " << ticks_in_regime_ << "\n"
                  << "  isHighRisk()    : " << (isHighRisk() ? "EVET" : "HAYIR") << "\n";

        auto tp = trailing_params();
        std::cout << std::fixed << std::setprecision(5)
                  << "  ATR             : " << tp.atr_value      << "\n"
                  << "  Activation bps  : " << tp.activation_bps << "\n"
                  << "  Callback bps    : " << tp.callback_bps   << "\n"
                  << "\n  HMM Olasılıkları (eşik=" << cfg_.hmm_high_risk_prob_threshold
                  << " persistence=" << cfg_.hmm_persistence << "):\n";

        for (int i = 1; i < 5; ++i) {
            auto r = static_cast<MarketRegime>(i);
            std::cout << "    " << names[i] << ": "
                      << std::setprecision(3) << hmm_.regime_probability(r) << "\n";
        }

        if (cfg_.track_accuracy) accuracy_.print();
        std::cout << "──────────────────────────────────────────────\n";
    }

    void reset() noexcept {
        current_         = MarketRegime::UNKNOWN;
        prev_regime_     = MarketRegime::UNKNOWN;
        ticks_in_regime_ = 0;
        regime_changed_  = false;
        accuracy_        = RegimeAccuracyStats{};
        hmm_.reset();
        for (auto& row : transitions_) row.fill(0);
    }

private:
    // [KUSAK V13]: Branchless classify_raw — predicate bits + LUT dispatch.
    // V1 ile birebir aynı topology; yalnızca recovery hesaplarında ternary (CMOV).
    MarketRegime classify_raw(const RegimeSnapshot& snap) const noexcept {
        // ── Recovery (CMOV-only, branchless) ────────────────────────────────
        const double effective_vpin = (snap.vpin > 0.0)
            ? snap.vpin
            : static_cast<double>(snap.vpin_x1e6) / 1'000'000.0;

        const double mid = (snap.mid_price > 0.0)
            ? snap.mid_price
            : static_cast<double>(snap.mid_price_fp)
              / static_cast<double>(kPriceScale);

        const double atr_ratio = (mid > 0.0) ? snap.atr / mid : 0.0;

        // ── Predicate bit üretimi (SETcc/CMOV hedefli, dalsız) ──────────────
        const uint8_t volatile_bit =
            static_cast<uint8_t>(effective_vpin > cfg_.vpin_toxic_threshold);

        const uint8_t has_data_bit = static_cast<uint8_t>(
            (snap.volume_1m  > 0.0) |
            (snap.spread_bps > 0.0) |
            (snap.mid_price  > 0.0) |
            (snap.mid_price_fp != 0));

        const uint8_t thin_bit = static_cast<uint8_t>(
            (snap.volume_1m  < cfg_.volume_dead_threshold) &
            (snap.spread_bps < cfg_.spread_dead_max_bps));

        const uint8_t trending_bit = static_cast<uint8_t>(
            (atr_ratio > cfg_.atr_trend_ratio) &
            (std::fabs(snap.imbalance) > cfg_.trend_imbalance_min));

        // ── 4-bit State Index (V,H,T,Tr) ────────────────────────────────────
        const uint8_t state_index =
            static_cast<uint8_t>((volatile_bit << 3) |
                                 (has_data_bit << 2) |
                                 (thin_bit     << 1) |
                                  trending_bit);

        // ── Static constexpr 16-entry LUT (compile-time topology) ──────────
        static constexpr std::array<MarketRegime, 16> kRegimeLUT = {{
            /* 0 0000 */ MarketRegime::RANGING,
            /* 1 0001 */ MarketRegime::RANGING,
            /* 2 0010 */ MarketRegime::RANGING,
            /* 3 0011 */ MarketRegime::RANGING,
            /* 4 0100 */ MarketRegime::RANGING,
            /* 5 0101 */ MarketRegime::TRENDING,
            /* 6 0110 */ MarketRegime::THIN,
            /* 7 0111 */ MarketRegime::THIN,
            /* 8 1000 */ MarketRegime::VOLATILE,
            /* 9 1001 */ MarketRegime::VOLATILE,
            /*10 1010 */ MarketRegime::VOLATILE,
            /*11 1011 */ MarketRegime::VOLATILE,
            /*12 1100 */ MarketRegime::VOLATILE,
            /*13 1101 */ MarketRegime::VOLATILE,
            /*14 1110 */ MarketRegime::VOLATILE,
            /*15 1111 */ MarketRegime::VOLATILE,
        }};

        return kRegimeLUT[state_index];
    }

    RegimeDetectorV2Config          cfg_;
    ATRTrailingStop                 atr_stop_;
    HMMRegimeFilter                 hmm_;
    RegimeAccuracyStats             accuracy_;

    MarketRegime                    current_         = MarketRegime::UNKNOWN;
    MarketRegime                    prev_regime_     = MarketRegime::UNKNOWN;
    int                             ticks_in_regime_ = 0;
    bool                            regime_changed_  = false;
    RegimeSnapshot                  last_snap_{};

    std::array<std::array<int,5>,5> transitions_{};
};

}  // namespace kusak::research
