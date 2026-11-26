// ============================================================================
// KUSAK V12 — RESEARCH FRAMEWORK
// risk_scorer.hpp  [HFT REFACTOR — KIYAMET PAKETİ v1]
//
// UYGULANAN DÜZELTMELER (orijinal):
// [FIX-NaN-1] refresh_inverses(): inv_spread_ref ve inv_depth_ref için
//             sıfır-bölme koruması → (val > 1e-9) ? (1.0/val) : 0.0
//
// [FIX-QUANT-5]: compute_fast() NaN zehirlenmesi giderildi.
//   fmin(NaN, 1.0) = 1.0 (IEEE 754 minNum semantiği) → ob_imbalance NaN girişte
//   yanlışlıkla 1.0 oluyordu → risk skoru yapay VETO seviyesine çıkıyordu.
//   Tüm türetilmiş alanlar için isfinite guard eklendi.
//   vpin_zscore, spread_norm, depth_inv NaN → güvenli sıfır/varsayılan.
//   ob_imbalance NaN → 0.0 (muhafazakâr: imbalance yok sayılır).
//   hmm_prob_high NaN → 0.0 (muhafazakâr: HMM katkısı yok sayılır).
//
// [FIX-QUANT-DS]: decide_branchless() — NaN skor → VETO (fail-closed).
//   NaN >= threshold her zaman false → idx=0 → ALLOW → kontrolsüz işlem.
//   score != score (NaN self-inequality) → doğrudan VETO döndür.
//
// [FIX-QUANT-GPT]: RiskWeights::normalize() NaN ağırlık temizliği.
//   Hot-reload sonrası bozuk ağırlık → kalıcı NaN scorer riski.
//   normalize() tüm ağırlıkları isfinite + non-negative filtresinden geçirir.
//
// [FIX-QUANT-GPT]: evaluate_fast() fail-closed NaN raw skoru.
//   raw NaN → r.score = 1.0 (VETO), değil 0.0 (ALLOW).
// ============================================================================

#pragma once

#include "research_types.hpp"
#include <cmath>
#include <algorithm>
#include <array>

// ─────────────────────────────────────────────────────────────────────────────
// Platform-agnostic always-inline
// ─────────────────────────────────────────────────────────────────────────────
#if defined(__GNUC__) || defined(__clang__)
#   define HFT_ALWAYS_INLINE __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
#   define HFT_ALWAYS_INLINE __forceinline
#else
#   define HFT_ALWAYS_INLINE inline
#endif

namespace kusak::research {

// ============================================================================
// §1  RiskWeights — startup / config path, NOT hot-path
// ============================================================================
struct RiskWeights {
    double vpin_zscore    = 0.0;
    double hmm_prob       = 0.0;
    double ob_imbalance   = 0.0;
    double spread_width   = 0.0;
    double depth_liquidity= 0.0;

    // [FIX-QUANT-GPT]: normalize() NaN/Inf/negatif ağırlıkları temizler.
    // Hot-reload sonrası bozuk config → kalıcı NaN scorer riski önlenir.
    void normalize() noexcept {
        // [FIX-QUANT-GPT]: Her ağırlığı NaN/Inf/negatif kontrolünden geçir
        auto clean = [](double& x) noexcept {
            if (!std::isfinite(x) || x < 0.0) x = 0.0;
        };
        clean(vpin_zscore);
        clean(hmm_prob);
        clean(ob_imbalance);
        clean(spread_width);
        clean(depth_liquidity);

        const double sum = vpin_zscore + hmm_prob + ob_imbalance
                         + spread_width + depth_liquidity;
        // [FIX-QUANT-GPT]: sum NaN/Inf olabilir (clean sonrası olası değil ama savunmacı)
        if (!std::isfinite(sum) || sum < 1e-9) return;
        const double inv = 1.0 / sum;
        vpin_zscore      *= inv;
        hmm_prob         *= inv;
        ob_imbalance     *= inv;
        spread_width     *= inv;
        depth_liquidity  *= inv;
    }
};

// ============================================================================
// §2  RiskThresholds
// ============================================================================
struct RiskThresholds {
    double allow   = 0.60;   // [PHASE1-FW] skor 0..1 oldugundan esikler <= 1.0 olmali
    double caution = 0.80;
    double pause   = 0.95;
};

// ============================================================================
// §3  RiskDecision
// ============================================================================
enum class RiskDecision : uint8_t {
    ALLOW   = 0,
    CAUTION = 1,
    PAUSE   = 2,
    VETO    = 3,
};

[[nodiscard]] HFT_ALWAYS_INLINE
const char* decision_name(RiskDecision d) noexcept {
    static constexpr const char* kNames[4] = {
        "ALLOW", "CAUTION", "PAUSE", "VETO"
    };
    const auto idx = static_cast<uint8_t>(d);
    return (idx < 4) ? kNames[idx] : "UNKNOWN";
}

// ============================================================================
// §4  RiskContext — hot-path compute
//
//  refresh_inverses(): sigma/ref değiştiğinde (config reload) çağrılır.
//  compute_fast():     her tick'te; sıfır bölme, yalnız çarpma + fmin/fmax.
// ============================================================================
struct RiskContext {
    // ── inputs ───────────────────────────────────────────────────────────────
    double vpin          = 0.0;
    double vpin_mean     = 0.0;
    double vpin_sigma    = 0.0;
    double hmm_prob_high = 0.0;
    double ob_imbalance  = 0.0;
    double spread_bps    = 0.0;
    double spread_ref_bps= 2.0;
    double depth_bid_usd = 0.0;
    double depth_ask_usd = 0.0;
    double depth_ref_usd = 50000.0;

