#pragma once
#ifndef KUSAK_RESEARCH_TYPES_HPP
#define KUSAK_RESEARCH_TYPES_HPP

// Çözülen:
// 1. core_types.hpp → FORCE_INLINE/now_us/FileHeader/TICK_FILE_MAGIC/TradeRecord buradan
// 2. std::chrono bağımlılığı silindi — make_tick_filename core_types.hpp içinde
// 3. Namespace-içi sabitler (TICK_FILE_MAGIC vb.) kaldırıldı, global core_types kullanılıyor

#include "core_types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace kusak::research {

// [V13-COMPAT] TickType core_types.hpp'te kusak:: altinda; research kodu
// kusak::research::TickType olarak da erisebilsin.
using ::kusak::TickType;

// ═════════════════════════════════════════════════════════════════════════════
// §1  ORDER BOOK PRİMİTİFLERİ
// ═════════════════════════════════════════════════════════════════════════════

struct PriceLevel {
    double   price    = 0.0;
    double   quantity = 0.0;
    double   size     = 0.0;
    uint64_t count    = 0;

    PriceLevel() = default;
    PriceLevel(double p, double q) noexcept : price(p), quantity(q), size(q) {}
    PriceLevel(double p, double q, uint64_t c) noexcept
        : price(p), quantity(q), size(q), count(c) {}
};

struct DiskRecord {
    uint64_t ts_exchange  = 0;
    uint64_t ts_local     = 0;
    TickType type         = TickType::UNKNOWN;
    uint8_t  bid_count    = 0;
    uint8_t  ask_count    = 0;
    uint8_t  pad0         = 0;
    uint32_t pad1         = 0;
    double   bid_prices[MAX_DEPTH] = {};
    double   bid_qtys  [MAX_DEPTH] = {};
    double   ask_prices[MAX_DEPTH] = {};
    double   ask_qtys  [MAX_DEPTH] = {};
    double   best_bid     = 0.0;
    double   best_bid_qty = 0.0;
    double   best_ask     = 0.0;
    double   best_ask_qty = 0.0;
    double   trade_price  = 0.0;
    double   trade_qty    = 0.0;
    bool     trade_buyer  = false;
    uint8_t  pad2[7]      = {};
};

struct TickRecord {
    uint64_t   ts_exchange = 0;
    uint64_t   ts_local    = 0;
    TickType   type        = TickType::UNKNOWN;
    uint32_t   bid_count   = 0;
    uint32_t   ask_count   = 0;
    PriceLevel bids[MAX_DEPTH];
    PriceLevel asks[MAX_DEPTH];
    double     trade_price = 0.0;
    double     trade_qty   = 0.0;
    bool       trade_buyer = false;
};

// ── [V13-COMPAT] Eski TickRecord alanlarinin (bid_qtys/ask_qtys/bid_prices/
// ask_prices/price/qty/side/mid_price/spread_bps) yeni karsiliklari ───────────
//   bid_qtys[i]   -> rec.bids[i].quantity      ask_qtys[i]   -> rec.asks[i].quantity
//   bid_prices[i] -> rec.bids[i].price         ask_prices[i] -> rec.asks[i].price
//   price / qty   -> rec.trade_price / rec.trade_qty
//   side          -> rec.trade_buyer (true = alici maker = satis yonlu agresor)
FORCE_INLINE bool tick_is_trade(const TickRecord& r) noexcept {
    return r.type == TickType::TRADE || r.type == TickType::AGG_TRADE;
}
FORCE_INLINE bool tick_is_book(const TickRecord& r) noexcept {
    return r.type == TickType::DEPTH || r.type == TickType::BOOK_TICKER;
}
// Agresor alici mi? (trade_buyer = buyer_is_maker -> agresor satici)
FORCE_INLINE bool tick_aggressor_is_buy(const TickRecord& r) noexcept {
    return !r.trade_buyer;
}
FORCE_INLINE double tick_mid_price(const TickRecord& r) noexcept {
    if (r.bid_count > 0 && r.ask_count > 0 && r.bids[0].price > 0.0 && r.asks[0].price > 0.0)
        return 0.5 * (r.bids[0].price + r.asks[0].price);
    return r.trade_price;
}
FORCE_INLINE double tick_spread_bps(const TickRecord& r) noexcept {
    if (r.bid_count == 0 || r.ask_count == 0) return 0.0;
    const double b = r.bids[0].price, a = r.asks[0].price;
    const double mid = 0.5 * (a + b);
    return (b > 0.0 && a > 0.0 && mid > 0.0) ? (a - b) / mid * 1e4 : 0.0;
}
FORCE_INLINE double tick_bid_depth(const TickRecord& r, int levels) noexcept {
    double s = 0.0;
    const int n = std::min<int>(std::min<int>(levels, static_cast<int>(r.bid_count)), MAX_DEPTH);
    for (int i = 0; i < n; ++i) s += r.bids[i].quantity;
    return s;
}
FORCE_INLINE double tick_ask_depth(const TickRecord& r, int levels) noexcept {
    double s = 0.0;
    const int n = std::min<int>(std::min<int>(levels, static_cast<int>(r.ask_count)), MAX_DEPTH);
    for (int i = 0; i < n; ++i) s += r.asks[i].quantity;
    return s;
}

