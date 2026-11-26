// ============================================================================
// KUSAK V9 — RESEARCH FRAMEWORK
// slippage_models.hpp — Gerçekçi Slippage Modelleri
//
// Rapor §1: Mevcut sabit "1 tick" slippage modeli büyük emirlerde gerçekçi
// sonuç vermez. Bu dosya üç gelişmiş model sunar:
//
//   1. FixedBasisPointsSlippage  — Sabit baz puan cinsinden
//   2. VolumeShareSlippage       — Hacim payı bazlı quadratic model (Zipline)
//   3. VolatilityVolumeShare     — Zipline MI = η·σ·√ψ formülü
//
// Kullanım:
//   SlippageConfig cfg;
//   cfg.model = SlippageModel::VOLUME_SHARE;
//   cfg.volume_share.price_impact = 0.1;
//   double slip = compute_slippage(cfg, order_qty, volume, atr, mid_price, is_buy);
//   double fill_price = mid_price + (is_buy ? +slip : -slip);
//
// Thread safety: tüm fonksiyonlar stateless & pure — thread-safe.
// ============================================================================

#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <string>

namespace kusak::research {

// ============================================================================
// §1  Slippage Model Enum
// ============================================================================

enum class SlippageModel : uint8_t {
    FIXED_TICKS          = 0,   // Sabit tick (eski davranış, varsayılan)
    FIXED_BASIS_POINTS   = 1,   // Sabit baz puan (FixedBasisPointsSlippage)
    VOLUME_SHARE         = 2,   // Quadratic hacim payı (VolumeShareSlippage)
    VOLATILITY_VOLUME    = 3,   // η·σ·√ψ market-impact (VolatilityVolumeShare)
};

inline std::string slippage_model_name(SlippageModel m) {
    switch (m) {
        case SlippageModel::FIXED_TICKS:        return "FixedTicks";
        case SlippageModel::FIXED_BASIS_POINTS: return "FixedBasisPoints";
        case SlippageModel::VOLUME_SHARE:       return "VolumeShare";
        case SlippageModel::VOLATILITY_VOLUME:  return "VolatilityVolume";
    }
    return "Unknown";
}

// ============================================================================
// §2  Model Parametreleri
// ============================================================================

struct FixedTicksParams {
    double tick_size    = 0.01;    // fiyatın yüzdesi olarak 1 tick = 0.0001 = %0.01
};

struct FixedBasisPointsParams {
    double basis_points  = 5.0;    // örn. 5 bps → 0.0005
    double volume_limit  = 0.25;   // emir, ilgili bar hacminin max %25'i olabilir
};

struct VolumeShareParams {
    double price_impact   = 0.1;    // katsayı b (quadratic: slippage = b × ψ²)
    double volume_limit   = 0.25;   // hacim sınırı ψ_max
    double min_slippage   = 0.0001; // minimum slippage alt sınırı
};

struct VolatilityVolumeParams {
    double eta            = 0.1;    // η — piyasa etkisi katsayısı
    double volume_limit   = 0.25;   // ψ_max — maksimum hacim payı
    double min_slippage   = 0.0001;
};

// ============================================================================
// §3  Birleşik SlippageConfig
// ============================================================================

struct SlippageConfig {
    SlippageModel model = SlippageModel::FIXED_TICKS;