    // ── derived (compute_fast() sonrası) ─────────────────────────────────────
    double vpin_zscore = 0.0;
    double spread_norm = 0.0;
    double depth_inv   = 0.0;

    // ── cached inverses (refresh_inverses() sonrası) ──────────────────────────
    double inv_vpin_sigma  = 0.0;
    double inv_spread_ref  = 0.0;
    double inv_depth_ref   = 0.0;

    // ── Config değişince çağrılır — bölme burada kalır ───────────────────────
    // [FIX-NaN-1]: Tüm üç inverse için aynı sıfır-guard uygulandı.
    HFT_ALWAYS_INLINE void refresh_inverses() noexcept {
        inv_vpin_sigma = (vpin_sigma     > 1e-12) ? (1.0 / vpin_sigma)     : 0.0;
        inv_spread_ref = (spread_ref_bps > 1e-9 ) ? (1.0 / spread_ref_bps) : 0.0; // [FIX-NaN-1]
        inv_depth_ref  = (depth_ref_usd  > 1e-9 ) ? (1.0 / depth_ref_usd)  : 0.0; // [FIX-NaN-1]
    }

    // ── Hot-path: NaN sanitization + branchless clamp ────────────────────────
    // [FIX-QUANT-5]: Tüm türetilmiş alanlar için isfinite guard eklendi.
    // fmin(NaN, 1.0) = 1.0 (IEEE 754): ob_imbalance NaN → yanlışlıkla 1.0
    // → risk skoru yapay VETO'ya çıkıyordu. isfinite ile NaN → 0.0 döner.
    HFT_ALWAYS_INLINE void compute_fast() noexcept {
        // [FIX-QUANT-5]: giriş NaN koruması — hot-path'te isfinite güvenlidir
        // (x86 intrinsic olarak derlenir, branch yok)
        const double vpin_safe    = std::isfinite(vpin)          ? vpin          : 0.0;
        const double mean_safe    = std::isfinite(vpin_mean)     ? vpin_mean     : 0.0;
        const double sigma_inv    = std::isfinite(inv_vpin_sigma) ? inv_vpin_sigma: 0.0;
        const double spread_safe  = std::isfinite(spread_bps)    ? spread_bps    : 0.0;
        const double sref_inv     = std::isfinite(inv_spread_ref) ? inv_spread_ref: 0.0;
        const double bid_safe     = (std::isfinite(depth_bid_usd) && depth_bid_usd >= 0.0)
                                    ? depth_bid_usd : 0.0;
        const double ask_safe     = (std::isfinite(depth_ask_usd) && depth_ask_usd >= 0.0)
                                    ? depth_ask_usd : 0.0;
        const double dref_inv     = std::isfinite(inv_depth_ref)  ? inv_depth_ref : 0.0;

        // z = (vpin - mean) * inv_sigma
        vpin_zscore = (vpin_safe - mean_safe) * sigma_inv;
        // [FIX-QUANT-5]: 0 * NaN = NaN olabilir; isfinite final guard
        if (!std::isfinite(vpin_zscore)) vpin_zscore = 0.0;

        // spread_norm = fmin(spread * inv_ref, 3.0)
        spread_norm = spread_safe * sref_inv;
        if (!std::isfinite(spread_norm)) spread_norm = 0.0; // [FIX-QUANT-5]
        spread_norm = std::fmin(spread_norm, 3.0);

        // depth_inv = fmax(0, 1 - fmin(depth_avg * inv_ref, 1.0))
        const double depth_avg = 0.5 * (bid_safe + ask_safe);
        depth_inv = 1.0 - std::fmin(depth_avg * dref_inv, 1.0);
        depth_inv = std::fmax(0.0, depth_inv);
        if (!std::isfinite(depth_inv)) depth_inv = 1.0; // [FIX-QUANT-5]: NaN → likidite yok

        // [FIX-QUANT-5]: ob_imbalance NaN → fmin(NaN,1.0)=1.0 → yanlış VETO
        // isfinite ile NaN ve Inf'i 0.0'a çek (muhafazakâr: imbalance bilinmiyor)
        ob_imbalance = std::isfinite(ob_imbalance)
                       ? std::fmin(std::fabs(ob_imbalance), 1.0)
                       : 0.0; // [FIX-QUANT-5]

        // [FIX-QUANT-5]: hmm_prob_high NaN → 0.0 (muhafazakâr: HMM katkısı yok)
        hmm_prob_high = std::isfinite(hmm_prob_high)
                        ? std::fmin(std::fmax(hmm_prob_high, 0.0), 1.0)
                        : 0.0; // [FIX-QUANT-5]
    }

