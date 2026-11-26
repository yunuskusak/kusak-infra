// ============================================================================
// KUSAK V12 — RESEARCH FRAMEWORK
// adverse_selection_tracker.hpp — Ters Seçim + Cancel/Fill Telemetrisi
//
// DEĞİŞİKLİKLER (V12 — Rapor §2):
//   ✓ CancelTelemetry yapısı: cancel_latency_us ölçümü ve loglama
//   ✓ FillTelemetry: sent/filled/canceled sayaçları
//   ✓ fill_rate(), cancel_rate() metotları
//   ✓ record_order_sent() / record_fill_confirmed() / record_cancel_with_latency()
//   ✓ <1ms cancel tespiti (pre-market cancel uyarısı)
//   ✓ print_telemetry_summary(): 100 emirde bir özet
//   ✓ Tüm orijinal TCA arayüzleri korundu
//
// RAPOR BULGUSU (Orta Öncelik):
//   "Pasif emirler 'picked off' durumuyla karşılaşıyor. Cancel latency
//   izlenmeli, fill rate / cancel rate sürekli raporlanmalıdır."
// ============================================================================

#pragma once

#include "research_types.hpp"
#include <deque>
#include <vector>
#include <functional>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <numeric>
#include <string>
#include <atomic>

namespace kusak::research {

// ============================================================================
// AdverseSelectionConfig
// ============================================================================

struct AdverseSelectionConfig {
    std::vector<uint64_t> measurement_windows_us = {
        10'000, 50'000, 100'000, 500'000, 1'000'000
    };

    // Erken iptal eşiği (bps)
    double cancel_threshold_bps   = 20.0;  // V12: 7.0->20.0 – scalping: %0.20 kaymaya kadar iptal etme

    // Cancel latency uyarı eşiği (µs)
    uint64_t pre_cancel_warn_us   = 1000;  // <1ms → agresif ön-iptal uyarısı

    // Telemetri penceresi (kaç emirde bir özet yazdırılır)
    int    telemetry_window       = 100;

    // TCA geçmişi
    int    history_size           = 5000;
    std::string report_path       = "adverse_selection_report.csv";
    bool   verbose                = false;

    // Cancel latency izleme penceresi (µs) — eski parametre uyumluluğu
    uint64_t cancel_track_window_us = 500'000;
};

// ============================================================================
// V12: Cancel / Fill Telemetri Verileri
// ============================================================================

struct CancelRecord {
    uint64_t send_ns        = 0;    // Emir gönderme zaman damgası (ns)
    uint64_t cancel_ns      = 0;    // İptal onay zaman damgası (ns)
    uint64_t latency_us     = 0;    // (cancel_ns - send_ns) / 1000
    double   price          = 0.0;
    bool     is_buy         = true;
    bool     pre_market     = false; // latency_us < pre_cancel_warn_us
};

// ============================================================================
// FillRecord (TCA için)
// ============================================================================

struct FillRecord {
    uint64_t fill_ts_us  = 0;
    double   fill_price  = 0.0;
    double   mid_at_fill = 0.0;
    double   qty         = 0.0;
    bool     is_buy      = true;
    bool     is_passive  = true;

    struct WindowMeasurement {
        uint64_t delay_us  = 0;
        double   mid_price = 0.0;
        double   adverse_bps = 0.0;
        bool     measured  = false;
    };
    std::vector<WindowMeasurement> windows;

    double realized_spread_bps      = 0.0;
    double adverse_selection_bps    = 0.0;
    double implementation_shortfall = 0.0;
    bool   tca_complete             = false;
};

// ============================================================================
// AdverseSelectionTracker — Ana Sınıf
// ============================================================================

class AdverseSelectionTracker {
public:
    using CancelSignalCallback = std::function<void(double adverse_bps)>;

    explicit AdverseSelectionTracker(AdverseSelectionConfig cfg = {})
        : cfg_(cfg) {}

    void set_cancel_callback(CancelSignalCallback cb) {
        cancel_cb_ = std::move(cb);
    }

    // ── V12: Emir gönderildiğinde çağrılır — send timestamp kaydeder ────
    void record_order_sent(uint64_t send_ns, double price, bool is_buy) noexcept {
        last_send_ns_  = send_ns;
        last_price_    = price;
        last_is_buy_   = is_buy;
        total_sent_++;
    }