    FixedTicksParams         fixed_ticks;
    FixedBasisPointsParams   basis_points;
    VolumeShareParams        volume_share;
    VolatilityVolumeParams   vol_volume;
};

// ============================================================================
// §4  Slippage Hesaplama Fonksiyonları
//
//   Tüm fonksiyonlar:
//     order_qty  : emir büyüklüğü (baz varlık cinsinden)
//     bar_volume : ilgili bar/penceredeki toplam işlem hacmi
//     sigma      : volatilite (ATR / mid_price normalize)
//     mid_price  : orta fiyat
//     is_buy     : alış emri mi?
//
//   Return: pozitif slippage miktarı (USDT cinsinden fiyat kayması)
//   Uygulama: fill_price = mid_price ± slippage
// ============================================================================

// §4.1  Sabit Tick
inline double slippage_fixed_ticks(const FixedTicksParams& p,
                                    double mid_price) noexcept {
    return mid_price * p.tick_size * 0.01; // tick_size bps → oran
}

// §4.2  Sabit Baz Puan
//   fill_price_buy  = price × (1 + bps/10000)
//   fill_price_sell = price × (1 - bps/10000)
//   Hacim sınırı: emir bu bar hacminin volume_limit'inden fazlasına dokunursa
//   slippage proportionally scale edilir (basit lineer).
inline double slippage_fixed_basis_points(const FixedBasisPointsParams& p,
                                           double order_qty,
                                           double bar_volume,
                                           double mid_price) noexcept {
    double bps = p.basis_points;

    // Hacim sınırı aşılıyorsa slippage artır
    if (bar_volume > 1e-12) {
        double volume_share = order_qty / bar_volume;
        if (volume_share > p.volume_limit) {
            // Sınırın üstünde: doğrusal ölçekleme
            double ratio = volume_share / p.volume_limit;
            bps *= ratio;
        }
    }

    return mid_price * (bps / 10000.0);
}

// §4.3  VolumeShare (Zipline benzeri)
//   slippage = price_impact × (ψ²)  ;  ψ = order_qty / bar_volume
//   ψ, volume_limit ile kırpılır (emir hacim sınırını aşamaz).
//   Referans: Zipline VolumeShareSlippage
inline double slippage_volume_share(const VolumeShareParams& p,
                                     double order_qty,
                                     double bar_volume,
                                     double mid_price) noexcept {
    if (bar_volume < 1e-12) {
        return mid_price * p.min_slippage;
    }

    double psi = std::min(order_qty / bar_volume, p.volume_limit);
    double slip = p.price_impact * (psi * psi) * mid_price;
    return std::max(slip, mid_price * p.min_slippage);
}

// §4.4  VolatilityVolumeShare (Zipline benzeri)
//   MI = η × σ × √ψ
//   σ = ATR / mid_price (normalize volatilite)
//   ψ = order_qty / bar_volume (hacim payı, volume_limit ile kırpılır)
//   Referans: Zipline VolatilityVolumeShare
inline double slippage_volatility_volume(const VolatilityVolumeParams& p,
                                          double order_qty,
                                          double bar_volume,
                                          double atr,
                                          double mid_price) noexcept {
    if (bar_volume < 1e-12 || mid_price < 1e-12) {
        return mid_price * p.min_slippage;
    }

    double sigma = atr / mid_price;                           // normalize volatilite
    double psi   = std::min(order_qty / bar_volume, p.volume_limit);
    double mi    = p.eta * sigma * std::sqrt(psi);           // MI = η·σ·√ψ
    double slip  = mi * mid_price;
    return std::max(slip, mid_price * p.min_slippage);
}

// ============================================================================
// §5  Birleşik dispatch — Backtester tarafından çağrılır
// ============================================================================

inline double compute_slippage(const SlippageConfig& cfg,
                                double order_qty,
                                double bar_volume,
                                double atr,
                                double mid_price,
                                bool   /*is_buy*/) noexcept {
    switch (cfg.model) {
        case SlippageModel::FIXED_TICKS:
            return slippage_fixed_ticks(cfg.fixed_ticks, mid_price);

        case SlippageModel::FIXED_BASIS_POINTS:
            return slippage_fixed_basis_points(cfg.basis_points,
                                               order_qty, bar_volume, mid_price);

        case SlippageModel::VOLUME_SHARE:
            return slippage_volume_share(cfg.volume_share,
                                          order_qty, bar_volume, mid_price);

        case SlippageModel::VOLATILITY_VOLUME:
            return slippage_volatility_volume(cfg.vol_volume,
                                               order_qty, bar_volume, atr, mid_price);
    }
    // Fallback
    return mid_price * 0.0001;
}

// ============================================================================
// §6  Slippage Raporu — optimizasyon sonuçlarına eklenir
// ============================================================================

struct SlippageStats {
    double total_slippage_cost = 0.0;  // kümülatif slippage maliyeti
    double avg_slippage_bps    = 0.0;  // ortalama bps
    int    n_trades            = 0;

    void record(double slip, double mid_price) {
        if (mid_price > 1e-12) {
            total_slippage_cost += slip;
            avg_slippage_bps = (avg_slippage_bps * n_trades +
                                (slip / mid_price * 10000.0)) / (n_trades + 1);
            n_trades++;
        }
    }
};

} // namespace kusak::research
