// ============================================================================
// KUSAK V9 — RESEARCH FRAMEWORK
// paper_trader.hpp — Paper Trading (Kağıt Ticaret) Modu
//
// Rapor §8: Bot canlı veri akışından (WebSocket) fiyat alır, emirleri
// simüle eder; gerçek işlem YAPILMAZ. Amaç: canlı ortamda sistemi
// gerçek para riske atmadan test etmek.
//
// Özellikler:
//   - Gerçek zamanlı tick akışı üzerinden sinyal + pozisyon simülasyonu
//   - İşlem ücreti ve slippage gerçekçi biçimde uygulanır
//   - Tüm performans metrikleri canlı takiple izlenir
//   - Bildirim sistemi (Notifier) ile entegre: her işlem Telegram/Slack'e gider
//   - Periyodik özet rapor (n tick'te bir veya zaman bazlı)
//   - --paper CLI modu ile aktif edilir
//
// Kullanım:
//   PaperTraderConfig cfg;
//   cfg.params = best_params_from_optimize;
//   cfg.symbol = "BTCUSDT";
//   PaperTrader trader(cfg, notifier_ptr);
//   trader.on_tick(rec); // WebSocket callback'inden çağrılır
//   trader.print_summary();
//
// Thread safety: on_tick() tek thread'den çağrılmalıdır.
// ============================================================================

#pragma once

#include "research_types.hpp"
#include "slippage_models.hpp"
#include "notification.hpp"
#include "vpin_velocity_tracker.hpp"

#include <vector>
#include <deque>
#include <optional>
#include <string>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <chrono>
#include <cassert>

namespace kusak::research {

// ============================================================================
// §0  RollingVolumeTracker — paper_trader için (backtester.hpp'den bağımsız)
// ============================================================================

class RollingVolumeTrackerPT {
public:
    explicit RollingVolumeTrackerPT(size_t w = 100) : window_(w) {}
    void push(double v) {
        if (v <= 0.0) return;
        buf_.push_back(v); sum_ += v;
        if (buf_.size() > window_) { sum_ -= buf_.front(); buf_.pop_front(); }
    }
    double average() const { return buf_.empty() ? 0.0 : sum_ / buf_.size(); }
    size_t count()   const { return buf_.size(); }
    void   reset()         { buf_.clear(); sum_ = 0.0; }
private:
    size_t window_;
    std::deque<double> buf_;
    double sum_ = 0.0;
};

// ============================================================================
// §0b  PaperBook — paper_trader'a ozel hafif order book (hft_models_v9.hpp'deki
//      OrderBook'a bagimlilik kaldirildi; o baslik GCC'de derlenmiyor)
// ============================================================================

struct PaperBook {
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;

    PaperBook() { bids.reserve(MAX_DEPTH); asks.reserve(MAX_DEPTH); }

    double getBestBid() const noexcept { return bids.empty() ? 0.0 : bids.front().price; }
    double getBestAsk() const noexcept { return asks.empty() ? 0.0 : asks.front().price; }
    double getMid() const noexcept {
        const double b = getBestBid(), a = getBestAsk();
        return (b > 0.0 && a > 0.0) ? 0.5 * (b + a) : 0.0;
    }
    double totalBidDepth(int n) const noexcept {
        double s = 0.0;
        for (int i = 0; i < n && i < static_cast<int>(bids.size()); ++i) s += bids[i].quantity;
        return s;
    }
    double totalAskDepth(int n) const noexcept {
        double s = 0.0;
        for (int i = 0; i < n && i < static_cast<int>(asks.size()); ++i) s += asks[i].quantity;
        return s;
    }
};

// ============================================================================
// §1  Paper Trade Konfigürasyonu
// ============================================================================

struct PaperTraderConfig {
    ResearchParams params;           // optimize edilmiş parametreler
    std::string    symbol = "BTCUSDT";
    double         initial_capital = 1000.0;  // USDT

    // Pozisyon boyutu
    double         qty_per_trade   = 0.01;
    double         buy_pct         = 0.20;   // >0 ise capital*buy_pct kullan  [VARSAYILAN GÜNCELLENDİ: 0.0→0.20]
    double         kelly_fraction  = 0.0;    // >0 ise Kelly kriteri uygula

