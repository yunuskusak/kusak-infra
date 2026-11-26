// ============================================================================
// KUSAK V12 — TASK 2: DRY-RUN ISOLATION
// SimulatedExchangeClient.hpp — Paper-Trading / Simulation Client
//
//  Fully implements IExchangeClient without any network calls.
//
//  Guarantees (per task specification):
//    • fetch_balance("USDT")   → exactly 12.97 (or cfg.simulated_usdt_balance)
//    • fetch_all_balances()    → one-entry vector with the above balance
//    • place_order()           → stores in internal log, returns fake orderId
//    • No exceptions escape    — all errors are encoded in return values
//    • Thread-safe             — std::mutex guards all mutable state
//    • RAII compliant          — no raw new/delete; destructor is trivially safe
//
//  Optional fill simulation:
//    • Configurable ACK latency (std::this_thread::sleep_for, default 0 µs)
//    • Simulated fill price: ref_price for MARKET, req.price for LIMIT
//    • PnL tracking: running cash balance updated on each simulated fill
//    • All fills stored in sim_fills_ for post-run analysis
//
//  Logging:
//    • Primary output via kusak::log::Logger reference when provided.
//    • Falls back to std::cerr for standalone / unit-test usage when Logger
//      is not available — no hard dependency on logger.hpp is introduced;
//      the constructor that accepts a Logger& is a convenience overload.
//
//  C++17. Header-only. No .cpp file required.
// ============================================================================

#pragma once

#include "IExchangeClient.hpp"
#include "logger.hpp"           // kusak::log::Logger (optional overload)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace kusak::exchange {

// ============================================================================
// §1  SimFill — record of every simulated order
// ============================================================================

struct SimFill {
    uint64_t    order_id      = 0;
    std::string symbol;
    std::string side;               // "BUY" / "SELL"
    std::string type;               // "LIMIT" / "MARKET"
    double      req_price     = 0.0;  // price in the original request
    double      fill_price    = 0.0;  // simulated execution price
    double      quantity      = 0.0;
    double      notional      = 0.0;  // fill_price * quantity
    double      ref_price     = 0.0;  // mid price supplied by the strategy
    std::string time_in_force;
    std::string client_oid;
    uint64_t    filled_at_ns  = 0;  // nanoseconds since epoch (wall clock)
};

// ============================================================================
// §2  PnL tracker — updated on every simulated fill
// ============================================================================

struct SimPnL {
    double  total_bought     = 0.0;   // cumulative notional of BUY fills
    double  total_sold       = 0.0;   // cumulative notional of SELL fills
    double  realized_pnl     = 0.0;   // total_sold - total_bought
    int64_t buy_fill_count   = 0;
    int64_t sell_fill_count  = 0;
};

// ============================================================================
// §3  SimulatedExchangeClient
// ============================================================================

class SimulatedExchangeClient final : public IExchangeClient {
public:
    // ── Configuration ────────────────────────────────────────────────────────

    struct Config {
        // Hardcoded simulated free balance — spec mandates 12.97 USDT.
        double      simulated_usdt_balance = 12.97;

        // Quote asset name matched by fetch_balance(); all others return 0.0.
        std::string quote_asset = "USDT";

        // Artificial ACK latency injected inside place_order().
        // Set to 0 µs (default) to disable; useful for tick-to-trade benchmarks.
        std::chrono::microseconds simulated_ack_latency{0};

        // When true, place_order() also writes to std::cout (useful for manual
        // smoke-tests without a Logger instance).
        bool verbose_stdout = false;

        // When true, the simulated balance is decremented/incremented on
        // every fill to reflect paper-trading cash flows.  Requires that the
        // strategy passes a non-zero ref_price for MARKET orders, or a valid
        // req.price for LIMIT orders.
        bool track_balance = false;

        // Starting order-id (far from 0 to distinguish from live exchange IDs).
        uint64_t initial_order_id = 1'000'000;
    };

    // ── Construction — Logger overload ───────────────────────────────────────

    explicit SimulatedExchangeClient(Config             cfg,
                                     kusak::log::Logger& logger)
        : cfg_(std::move(cfg))
        , logger_(&logger)
        , next_order_id_(cfg_.initial_order_id)
    {
        log_info("SIM_CLIENT",
                 "SimulatedExchangeClient started"
                 " balance=", cfg_.simulated_usdt_balance,
                 " ", cfg_.quote_asset);
    }

    // Construction — no Logger (logs to std::cerr only)
    explicit SimulatedExchangeClient()
        : next_order_id_(Config{}.initial_order_id)
    {}