    // ── V12: Emir dolduğunda çağrılır ────────────────────────────────────
    void record_fill_confirmed(uint64_t fill_ns) noexcept {
        (void)fill_ns;
        total_filled_++;
        maybe_print_telemetry();
    }

    // ── V12: İptal edildiğinde çağrılır — cancel latency hesaplar ────────
    // cancel_ns: Binance executionReport'tan gelen zaman (ns)
    // Eğer record_order_sent çağrılmışsa latency hesaplanır.
    CancelRecord record_cancel_with_latency(uint64_t cancel_ns,
                                             double price,
                                             bool is_buy) noexcept {
        CancelRecord rec;
        rec.send_ns   = last_send_ns_;
        rec.cancel_ns = cancel_ns;
        rec.price     = price;
        rec.is_buy    = is_buy;

        if (last_send_ns_ > 0 && cancel_ns >= last_send_ns_) {
            rec.latency_us = (cancel_ns - last_send_ns_) / 1000ULL;
        } else {
            rec.latency_us = 0;
        }

        rec.pre_market = (rec.latency_us < cfg_.pre_cancel_warn_us &&
                          last_send_ns_ > 0);

        total_canceled_++;
        cancel_history_.push_back(rec);
        if (cancel_history_.size() > 10000)
            cancel_history_.pop_front();

        maybe_print_telemetry();
        return rec;
    }

    // ── V12: Fill / Cancel oranları ──────────────────────────────────────
    double fill_rate() const noexcept {
        return (total_sent_ > 0)
            ? static_cast<double>(total_filled_) / static_cast<double>(total_sent_)
            : 0.0;
    }

    double cancel_rate() const noexcept {
        return (total_sent_ > 0)
            ? static_cast<double>(total_canceled_) / static_cast<double>(total_sent_)
            : 0.0;
    }

    uint64_t total_sent()     const noexcept { return total_sent_;    }
    uint64_t total_filled()   const noexcept { return total_filled_;  }
    uint64_t total_canceled() const noexcept { return total_canceled_;}

    // Son N cancel kaydının ortalama latency (µs)
    double avg_cancel_latency_us(int n = 50) const noexcept {
        if (cancel_history_.empty()) return 0.0;
        int cnt  = std::min(n, static_cast<int>(cancel_history_.size()));
        auto beg = cancel_history_.end() - cnt;
        double sum = 0.0;
        for (auto it = beg; it != cancel_history_.end(); ++it)
            sum += static_cast<double>(it->latency_us);
        return sum / cnt;
    }

    // Son N cancel kaydında pre-market (< threshold) olanların oranı
    double pre_market_cancel_rate(int n = 50) const noexcept {
        if (cancel_history_.empty()) return 0.0;
        int cnt  = std::min(n, static_cast<int>(cancel_history_.size()));
        auto beg = cancel_history_.end() - cnt;
        int pmc  = 0;
        for (auto it = beg; it != cancel_history_.end(); ++it)
            if (it->pre_market) pmc++;
        return static_cast<double>(pmc) / cnt;
    }

    // ── V12: Telemetri özet yazdırma ─────────────────────────────────────
    // Her telemetry_window emirde bir otomatik çağrılır.
    // Dışarıdan da çağrılabilir.
    void print_telemetry_summary() const {
        double fr  = fill_rate()   * 100.0;
        double cr  = cancel_rate() * 100.0;
        double alat = avg_cancel_latency_us();
        double pmr  = pre_market_cancel_rate() * 100.0;

        std::cout << "\n─── Adverse Selection / Fill Telemetrisi (V12) ──\n"
                  << std::fixed << std::setprecision(1)
                  << "  Gönderilen emir  : " << total_sent_    << "\n"
                  << "  Dolan emir       : " << total_filled_  << "\n"
                  << "  İptal edilen     : " << total_canceled_ << "\n"
                  << "  Fill Rate        : " << fr  << " %"
                  << (fr  < 40.0  ? "  ⚠ DÜŞÜK" : "  ✓") << "\n"
                  << "  Cancel Rate      : " << cr  << " %"
                  << (cr  > 60.0  ? "  ⚠ YÜKSEK" : "  ✓") << "\n"
                  << "  Ort. Cancel Lat  : " << alat << " µs"
                  << (alat < 1000.0 && total_canceled_ > 0 ? "  ⚠ PRE-MARKET" : "") << "\n"
                  << "  Pre-Market Cancel: " << pmr  << " %"
                  << (pmr > 50.0  ? "  ⚠ AGRESİF" : "") << "\n"
                  << "  cancel_thr_bps   : " << cfg_.cancel_threshold_bps << "\n"
                  << "──────────────────────────────────────────────────\n";
    }

