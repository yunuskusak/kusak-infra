// ============================================================================
// KUSAK V12 — TASK 2: DRY-RUN ISOLATION
// IExchangeClient.hpp — Abstract Exchange Client Interface
//
//  Purpose:
//    Defines the polymorphic boundary between the strategy engine and the
//    exchange layer.  Every concrete client (live BinanceHttpClient wrapper,
//    SimulatedExchangeClient, replay client, …) must inherit from this class
//    and implement the pure-virtual methods below.
//
//  Design goals:
//    • Zero coupling to libcurl, OpenSSL, or any network header.
//    • Reuses existing value types from binance_rest_client.hpp so that the
//      strategy engine does not need separate DTO mappings.
//    • All virtual methods are noexcept-annotated where the implementation
//      can guarantee exception-safety (simulation path); the live path wraps
//      network exceptions internally and surfaces errors through return values.
//
//  Thread-safety contract:
//    Implementations are responsible for their own internal thread-safety.
//    The strategy engine calls these methods from a single event-loop thread,
//    so they do not need to be re-entrant with respect to each other.
// ============================================================================

#pragma once

#include "binance_rest_client.hpp"   // OrderRequest, OrderResponse, Balance types

#include <string>
#include <vector>

namespace kusak::exchange {

// ============================================================================
// §1  Shared value types — exposed through the interface
// ============================================================================

// Re-export the canonical Balance type so callers include only this header.
// BinanceHttpClient::Balance is defined in binance_http_client.hpp; we
// forward-declare an equivalent here to keep this header network-free.
struct AssetBalance {
    std::string asset;
    double      free   = 0.0;
    double      locked = 0.0;
};

// Convenience alias: the OrderRequest and OrderResponse already live in
// RestOrderClient (binance_rest_client.hpp), so we import them into the
// interface's namespace to avoid long qualified names at call sites.
using OrderRequest  = RestOrderClient::OrderRequest;
using OrderResponse = RestOrderClient::OrderResponse;

// ============================================================================
// §2  IExchangeClient — pure-virtual interface
// ============================================================================

class IExchangeClient {
public:
    // ── Lifecycle ───────────────────────────────────────────────────────────

    // Virtual destructor is mandatory: the engine holds a unique_ptr<IExchangeClient>
    // and destroys it through the base pointer on every immortality-loop restart.
    virtual ~IExchangeClient() = default;

    // Non-copyable, non-movable: owning smart pointers manage lifetime.
    IExchangeClient(const IExchangeClient&)            = delete;
    IExchangeClient& operator=(const IExchangeClient&) = delete;
    IExchangeClient(IExchangeClient&&)                 = delete;
    IExchangeClient& operator=(IExchangeClient&&)      = delete;

    // ── Balance Queries ─────────────────────────────────────────────────────

    // Returns a snapshot of all asset balances in the account.
    // On network or authentication failure, implementations return an empty
    // vector (never throw); callers must treat an empty result as an error.
    virtual std::vector<AssetBalance> fetch_all_balances() = 0;

    // Convenience: returns the *free* balance for a single asset (e.g. "USDT").
    // Returns 0.0 when the asset is not found or on error.
    // Default implementation delegates to fetch_all_balances() so concrete
    // classes only need to override one method if preferred.
    virtual double fetch_balance(const std::string& asset) {
        for (const auto& b : fetch_all_balances()) {
            if (b.asset == asset) return b.free;
        }
        return 0.0;
    }

    // ── Order Management ────────────────────────────────────────────────────

    // Submits a new order.
    //
    //   req          — order parameters (symbol, side, type, qty, price …)
    //   ref_price    — current mid price used by FilterValidator for
    //                  PERCENT_PRICE checks; 0.0 skips that validation.
    //
    // Returns an OrderResponse whose `success` field is the single source of
    // truth.  Implementations must NEVER throw; all errors are encoded in
    // OrderResponse::error.
    virtual OrderResponse place_order(const OrderRequest& req,
                                      double ref_price = 0.0) = 0;

    // ── Diagnostics / Meta ──────────────────────────────────────────────────

    // Human-readable name used in log messages to identify the client type.
    // e.g. "LiveBinanceClient", "SimulatedExchangeClient".
    virtual std::string client_name() const = 0;

    // Returns true when this client routes orders to real exchange endpoints.
    // Strategy engine uses this flag to add extra safety guards (e.g. capital
    // confirmation prompts, kill-switch activation).
    virtual bool is_live() const noexcept = 0;

protected:
    // Protected default constructor: interface is only constructible through
    // derived classes.
    IExchangeClient() = default;
};

} // namespace kusak::exchange
