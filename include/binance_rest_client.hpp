// ============================================================================
// KUSAK V12 — LIVE TRADING (REVİZE)
// binance_rest_client.hpp – Binance REST emir arayüzü, locale‑safe ve encode edilmiş
//
// DENETİM RAPORU GEREKSİNİMLERİ:
//   ✓ fmt8() → std::locale::classic() (virgül asla çıkmaz)
//   ✓ build_orderParams() → PercentEncoder kullanır
//   ✓ MARKET emirlerinde quoteOrderQty desteği
//   ✓ Filtre doğrulama (LOT_SIZE, PRICE_FILTER, MIN_NOTIONAL, PERCENT_PRICE)
//
// PATCH v12.1:
//   ✓ HttpResponse struct – HTTP status kodu körlüğü giderildi
//   ✓ HttpFunc / AccountHttpFunc – HttpResponse döner
//   ✓ OrderResponse – http_status alanı eklendi
//   ✓ new_order() – HTTP 2xx + orderId/status doğrulaması (ghost-trade kill-switch)
//   ✓ Endpoint routing – member değişkenler + set_market_endpoints() setter
//
// [FIX-NETWORK-2]: refresh_exchange_info() — güvensiz parse giderildi
//   1. looks_like_json_body(): 502/504 HTML → erken dön, parse denenmez
//   2. nlohmann::json::parse(..., false): exception atmaz, is_discarded kontrol edilir
//   3. extract lambda: std::stod kaldırıldı → safe_parse_double_sv() kullanılır
//      (exception-safe strtod/from_chars tabanlı dönüşüm)
//   4. fetch_account() → aynı savunmacı parse pattern'i uygulandı
// ============================================================================

#pragma once

#include "binance_signer.hpp"          // PercentEncoder
#include "research_types.hpp"          // FilterResult, SymbolFilterCache
#include "engine/send_outcome.hpp"     // [FIX-P36-C15] SendOutcome / classify_send_outcome

#include <nlohmann/json.hpp>

#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <memory>   // [FIX-RACE-1] shared_ptr + atomic_load/store icin
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kusak::exchange {

// ============================================================================
// §0  [FIX-NETWORK-2] Parse yardımcıları — dosya genelinde kullanılır
// ============================================================================

// [FIX-NETWORK-2]: güvenli double parse — exception-free, stod/strtod yerine
// from_chars float desteği C++17'de implementation-defined; strtod fallback ile
// tam portabilite. Boş/boşluk dolu string, "NaN", "-Inf" → false döner.
static inline bool safe_parse_double_sv(std::string_view sv, double& out) noexcept {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())))
        sv.remove_prefix(1);
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back())))
        sv.remove_suffix(1);
    if (sv.empty()) return false;

#if defined(__cpp_lib_to_chars) && (__cpp_lib_to_chars >= 201611L)
    const char* b = sv.data();
    const char* e = sv.data() + sv.size();
    auto res = std::from_chars(b, e, out);
    return res.ec == std::errc{} && res.ptr == e;
#else
    std::string tmp(sv);
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(tmp.c_str(), &end);
    if (errno != 0 || end != tmp.c_str() + tmp.size()) return false;
    out = v;
    return true;
#endif
}

// [FIX-NETWORK-2]: Hızlı içerik tipi kontrolü.
// Binance 502/504 durumunda HTML döndürür; ilk anlamlı karakter '{' veya '['
// değilse JSON parse hiç denenmez → nlohmann exception riski sıfırlanır.
static inline bool looks_like_json_body(std::string_view body) noexcept {
    for (unsigned char c : body) {
        if (std::isspace(c)) continue;
        return c == '{' || c == '[';
    }
    return false;
}

// ============================================================================
// §1  FilterResult (korundu)
// ============================================================================

enum class FilterResult : uint8_t {
    VALID               = 0,
    PRICE_TOO_LOW       = 1,
    PRICE_TOO_HIGH      = 2,
    PRICE_FILTER_FAIL   = 3,
    QTY_TOO_SMALL       = 4,
    QTY_TOO_LARGE       = 5,
    QTY_STEP_FAIL       = 6,
    NOTIONAL_TOO_LOW    = 7,
    PERCENT_PRICE_FAIL  = 8,
    UNKNOWN_SYMBOL      = 9,
};