    explicit SimulatedExchangeClient(Config cfg)
        : cfg_(std::move(cfg))
        , next_order_id_(cfg_.initial_order_id)
    {}

    // Convenience: logger only, default Config (12.97 USDT)
    explicit SimulatedExchangeClient(kusak::log::Logger& logger)
        : SimulatedExchangeClient(Config{}, logger)
    {}

    ~SimulatedExchangeClient() override {
        log_info("SIM_CLIENT",
                 "SimulatedExchangeClient shutting down"
                 " total_sim_orders=", sim_order_count());
    }

    // ── IExchangeClient — balance ─────────────────────────────────────────────

    // Returns a single-element vector containing the simulated quote balance.
    // Any additional base-asset balances (derived from fill tracking) are
    // appended when track_balance is enabled.
    std::vector<AssetBalance> fetch_all_balances() override {
        std::lock_guard<std::mutex> lk(state_mutex_);

        std::vector<AssetBalance> result;
        result.reserve(2);

        AssetBalance quote;
        quote.asset  = cfg_.quote_asset;
        quote.free   = cfg_.simulated_usdt_balance;
        quote.locked = 0.0;
        result.push_back(quote);

        // Optionally expose accumulated base-asset qty from fills
        if (cfg_.track_balance && base_qty_held_ > 1e-12) {
            // Extract base asset name from symbol (e.g. "BTC" from "BTCUSDT")
            AssetBalance base;
            base.asset  = base_asset_name_;
            base.free   = base_qty_held_;
            base.locked = 0.0;
            result.push_back(base);
        }

        log_debug("SIM_CLIENT",
                  "fetch_all_balances() → ",
                  cfg_.simulated_usdt_balance, " ", cfg_.quote_asset,
                  " (simulated, no network)");
        return result;
    }

    // Returns free balance for the requested asset.
    // Guaranteed 12.97 for the quote asset; 0.0 for anything unknown.
    double fetch_balance(const std::string& asset) override {
        std::lock_guard<std::mutex> lk(state_mutex_);

        if (asset == cfg_.quote_asset) {
            log_debug("SIM_CLIENT",
                      "fetch_balance(", asset, ") → ",
                      cfg_.simulated_usdt_balance, " [simulated]");
            return cfg_.simulated_usdt_balance;
        }

        if (cfg_.track_balance && asset == base_asset_name_) {
            log_debug("SIM_CLIENT",
                      "fetch_balance(", asset, ") → ",
                      base_qty_held_, " [simulated]");
            return base_qty_held_;
        }

        log_debug("SIM_CLIENT",
                  "fetch_balance(", asset, ") → 0.0 [unknown asset, simulated]");
        return 0.0;
    }

    // ── IExchangeClient — order placement ────────────────────────────────────

    // Simulates an order placement.  NEVER performs any network call.
    // All errors are encoded in OrderResponse::error; exceptions never escape.
    OrderResponse place_order(const OrderRequest& req,
                              double              ref_price = 0.0) override
    {
        // Optional simulated ACK latency (mimics realistic round-trip time)
        if (cfg_.simulated_ack_latency.count() > 0) {
            std::this_thread::sleep_for(cfg_.simulated_ack_latency);
        }

        // Assign a unique monotonically-increasing fake order-id.
        // fetch_add is lock-free on all major platforms.
        const uint64_t oid = next_order_id_.fetch_add(1,
                                                       std::memory_order_relaxed);

        // Determine execution price:
        //   MARKET + ref_price available → use ref_price (best approximation)
        //   LIMIT  or ref_price == 0.0  → use req.price
        const double fill_price = (req.type == "MARKET" && ref_price > 1e-15)
                                    ? ref_price
                                    : req.price;
        const double notional = fill_price * req.quantity;

        // Timestamp (nanoseconds since epoch, wall clock)
        const uint64_t now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count());

        const uint64_t now_ms = now_ns / 1'000'000ULL;

        // ── Build fill record ──────────────────────────────────────────────
        SimFill fill;
        fill.order_id     = oid;
        fill.symbol       = req.symbol;
        fill.side         = req.side;
        fill.type         = req.type;
        fill.req_price    = req.price;
        fill.fill_price   = fill_price;
        fill.quantity     = req.quantity;
        fill.notional     = notional;
        fill.ref_price    = ref_price;
        fill.time_in_force = req.time_in_force;
        fill.client_oid   = req.client_oid;
        fill.filled_at_ns = now_ns;