    // Ücret
    double         maker_fee       = 0.0002;
    double         taker_fee       = 0.0004;

    // Exit
    double         tp_atr_mult     = 0.3;   // [VARSAYILAN GÜNCELLENDİ: 1.5→0.3]
    double         sl_atr_mult     = 0.2;   // [VARSAYILAN GÜNCELLENDİ: 1.0→0.2]
    int            max_hold_ticks  = 50;    // [VARSAYILAN GÜNCELLENDİ: 2000→50]

    // Slippage
    SlippageConfig slippage;

    // Circuit breaker
    double         max_drawdown_pct  = 0.20;  // %20 çekimde dur
    double         max_position_pct  = 0.30;  // tek pozisyon max %30 sermaye  [YENİ ALAN]
    bool           circuit_breaker_enabled = false; // scalping'de devre dışı  [YENİ ALAN]
    int            max_orders_per_day= 200;   // günlük max emir

    // Raporlama
    int            report_every_n    = 1000;  // N tick'te bir konsola yaz
    bool           verbose           = true;
};

// ============================================================================
// §2  Paper Pozisyon
// ============================================================================

struct PaperPosition {
    enum class State { FLAT, LONG, SHORT } state = State::FLAT;

    double   entry_price   = 0.0;
    double   qty           = 0.0;
    double   tp_price      = 0.0;
    double   sl_price      = 0.0;
    uint64_t entry_ts      = 0;

    // Trailing stop
    double   extreme_price  = 0.0;
    bool     trailing_active= false;

    bool flat() const { return state == State::FLAT; }
};

// ============================================================================
// §3  Paper Trader
// ============================================================================

class PaperTrader {
public:
    using NotifierPtr = kusak::notify::Notifier*;

    explicit PaperTrader(PaperTraderConfig cfg,
                          NotifierPtr notifier = nullptr)
        : cfg_(std::move(cfg))
        , notifier_(notifier)
        , capital_(cfg_.initial_capital)
        , vpin_(make_research_vpin_config(cfg_.params.vpin_buckets, 0.8))
        , atr_(14)
        , bar_builder_(60000)
        , vol_tracker_(100)
    {
        start_time_ = std::chrono::steady_clock::now();
        if (cfg_.verbose)
            std::cout << "[PaperTrader] Başlatıldı. Sembol=" << cfg_.symbol
                      << "  Başlangıç sermaye=" << cfg_.initial_capital << " USDT\n";
    }

    // ── Ana tick işleyici — WebSocket callback'inden çağrılır ────────────
    void on_tick(const TickRecord& rec) {
        tick_count_++;

        // 1. Order book güncelle
        if (tick_is_book(rec)) {
            update_book(rec);
        }

        // 2. Trade tick → ATR + hacim
        if (tick_is_trade(rec)) {
            vpin_.update_trade_f(rec.trade_qty, rec.trade_price,
                                 tick_aggressor_is_buy(rec), rec.ts_exchange);
            last_vpin_ = vpin_.vpin_f();
            vol_tracker_.push(rec.trade_qty);

            int64_t ts_ms = static_cast<int64_t>(rec.ts_exchange / 1000);
            auto bar = bar_builder_.onTrade(rec.trade_price, rec.trade_qty, ts_ms);
            if (bar.has_value())
                atr_.onBar(bar->high, bar->low, bar->close);
        }

        // 3. Circuit breaker kontrol
        if (circuit_broken_) return;
        if (check_circuit_breaker()) return;

        // 4. Açık pozisyon → çıkış kontrolü
        if (!pos_.flat()) {
            tick_since_entry_++;
            check_exit(rec);
        } else {
            // 5. Giriş sinyali değerlendir
            evaluate_entry(rec);
        }

        // 6. Periyodik rapor
        if (cfg_.verbose && tick_count_ % cfg_.report_every_n == 0)
            print_summary();
    }

    void print_summary() const {
        double el = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_time_).count();

        int wins = 0;
        for (double p : trade_pnls_) if (p > 0) wins++;
        double win_rate = trade_pnls_.empty() ? 0.0
            : static_cast<double>(wins) / trade_pnls_.size();