inline const char* filter_result_name(FilterResult r) noexcept {
    switch (r) {
        case FilterResult::VALID:              return "VALID";
        case FilterResult::PRICE_TOO_LOW:      return "PRICE_TOO_LOW";
        case FilterResult::PRICE_TOO_HIGH:     return "PRICE_TOO_HIGH";
        case FilterResult::PRICE_FILTER_FAIL:  return "PRICE_FILTER_FAIL";
        case FilterResult::QTY_TOO_SMALL:      return "QTY_TOO_SMALL";
        case FilterResult::QTY_TOO_LARGE:      return "QTY_TOO_LARGE";
        case FilterResult::QTY_STEP_FAIL:      return "QTY_STEP_FAIL";
        case FilterResult::NOTIONAL_TOO_LOW:   return "NOTIONAL_TOO_LOW";
        case FilterResult::PERCENT_PRICE_FAIL: return "PERCENT_PRICE_FAIL";
        case FilterResult::UNKNOWN_SYMBOL:     return "UNKNOWN_SYMBOL";
        default:                               return "UNKNOWN";
    }
}

// ============================================================================
// §2  SymbolFilterCache + FilterValidator (korundu, hata düzeltmeleri eklendi)
// ============================================================================

struct SymbolFilterCache {
    std::string symbol;
    double   min_price        = 0.0;
    double   max_price        = 1e15;
    double   tick_size        = 0.01;
    double   min_qty          = 0.00001;
    double   max_qty          = 9000.0;
    double   step_size        = 0.00001;
    double   min_notional     = 10.0;
    double   multiplier_up    = 1.05;
    double   multiplier_down  = 0.95;
    uint64_t last_update_s    = 0;
    bool     valid            = false;
};

class FilterValidator {
public:
    using FilterCache = std::unordered_map<std::string, SymbolFilterCache>;

    FilterValidator() = default;

    void update_filter(const SymbolFilterCache& fc) {
        // [FIX-RACE-1] Mevcut map'in KOPYASINI al, kopya uzerinde degisiklik
        // yap, sonra pointer'i tek bir atomik islemle degistir. Okuyan thread
        // hep ya eski ya da yeni tam-tutarli haritayi gorur, asla yarim degil.
        auto old_ptr = std::atomic_load(&cache_ptr_);
        auto new_map = std::make_shared<FilterCache>(*old_ptr);
        auto& entry = (*new_map)[fc.symbol];
        entry = fc;
        entry.last_update_s = now_s();
        entry.valid = true;
        std::atomic_store(&cache_ptr_, std::shared_ptr<const FilterCache>(new_map));
    }

    bool has_symbol(const std::string& symbol) const noexcept {
        auto snap = std::atomic_load(&cache_ptr_);   // [FIX-RACE-1]
        auto it = snap->find(symbol);
        return it != snap->end() && it->second.valid;
    }

    uint64_t cache_age_s(const std::string& symbol) const noexcept {
        auto snap = std::atomic_load(&cache_ptr_);   // [FIX-RACE-1]
        auto it = snap->find(symbol);
        if (it == snap->end()) return UINT64_MAX;
        return now_s() - it->second.last_update_s;
    }

    bool needs_refresh(const std::string& symbol,
                       uint64_t max_age_s = 3600) const noexcept
    {
        return cache_age_s(symbol) >= max_age_s;
    }

    FilterResult validate_order(const std::string& symbol,
                                double             order_price,
                                double             order_qty,
                                double             current_price = 0.0) const noexcept
    {
        auto snap = std::atomic_load(&cache_ptr_);   // [FIX-RACE-1]
        auto it = snap->find(symbol);
        if (it == snap->end() || !it->second.valid)
            return FilterResult::UNKNOWN_SYMBOL;

        const auto& f = it->second;

        // PRICE_FILTER
        if (order_price < f.min_price) return FilterResult::PRICE_TOO_LOW;
        if (order_price > f.max_price) return FilterResult::PRICE_TOO_HIGH;
        if (f.tick_size > 1e-12) {
            double rounded = std::round(order_price / f.tick_size) * f.tick_size;
            if (std::fabs(rounded - order_price) > f.tick_size * 1e-6)
                return FilterResult::PRICE_FILTER_FAIL;
        }

        // LOT_SIZE
        if (order_qty < f.min_qty) return FilterResult::QTY_TOO_SMALL;
        if (order_qty > f.max_qty) return FilterResult::QTY_TOO_LARGE;
        if (f.step_size > 1e-12) {
            double rounded = std::round(order_qty / f.step_size) * f.step_size;
            if (std::fabs(rounded - order_qty) > f.step_size * 1e-6)
                return FilterResult::QTY_STEP_FAIL;
        }

        // MIN_NOTIONAL
        if (order_price * order_qty < f.min_notional)
            return FilterResult::NOTIONAL_TOO_LOW;

        // PERCENT_PRICE (mevcut fiyata göre izin verilen fiyat aralığı)
        if (current_price > 1e-12) {
            if (order_price > current_price * f.multiplier_up)
                return FilterResult::PERCENT_PRICE_FAIL;
            if (order_price < current_price * f.multiplier_down)
                return FilterResult::PERCENT_PRICE_FAIL;
        }

        return FilterResult::VALID;
    }