        // ── Update mutable state under lock ──────────────────────────────
        {
            std::lock_guard<std::mutex> lk(state_mutex_);

            sim_fills_.push_back(fill);

            pnl_.buy_fill_count  += (req.side == "BUY")  ? 1 : 0;
            pnl_.sell_fill_count += (req.side == "SELL") ? 1 : 0;

            if (cfg_.track_balance) {
                update_balance_locked(req, fill_price, notional);
            }
        }

        // ── Logging ───────────────────────────────────────────────────────
        log_info("SIM_ORDER",
                 "[SIMULATED ORDER]",
                 " orderId=",     oid,
                 " symbol=",      req.symbol,
                 " side=",        req.side,
                 " type=",        req.type,
                 " qty=",         req.quantity,
                 " req_price=",   req.price,
                 " fill_price=",  fill_price,
                 " ref_price=",   ref_price,
                 " notional=",    notional,
                 " tif=",         req.time_in_force.empty() ? "N/A" : req.time_in_force,
                 " client_oid=",  req.client_oid.empty()   ? "N/A" : req.client_oid,
                 " status=FILLED [no network call]");

        if (cfg_.verbose_stdout) {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(8);
            oss << "[SIM] " << req.side << " " << req.symbol
                << " qty="      << req.quantity
                << " price="    << fill_price
                << " notional=" << notional
                << " orderId="  << oid << "\n";
            std::cout << oss.str() << std::flush;
        }

        // ── Build fake JSON body (matches parse_new_order_response format) ─
        std::ostringstream jbody;
        jbody << std::fixed << std::setprecision(8);
        jbody << "{\"orderId\":"             << oid
              << ",\"status\":\"FILLED\""
              << ",\"symbol\":\""            << req.symbol       << "\""
              << ",\"side\":\""              << req.side         << "\""
              << ",\"type\":\""              << req.type         << "\""
              << ",\"timeInForce\":\""       << req.time_in_force << "\""
              << ",\"price\":\""             << fill_price       << "\""
              << ",\"origQty\":\""           << req.quantity     << "\""
              << ",\"executedQty\":\""       << req.quantity     << "\""
              << ",\"cummulativeQuoteQty\":\"" << notional       << "\""
              << ",\"clientOrderId\":\""     << req.client_oid   << "\""
              << ",\"transactTime\":"        << now_ms
              << ",\"fills\":["
              << "{\"price\":\""             << fill_price       << "\""
              << ",\"qty\":\""               << req.quantity     << "\""
              << ",\"commission\":\"0.00000000\""
              << ",\"commissionAsset\":\"BNB\""
              << ",\"tradeId\":\"" << (oid * 10ULL) << "\"}"
              << "]}";

        OrderResponse resp;
        resp.success  = true;
        resp.order_id = oid;
        resp.status   = "FILLED";
        resp.raw_body = jbody.str();
        // resp.error is default-constructed: code=0, message="" — no error.

        return resp;
    }

    // ── IExchangeClient — meta ───────────────────────────────────────────────

    std::string client_name() const override {
        return "SimulatedExchangeClient";
    }

    bool is_live() const noexcept override {
        return false;   // no live orders, ever
    }

    // ── Simulation-specific accessors ────────────────────────────────────────

    // Thread-safe snapshot of all simulated fills.
    std::vector<SimFill> sim_fills() const {
        std::lock_guard<std::mutex> lk(state_mutex_);
        return sim_fills_;
    }

    // Total number of orders submitted since construction.
    std::size_t sim_order_count() const noexcept {
        return static_cast<std::size_t>(
            next_order_id_.load(std::memory_order_relaxed) -
            cfg_.initial_order_id);
    }

    // PnL snapshot (thread-safe).
    SimPnL sim_pnl() const {
        std::lock_guard<std::mutex> lk(state_mutex_);
        return pnl_;
    }

    // Override the simulated balance at runtime (e.g. to reflect external PnL).
    // Thread-safe.
    void set_simulated_balance(double new_balance) noexcept {
        std::lock_guard<std::mutex> lk(state_mutex_);
        cfg_.simulated_usdt_balance = new_balance;
    }

    // Reset all fill history and PnL counters (thread-safe).
    void reset() {
        std::lock_guard<std::mutex> lk(state_mutex_);
        sim_fills_.clear();
        pnl_            = SimPnL{};
        base_qty_held_  = 0.0;
        base_asset_name_.clear();
        next_order_id_.store(cfg_.initial_order_id,
                             std::memory_order_relaxed);
    }

    // Prints a compact summary of all fills and PnL to the logger / stderr.
    void print_sim_summary() const {
        std::lock_guard<std::mutex> lk(state_mutex_);

        double total_notional = 0.0;
        for (const auto& f : sim_fills_) total_notional += f.notional;

        log_info_raw("SIM_SUMMARY",
                     "=== Simulation Summary ==="
                     " total_orders=", sim_fills_.size(),
                     " total_notional_USD=", total_notional,
                     " buys=", pnl_.buy_fill_count,
                     " sells=", pnl_.sell_fill_count,
                     " realized_pnl=", pnl_.realized_pnl);

        for (std::size_t i = 0; i < sim_fills_.size(); ++i) {
            const auto& f = sim_fills_[i];
            log_info_raw("SIM_FILL",
                         "[", i + 1, "/", sim_fills_.size(), "]",
                         " id=",    f.order_id,
                         " sym=",   f.symbol,
                         " side=",  f.side,
                         " type=",  f.type,
                         " qty=",   f.quantity,
                         " price=", f.fill_price,
                         " not=",   f.notional);
        }
    }

