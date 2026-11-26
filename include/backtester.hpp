#pragma once
#ifndef KUSAK_BACKTESTER_HPP
#define KUSAK_BACKTESTER_HPP

// Fixed:
// 1. BacktesterConfig lifted outside class → default-arg in any context
// 2. All ctors: span(ptr,n) + span(vector) + span directly — zero-copy
// 3. make_objective default arg works outside class body

#include "core_types.hpp"
#include "research_types.hpp"

#include <span>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace kusak::research {

// ── Config outside class (CWG 2346 — default arg on incomplete type) ────────
struct BacktesterConfig {
    double commission_bps = 4.0;
    double slippage_bps   = 1.0;
    double slippage       = 0.0;
    double position_size  = 1.0;
};

// ═════════════════════════════════════════════════════════════════════════════
// TickDataLoader
// ═════════════════════════════════════════════════════════════════════════════
class TickDataLoader {
public:
    explicit TickDataLoader(const std::string& path) {
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (UNLIKELY(fd < 0))
            throw std::runtime_error("TickDataLoader: open: " + path);
        struct stat st{};
        ::fstat(fd, &st);
        const std::size_t fsz = static_cast<std::size_t>(st.st_size);
        if (UNLIKELY(fsz < sizeof(FileHeader))) { ::close(fd); throw std::runtime_error("TickDataLoader: too small"); }
        const uint8_t* base = static_cast<const uint8_t*>(
            ::mmap(nullptr, fsz, PROT_READ, MAP_SHARED | MAP_POPULATE, fd, 0));
        ::close(fd);
        if (UNLIKELY(base == MAP_FAILED)) throw std::runtime_error("TickDataLoader: mmap");
        uint32_t magic; uint16_t ver; uint64_t rcount;
        std::memcpy(&magic,  base + __builtin_offsetof(FileHeader, magic),        4);
        std::memcpy(&ver,    base + __builtin_offsetof(FileHeader, version),      2);
        std::memcpy(&rcount, base + __builtin_offsetof(FileHeader, record_count), 8);
        if (magic != TICK_FILE_MAGIC || ver != TICK_FILE_VERSION) {
            ::munmap(const_cast<uint8_t*>(base), fsz);
            throw std::runtime_error("TickDataLoader: bad header");
        }
        const std::size_t n = std::min(
            (fsz - sizeof(FileHeader)) / sizeof(DiskRecord),
            static_cast<std::size_t>(rcount));
        ticks_.resize(n);
        const uint8_t* dp = base + sizeof(FileHeader);
        for (std::size_t i = 0; i < n; ++i) {
            DiskRecord dr{}; std::memcpy(&dr, dp + i * sizeof(DiskRecord), sizeof(DiskRecord));
            from_disk(dr, ticks_[i]);
        }
        ::munmap(const_cast<uint8_t*>(base), fsz);
    }
    std::span<const TickRecord>    span() const noexcept { return ticks_; }
    const std::vector<TickRecord>& vec()  const noexcept { return ticks_; }
    const TickRecord*              data() const noexcept { return ticks_.data(); }
    std::size_t                    size() const noexcept { return ticks_.size(); }
private:
    std::vector<TickRecord> ticks_;
};

// ═════════════════════════════════════════════════════════════════════════════
// Backtester
// ═════════════════════════════════════════════════════════════════════════════
class Backtester {
public:
    using Config = BacktesterConfig;   // alias so existing code using Backtester::Config compiles

    // (A) span — canonical ctor
    explicit Backtester(std::span<const TickRecord> ticks,
                        Config cfg = Config{}) noexcept
        : ticks_(ticks), cfg_(cfg) {}

    // (B) ptr + size  ← research_main.cpp: mv.data(), mv.size()
    Backtester(const TickRecord* data, std::size_t n,
               Config cfg = Config{}) noexcept
        : Backtester(std::span<const TickRecord>{data, n}, cfg) {}

    // (C) vector& → implicit span
    explicit Backtester(const std::vector<TickRecord>& v,
                        Config cfg = Config{}) noexcept
        : Backtester(std::span<const TickRecord>{v}, cfg) {}

    // (D) ptr + size slice (walk-forward window, zero-copy)
    Backtester(const TickRecord* data, std::size_t total,
               std::size_t slice_start, std::size_t slice_len,
               Config cfg = Config{}) noexcept
        : Backtester(std::span<const TickRecord>{
              data + slice_start,
              std::min(slice_len, total > slice_start ? total - slice_start : 0u)},
              cfg) {}

    BacktestResult run(const ResearchParams& p) {
        reset(p);
        for (const TickRecord& rec : ticks_) {
            update_book(rec);
            check_entry(rec, p);
            check_exit(rec);
        }
        if (position_open_ && !ticks_.empty())
            close_position(ticks_.back(), "end_of_data");
        return finalize();
    }

    ObjectiveFn make_objective(Config bt_cfg = Config{}) const {
        const TickRecord* ptr = ticks_.data();
        const std::size_t n   = ticks_.size();
        return [ptr, n, bt_cfg](const ResearchParams& p) -> BacktestResult {
            return Backtester(ptr, n, bt_cfg).run(p);
        };
    }

private:
    std::span<const TickRecord> ticks_;
    Config  cfg_;
    PriceLevel bids_[MAX_DEPTH], asks_[MAX_DEPTH];
    int    bid_count_ = 0, ask_count_ = 0;
    bool   position_open_ = false;
    int    position_side_ = 0;
    double entry_price_ = 0, position_size_ = 0, stop_price_ = 0, take_price_ = 0;
    BacktestResult result_;
    double peak_eq_ = 0, cur_eq_ = 0, sum_pnl2_ = 0;
    int    trade_count_ = 0;