    // [FIX-P36-C14] MARKET emir yerel dogrulamasi. MARKET emrin fiyati yoktur (req.price=0); validate_order
    // fiyat filtresinde (PRICE_TOO_LOW / NOTIONAL) her MARKET'i reddediyordu. Burada:
    //  - KORUNUR: LOT_SIZE (min_qty, max_qty, stepSize) ve MIN_NOTIONAL (qty * ref_price).
    //  - ATLANIR: PRICE_FILTER (min/max/tick) ve PERCENT_PRICE -- fiyatsiz emirde anlamsiz.
    //  - Fail-closed: ref_price veya qty 0 / NaN / inf ise RED (PRICE_TOO_LOW = gecersiz referans fiyat).
    // [DOGRULANMADI] Binance MARKET_LOT_SIZE filtresi onbellekte yok; LOT_SIZE ile dogrulanir.
    FilterResult validate_market_order(const std::string& symbol,
                                       double             order_qty,
                                       double             ref_price) const noexcept
    {
        auto snap = std::atomic_load(&cache_ptr_);
        auto it = snap->find(symbol);
        if (it == snap->end() || !it->second.valid)
            return FilterResult::UNKNOWN_SYMBOL;

        const auto& f = it->second;

        if (!std::isfinite(ref_price) || !(ref_price > 0.0)) return FilterResult::PRICE_TOO_LOW;
        if (!std::isfinite(order_qty) || !(order_qty > 0.0)) return FilterResult::QTY_TOO_SMALL;

        // LOT_SIZE (validate_order ile ayni kurallar)
        if (order_qty < f.min_qty) return FilterResult::QTY_TOO_SMALL;
        if (order_qty > f.max_qty) return FilterResult::QTY_TOO_LARGE;
        if (f.step_size > 1e-12) {
            double rounded = std::round(order_qty / f.step_size) * f.step_size;
            if (std::fabs(rounded - order_qty) > f.step_size * 1e-6)
                return FilterResult::QTY_STEP_FAIL;
        }

        // MIN_NOTIONAL (referans fiyatla)
        const double notional = order_qty * ref_price;
        if (!std::isfinite(notional) || notional < f.min_notional)
            return FilterResult::NOTIONAL_TOO_LOW;

        return FilterResult::VALID;
    }

    // [FIX-P0-03] stepSize okuma (firewall EXIT miktar toleransi); sembol yoksa 0.0 (cagiran fallback uygular)
    double step_size_of(const std::string& symbol) const noexcept {
        auto snap = std::atomic_load(&cache_ptr_);
        auto it = snap->find(symbol);
        return (it == snap->end()) ? 0.0 : it->second.step_size;
    }

    double round_qty(const std::string& symbol, double qty) const noexcept {
        auto snap = std::atomic_load(&cache_ptr_);   // [FIX-RACE-1]
        auto it = snap->find(symbol);
        if (it == snap->end()) return qty;
        double step = it->second.step_size;
        if (step < 1e-12) return qty;
        int64_t ticks = static_cast<int64_t>(std::round(qty / step));
        return static_cast<double>(ticks) * step;
    }
        double floor_qty(const std::string& symbol, double qty) const noexcept {
        auto snap = std::atomic_load(&cache_ptr_);
        auto it = snap->find(symbol);
        if (it == snap->end()) return qty;
        const double step = it->second.step_size;
        if (step < 1e-12) return std::floor(qty);   // [FIX-P35] step yok dali degismedi
        return static_cast<double>(static_cast<int64_t>(std::floor(qty / step + 1e-9))) * step;   // [FIX-P35]
    }
    double round_price(const std::string& symbol, double price) const noexcept {
        auto snap = std::atomic_load(&cache_ptr_);   // [FIX-RACE-1]
        auto it = snap->find(symbol);
        if (it == snap->end()) return price;
        double tick = it->second.tick_size;
        if (tick < 1e-12) return price;
        int64_t ticks = static_cast<int64_t>(std::round(price / tick));
        return static_cast<double>(ticks) * tick;
    }