FORCE_INLINE void to_disk(const TickRecord& src, DiskRecord& dst) noexcept {
    dst.ts_exchange = src.ts_exchange;
    dst.ts_local    = src.ts_local;
    dst.type        = src.type;
    dst.bid_count   = static_cast<uint8_t>(src.bid_count);
    dst.ask_count   = static_cast<uint8_t>(src.ask_count);
    for (int i = 0; i < static_cast<int>(src.bid_count) && i < MAX_DEPTH; ++i) {
        dst.bid_prices[i] = src.bids[i].price;
        dst.bid_qtys  [i] = src.bids[i].quantity;
    }
    for (int i = 0; i < static_cast<int>(src.ask_count) && i < MAX_DEPTH; ++i) {
        dst.ask_prices[i] = src.asks[i].price;
        dst.ask_qtys  [i] = src.asks[i].quantity;
    }
    if (src.bid_count > 0) {
        dst.best_bid     = src.bids[0].price;
        dst.best_bid_qty = src.bids[0].quantity;
    }
    if (src.ask_count > 0) {
        dst.best_ask     = src.asks[0].price;
        dst.best_ask_qty = src.asks[0].quantity;
    }
    dst.trade_price = src.trade_price;
    dst.trade_qty   = src.trade_qty;
    dst.trade_buyer = src.trade_buyer;
}

FORCE_INLINE void from_disk(const DiskRecord& src, TickRecord& dst) noexcept {
    dst.ts_exchange = src.ts_exchange;
    dst.ts_local    = src.ts_local;
    dst.type        = src.type;
    dst.bid_count   = src.bid_count;
    dst.ask_count   = src.ask_count;
    for (int i = 0; i < static_cast<int>(src.bid_count) && i < MAX_DEPTH; ++i)
        dst.bids[i] = PriceLevel(src.bid_prices[i], src.bid_qtys[i]);
    for (int i = 0; i < static_cast<int>(src.ask_count) && i < MAX_DEPTH; ++i)
        dst.asks[i] = PriceLevel(src.ask_prices[i], src.ask_qtys[i]);
    dst.trade_price = src.trade_price;
    dst.trade_qty   = src.trade_qty;
    dst.trade_buyer = src.trade_buyer;
}

// ═════════════════════════════════════════════════════════════════════════════
// §2  BACKTEST / ARAŞTIRMA YAPILARI
// ═════════════════════════════════════════════════════════════════════════════

struct BacktestTradeRecord {
    uint64_t    ts_us      = 0;
    double      price      = 0.0;
    double      qty        = 0.0;
    double      pnl        = 0.0;
    double      commission = 0.0;
    int         side       = 0;
    std::string reason;
};

struct BacktestResult {
    double net_pnl        = 0.0;
    double gross_pnl      = 0.0;
    double max_drawdown   = 0.0;
    double sharpe         = 0.0;
    double win_rate       = 0.0;
    double profit_factor  = 0.0;
    int    total_trades   = 0;
    int    winning_trades = 0;
    int    losing_trades  = 0;
    std::vector<BacktestTradeRecord> trades;

    // ── [V13-COMPAT] Eski arastirma katmaninin bekledigi genisletilmis alanlar ──
    // Backtester::finalize() bunlari doldurmaz; finalize_derived() trades[]
    // listesinden turetir. Turetilemeyenler guvenli varsayilanda kalir.
    bool   valid               = false;  // total_trades >= min_trades
    double sharpe_ratio        = 0.0;    // = sharpe (islem basina, yillandirilmamis)
    double sortino_ratio       = 0.0;
    double calmar_ratio        = 0.0;
    double avg_trade_pnl       = 0.0;
    double avg_win_pnl         = 0.0;
    double avg_loss_pnl        = 0.0;    // isaretli (negatif)
    double total_gain          = 0.0;
    double total_loss          = 0.0;    // pozitif buyukluk
    int    max_win_streak      = 0;
    int    max_loss_streak     = 0;
    double fill_rate           = 0.0;    // mock: simulatorde emirler dolu varsayilir
    double exposure_pct        = 0.0;    // mock: modellenmiyor
    double buy_hold_return     = 0.0;    // runner tarafindan doldurulur
    double alpha               = 0.0;    // net_pnl - buy_hold_return
    double kelly_fraction_est  = 0.0;
    double total_slippage_cost = 0.0;    // mock: Backtester entry fiyatina gomer
    double avg_slippage_bps    = 0.0;    // mock