private:
    // ── Balance update (called under state_mutex_) ────────────────────────────
    //
    //  Tracks cash and base-asset inventory from fills.
    //  BUY  → deduct notional from cash; add qty to base inventory.
    //  SELL → deduct qty from base inventory; add notional to cash.
    void update_balance_locked(const OrderRequest& req,
                               double              fill_price,
                               double              notional) noexcept
    {
        // Derive base asset name on first fill (e.g. "BTC" from "BTCUSDT")
        if (base_asset_name_.empty() &&
            req.symbol.size() > cfg_.quote_asset.size())
        {
            // Check that symbol ends with the quote asset string
            const std::size_t base_len =
                req.symbol.size() - cfg_.quote_asset.size();
            if (req.symbol.substr(base_len) == cfg_.quote_asset) {
                base_asset_name_ = req.symbol.substr(0, base_len);
            }
        }

        if (req.side == "BUY") {
            cfg_.simulated_usdt_balance -= notional;
            base_qty_held_              += req.quantity;
            pnl_.total_bought           += notional;
        } else if (req.side == "SELL") {
            cfg_.simulated_usdt_balance += notional;
            base_qty_held_              -= req.quantity;
            if (base_qty_held_ < 0.0) base_qty_held_ = 0.0;
            pnl_.total_sold             += notional;
        }

        pnl_.realized_pnl = pnl_.total_sold - pnl_.total_bought;
    }

    // ── Logging helpers (variadic, forward to Logger or std::cerr) ───────────

    // Build a single log line from variadic args → std::string
    template<typename... Args>
    static std::string build_log_line(Args&&... args) {
        std::ostringstream oss;
        (void)std::initializer_list<int>{
            (oss << std::forward<Args>(args), 0)...
        };
        return oss.str();
    }

    template<typename... Args>
    void log_info(std::string_view module, Args&&... args) const {
        if (logger_) {
            logger_->info(module, std::forward<Args>(args)...);
        } else {
            std::cerr << "[INFO][" << module << "] "
                      << build_log_line(std::forward<Args>(args)...)
                      << "\n";
        }
    }

    template<typename... Args>
    void log_debug(std::string_view module, Args&&... args) const {
        if (logger_) {
            logger_->debug(module, std::forward<Args>(args)...);
        } else {
            // stderr only in verbose mode to avoid flooding unit tests
            if (cfg_.verbose_stdout) {
                std::cerr << "[DEBUG][" << module << "] "
                          << build_log_line(std::forward<Args>(args)...)
                          << "\n";
            }
        }
    }

    // log_info_raw: called when state_mutex_ is already held — cannot call
    // logger_ (which may take its own lock). Writes directly to std::cerr.
    template<typename... Args>
    void log_info_raw(std::string_view module, Args&&... args) const {
        std::cerr << "[INFO][" << module << "] "
                  << build_log_line(std::forward<Args>(args)...)
                  << "\n";
    }

    // ── Data members ─────────────────────────────────────────────────────────

    Config                   cfg_;
    kusak::log::Logger*      logger_ = nullptr;   // optional; non-owning
    std::atomic<uint64_t>    next_order_id_;

    mutable std::mutex       state_mutex_;        // guards all fields below
    std::vector<SimFill>     sim_fills_;
    SimPnL                   pnl_;
    double                   base_qty_held_  = 0.0;
    std::string              base_asset_name_;
};

} // namespace kusak::exchange