        std::cout << "\n╔══ PAPER TRADING ÖZET ══════════════════════════\n"
                  << "║ Sembol     : " << cfg_.symbol << "\n"
                  << "║ Tick sayısı: " << tick_count_ << "\n"
                  << "║ İşlem sayısı: " << trade_pnls_.size() << "\n"
                  << "║ Kümül PnL  : " << std::fixed << std::setprecision(4) << cum_pnl_ << " USDT\n"
                  << "║ Sermaye    : " << capital_ << " USDT\n"
                  << "║ Win Rate   : " << std::setprecision(1) << win_rate * 100 << "%\n"
                  << "║ Max DD     : " << max_drawdown_ << " USDT\n"
                  << "║ Süre       : " << std::setprecision(1) << el << " s\n"
                  << "╚═══════════════════════════════════════════════\n\n";
    }

    // ── Getters ──────────────────────────────────────────────────────────
    double   cum_pnl()       const { return cum_pnl_;         }
    double   capital()       const { return capital_;          }
    uint64_t tick_count()    const { return tick_count_;       }
    int      trade_count()   const { return (int)trade_pnls_.size(); }
    bool     circuit_broken()const { return circuit_broken_;   }

    BacktestResult get_result() const {
        BacktestResult r;
        r.total_trades = static_cast<int>(trade_pnls_.size());
        r.net_pnl      = cum_pnl_;
        r.max_drawdown = max_drawdown_;
        r.valid        = r.total_trades >= 5;

        if (!trade_pnls_.empty()) {
            double sum = 0.0, tot_gain = 0.0, tot_loss = 0.0;
            int wins = 0;
            for (double p : trade_pnls_) {
                sum += p;
                if (p > 0) { wins++; tot_gain += p; }
                else        { tot_loss += -p; }
            }
            r.avg_trade_pnl = sum / trade_pnls_.size();
            r.win_rate      = static_cast<double>(wins) / trade_pnls_.size();
            r.total_gain    = tot_gain;
            r.total_loss    = tot_loss;
            r.profit_factor = (tot_loss > 1e-12) ? tot_gain / tot_loss : 0.0;
            r.winning_trades = wins;
            r.losing_trades  = static_cast<int>(trade_pnls_.size()) - wins;

            double var = 0.0;
            double mean = r.avg_trade_pnl;
            for (double p : trade_pnls_) var += (p - mean) * (p - mean);
            var /= trade_pnls_.size();
            double std = std::sqrt(var + 1e-12);
            r.sharpe_ratio = (std > 0) ? (mean / std) * std::sqrt(252.0 * 500.0) : 0.0;
            r.sharpe = r.sharpe_ratio;
        }
        return r;
    }

private:
    // ── Order book güncelle ───────────────────────────────────────────────
    void update_book(const TickRecord& rec) {
        book_.bids.clear(); book_.asks.clear();
        const int nb = std::min<int>(static_cast<int>(rec.bid_count), MAX_DEPTH);
        const int na = std::min<int>(static_cast<int>(rec.ask_count), MAX_DEPTH);
        for (int i = 0; i < nb; ++i)
            if (rec.bids[i].price > 0) book_.bids.push_back(PriceLevel(rec.bids[i].price, rec.bids[i].quantity));
        for (int i = 0; i < na; ++i)
            if (rec.asks[i].price > 0) book_.asks.push_back(PriceLevel(rec.asks[i].price, rec.asks[i].quantity));
    }

    // ── Giriş değerlendirme ───────────────────────────────────────────────
    void evaluate_entry(const TickRecord& rec) {
        if (last_vpin_ > cfg_.params.vpin_threshold) return;
        if (book_.bids.empty() || book_.asks.empty()) return;
        if (daily_order_count_ >= cfg_.max_orders_per_day) return;

        double bid_d = book_.totalBidDepth(5);
        double ask_d = book_.totalAskDepth(5);
        double total = bid_d + ask_d;
        if (total < 1e-12) return;
        double imbalance = (bid_d - ask_d) / total;

        if (std::fabs(imbalance) < cfg_.params.imbalance_threshold) return;

        double atr_val = atr_.value();
        if (atr_val <= 0.0) return;

        bool go_long  = imbalance >  cfg_.params.imbalance_threshold;
        bool go_short = imbalance < -cfg_.params.imbalance_threshold;

        double qty = compute_qty();
        if (qty <= 0.0) return;

        double mid = book_.getMid();
        double slip= compute_slippage(cfg_.slippage, qty,
                                       vol_tracker_.average(),
                                       atr_val, mid, go_long);

        if (go_long) {
            double fill_price = book_.getBestAsk() + slip;
            open_position(fill_price, true, qty, atr_val, rec.ts_exchange);
        } else if (go_short) {
            double fill_price = book_.getBestBid() - slip;
            open_position(fill_price, false, qty, atr_val, rec.ts_exchange);
        }
    }