    void finalize_derived(int min_trades = 5) noexcept {
        if (total_trades <= 0) total_trades = static_cast<int>(trades.size());
        valid = (total_trades >= min_trades);
        if (sharpe_ratio == 0.0) sharpe_ratio = sharpe;
        if (total_trades <= 0) return;

        double gain = 0.0, loss = 0.0, down2 = 0.0;
        int    nw = 0, nl = 0, cw = 0, cl = 0;
        for (const auto& t : trades) {
            if (t.pnl > 0.0) {
                gain += t.pnl; ++nw; ++cw; cl = 0;
                max_win_streak = std::max(max_win_streak, cw);
            } else {
                loss += -t.pnl; ++nl; ++cl; cw = 0;
                max_loss_streak = std::max(max_loss_streak, cl);
                down2 += t.pnl * t.pnl;
            }
        }
        if (!trades.empty()) {
            total_gain = gain; total_loss = loss;
            avg_win_pnl  = nw ? gain / nw : 0.0;
            avg_loss_pnl = nl ? -(loss / nl) : 0.0;
        }
        avg_trade_pnl = net_pnl / static_cast<double>(total_trades);
        if (!trades.empty()) {
            const double dd = std::sqrt(down2 / static_cast<double>(trades.size()));
            sortino_ratio = (dd > 1e-12) ? avg_trade_pnl / dd : 0.0;
        }
        calmar_ratio = (max_drawdown > 1e-12) ? net_pnl / max_drawdown : 0.0;
        if (fill_rate == 0.0) fill_rate = 1.0;
        if (nw > 0 && nl > 0) {
            const double b = avg_win_pnl / std::max(-avg_loss_pnl, 1e-12);
            const double p = static_cast<double>(nw) / static_cast<double>(nw + nl);
            kelly_fraction_est = std::max(0.0, (b * p - (1.0 - p)) / b);
        }
    }

    // Genetic/WFO ile ayni agirlik: Sharpe*0.5 + net_pnl*0.3 - max_dd*0.2
    double objective_score() const noexcept {
        if (!valid) return -1e18;
        return sharpe_ratio * 0.5 + net_pnl * 0.3 - max_drawdown * 0.2;
    }
};

struct ResearchParams {
    double  entry_threshold  = 0.002;
    double  exit_threshold   = 0.001;
    double  stop_loss        = 0.005;
    double  take_profit      = 0.008;
    double  position_size    = 1.0;
    int     vpin_buckets     = 50;
    double  vpin_threshold   = 0.7;
    double  imbalance_thresh = 0.6;
    int     depth_levels     = 10;
    double  commission_bps   = 4.0;
    double  slippage_bps     = 1.0;
    uint8_t flags            = 0;

    // [V13-COMPAT] genetic_optimizer / paper_trader'in kullandigi alanlar
    double  imbalance_threshold    = 0.6;   // imbalance_thresh ile ayni varsayilan
    double  trailing_activation_bps = 0.0;  // 0 = trailing kapali
    double  trailing_callback_bps   = 0.0;
};

static constexpr uint8_t FLAG_DISABLE_BUYS  = 0x01;
static constexpr uint8_t FLAG_DISABLE_SELLS = 0x02;
static constexpr uint8_t FLAG_SNIPER_MODE   = 0x04;

using ObjectiveFn = std::function<BacktestResult(const ResearchParams&)>;

using WFOObjectiveFn = std::function<BacktestResult(
    const ResearchParams&,
    const TickRecord*, size_t,
    const TickRecord*, size_t)>;

// ═════════════════════════════════════════════════════════════════════════════
// §3  ENUM'LAR
// ═════════════════════════════════════════════════════════════════════════════

enum class MarketRegime : uint8_t {
    UNKNOWN  = 0,
    TRENDING = 1,
    RANGING  = 2,
    VOLATILE = 3,
    THIN     = 4,
};

