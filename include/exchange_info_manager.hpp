// ============================================================================
// KUSAK V12 — LIVE TRADING
// exchange_info_manager.hpp
//
//  ExchangeInfo periyodik çekme ve FilterValidator güncelleme yöneticisi.
//
//  Rapor Madde 1.2:
//   "Başlangıçta ve her exchangeinfo_ttl_s saniyede bir REST ile filtreleri
//    çekip FilterValidator::update_filter ile güncelleyin."
//
//  Parser:
//   • nlohmann/json kullanılarak root["symbols"] dizisi iterate edilir.
//   • Her sembol için filterType anahtarına göre PRICE_FILTER, LOT_SIZE,
//     MIN_NOTIONAL / NOTIONAL, PERCENT_PRICE filtreleri doğru şekilde parse
//     edilir — substring/find hileleri YOK.
//   • Sembol bulunamazsa start() içinden std::runtime_error fırlatılır.
//
//  Özellikler:
//   • Başlangıçta senkron fetch (bağlantı kurulmadan önce filter hazır)
//   • Arka plan thread'i ile periyodik yenileme (TTL bazlı)
//   • Hata durumunda exponential back-off (max 5 dakika)
//   • Thread-safe erişim
//   • Graceful shutdown (stop() veya destructor'da)
//
//  PATCH v12.1:
//   • endpoint_ private üye — varsayılan /api/v3/exchangeInfo
//   • set_market(bool) — Futures için /fapi/v1/exchangeInfo
//   • do_fetch_or_throw() → endpoint_ kullanır (hard-code kaldırıldı)
//   • parse_and_update() → Spot+Futures+edge-case JSON dayanıklılığı
//   • filter parsing → filterType is_string guard + is_object guard
//
//  Kullanım (main.cpp):
//
//   auto http = std::make_shared<kusak::net::BinanceHttpClient>(key, secret);
//   kusak::net::ExchangeInfoManager ei_mgr(
//       http, ctx.filter_validator,
//       {"BTCUSDT", "ETHUSDT"},
//       cfg.exchangeinfo_ttl_s);
//
//   ei_mgr.set_market(cfg.futures_mode);   // PATCH v12.1
//   ei_mgr.start();
//   // ... strateji döngüsü ...
//   ei_mgr.stop();
//
// ============================================================================

#pragma once