    // ── Pozisyon aç ───────────────────────────────────────────────────────
    void open_position(double fill_price, bool is_long, double qty,
                        double atr_val, uint64_t ts) {
        double fee = fill_price * qty * cfg_.taker_fee;
        pos_.state       = is_long ? PaperPosition::State::LONG
                                   : PaperPosition::State::SHORT;
        pos_.entry_price = fill_price;
        pos_.qty         = qty;
        pos_.entry_ts    = ts;
        pos_.tp_price    = is_long ? fill_price + cfg_.tp_atr_mult * atr_val
                                   : fill_price - cfg_.tp_atr_mult * atr_val;
        pos_.sl_price    = is_long ? fill_price - cfg_.sl_atr_mult * atr_val
                                   : fill_price + cfg_.sl_atr_mult * atr_val;
        pos_.extreme_price   = fill_price;
        pos_.trailing_active = false;
        cum_pnl_ -= fee;
        capital_ -= fee;
        tick_since_entry_ = 0;
        daily_order_count_++;

        if (notifier_)
            notifier_->send_trade_open(fill_price, qty, is_long, cfg_.symbol, cum_pnl_);
    }

    // ── Çıkış kontrol ─────────────────────────────────────────────────────
    void check_exit(const TickRecord& rec) {
        if (book_.bids.empty() || book_.asks.empty()) return;

        const bool is_long = (pos_.state == PaperPosition::State::LONG);
        double best_bid = book_.getBestBid();
        double best_ask = book_.getBestAsk();

        // Trailing stop
        if (is_long && best_bid > pos_.extreme_price) pos_.extreme_price = best_bid;
        if (!is_long && best_ask < pos_.extreme_price) pos_.extreme_price = best_ask;

        double act_bps  = cfg_.params.trailing_activation_bps;
        double call_bps = cfg_.params.trailing_callback_bps;
        if (call_bps > 0.0) {
            if (!pos_.trailing_active) {
                double act_p = is_long
                    ? pos_.entry_price * (1.0 + act_bps * 1e-4)
                    : pos_.entry_price * (1.0 - act_bps * 1e-4);
                if ( is_long && best_bid >= act_p) pos_.trailing_active = true;
                if (!is_long && best_ask <= act_p) pos_.trailing_active = true;
            }
            if (pos_.trailing_active) {
                bool triggered = is_long
                    ? best_bid < pos_.extreme_price * (1.0 - call_bps * 1e-4)
                    : best_ask > pos_.extreme_price * (1.0 + call_bps * 1e-4);
                if (triggered) { close_position(rec, "trailing"); return; }
            }
        }

        bool hit_tp  = is_long ? best_ask >= pos_.tp_price : best_bid <= pos_.tp_price;
        bool hit_sl  = is_long ? best_bid <= pos_.sl_price : best_ask >= pos_.sl_price;
        bool timeout = tick_since_entry_ >= cfg_.max_hold_ticks;

        if (hit_tp || hit_sl || timeout)
            close_position(rec, hit_tp ? "tp" : (hit_sl ? "sl" : "timeout"));
    }