    // Legacy shim
    HFT_ALWAYS_INLINE void compute() noexcept { compute_fast(); }
};

// ============================================================================
// §5  RiskScorer
// ============================================================================
class RiskScorer {
public:
    using Thresholds = RiskThresholds;

    static constexpr double kInv3 = 0.3333333333333333;

    // [PHASE1-FW] Ulasilamaz/NaN/sirasi bozuk esikleri [0,1]'e cek: allow <= caution <= pause <= 1.0
    [[nodiscard]] static RiskThresholds sanitize_thresholds(RiskThresholds t) noexcept {
        const RiskThresholds d{};
        auto unit = [](double x, double fb) noexcept {
            return std::isfinite(x) ? std::fmin(std::fmax(x, 0.0), 1.0) : fb;
        };
        t.allow   = unit(t.allow,   d.allow);
        t.caution = std::fmax(unit(t.caution, d.caution), t.allow);
        t.pause   = std::fmax(unit(t.pause,   d.pause),   t.caution);
        return t;
    }

    explicit RiskScorer(const RiskWeights& weights    = RiskWeights{},
                        const Thresholds&  thresholds = Thresholds{}) noexcept
        : w_(weights), t_(sanitize_thresholds(thresholds))
    {}

    // ── computeScore: sıfır bölme, sıfır branch ──────────────────────────────
    [[nodiscard]] HFT_ALWAYS_INLINE
    double computeScore(double vpink, double hmmProb, double imbalance,
                        double spread, double depth) const noexcept
    {
        const double vpin_c   = std::fmin(std::fmax(vpink, 0.0), 3.0) * kInv3;
        const double spread_t = std::fmin(spread * kInv3, 1.0);

        const double raw =
            w_.vpin_zscore     * vpin_c   +
            w_.hmm_prob        * hmmProb  +
            w_.ob_imbalance    * imbalance+
            w_.spread_width    * spread_t +
            w_.depth_liquidity * depth;

        return std::fmin(std::fmax(raw, 0.0), 1.0);
    }

    [[nodiscard]] HFT_ALWAYS_INLINE
    double computeFromContext(const RiskContext& ctx) const noexcept {
        return computeScore(ctx.vpin_zscore, ctx.hmm_prob_high,
                            ctx.ob_imbalance, ctx.spread_norm, ctx.depth_inv);
    }

    // ── decide_branchless: NaN → VETO (fail-closed) ──────────────────────────
    // [FIX-QUANT-DS]: NaN >= threshold her zaman false → idx=0 → ALLOW → UB.
    // score != score: IEEE 754 NaN self-inequality → doğrudan VETO.
    // Bu tek branch hot-path'te ihmal edilebilir maliyet (~1 ns).
    [[nodiscard]] HFT_ALWAYS_INLINE
    RiskDecision decide_branchless(double score) const noexcept {
        // [FIX-QUANT-DS]: NaN skoru → VETO (fail-closed davranış)
        if (score != score) return RiskDecision::VETO; // NaN self-inequality
        const uint32_t idx =
            static_cast<uint32_t>(score >= t_.allow)   +
            static_cast<uint32_t>(score >= t_.caution) +
            static_cast<uint32_t>(score >= t_.pause);
        return static_cast<RiskDecision>(idx);
    }