    void reset(const ResearchParams& p) noexcept {
        position_open_ = false; position_side_ = 0;
        entry_price_ = stop_price_ = take_price_ = 0;
        position_size_ = p.position_size;
        peak_eq_ = cur_eq_ = sum_pnl2_ = 0; trade_count_ = 0;
        result_ = {}; bid_count_ = ask_count_ = 0;
    }

    FORCE_INLINE void update_book(const TickRecord& r) noexcept {
        bid_count_ = static_cast<int>(r.bid_count);
        ask_count_ = static_cast<int>(r.ask_count);
        for (int i = 0; i < bid_count_; ++i) bids_[i] = r.bids[i];
        for (int i = 0; i < ask_count_; ++i) asks_[i] = r.asks[i];
    }

    FORCE_INLINE void check_entry(const TickRecord& r, const ResearchParams& p) noexcept {
        if (position_open_ || !bid_count_ || !ask_count_) return;
        const double imb = imbalance(p.depth_levels);
        if (!(p.flags & FLAG_DISABLE_BUYS)  && imb >  p.entry_threshold)
            open_pos(r, +1, asks_[0].price, p);
        else if (!(p.flags & FLAG_DISABLE_SELLS) && imb < -p.entry_threshold)
            open_pos(r, -1, bids_[0].price, p);
    }

    FORCE_INLINE void open_pos(const TickRecord&, int side, double px,
                                const ResearchParams& p) noexcept {
        position_open_ = true; position_side_ = side;
        entry_price_   = px + side * px * cfg_.slippage_bps * 1e-4;
        position_size_ = p.position_size;
        stop_price_    = entry_price_ * (1.0 - side * p.stop_loss);
        take_price_    = entry_price_ * (1.0 + side * p.take_profit);
    }

    FORCE_INLINE void check_exit(const TickRecord& r) noexcept {
        if (!position_open_ || !bid_count_ || !ask_count_) return;
        const double bid = bids_[0].price, ask = asks_[0].price;
        const char* rsn = nullptr;
        if (position_side_ > 0) {
            if (bid <= stop_price_)  rsn = "stop_loss";
            else if (ask >= take_price_) rsn = "take_profit";
        } else {
            if (ask >= stop_price_)  rsn = "stop_loss";
            else if (bid <= take_price_) rsn = "take_profit";
        }
        if (UNLIKELY(rsn)) close_position(r, rsn);
    }

    void close_position(const TickRecord&, const char* rsn) noexcept {
        if (!position_open_) return;
        position_open_ = false;
        const double xpx  = position_side_ > 0 ? bids_[0].price : asks_[0].price;
        const double raw   = position_side_ * (xpx - entry_price_) * position_size_;
        const double comm  = (entry_price_ + xpx) * position_size_ * cfg_.commission_bps * 1e-4;
        const double net   = raw - comm;
        cur_eq_ += net;
        peak_eq_ = std::max(peak_eq_, cur_eq_);
        result_.max_drawdown = std::max(result_.max_drawdown, peak_eq_ - cur_eq_);
        result_.net_pnl += net; result_.gross_pnl += raw; sum_pnl2_ += net * net;
        ++trade_count_;
        if (net > 0) ++result_.winning_trades; else ++result_.losing_trades;
        BacktestTradeRecord tr;
        tr.price = xpx; tr.qty = position_size_; tr.pnl = net;
        tr.commission = comm; tr.side = position_side_; tr.reason = rsn ? rsn : "";
        result_.trades.push_back(std::move(tr));
    }

    BacktestResult finalize() noexcept {
        result_.total_trades = trade_count_;
        if (trade_count_ > 0)
            result_.win_rate = static_cast<double>(result_.winning_trades) / trade_count_;
        if (trade_count_ > 1) {
            const double mu = result_.net_pnl / trade_count_;
            const double var = sum_pnl2_ / trade_count_ - mu * mu;
            result_.sharpe = (var > 1e-12) ? mu / std::sqrt(var) : 0.0;
        }
        double gw = 0, gl = 0;
        for (const auto& t : result_.trades) { if (t.pnl > 0) gw += t.pnl; else gl -= t.pnl; }
        result_.profit_factor = (gl > 1e-12) ? gw / gl : 0.0;
        result_.finalize_derived();   // [V13-COMPAT] valid / sharpe_ratio / sortino ... (optimizer'lar icin)
        return result_;
    }

    FORCE_INLINE double imbalance(int lv) const noexcept {
        double bd = 0, ad = 0;
        for (int i = 0; i < std::min(bid_count_, lv); ++i) bd += bids_[i].quantity;
        for (int i = 0; i < std::min(ask_count_, lv); ++i) ad += asks_[i].quantity;
        const double tot = bd + ad;
        return (tot > 0.0) ? (bd - ad) / tot : 0.0;
    }
};

} // namespace kusak::research
#endif // KUSAK_BACKTESTER_HPP