enum class VPINAlarm : uint8_t {
    NONE     = 0,
    WARNING  = 1,
    HIGH     = 2,
    CRITICAL = 3,
};

struct VPINUpdate {
    double    vpin                   = 0.0;
    double    dynamic_threshold      = 0.0;
    int32_t   vpin_x1e6              = 0;
    int32_t   dynamic_threshold_x1e6 = 0;
    VPINAlarm alarm                  = VPINAlarm::NONE;
    bool      valid                  = false;
};

enum class ReplayMode : uint8_t {
    REALTIME = 0,
    FAST     = 1,
    STEP     = 2,
};

// ═════════════════════════════════════════════════════════════════════════════
// §4  EMİR YAŞAM DÖNGÜSÜ
// ═════════════════════════════════════════════════════════════════════════════

struct OrderLifecycle {
    uint64_t tickRecvNS  = 0;
    uint64_t decisionNS  = 0;
    uint64_t sendOrderNS = 0;
    uint64_t ackNS       = 0;
    uint64_t fillNS      = 0;

    double tick_to_decision_us() const noexcept {
        return (decisionNS > tickRecvNS) ? static_cast<double>(decisionNS - tickRecvNS) / 1000.0 : 0.0;
    }
    double decision_to_send_us() const noexcept {
        return (sendOrderNS > decisionNS) ? static_cast<double>(sendOrderNS - decisionNS) / 1000.0 : 0.0;
    }
    double send_to_ack_us() const noexcept {
        return (ackNS > sendOrderNS) ? static_cast<double>(ackNS - sendOrderNS) / 1000.0 : 0.0;
    }
    double tick_to_trade_us() const noexcept {
        return (fillNS > tickRecvNS) ? static_cast<double>(fillNS - tickRecvNS) / 1000.0 : 0.0;
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// §5  [V13-COMPAT] SyntheticBarBuilder + ATRCalculator
//     (eski research katmaninda vardi, V13 ana motorunda yok)
// ═════════════════════════════════════════════════════════════════════════════

struct SyntheticBar {
    double  open   = 0.0;
    double  high   = 0.0;
    double  low    = 0.0;
    double  close  = 0.0;
    double  volume = 0.0;
    int64_t ts_ms  = 0;   // bar baslangic zamani (ms)
};

class SyntheticBarBuilder {
public:
    explicit SyntheticBarBuilder(int64_t period_ms = 60000) noexcept
        : period_ms_(period_ms > 0 ? period_ms : 60000) {}

    // Bar kapandiysa tamamlanan bari dondurur.
    std::optional<SyntheticBar> onTrade(double price, double qty, int64_t ts_ms) noexcept {
        if (!(price > 0.0) || !std::isfinite(price)) return std::nullopt;
        const int64_t bucket = ts_ms - (ts_ms % period_ms_);
        std::optional<SyntheticBar> done;
        if (open_ && bucket != cur_.ts_ms) {
            done  = cur_;
            open_ = false;
        }
        if (!open_) {
            cur_ = SyntheticBar{price, price, price, price, 0.0, bucket};
            open_ = true;
        }
        cur_.high    = std::max(cur_.high, price);
        cur_.low     = std::min(cur_.low,  price);
        cur_.close   = price;
        cur_.volume += (qty > 0.0 ? qty : 0.0);
        return done;
    }

private:
    int64_t      period_ms_;
    SyntheticBar cur_{};
    bool         open_ = false;
};

// Wilder ATR
class ATRCalculator {
public:
    explicit ATRCalculator(int period = 14) noexcept
        : period_(period > 0 ? period : 14) {}

    void onBar(double high, double low, double close) noexcept {
        double tr = high - low;
        if (have_prev_) {
            tr = std::max({tr, std::fabs(high - prev_close_), std::fabs(low - prev_close_)});
        }
        prev_close_ = close;
        have_prev_  = true;

        if (n_ < period_) {
            sum_ += tr;
            ++n_;
            atr_ = sum_ / static_cast<double>(n_);
        } else {
            atr_ = (atr_ * static_cast<double>(period_ - 1) + tr) / static_cast<double>(period_);
        }
    }

    double value() const noexcept { return atr_; }
    bool   ready() const noexcept { return n_ >= period_; }

private:
    int    period_;
    int    n_          = 0;
    double sum_        = 0.0;
    double atr_        = 0.0;
    double prev_close_ = 0.0;
    bool   have_prev_  = false;
};

} // namespace kusak::research

#endif // KUSAK_RESEARCH_TYPES_HPP