    double min_qty_for_notional(const std::string& symbol,
                                double price) const noexcept
    {
        auto snap = std::atomic_load(&cache_ptr_);   // [FIX-RACE-1]
        auto it = snap->find(symbol);
        if (it == snap->end() || price < 1e-12) return 0.0;
        double min_n = it->second.min_notional;
        double step  = it->second.step_size;
        double raw   = min_n / price;
        if (step > 1e-12)
            raw = std::ceil(raw / step) * step;
        return raw;
    }

    // [FIX-RACE-1] Not: bu artik bir kopya doner (referans degil), cunku ic
    // veri artik shared_ptr arkasinda. cache() cagiran yerler (varsa) hala
    // calisir ama artik anlik-goruntu uzerinde calisir.
    FilterCache cache() const noexcept { return *std::atomic_load(&cache_ptr_); }

private:
    static uint64_t now_s() noexcept {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
    }

    // [FIX-RACE-1] Eskiden korumasiz std::unordered_map idi -- bg_thread_
    // (ExchangeInfoManager) yazarken, strateji thread'i ayni anda okuyordu,
    // bu tanimsiz davranis (UB) ve cokme riski demekti. Artik okuyan taraf
    // hicbir kilide girmeden atomic_load ile anlik goruntu (snapshot) alir;
    // yazan taraf yeni bir kopya hazirlayip atomic_store ile pointer'i degistirir.
    std::shared_ptr<const FilterCache> cache_ptr_ = std::make_shared<const FilterCache>();
};

// ============================================================================
// §3  BinanceError (korundu)
// ============================================================================

struct BinanceError {
    int         code    = 0;
    std::string message;

    bool is_min_notional()      const noexcept { return code == -1013; }
    bool is_percent_price()     const noexcept { return code == -1111; }
    bool is_invalid_param()     const noexcept { return code == -1100; }
    bool is_rate_limit()        const noexcept { return code == -1003; }
    bool is_bad_api_key()       const noexcept { return code == -2014; }
    bool is_timestamp()         const noexcept { return code == -1021; }
    bool is_invalid_signature() const noexcept { return code == -1022; }

    std::string describe() const {
        switch (code) {
            case -1013: return "MIN_NOTIONAL: notional below minimum threshold";
            case -1111: return "PERCENT_PRICE: price outside permitted band";
            case -1100: return "INVALID_PARAMETER: malformed order parameter";
            case -1003: return "RATE_LIMIT: request rate exceeded";
            case -2014: return "BAD_API_KEY: API key invalid or missing";
            case -1021: return "TIMESTAMP: timestamp out of recvWindow";
            case -1022: return "INVALID_SIGNATURE: signature check failed";
            case -2010: return "NEW_ORDER_REJECTED: order rejected by exchange";
            case -2011: return "CANCEL_REJECTED: cancel failed";
            default:    return "Binance error code=" + std::to_string(code)
                               + " msg=" + message;
        }
    }
};

inline BinanceError parse_error_response(std::string_view body) noexcept {
    BinanceError e;
    try {
        auto j = nlohmann::json::parse(body);
        if (j.contains("code") && j["code"].is_number_integer())
            e.code = j["code"].get<int>();
        if (j.contains("msg") && j["msg"].is_string())
            e.message = j["msg"].get<std::string>();
    } catch (...) {
        e.message = "(non-JSON) " + std::string(body.substr(0, 120));
    }
    return e;
}

// ============================================================================
// §4  AccountResponse (korundu)
// ============================================================================

struct AccountResponse {
    bool        ok                = false;
    std::string account_type;
    bool        has_account_type  = false;
    bool        has_balances      = false;
    std::string diagnostics;
    std::string raw_body;
};

// ============================================================================
// §5  HttpResponse – PATCH v12.1
// ============================================================================

struct HttpResponse {
    long        status_code = 0;
    std::string body;
    // [FIX-P37-C19A] false YALNIZ transport katmani hicbir istek baytinin gonderilmedigini kanitladiginda.
    // Varsayilan true (fail-closed). Uretimde BinanceHttpClient::make_signed_post_func doldurur.
    bool        request_sent = true;

    bool ok() const noexcept { return status_code >= 200 && status_code < 300; }
};

// ============================================================================
// §6  RestOrderClient
// ============================================================================

class RestOrderClient {
public:
    // PATCH v12.1: HttpFunc / AccountHttpFunc → HttpResponse döner
    using HttpFunc = std::function<HttpResponse(
        const std::string& endpoint,
        const std::string& params)>;

    using AccountHttpFunc = std::function<HttpResponse(
        const std::string& endpoint,
        const std::string& query,
        long               recv_window_ms)>;