    // Legacy shim
    [[nodiscard]] HFT_ALWAYS_INLINE
    RiskDecision decide(double score) const noexcept {
        return decide_branchless(score);
    }

    // ── Result ───────────────────────────────────────────────────────────────
    struct Result {
        double       score    = 0.0;
        RiskDecision decision = RiskDecision::ALLOW;

        double vpin_contribution      = 0.0;
        double hmm_contribution       = 0.0;
        double imbalance_contribution = 0.0;
        double spread_contribution    = 0.0;
        double depth_contribution     = 0.0;

        [[nodiscard]] HFT_ALWAYS_INLINE
        bool can_trade() const noexcept {
            return static_cast<uint8_t>(decision) < 2u;
        }
    };

    // ── evaluate_fast: tek geçişte, NaN-safe, branchless ─────────────────────
    // [FIX-QUANT-GPT]: raw NaN → r.score = 1.0 (VETO/fail-closed).
    // Önceki kod: fmax(NaN, 0.0) = 0.0 → ALLOW → kontrolsüz işlem.
    [[nodiscard]] HFT_ALWAYS_INLINE
    Result evaluate_fast(RiskContext& ctx) const noexcept {
        ctx.compute_fast(); // [FIX-QUANT-5] NaN sanitization burada yapılır

        const double vpin_c   = std::fmin(std::fmax(ctx.vpin_zscore, 0.0), 3.0) * kInv3;
        const double spread_t = std::fmin(ctx.spread_norm * kInv3, 1.0);

        Result r;

        // [FIX-QUANT-GPT]: safe_mul — ağırlık veya katkı NaN ise 0.0 döndür
        auto safe_mul = [](double a, double b) noexcept -> double {
            if (!std::isfinite(a) || !std::isfinite(b)) return 0.0;
            const double v = a * b;
            return std::isfinite(v) ? v : 0.0;
        };

        r.vpin_contribution      = safe_mul(w_.vpin_zscore,     vpin_c);
        r.hmm_contribution       = safe_mul(w_.hmm_prob,        ctx.hmm_prob_high);
        r.imbalance_contribution = safe_mul(w_.ob_imbalance,    ctx.ob_imbalance);
        r.spread_contribution    = safe_mul(w_.spread_width,    spread_t);
        r.depth_contribution     = safe_mul(w_.depth_liquidity, ctx.depth_inv);

        const double raw = r.vpin_contribution      +
                           r.hmm_contribution       +
                           r.imbalance_contribution +
                           r.spread_contribution    +
                           r.depth_contribution;

        // [FIX-QUANT-GPT]: raw NaN → 1.0 (VETO/fail-closed); raw finite → clamp [0,1]
        r.score    = std::isfinite(raw) ? std::fmin(std::fmax(raw, 0.0), 1.0) : 1.0;
        r.decision = decide_branchless(r.score); // [FIX-QUANT-DS] NaN → VETO
        return r;
    }

    // Legacy shim
    [[nodiscard]] HFT_ALWAYS_INLINE
    Result evaluate(RiskContext& ctx) const noexcept {
        return evaluate_fast(ctx);
    }

    // ── Hot-reload ────────────────────────────────────────────────────────────
    void set_weights(const RiskWeights& w)   noexcept { w_ = w; }
    void set_thresholds(const Thresholds& t) noexcept { t_ = sanitize_thresholds(t); }
    [[nodiscard]] const RiskWeights& weights()    const noexcept { return w_; }
    [[nodiscard]] const Thresholds&  thresholds() const noexcept { return t_; }

    // ── EMA adaptif kalibrasyon — config/learning path, NOT hot-path ──────────
    void ema_update_weights(const Result& r, bool win_signal,
                            double alpha = 0.01) noexcept
    {
        const double sign = win_signal ? -1.0 : 1.0;
        auto nudge = [&](double& w, double contrib) noexcept {
            w = std::fmin(std::fmax(w + sign * alpha * contrib, 0.01), 0.99);
        };
        nudge(w_.vpin_zscore,     r.vpin_contribution);
        nudge(w_.hmm_prob,        r.hmm_contribution);
        nudge(w_.ob_imbalance,    r.imbalance_contribution);
        nudge(w_.spread_width,    r.spread_contribution);
        nudge(w_.depth_liquidity, r.depth_contribution);
        w_.normalize(); // [FIX-QUANT-GPT]: normalize NaN temizliği ile
    }

#ifdef KUSAK_RISK_DIAGNOSTICS
    void print_result(const Result& r) const;
#endif

private:
    RiskWeights w_;
    Thresholds  t_;
};

} // namespace kusak::research