#include "binance_http_client.hpp"
#include "binance_rest_client.hpp"   // FilterValidator
#include "research_types.hpp"        // SymbolFilterCache

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace kusak::net {

class ExchangeInfoManager {
public:
    using ErrorCallback = std::function<void(const std::string& msg)>;

    ExchangeInfoManager(std::shared_ptr<BinanceHttpClient> http,
                        kusak::exchange::FilterValidator&  validator,
                        std::vector<std::string>           symbols,
                        uint64_t                           ttl_s    = 3600,
                        ErrorCallback                      on_error = nullptr)
        : http_(std::move(http))
        , validator_(validator)
        , symbols_(std::move(symbols))
        , ttl_s_(ttl_s)
        , on_error_(std::move(on_error))
    {}

    ~ExchangeInfoManager() { stop(); }

    ExchangeInfoManager(const ExchangeInfoManager&)            = delete;
    ExchangeInfoManager& operator=(const ExchangeInfoManager&) = delete;

    // PATCH v12.1: Futures/Spot endpoint seçimi
    void set_market(bool futures_usdtm) {
        endpoint_ = futures_usdtm ? "/fapi/v1/exchangeInfo" : "/api/v3/exchangeInfo";
    }

    void start() {
        if (running_.exchange(true)) return;
        do_fetch_or_throw();
        last_success_ = std::chrono::steady_clock::now();
        bg_thread_ = std::thread([this] { bg_loop(); });
    }

    void stop() noexcept {
        running_ = false;
        cv_.notify_all();
        if (bg_thread_.joinable()) bg_thread_.join();
    }

    bool refresh_now() noexcept {
        try {
            do_fetch_or_throw();
            last_success_ = std::chrono::steady_clock::now();
            return true;
        } catch (const std::exception& e) {
            notify_error(std::string("refresh_now failed: ") + e.what());
            return false;
        } catch (...) {
            notify_error("refresh_now: unknown exception");
            return false;
        }
    }

    uint64_t age_s() const noexcept {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<seconds>(
                steady_clock::now() - last_success_).count());
    }

    uint64_t total_fetches()  const noexcept { return total_fetches_.load();  }
    uint64_t total_failures() const noexcept { return total_failures_.load(); }

private:
    void bg_loop() noexcept {
        uint64_t backoff_s = 30;

        while (running_.load()) {
            {
                std::unique_lock<std::mutex> lk(sleep_mu_);
                cv_.wait_for(lk, std::chrono::seconds(ttl_s_),
                             [this]{ return !running_.load(); });
            }
            if (!running_.load()) break;

            try {
                do_fetch_or_throw();
                last_success_ = std::chrono::steady_clock::now();
                backoff_s     = 30;
            } catch (const std::exception& e) {
                ++total_failures_;
                notify_error(std::string("bg_loop fetch failed: ") + e.what()
                             + " -- retry in " + std::to_string(backoff_s) + "s");
                {
                    std::unique_lock<std::mutex> lk(sleep_mu_);
                    cv_.wait_for(lk, std::chrono::seconds(backoff_s),
                                 [this]{ return !running_.load(); });
                }
                backoff_s = std::min(backoff_s * 2, uint64_t{300});
            } catch (...) {
                ++total_failures_;
                notify_error("bg_loop: unknown exception");
            }
        }
    }

    // ────────────────────────────────────────────────────────────────────────
    // do_fetch_or_throw():
    //   PATCH v12.1: hard-coded "/api/v3/exchangeInfo" → endpoint_ member
    // ────────────────────────────────────────────────────────────────────────
    void do_fetch_or_throw() {
        if (!http_) throw std::runtime_error("[ExchangeInfoManager] http_ is null");

        // ── Build query string ──────────────────────────────────────────────
        std::string query;
        if (symbols_.size() == 1) {
            query = "symbol=" + symbols_.front();
        } else {
            query = "symbols=[";
            for (std::size_t i = 0; i < symbols_.size(); ++i) {
                query += "%22" + symbols_[i] + "%22";
                if (i + 1 < symbols_.size()) query += ',';
            }
            query += ']';
        }

        // PATCH v12.1: endpoint_ kullanılıyor (Spot veya Futures)
        const HttpResponse resp =
            http_->get_public(endpoint_.c_str(), query.c_str(), /*weight=*/10);

        if (!resp.ok()) {
            throw std::runtime_error(
                "[ExchangeInfoManager] HTTP " +
                std::to_string(resp.status_code) +
                " fetching " + endpoint_ +
                ". Body: " + resp.body.substr(0, 256));
        }

        parse_and_update(resp.body);
    }

    // ────────────────────────────────────────────────────────────────────────
    // parse_and_update():
    //   PATCH v12.1:
    //   • Spot+Futures+edge-case JSON arama mantığı
    //     (root["symbols"] array | object | root doğrudan symbol objesi)
    //   • filterType is_string + is_object guard'ları
    //   • MIN_NOTIONAL + NOTIONAL ikisi de işleniyor (zaten vardı, korundu)
    // ────────────────────────────────────────────────────────────────────────
    void parse_and_update(const std::string& body) {
        nlohmann::json root;
        try {
            root = nlohmann::json::parse(body);
        } catch (const nlohmann::json::exception& e) {
            throw std::runtime_error(
                std::string("[ExchangeInfoManager] JSON parse error: ") +
                e.what() + " | body prefix: " + body.substr(0, 128));
        }

        std::vector<bool> found(symbols_.size(), false);

        // ── Sembol kaydı için yardımcı lambda ──────────────────────────────
        auto process_sym_entry = [&](const nlohmann::json& sym_entry) {
            if (!sym_entry.is_object()) return;
            if (!sym_entry.contains("symbol") || !sym_entry["symbol"].is_string())
                return;

            const std::string sym = sym_entry["symbol"].get<std::string>();

            std::size_t target_idx = symbols_.size();
            for (std::size_t i = 0; i < symbols_.size(); ++i) {
                if (symbols_[i] == sym) { target_idx = i; break; }
            }
            if (target_idx == symbols_.size()) return;

            kusak::exchange::SymbolFilterCache fc;
            fc.symbol        = sym;
            fc.min_price     = 0.0;
            fc.max_price     = 1e15;
            fc.tick_size     = 0.01;
            fc.min_qty       = 0.00001;
            fc.max_qty       = 9000.0;
            fc.step_size     = 0.00001;
            fc.min_notional  = 10.0;
            fc.multiplier_up = 1.05;
            fc.multiplier_down = 0.95;
            fc.valid         = false;

            // PATCH v12.1: filter guard — is_object + filterType is_string
            if (sym_entry.contains("filters") && sym_entry["filters"].is_array()) {
                for (const auto& filter : sym_entry["filters"]) {
                    if (!filter.is_object()) continue;

                    std::string ft;
                    if (filter.contains("filterType")) {
                        if (filter["filterType"].is_string())
                            ft = filter["filterType"].get<std::string>();
                        else if (!filter["filterType"].is_null())
                            ft = filter["filterType"].dump();  // son çare
                    }
                    if (ft.empty()) continue;

                    if (ft == "PRICE_FILTER") {
                        extract_double(filter, "minPrice", fc.min_price);
                        extract_double(filter, "maxPrice", fc.max_price);
                        extract_double(filter, "tickSize", fc.tick_size);
                    }
                    else if (ft == "LOT_SIZE") {
                        extract_double(filter, "minQty",   fc.min_qty);
                        extract_double(filter, "maxQty",   fc.max_qty);
                        extract_double(filter, "stepSize", fc.step_size);
                    }
                    // Spot: MIN_NOTIONAL / Futures: NOTIONAL
                    else if (ft == "MIN_NOTIONAL" || ft == "NOTIONAL") {
                        extract_double(filter, "minNotional", fc.min_notional);
                    }
                    else if (ft == "PERCENT_PRICE") {
                        extract_double(filter, "multiplierUp",   fc.multiplier_up);
                        extract_double(filter, "multiplierDown", fc.multiplier_down);
                    }
                    else if (ft == "PERCENT_PRICE_BY_SIDE") {
                        extract_double(filter, "bidMultiplierUp",   fc.multiplier_up);
                        extract_double(filter, "bidMultiplierDown", fc.multiplier_down);
                    }
                }
            }

            fc.valid = true;
            validator_.update_filter(fc);
            found[target_idx] = true;

            std::cout << "[ExchangeInfoManager] Parsed " << sym
                      << " tick="         << fc.tick_size
                      << " step="         << fc.step_size
                      << " minNotional="  << fc.min_notional
                      << " multiplierUp=" << fc.multiplier_up
                      << " multiplierDown=" << fc.multiplier_down
                      << "\n";
        };

        // PATCH v12.1: Spot+Futures+edge-case JSON arama
        if (root.contains("symbols")) {
            const auto& syms = root["symbols"];

            if (syms.is_array()) {
                // Standart format (Spot ve Futures)
                for (const auto& s : syms)
                    process_sym_entry(s);
            }
            else if (syms.is_object()) {
                // "symbols" object olarak geldiyse (edge-case)
                process_sym_entry(syms);
            }
        }
        else if (root.is_object() && root.contains("symbol")) {
            // Root doğrudan symbol objesi (bazı wrapper implementasyonlar)
            process_sym_entry(root);
        }
        else {
            throw std::runtime_error(
                "[ExchangeInfoManager] exchangeInfo JSON: "
                "ne 'symbols' alanı ne de doğrudan symbol objesi bulundu. "
                "Body prefix: " + body.substr(0, 128));
        }

        // Her konfigüreli sembolün bulunduğunu doğrula
        for (std::size_t i = 0; i < symbols_.size(); ++i) {
            if (!found[i]) {
                throw std::runtime_error(
                    "[ExchangeInfoManager] Symbol not found in exchangeInfo: " +
                    symbols_[i] +
                    ". Verify the symbol name and exchange connectivity.");
            }
        }

        ++total_fetches_;
        std::cout << "[ExchangeInfoManager] Updated " << symbols_.size()
                  << " symbol(s) via " << endpoint_
                  << ". Total fetches: " << total_fetches_.load() << "\n";
    }

    // ────────────────────────────────────────────────────────────────────────
    static void extract_double(const nlohmann::json& obj,
                                const std::string&    key,
                                double&               out) noexcept
    {
        if (!obj.contains(key)) return;
        const auto& val = obj[key];
        try {
            if (val.is_string()) {
                const std::string s = val.get<std::string>();
                if (!s.empty()) out = std::stod(s);
            } else if (val.is_number()) {
                out = val.get<double>();
            }
        } catch (...) {}
    }

    void notify_error(const std::string& msg) noexcept {
        std::cerr << "[ExchangeInfoManager] ERROR: " << msg << "\n";
        if (on_error_) {
            try { on_error_(msg); } catch (...) {}
        }
    }

    // ── Members ──────────────────────────────────────────────────────────────
    std::shared_ptr<BinanceHttpClient>  http_;
    kusak::exchange::FilterValidator&   validator_;
    std::vector<std::string>            symbols_;
    uint64_t                            ttl_s_;
    ErrorCallback                       on_error_;

    std::atomic<bool>                   running_{false};
    std::thread                         bg_thread_;
    std::mutex                          sleep_mu_;
    std::condition_variable             cv_;

    std::chrono::steady_clock::time_point last_success_{};
    std::atomic<uint64_t>               total_fetches_{0};
    std::atomic<uint64_t>               total_failures_{0};

    // PATCH v12.1: endpoint routing
    std::string endpoint_ = "/api/v3/exchangeInfo";
};

} // namespace kusak::net