    struct OrderRequest {
        std::string symbol;
        std::string side;                    // "BUY" / "SELL"
        std::string type;                    // "LIMIT" / "MARKET"
        std::string time_in_force = "GTC";
        double      price           = 0.0;
        double      quantity        = 0.0;   // base asset quantity
        double      quote_order_qty = 0.0;   // MARKET only: quote asset qty
        std::string client_oid;
        bool        reduce_only     = false;     // [FIX-P0-03] yalniz FUTURES EXIT
    };

    struct OrderResponse {
        bool         success     = false;
        uint64_t     order_id    = 0;
        std::string  status;
        double       fill_qty    = 0.0;      // executed quantity
        double       fill_price  = 0.0;      // average fill price
        BinanceError error;
        std::string  raw_body;
        long         http_status = 0;        // PATCH v12.1
        // [FIX-P36-C15] new_order sonucu sinifi. Varsayilan AMBIGUOUS (fail-closed): set edilmeyen yol belirsiz sayilir.
        SendOutcome  outcome     = SendOutcome::AMBIGUOUS;
    };

    explicit RestOrderClient(FilterValidator& validator,
                             HttpFunc         http_func = nullptr)
        : validator_(validator)
        , http_func_(std::move(http_func))
    {}

    void set_account_http_func(AccountHttpFunc fn) {
        account_http_func_ = std::move(fn);
    }

    void set_debug_logging(bool enabled) noexcept {
        debug_logging_ = enabled;
    }

    // PATCH v12.1: Futures / Spot endpoint routing
    void set_market_endpoints(bool futures_usdtm) {
        if (futures_usdtm) {
            order_endpoint_    = "/fapi/v1/order";
            account_endpoint_  = "/fapi/v2/account";
            exchinfo_endpoint_ = "/fapi/v1/exchangeInfo";
        } else {
            order_endpoint_    = "/api/v3/order";
            account_endpoint_  = "/api/v3/account";
            exchinfo_endpoint_ = "/api/v3/exchangeInfo";
        }
    }

    // ========================================================================
    // new_order – PATCH v12.1: HTTP 2xx + orderId/status doğrulaması
    // ========================================================================
    OrderResponse new_order(const OrderRequest& req,
                            double              current_price = 0.0) noexcept
    {
        OrderResponse resp;

        // Local filter validation
        // [FIX-P36-C14] MARKET: fiyat yok (req.price=0); LOT_SIZE + MIN_NOTIONAL current_price (referans) ile.
        //               Diger tipler: mevcut validate_order (degismedi).
        const bool is_market = (req.type == "MARKET");
        FilterResult fr = is_market
            ? validator_.validate_market_order(req.symbol, req.quantity, current_price)
            : validator_.validate_order(req.symbol, req.price, req.quantity, current_price);

        if (fr != FilterResult::VALID) {
            resp.outcome       = classify_send_outcome(SendStage::LOCAL, 0, 0);   // [FIX-P36-C15] ag'a cikmadi
            resp.success       = false;
            resp.error.message = filter_result_name(fr);
            resp.error.code    = (fr == FilterResult::NOTIONAL_TOO_LOW)   ? -1013
                               : (fr == FilterResult::PERCENT_PRICE_FAIL) ? -1111
                               : -1100;
            if (debug_logging_)
                dbg_order_error("LOCAL_FILTER", req, resp.error);
            return resp;
        }

        if (!http_func_) {
            resp.outcome       = classify_send_outcome(SendStage::LOCAL, 0, 0);   // [FIX-P36-C15] ag'a cikmadi
            resp.error.code    = -9999;
            resp.error.message = "HTTP client not connected";
            return resp;
        }

        try {
            const std::string params = build_order_params(req);

            if (debug_logging_)
                std::cerr << "[RestOrderClient][DEBUG] POST " << order_endpoint_
                          << " params=" << params << "\n";

            // PATCH v12.1: HttpResponse döner
            const HttpResponse http = http_func_(order_endpoint_, params);
            resp.http_status = http.status_code;
            resp.raw_body    = http.body;

            if (debug_logging_)
                std::cerr << "[RestOrderClient][DEBUG] HTTP " << resp.http_status
                          << " body(300c): " << resp.raw_body.substr(0, 300) << "\n";

            // PATCH v12.1: HTTP 2xx değilse ghost-trade kill-switch
            if (!http.ok()) {
                resp.success = false;
                resp.error   = parse_error_response(resp.raw_body);
                // [FIX-P36-C15] siniflama -9001 sentinel'inden ONCE: ayristirilan Binance kodu (yoksa 0) ile.
                resp.outcome = classify_send_outcome(SendStage::HTTP_RESPONSE, resp.http_status, resp.error.code,
                                                     http.request_sent);   // [FIX-P37-C19A]
                if (resp.error.code == 0) {
                    resp.error.code    = -9001;
                    resp.error.message = "HTTP non-2xx: " + std::to_string(resp.http_status);
                }
                if (debug_logging_)
                    dbg_order_error("HTTP_NON_2XX", req, resp.error);
                return resp;
            }

            // HTTP 2xx → JSON parse
            parse_new_order_response(resp);

            // PATCH v12.1: orderId + status doğrulaması
            if (resp.order_id == 0 || resp.status.empty()) {
                resp.success = false;
                resp.outcome = classify_send_outcome(SendStage::PARSE_FAIL_2XX, resp.http_status, 0);   // [FIX-P36-C15]
                if (resp.error.code == 0) {
                    resp.error.code    = -9002;
                    resp.error.message = "HTTP 2xx but missing orderId/status";
                }
                if (debug_logging_)
                    dbg_order_error("MISSING_ORDERID_STATUS", req, resp.error);
            } else {
                resp.outcome = SendOutcome::ACCEPTED;   // [FIX-P36-C15]
            }

        } catch (const std::exception& ex) {
            resp.outcome = classify_send_outcome(SendStage::EXCEPTION, 0, 0);   // [FIX-P36-C15]
            resp.success = false;
            resp.error   = parse_error_response(ex.what());
            if (resp.error.code == 0) {
                resp.error.code    = -9998;
                resp.error.message = ex.what();
            }
            if (debug_logging_)
                dbg_order_error("REST_EXCEPTION", req, resp.error);
        } catch (...) {
            resp.outcome       = classify_send_outcome(SendStage::EXCEPTION, 0, 0);   // [FIX-P36-C15]
            resp.success       = false;
            resp.error.code    = -9997;
            resp.error.message = "Unknown exception in new_order";
        }

        return resp;
    }