    // ── Orijinal TCA arayüzleri (korundu) ────────────────────────────────

    size_t record_fill(uint64_t ts_us, double fill_price, double mid_price,
                       double qty, bool is_buy, bool is_passive = true) {
        FillRecord rec;
        rec.fill_ts_us  = ts_us;
        rec.fill_price  = fill_price;
        rec.mid_at_fill = mid_price;
        rec.qty         = qty;
        rec.is_buy      = is_buy;
        rec.is_passive  = is_passive;

        if (is_buy)
            rec.implementation_shortfall = (fill_price - mid_price) / mid_price * 10000.0;
        else
            rec.implementation_shortfall = (mid_price - fill_price) / mid_price * 10000.0;

        rec.windows.resize(cfg_.measurement_windows_us.size());
        for (size_t i = 0; i < cfg_.measurement_windows_us.size(); ++i)
            rec.windows[i].delay_us = cfg_.measurement_windows_us[i];

        pending_.push_back(std::move(rec));
        return pending_.size() - 1;
    }

    void update_mid(uint64_t ts_us, double mid_price) {
        auto it = pending_.begin();
        while (it != pending_.end()) {
            bool all_measured = true;
            for (auto& w : it->windows) {
                if (w.measured) continue;
                if (ts_us >= it->fill_ts_us + w.delay_us) {
                    w.mid_price   = mid_price;
                    w.measured    = true;
                    if (it->is_buy)
                        w.adverse_bps = (it->fill_price - mid_price) / mid_price * 10000.0;
                    else
                        w.adverse_bps = (mid_price - it->fill_price) / it->fill_price * 10000.0;
                } else {
                    all_measured = false;
                }
            }
            if (all_measured) {
                finalize_fill(*it);
                completed_.push_back(*it);
                if (completed_.size() > static_cast<size_t>(cfg_.history_size))
                    completed_.pop_front();
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Bekleyen pasif emir için drift kontrolü
    void monitor_pending_order(uint64_t order_ts_us, double order_price,
                                double current_mid, uint64_t now_us,
                                bool is_buy) {
        double drift_bps;
        if (is_buy)
            drift_bps = (current_mid - order_price) / order_price * 10000.0;
        else
            drift_bps = (order_price - current_mid) / current_mid * 10000.0;

        uint64_t wait_us = now_us - order_ts_us;
        if (drift_bps > cfg_.cancel_threshold_bps && wait_us > 50'000) {
            if (cancel_cb_) cancel_cb_(drift_bps);
        }
    }

    // cancel uyum metodu (eski çağrı imzası — research_main.cpp uyumu)
    void record_cancel(uint64_t cancel_ns, double price, bool is_buy) noexcept {
        record_cancel_with_latency(cancel_ns, price, is_buy);
    }

    struct AdverseSelectionStats {
        double avg_adverse_100ms_bps    = 0.0;
        double avg_adverse_500ms_bps    = 0.0;
        double avg_realized_spread_bps  = 0.0;
        double avg_impl_shortfall_bps   = 0.0;
        double pick_off_rate            = 0.0;
        int    n_fills                  = 0;
    };

    AdverseSelectionStats compute_stats(int last_n = 500) const {
        AdverseSelectionStats stats;
        if (completed_.empty()) return stats;

        int n     = std::min(last_n, static_cast<int>(completed_.size()));
        auto beg  = completed_.end() - n;
        double sum_adv100 = 0.0, sum_adv500 = 0.0;
        double sum_rs = 0.0, sum_is = 0.0;
        int n_pickoff = 0;

        for (auto it = beg; it != completed_.end(); ++it) {
            if (it->windows.size() > 2 && it->windows[2].measured)
                sum_adv100 += it->windows[2].adverse_bps;
            if (it->windows.size() > 3 && it->windows[3].measured)
                sum_adv500 += it->windows[3].adverse_bps;
            sum_rs += it->realized_spread_bps;
            sum_is += it->implementation_shortfall;
            if (it->adverse_selection_bps > 1.0) n_pickoff++;
        }

        stats.avg_adverse_100ms_bps   = sum_adv100 / n;
        stats.avg_adverse_500ms_bps   = sum_adv500 / n;
        stats.avg_realized_spread_bps = sum_rs / n;
        stats.avg_impl_shortfall_bps  = sum_is / n;
        stats.pick_off_rate           = 100.0 * n_pickoff / n;
        stats.n_fills                 = n;
        return stats;
    }

    void write_report() const {
        if (cfg_.report_path.empty()) return;
        std::ofstream f(cfg_.report_path);
        if (!f.is_open()) return;

        f << "fill_ts_us,side,fill_price,mid_at_fill,"
             "impl_shortfall_bps,realized_spread_bps,adverse_sel_bps\n";
        for (auto& rec : completed_) {
            f << rec.fill_ts_us << ","
              << (rec.is_buy ? "BUY" : "SELL") << ","
              << rec.fill_price << ","
              << rec.mid_at_fill << ","
              << rec.implementation_shortfall << ","
              << rec.realized_spread_bps << ","
              << rec.adverse_selection_bps << "\n";
        }
    }

    void print_summary() const {
        auto s = compute_stats();
        std::cout << "\n─── Ters Seçim TCA Özeti ──────────────────────\n"
                  << std::fixed << std::setprecision(3)
                  << "  Fill sayısı             : " << s.n_fills << "\n"
                  << "  Ort. ters seçim 100ms   : " << s.avg_adverse_100ms_bps << " bps\n"
                  << "  Ort. ters seçim 500ms   : " << s.avg_adverse_500ms_bps << " bps\n"
                  << "  Ort. uygulama açığı     : " << s.avg_impl_shortfall_bps << " bps\n"
                  << "  Pick-off oranı          : " << s.pick_off_rate << " %\n"
                  << "──────────────────────────────────────────────\n";
        print_telemetry_summary();
    }

private:
    // Otomatik telemetri (telemetry_window'da bir)
    void maybe_print_telemetry() noexcept {
        uint64_t total = total_sent_ + total_filled_ + total_canceled_;
        if (cfg_.telemetry_window > 0 && total > 0 &&
            total % static_cast<uint64_t>(cfg_.telemetry_window) == 0) {
            print_telemetry_summary();
        }
    }

    void finalize_fill(FillRecord& rec) {
        if (!rec.windows.empty() && rec.windows[0].measured) {
            if (rec.is_buy)
                rec.realized_spread_bps = (rec.windows[0].mid_price - rec.fill_price)
                                          / rec.fill_price * 10000.0;
            else
                rec.realized_spread_bps = (rec.fill_price - rec.windows[0].mid_price)
                                          / rec.fill_price * 10000.0;
        }

        double max_adv = 0.0;
        for (auto& w : rec.windows)
            if (w.measured && w.adverse_bps > max_adv) max_adv = w.adverse_bps;

        rec.adverse_selection_bps = max_adv;
        rec.tca_complete          = true;

        if (cfg_.verbose) {
            std::cout << "[TCA] Fill @" << rec.fill_price
                      << " IS="  << rec.implementation_shortfall << " bps"
                      << " AS="  << rec.adverse_selection_bps    << " bps\n";
        }
    }

    AdverseSelectionConfig   cfg_;
    CancelSignalCallback      cancel_cb_;

    // TCA
    std::deque<FillRecord>   pending_;
    std::deque<FillRecord>   completed_;

    // V12: Fill / Cancel Telemetrisi
    uint64_t   total_sent_     = 0;
    uint64_t   total_filled_   = 0;
    uint64_t   total_canceled_ = 0;

    // Son emir send timestamp (record_cancel_with_latency için)
    uint64_t   last_send_ns_   = 0;
    double     last_price_     = 0.0;
    bool       last_is_buy_    = true;

    std::deque<CancelRecord> cancel_history_;
};

}  // namespace kusak::research