    // ── Pozisyon kapat ────────────────────────────────────────────────────
    void close_position(const TickRecord& rec, const std::string& reason) {
        const bool is_long = (pos_.state == PaperPosition::State::LONG);
        double exit_price  = is_long ? book_.getBestBid() : book_.getBestAsk();
        if (exit_price <= 0.0) exit_price = book_.getMid();

        double gross = is_long
            ? (exit_price - pos_.entry_price) * pos_.qty
            : (pos_.entry_price - exit_price) * pos_.qty;
        double fee    = exit_price * pos_.qty * cfg_.taker_fee;
        double net    = gross - fee;

        cum_pnl_ += net;
        capital_ += net;
        trade_pnls_.push_back(net);

        if (cum_pnl_ > peak_pnl_) peak_pnl_ = cum_pnl_;
        double dd = peak_pnl_ - cum_pnl_;
        if (dd > max_drawdown_) max_drawdown_ = dd;

        pos_.state = PaperPosition::State::FLAT;

        if (notifier_)
            notifier_->send_trade_close(exit_price, net, reason, cfg_.symbol, cum_pnl_);
    }

    // ── Pozisyon büyüklüğü ────────────────────────────────────────────────
    double compute_qty() const {
        // Kelly kriteri
        if (cfg_.kelly_fraction > 0.0 && !trade_pnls_.empty()) {
            int wins = 0;
            double tot_win = 0.0, tot_loss = 0.0;
            for (double p : trade_pnls_) {
                if (p > 0) { wins++; tot_win += p; }
                else        { tot_loss += -p; }
            }
            double win_prob = static_cast<double>(wins) / trade_pnls_.size();
            double avg_win  = wins > 0 ? tot_win / wins : 1.0;
            double avg_loss = (trade_pnls_.size() - wins) > 0
                              ? tot_loss / (trade_pnls_.size() - wins) : 1.0;
            double b   = avg_win / std::max(avg_loss, 1e-12);
            double kelly= (b * win_prob - (1.0 - win_prob)) / b;
            kelly = std::clamp(kelly * cfg_.kelly_fraction, 0.01, 0.50);
            double notional = capital_ * kelly;
            double mid = book_.getMid();
            return (mid > 0.0) ? notional / mid : cfg_.qty_per_trade;
        }
        // Sermaye yüzdesi
        if (cfg_.buy_pct > 0.0) {
            double notional = capital_ * cfg_.buy_pct;
            double mid = book_.getMid();
            return (mid > 0.0) ? notional / mid : cfg_.qty_per_trade;
        }
        return cfg_.qty_per_trade;
    }

    // ── Circuit breaker ───────────────────────────────────────────────────
    bool check_circuit_breaker() {
        if (capital_ <= 0.0) {
            trigger_circuit_breaker("Sermaye sıfırlandı!", 100.0);
            return true;
        }
        double dd_pct = (peak_pnl_ - cum_pnl_) / std::max(cfg_.initial_capital, 1.0);
        if (dd_pct >= cfg_.max_drawdown_pct) {
            trigger_circuit_breaker("Max drawdown aşıldı!", dd_pct * 100.0);
            return true;
        }
        return false;
    }

    void trigger_circuit_breaker(const std::string& reason, double dd_pct) {
        if (circuit_broken_) return;
        circuit_broken_ = true;
        // Açık pozisyon varsa kapat (safety)
        if (!pos_.flat()) {
            TickRecord dummy{};   // close_position fiyatlari book_'tan alir
            close_position(dummy, "circuit_break");
        }
        std::cerr << "\n[PaperTrader] ⚠️ CIRCUIT BREAKER: " << reason
                  << " DD=%" << std::fixed << std::setprecision(2) << dd_pct << "\n";
        if (notifier_)
            notifier_->send_circuit_breaker(reason, dd_pct);
    }

    // ── Üyeler ───────────────────────────────────────────────────────────
    PaperTraderConfig   cfg_;
    NotifierPtr         notifier_;
    double              capital_;
    double              cum_pnl_     = 0.0;
    double              peak_pnl_    = 0.0;
    double              max_drawdown_= 0.0;
    uint64_t            tick_count_  = 0;
    int                 tick_since_entry_ = 0;
    int                 daily_order_count_= 0;
    bool                circuit_broken_ = false;
    double              last_vpin_   = 0.0;

    PaperPosition       pos_;
    PaperBook           book_;
    VPINVelocityTracker vpin_;
    ATRCalculator       atr_;
    SyntheticBarBuilder bar_builder_;
    RollingVolumeTrackerPT vol_tracker_;
    std::vector<double> trade_pnls_;

    std::chrono::time_point<std::chrono::steady_clock> start_time_;
};

} // namespace kusak::research