    // ========================================================================
    // query_order – emir sorgulama
    // ========================================================================
    OrderResponse query_order(const std::string& symbol,
                              uint64_t           order_id) noexcept
    {
        OrderResponse resp;
        if (!http_func_) {
            resp.error.message = "HTTP client not connected";
            return resp;
        }
        try {
            const std::string params =
                "symbol=" + symbol + "&orderId=" + std::to_string(order_id);
            if (debug_logging_)
                std::cerr << "[RestOrderClient][DEBUG] query_order params=" << params << "\n";
            const HttpResponse http = http_func_(order_endpoint_, params);
            resp.http_status = http.status_code;
            resp.raw_body    = http.body;

            if (!http.ok()) {
                resp.success = false;
                resp.error   = parse_error_response(resp.raw_body);
                return resp;
            }
            parse_query_response(resp);
        } catch (const std::exception& ex) {
            resp.success = false;
            resp.error   = parse_error_response(ex.what());
        }
        return resp;
    }

    // ========================================================================
    // fetch_account – hesap bilgisi (SIGNED GET)
    // [FIX-NETWORK-2]: looks_like_json_body + allow_exceptions=false guard eklendi
    // ========================================================================
    AccountResponse fetch_account(long recv_window_ms = 5000) noexcept {
        AccountResponse ar;
        if (!account_http_func_) {
            ar.diagnostics = "account_http_func_ not set";
            return ar;
        }
        try {
            const HttpResponse http = account_http_func_(account_endpoint_, "", recv_window_ms);
            ar.raw_body = http.body;
            if (!http.ok()) {
                ar.diagnostics = "HTTP non-2xx: " + std::to_string(http.status_code);
                return ar;
            }

            // [FIX-NETWORK-2]: HTML / binary body erken reddi
            if (!looks_like_json_body(ar.raw_body)) {
                ar.diagnostics = "fetch_account returned non-JSON body";
                return ar;
            }

            // [FIX-NETWORK-2]: allow_exceptions=false — is_discarded kontrol edilir
            const auto j = nlohmann::json::parse(ar.raw_body, nullptr, /*allow_exceptions=*/false);
            if (j.is_discarded() || !j.is_object()) {
                ar.diagnostics = "fetch_account JSON parse failed (discarded)";
                return ar;
            }

            if (j.contains("accountType") && j["accountType"].is_string()) {
                ar.account_type     = j["accountType"].get<std::string>();
                ar.has_account_type = true;
            }
            ar.has_balances = j.contains("balances") && j["balances"].is_array();
            ar.ok = ar.has_balances;
        } catch (const std::exception& e) {
            ar.diagnostics = e.what();
        }
        return ar;
    }

    // ========================================================================
    // refresh_exchange_info – PATCH v12.1: exchinfo_endpoint_ kullanır
    // [FIX-NETWORK-2]: HTML/bozuk body erken reddi + güvenli extract lambda
    //   1. looks_like_json_body(): 502/504 HTML → throw "non-JSON body"
    //   2. parse(..., false): exception atmaz, is_discarded kontrol edilir
    //   3. extract(): std::stod kaldırıldı → safe_parse_double_sv()
    //      boş string, "NaN", beklenmedik tipler → değer değişmez (sessiz atla)
    // ========================================================================
    bool refresh_exchange_info(const std::string& symbol) {
        if (!http_func_) throw std::runtime_error("http_func_ not set");

        const HttpResponse http = http_func_(exchinfo_endpoint_, "symbol=" + symbol);
        if (!http.ok()) {
            throw std::runtime_error(
                "exchangeInfo HTTP non-2xx: " + std::to_string(http.status_code));
        }

        // [FIX-NETWORK-2]: HTML/binary body erken kontrol
        if (!looks_like_json_body(http.body)) {
            throw std::runtime_error(
                "exchangeInfo returned non-JSON body (possible 502/504 HTML response)");
        }

        // [FIX-NETWORK-2]: allow_exceptions=false → exception atmaz
        const auto j = nlohmann::json::parse(http.body, nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object()) {
            throw std::runtime_error(
                "exchangeInfo JSON parse failed (malformed or discarded)");
        }
        if (!j.contains("symbols") || !j["symbols"].is_array())
            throw std::runtime_error("symbols array missing in exchangeInfo");

        bool found = false;
        for (const auto& sym_entry : j["symbols"]) {
            if (!sym_entry.is_object()) continue;

            const auto sit = sym_entry.find("symbol");
            if (sit == sym_entry.end() || !sit->is_string()) continue;
            if (sit->get<std::string>() != symbol) continue;

            SymbolFilterCache fc;
            fc.symbol = symbol;

            if (sym_entry.contains("filters") && sym_entry["filters"].is_array()) {
                for (const auto& filter : sym_entry["filters"]) {
                    if (!filter.is_object()) continue;

                    const auto fit = filter.find("filterType");
                    if (fit == filter.end() || !fit->is_string()) continue;
                    const std::string ft = fit->get<std::string>();

                    // [FIX-NETWORK-2]: std::stod kaldırıldı → safe_parse_double_sv()
                    // Boş string, "NaN", HTML parçaları → sessizce 0 döner, out değişmez.
                    auto extract = [&](const char* key, double& out) {
                        const auto it = filter.find(key);
                        if (it == filter.end()) return;
                        if (it->is_number_float() || it->is_number_integer() ||
                            it->is_number_unsigned()) {
                            out = it->get<double>();
                            return;
                        }
                        if (it->is_string()) {
                            const std::string& s = it->get_ref<const std::string&>();
                            double v = 0.0;
                            // [FIX-NETWORK-2]: exception-safe dönüşüm
                            if (safe_parse_double_sv(s, v)) out = v;
                        }
                        // Diğer tipler (null, bool, array, object) → sessizce atla
                    };

                    if (ft == "PRICE_FILTER") {
                        extract("minPrice",  fc.min_price);
                        extract("maxPrice",  fc.max_price);
                        extract("tickSize",  fc.tick_size);
                    } else if (ft == "LOT_SIZE") {
                        extract("minQty",    fc.min_qty);
                        extract("maxQty",    fc.max_qty);
                        extract("stepSize",  fc.step_size);
                    } else if (ft == "MIN_NOTIONAL" || ft == "NOTIONAL") {
                        extract("minNotional", fc.min_notional);
                        extract("notional",    fc.min_notional); // futures alias
                    } else if (ft == "PERCENT_PRICE" || ft == "PERCENT_PRICE_BY_SIDE") {
                        extract("multiplierUp",   fc.multiplier_up);
                        extract("multiplierDown", fc.multiplier_down);
                    }
                }
            }

            fc.valid = true;
            validator_.update_filter(fc);
            found = true;
            break;
        }

        if (!found) throw std::runtime_error("Symbol not found: " + symbol);
        return true;
    }

    FilterValidator& validator() noexcept { return validator_; }

private:
    // ========================================================================
    // fmt8 – locale‑safe sayı formatlama (daima nokta ondalık ayracı)
    // ========================================================================
    static std::string fmt8(double v) {
        std::ostringstream oss;
        oss.imbue(std::locale::classic());
        oss << std::fixed << std::setprecision(8) << v;
        return oss.str();
    }

    // ========================================================================
    // build_order_params – tüm parametreleri encode ederek oluştur
    // ========================================================================
public:   // [FIX-P35] reduceOnly sorgusu testten dogrulanabilsin diye yalniz bu fonksiyon public
    static std::string build_order_params(const OrderRequest& r) {
        std::string out;
        out.reserve(256);

        // PATCH v12.1: using-declaration kaldırıldı, fully-qualified path
        out += "symbol=" + kusak::sign::PercentEncoder::encode(r.symbol) + "&";
        out += "side="   + kusak::sign::PercentEncoder::encode(r.side)   + "&";
        out += "type="   + kusak::sign::PercentEncoder::encode(r.type)   + "&";

        if (r.type == "LIMIT") {
            out += "timeInForce=" + kusak::sign::PercentEncoder::encode(r.time_in_force) + "&";
            out += "price="       + fmt8(r.price)                                        + "&";
        }

        // [FIX-MVP-MAKER]: LIMIT_MAKER, LIMIT'in aksine timeInForce KABUL ETMEZ --
        // borsa emri zimnen "post-only" davranir, cross ederse REDDEDER (400 -2010).
        // timeInForce eklenirse Binance istegi tumden reddeder.
        if (r.type == "LIMIT_MAKER") {
            out += "price=" + fmt8(r.price) + "&";
        }

        if (r.type == "MARKET") {
            if (r.quote_order_qty > 1e-12) {
                out += "quoteOrderQty=" + fmt8(r.quote_order_qty) + "&";
            } else {
                out += "quantity=" + fmt8(r.quantity) + "&";
            }
        } else {
            out += "quantity=" + fmt8(r.quantity) + "&";
        }

        if (!r.client_oid.empty()) {
            out += "newClientOrderId=" + kusak::sign::PercentEncoder::encode(r.client_oid) + "&";
        }

        if (r.reduce_only) {   // [FIX-P0-03]
            out += "reduceOnly=true&";
        }

        return out;
    }
private:   // [FIX-P35] public bolumu sonu

    // ========================================================================
    // parse_new_order_response – cevap ayrıştırma
    // ========================================================================
    void parse_new_order_response(OrderResponse& resp) const {
        try {
            auto j = nlohmann::json::parse(resp.raw_body);
            if (j.contains("code") && !j.contains("orderId")) {
                resp.error   = parse_error_response(resp.raw_body);
                resp.success = false;
                return;
            }
            if (j.contains("orderId"))
                resp.order_id = j["orderId"].get<uint64_t>();
            if (j.contains("status") && j["status"].is_string())
                resp.status = j["status"].get<std::string>();

            // Fix 16: Extract fill_qty and fill_price from JSON
            if (j.contains("executedQty") && j["executedQty"].is_string()) {
                double v = 0.0;
                if (safe_parse_double_sv(j["executedQty"].get<std::string>(), v))
                    resp.fill_qty = v;
            }
            if (j.contains("avgPrice") && j["avgPrice"].is_string()) {
                double v = 0.0;
                if (safe_parse_double_sv(j["avgPrice"].get<std::string>(), v))
                    resp.fill_price = v;
            } else if (j.contains("price") && j["price"].is_string()) {
                double v = 0.0;
                if (safe_parse_double_sv(j["price"].get<std::string>(), v))
                    resp.fill_price = v;
            }

            resp.success = !resp.status.empty();
        } catch (...) {
            resp.success = false;
        }
    }

    static void parse_query_response(OrderResponse& resp) {
        try {
            auto j = nlohmann::json::parse(resp.raw_body);
            if (j.contains("status") && j["status"].is_string())
                resp.status = j["status"].get<std::string>();
            resp.success = !resp.status.empty();
        } catch (...) {
            resp.success = false;
        }
    }

    void dbg_order_error(const char*         ctx,
                         const OrderRequest& req,
                         const BinanceError& e) const noexcept
    {
        std::cerr << "[RestOrderClient][DEBUG] " << ctx
                  << " sym="   << req.symbol
                  << " side="  << req.side
                  << " type="  << req.type
                  << " qty="   << req.quantity
                  << " price=" << req.price
                  << " | code=" << e.code
                  << " msg=" << e.describe() << "\n";
    }

    FilterValidator&  validator_;
    HttpFunc          http_func_;
    AccountHttpFunc   account_http_func_;
    bool              debug_logging_ = false;

    // PATCH v12.1: endpoint routing member'ları
    std::string order_endpoint_    = "/api/v3/order";
    std::string account_endpoint_  = "/api/v3/account";
    std::string exchinfo_endpoint_ = "/api/v3/exchangeInfo";
};

} // namespace kusak::exchange
