// ============================================================================
// KUSAK V12 — LIVE TRADING
// config.hpp – JSON-based configuration loader & KusakConfig definition
//
// PATCH v12.1:
//   ✓ KusakConfig::market      – "SPOT" | "FUTURES", default "SPOT"
//   ✓ KusakConfig::recvWindow_ms – uint64_t, default 5000
//   ✓ market validation: unknown value → std::cerr warning + fallback "SPOT"
//   ✓ write_example() emits market + recvWindow_ms
// ============================================================================

#pragma once

#include "research_types.hpp"
#include "risk_scorer.hpp"

#include <nlohmann/json.hpp>

#include <sys/stat.h>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace kusak::config {

// ============================================================================
// §1  KusakConfig – all runtime parameters in one flat struct
// ============================================================================

// ─────────────────────────────────────────────────────────────────────────────
// [PHASE3-KS] Kill-switch esikleri. KURAL: deger <= 0  =>  o kontrol KAPALI.
// Tek-sembol motor: "toplam maruziyet" = acik pozisyon + bekleyen emir kilidi.
// ─────────────────────────────────────────────────────────────────────────────
struct KillSwitchConfig {
    bool   enabled                 = true;     // false => tum esik kontrolleri kapali (yine de dis tetik/kilit calisir)
    double max_position_usd        = 0.0;      // MAX_POSITION_USD       : acik pozisyonun USD (mark) degeri
    double max_total_exposure_usd  = 0.0;      // MAX_TOTAL_EXPOSURE     : pozisyon + kilitli emirler
    double max_daily_loss_usd      = 0.0;      // MAX_DAILY_LOSS         : gerceklesen + floating, UTC gun
    int    max_consecutive_losses  = 0;        // MAX_CONSECUTIVE_LOSSES : ardisik zararli kapanis
    int    max_order_rejects       = 0;        // MAX_ORDER_REJECTS      : UTC gun icinde borsa reddi
    double max_cancel_rate         = 0.0;      // MAX_CANCEL_RATE        : iptal/gonderilen (0..1), kayan pencere
    double cancel_rate_window_sec  = 600.0;
    int    cancel_rate_min_orders  = 20;       // ornek az ise oran hesaplanmaz
    double max_latency_ms          = 0.0;      // MAX_LATENCY            : emir gonderim->borsa cevabi (REST RTT)
    int    latency_breach_samples  = 3;        // ardisik kac olcum esigi asmali (tek spike'ta tetikleme yok)
    double max_stale_data_ms       = 0.0;      // MAX_STALE_MARKET_DATA  : yalniz maruziyet varken (pozisyon/emir)
    int    max_ws_disconnects      = 0;        // MAX_WS_DISCONNECT      : pencere icinde kurulu baglanti kopmasi
    double ws_window_sec           = 3600.0;
    double max_balance_drop_pct    = 0.0;      // MAX_ACCOUNT_BALANCE_DROP : HWM'den dusus orani (0.10 = %10)
    double max_order_notional_usd  = 0.0;      // MAX_ORDER_NOTIONAL     : tek emir; asilirsa emir RED + kill-switch
    bool   flatten_on_trip         = true;     // SAFE MODE'da acik pozisyonu kapat (false: oldugu gibi birak)
    std::string lock_file          = "kusak_kill_switch.lock";  // var = kilitli; SILINCE manuel reset
};

struct KusakConfig {

    // ── Identity ─────────────────────────────────────────────────────────────
    std::string symbol        = "BTCUSDT";
    std::string quote_asset   = "USDT";

    // ── Market type (PATCH v12.1) ─────────────────────────────────────────────
    std::string market        = "SPOT";     // "SPOT" | "FUTURES"

    // ── Exchange connectivity ─────────────────────────────────────────────────
    std::string api_key;
    std::string api_secret;
    bool        testnet       = false;
    uint64_t    recvWindow_ms = 5000;       // PATCH v12.1 (was inline default)

    // ── Run mode ─────────────────────────────────────────────────────────────
    bool dry_run              = true;
    bool verbose              = false;
    bool log_raw_account_json = false;

    // ── Capital ───────────────────────────────────────────────────────────────
    double initial_capital    = 0.0;
    double min_free_usdt      = 11.0;
    std::string balance_source = "API";    // "API" | "MANUAL"

    // ── Position sizing ───────────────────────────────────────────────────────
    double buy_pct            = 0.95;      // fraction of usable capital per order
    double kelly_fraction     = 0.0;       // 0 = disabled, use buy_pct
    double max_position_usd   = 0.0;       // 0 = no hard cap

    // ── Risk / kill-switch ────────────────────────────────────────────────────
    double max_fill_rate_floor = 0.05;     // kill-switch below this fill rate
    KillSwitchConfig ks;                   // [PHASE3-KS] bkz. kill_switch bolumu

    // ── VPIN ──────────────────────────────────────────────────────────────────
    // [PHASE5] Kova (bucket) hacmi -- ESKI SURUMDE vpin_bucket_mult hesaplamada HIC okunmuyordu ve kova
    // sabit 1000 birimdi. Artik:
    //   vpin_bucket_volume > 0 : SABIT kova hacmi, TABAN BIRIM cinsinden (BTCUSDT icin BTC). Orn. ADV/50.
    //   vpin_bucket_volume = 0 : OTOMATIK = (ilk vpin_calib_trades islemin ortalama boyutu) x vpin_bucket_mult.
    //                            vpin_bucket_mult = kova basina hedef ISLEM sayisi (alt sinir 10; oneri 20-100).
    double   vpin_bucket_mult   = 50.0;
    double   vpin_bucket_volume = 0.0;
    int      vpin_calib_trades  = 500;
    int      vpin_n_buckets     = 50;
    bool     vpin_use_dynamic   = true;
    int      vpin_dynamic_window = 20;
    double   vpin_dynamic_k     = 2.0;
    double   vpin_vel_surge_thr = 3.0;

    // ── HMM / Regime ──────────────────────────────────────────────────────────
    double hmm_enter_threshold  = 0.65;
    double hmm_persistence      = 0.85;

    // ── OBI (Order Book Imbalance) sinyal esigi ──────────────────────────────
    // [FIX-CONFIG-BRIDGE] Onceden main.cpp icinde +-0.35 olarak sabit
    // kodluydu (hardcoded). Artik kusak_config.json'dan okunuyor.
    double obi_threshold        = 0.35;

    // ── Adverse selection ─────────────────────────────────────────────────────
    uint64_t cancel_track_win_us = 1000;
    double   cancel_thr_bps      = 5.0;

    // ── Latency monitor ───────────────────────────────────────────────────────
    uint64_t latency_alert_us   = 500;
    int      report_every_n     = 1000;

    // ── Telemetry / CSV / Prometheus ──────────────────────────────────────────
    bool        write_csv        = true;
    bool        write_prometheus = false;
    std::string csv_path         = "kusak_telemetry.csv";
    std::string prom_path        = "kusak_metrics.prom";

    // ── ExchangeInfo refresh ──────────────────────────────────────────────────
    uint64_t exchangeinfo_ttl_s  = 3600;

    // ── WebSocket ─────────────────────────────────────────────────────────────
    bool enable_depth            = false;
    bool enable_ticker           = true;
    bool enable_trade            = true;

    // ── Risk scorer weights & thresholds ─────────────────────────────────────
    kusak::research::RiskWeights    risk_weights;
    kusak::research::RiskThresholds risk_thresholds;
};

// ============================================================================
// §2  Detail helpers
// ============================================================================

namespace detail {

inline bool to_bool(const std::string& v) noexcept {
    return v == "true" || v == "1" || v == "yes";
}

inline double to_double(const std::string& v) noexcept {
    try { return std::stod(v); } catch (...) { return 0.0; }
}

inline int to_int(const std::string& v) noexcept {
    try { return std::stoi(v); } catch (...) { return 0; }
}

inline uint64_t to_u64(const std::string& v) noexcept {
    try { return std::stoull(v); } catch (...) { return 0; }
}

// PATCH v12.1: market validation with fallback
inline std::string validated_market(const std::string& raw) noexcept {
    if (raw == "SPOT" || raw == "FUTURES") return raw;
    std::cerr << "[ConfigManager] UYARI: Gecersiz market degeri '"
              << raw << "' -- varsayilan 'SPOT' kullaniliyor.\n";
    return "SPOT";
}

} // namespace detail

// ============================================================================
// §3  ConfigManager – JSON file loader with hot-reload support
// ============================================================================

class ConfigManager {
public:
    explicit ConfigManager(const std::string& path) : path_(path) {}

    // Returns true on success; false if file not found or parse error.
    bool load(KusakConfig& cfg) noexcept {
        std::ifstream f(path_);
        if (!f.is_open()) return false;

        try {
            nlohmann::json j;
            f >> j;
            from_json(j, cfg);
            last_mtime_ = file_mtime();
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[ConfigManager] Parse hatasi: " << e.what() << "\n";
            return false;
        } catch (...) {
            std::cerr << "[ConfigManager] Bilinmeyen parse hatasi\n";
            return false;
        }
    }

    // Returns true if file changed and cfg was reloaded.
    bool check_reload(KusakConfig& cfg) noexcept {
        auto mt = file_mtime();
        if (mt == 0 || mt == last_mtime_) return false;
        return load(cfg);
    }

    // Writes a commented example config file.
    static void write_example(const std::string& path,
                               const KusakConfig& cfg) noexcept
    {
        std::ofstream f(path);
        if (!f.is_open()) {
            std::cerr << "[ConfigManager] write_example: dosya acilamadi: " << path << "\n";
            return;
        }
        f << "{\n";

        // Identity
        f << "  \"symbol\":        \"" << cfg.symbol      << "\",\n";
        f << "  \"quote_asset\":   \"" << cfg.quote_asset << "\",\n";

        // PATCH v12.1
        f << "  \"market\":        \"" << cfg.market << "\",\n";

        // Connectivity
        f << "  \"api_key\":       \"\",\n";
        f << "  \"api_secret\":    \"\",\n";
        f << "  \"testnet\":       " << (cfg.testnet ? "true" : "false") << ",\n";
        f << "  \"recvWindow_ms\": " << cfg.recvWindow_ms << ",\n";

        // Run mode
        f << "  \"dry_run\":       " << (cfg.dry_run  ? "true" : "false") << ",\n";
        f << "  \"verbose\":       " << (cfg.verbose  ? "true" : "false") << ",\n";
        f << "  \"log_raw_account_json\": false,\n";

        // Capital
        f << "  \"initial_capital\":  " << cfg.initial_capital  << ",\n";
        f << "  \"min_free_usdt\":    " << cfg.min_free_usdt    << ",\n";
        f << "  \"balance_source\":   \"" << cfg.balance_source << "\",\n";

        // Sizing
        f << "  \"buy_pct\":          " << cfg.buy_pct         << ",\n";
        f << "  \"kelly_fraction\":   " << cfg.kelly_fraction  << ",\n";
        f << "  \"max_position_usd\": " << cfg.max_position_usd << ",\n";

        // Risk
        f << "  \"max_fill_rate_floor\": " << cfg.max_fill_rate_floor << ",\n";
        {   // [PHASE3-KS]
            const auto& k = cfg.ks;
            f << "  \"kill_switch\": {\n"
              << "    \"enabled\": " << (k.enabled ? "true" : "false") << ",\n"
              << "    \"max_position_usd\": " << k.max_position_usd << ",\n"
              << "    \"max_total_exposure_usd\": " << k.max_total_exposure_usd << ",\n"
              << "    \"max_daily_loss_usd\": " << k.max_daily_loss_usd << ",\n"
              << "    \"max_consecutive_losses\": " << k.max_consecutive_losses << ",\n"
              << "    \"max_order_rejects\": " << k.max_order_rejects << ",\n"
              << "    \"max_cancel_rate\": " << k.max_cancel_rate << ",\n"
              << "    \"cancel_rate_window_sec\": " << k.cancel_rate_window_sec << ",\n"
              << "    \"cancel_rate_min_orders\": " << k.cancel_rate_min_orders << ",\n"
              << "    \"max_latency_ms\": " << k.max_latency_ms << ",\n"
              << "    \"latency_breach_samples\": " << k.latency_breach_samples << ",\n"
              << "    \"max_stale_data_ms\": " << k.max_stale_data_ms << ",\n"
              << "    \"max_ws_disconnects\": " << k.max_ws_disconnects << ",\n"
              << "    \"ws_window_sec\": " << k.ws_window_sec << ",\n"
              << "    \"max_balance_drop_pct\": " << k.max_balance_drop_pct << ",\n"
              << "    \"max_order_notional_usd\": " << k.max_order_notional_usd << ",\n"
              << "    \"flatten_on_trip\": " << (k.flatten_on_trip ? "true" : "false") << ",\n"
              << "    \"lock_file\": \"" << k.lock_file << "\"\n"
              << "  },\n";
        }

        // VPIN
        f << "  \"vpin_bucket_mult\":    " << cfg.vpin_bucket_mult    << ",\n";
        f << "  \"vpin_bucket_volume\":  " << cfg.vpin_bucket_volume  << ",\n";
        f << "  \"vpin_calib_trades\":   " << cfg.vpin_calib_trades   << ",\n";
        f << "  \"vpin_n_buckets\":      " << cfg.vpin_n_buckets      << ",\n";
        f << "  \"vpin_use_dynamic\":    " << (cfg.vpin_use_dynamic ? "true" : "false") << ",\n";
        f << "  \"vpin_dynamic_window\": " << cfg.vpin_dynamic_window << ",\n";
        f << "  \"vpin_dynamic_k\":      " << cfg.vpin_dynamic_k      << ",\n";
        f << "  \"vpin_vel_surge_thr\":  " << cfg.vpin_vel_surge_thr  << ",\n";

        // HMM
        f << "  \"hmm_enter_threshold\": " << cfg.hmm_enter_threshold << ",\n";
        f << "  \"hmm_persistence\":     " << cfg.hmm_persistence     << ",\n";
        f << "  \"obi_threshold\":       " << cfg.obi_threshold       << ",\n";

        // Adverse selection
        f << "  \"cancel_track_win_us\": " << cfg.cancel_track_win_us << ",\n";
        f << "  \"cancel_thr_bps\":      " << cfg.cancel_thr_bps      << ",\n";

        // Latency
        f << "  \"latency_alert_us\": " << cfg.latency_alert_us << ",\n";
        f << "  \"report_every_n\":   " << cfg.report_every_n   << ",\n";

        // Telemetry
        f << "  \"write_csv\":        " << (cfg.write_csv        ? "true" : "false") << ",\n";
        f << "  \"write_prometheus\": " << (cfg.write_prometheus ? "true" : "false") << ",\n";
        f << "  \"csv_path\":  \"" << cfg.csv_path  << "\",\n";
        f << "  \"prom_path\": \"" << cfg.prom_path << "\",\n";

        // ExchangeInfo
        f << "  \"exchangeinfo_ttl_s\": " << cfg.exchangeinfo_ttl_s << ",\n";

        // WebSocket
        f << "  \"enable_depth\":  " << (cfg.enable_depth  ? "true" : "false") << ",\n";
        f << "  \"enable_ticker\": " << (cfg.enable_ticker ? "true" : "false") << ",\n";
        f << "  \"enable_trade\":  " << (cfg.enable_trade  ? "true" : "false") << ",\n";

        // Risk Scorer
        f << "  \"risk_weight_vpin_zscore\":     " << cfg.risk_weights.vpin_zscore     << ",\n";
        f << "  \"risk_weight_hmm_prob\":        " << cfg.risk_weights.hmm_prob        << ",\n";
        f << "  \"risk_weight_ob_imbalance\":    " << cfg.risk_weights.ob_imbalance    << ",\n";
        f << "  \"risk_weight_spread_width\":    " << cfg.risk_weights.spread_width    << ",\n";
        f << "  \"risk_weight_depth_liquidity\": " << cfg.risk_weights.depth_liquidity << ",\n";
        f << "  \"risk_threshold_allow\":   " << cfg.risk_thresholds.allow   << ",\n";
        f << "  \"risk_threshold_caution\": " << cfg.risk_thresholds.caution << ",\n";
        f << "  \"risk_threshold_pause\":   " << cfg.risk_thresholds.pause << "\n";

        f << "}\n";
    }

private:
    // ── JSON → KusakConfig ───────────────────────────────────────────────────
    static void from_json(const nlohmann::json& j, KusakConfig& cfg) {

        auto str = [&](const char* key, std::string& out) {
            if (j.contains(key) && j[key].is_string()) out = j[key].get<std::string>();
        };
        auto b = [&](const char* key, bool& out) {
            if (!j.contains(key)) return;
            const auto& v = j[key];
            if (v.is_boolean()) out = v.get<bool>();
            else if (v.is_string()) out = detail::to_bool(v.get<std::string>());
        };
        auto d = [&](const char* key, double& out) {
            if (!j.contains(key)) return;
            const auto& v = j[key];
            if (v.is_number()) out = v.get<double>();
            else if (v.is_string()) out = detail::to_double(v.get<std::string>());
        };
        auto i = [&](const char* key, int& out) {
            if (!j.contains(key)) return;
            const auto& v = j[key];
            if (v.is_number_integer()) out = v.get<int>();
            else if (v.is_number())    out = static_cast<int>(v.get<double>());
            else if (v.is_string())    out = detail::to_int(v.get<std::string>());
        };
        auto u64 = [&](const char* key, uint64_t& out) {
            if (!j.contains(key)) return;
            const auto& v = j[key];
            if (v.is_number_unsigned()) out = v.get<uint64_t>();
            else if (v.is_number())     out = static_cast<uint64_t>(v.get<double>());
            else if (v.is_string())     out = detail::to_u64(v.get<std::string>());
        };

        // Identity
        str("symbol",        cfg.symbol);
        str("quote_asset",   cfg.quote_asset);
        str("balance_source",cfg.balance_source);

        // PATCH v12.1: market with validation + recvWindow_ms
        if (j.contains("market") && j["market"].is_string())
            cfg.market = detail::validated_market(j["market"].get<std::string>());

        // Connectivity
        str("api_key",    cfg.api_key);
        str("api_secret", cfg.api_secret);
        b  ("testnet",    cfg.testnet);
        u64("recvWindow_ms", cfg.recvWindow_ms);

        // Run mode
        b("dry_run",              cfg.dry_run);
        b("verbose",              cfg.verbose);
        b("log_raw_account_json", cfg.log_raw_account_json);

        // Capital
        d  ("initial_capital",  cfg.initial_capital);
        d  ("min_free_usdt",    cfg.min_free_usdt);

        // Sizing
        d("buy_pct",          cfg.buy_pct);
        d("kelly_fraction",   cfg.kelly_fraction);
        d("max_position_usd", cfg.max_position_usd);

        // Risk
        d("max_fill_rate_floor", cfg.max_fill_rate_floor);

        // [PHASE3-KS] kill_switch blogu. Negatif/gecersiz deger = KAPALI (0'a cekilir).
        if (j.contains("kill_switch") && j["kill_switch"].is_object()) {
            const auto& js = j["kill_switch"];
            auto kd = [&](const char* key, double& o) {
                if (js.contains(key) && js[key].is_number()) o = std::max(0.0, js[key].get<double>()); };
            auto ki = [&](const char* key, int& o) {
                if (js.contains(key) && js[key].is_number()) o = std::max(0, js[key].get<int>()); };
            auto& k = cfg.ks;
            if (js.contains("enabled") && js["enabled"].is_boolean()) k.enabled = js["enabled"].get<bool>();
            if (js.contains("flatten_on_trip") && js["flatten_on_trip"].is_boolean())
                k.flatten_on_trip = js["flatten_on_trip"].get<bool>();
            if (js.contains("lock_file") && js["lock_file"].is_string() && !js["lock_file"].get<std::string>().empty())
                k.lock_file = js["lock_file"].get<std::string>();
            kd("max_position_usd", k.max_position_usd);       kd("max_total_exposure_usd", k.max_total_exposure_usd);
            kd("max_daily_loss_usd", k.max_daily_loss_usd);   ki("max_consecutive_losses", k.max_consecutive_losses);
            ki("max_order_rejects", k.max_order_rejects);     kd("max_cancel_rate", k.max_cancel_rate);
            kd("cancel_rate_window_sec", k.cancel_rate_window_sec); ki("cancel_rate_min_orders", k.cancel_rate_min_orders);
            kd("max_latency_ms", k.max_latency_ms);           ki("latency_breach_samples", k.latency_breach_samples);
            kd("max_stale_data_ms", k.max_stale_data_ms);     ki("max_ws_disconnects", k.max_ws_disconnects);
            kd("ws_window_sec", k.ws_window_sec);             kd("max_balance_drop_pct", k.max_balance_drop_pct);
            kd("max_order_notional_usd", k.max_order_notional_usd);
            if (k.latency_breach_samples < 1) k.latency_breach_samples = 1;
            if (k.cancel_rate_window_sec < 1.0) k.cancel_rate_window_sec = 1.0;
            if (k.ws_window_sec < 1.0) k.ws_window_sec = 1.0;
        }

        // VPIN
        d("vpin_bucket_mult",    cfg.vpin_bucket_mult);
        d("vpin_bucket_volume",  cfg.vpin_bucket_volume);        // [PHASE5]
        i("vpin_calib_trades",   cfg.vpin_calib_trades);
        if (cfg.vpin_bucket_volume < 0.0) cfg.vpin_bucket_volume = 0.0;   // negatif => otomatik
        if (cfg.vpin_calib_trades  < 10)  cfg.vpin_calib_trades  = 10;    // anlamli ortalama icin alt sinir
        i("vpin_n_buckets",      cfg.vpin_n_buckets);
        b("vpin_use_dynamic",    cfg.vpin_use_dynamic);
        i("vpin_dynamic_window", cfg.vpin_dynamic_window);
        d("vpin_dynamic_k",      cfg.vpin_dynamic_k);
        d("vpin_vel_surge_thr",  cfg.vpin_vel_surge_thr);

        // HMM
        d("hmm_enter_threshold", cfg.hmm_enter_threshold);
        d("hmm_persistence",     cfg.hmm_persistence);
        d("obi_threshold",       cfg.obi_threshold);

        // Adverse selection
        u64("cancel_track_win_us", cfg.cancel_track_win_us);
        d  ("cancel_thr_bps",      cfg.cancel_thr_bps);

        // Latency
        u64("latency_alert_us", cfg.latency_alert_us);
        i  ("report_every_n",   cfg.report_every_n);

        // Telemetry
        b  ("write_csv",        cfg.write_csv);
        b  ("write_prometheus", cfg.write_prometheus);
        str("csv_path",         cfg.csv_path);
        str("prom_path",        cfg.prom_path);

        // ExchangeInfo
        u64("exchangeinfo_ttl_s", cfg.exchangeinfo_ttl_s);

        // WebSocket
        b("enable_depth",  cfg.enable_depth);
        b("enable_ticker", cfg.enable_ticker);
        b("enable_trade",  cfg.enable_trade);

        // Risk weights
        d("risk_weight_vpin_zscore",     cfg.risk_weights.vpin_zscore);
        d("risk_weight_hmm_prob",        cfg.risk_weights.hmm_prob);
        d("risk_weight_ob_imbalance",    cfg.risk_weights.ob_imbalance);
        d("risk_weight_spread_width",    cfg.risk_weights.spread_width);
        d("risk_weight_depth_liquidity", cfg.risk_weights.depth_liquidity);

        // Risk thresholds
        d("risk_allow_thr",   cfg.risk_thresholds.allow);     // [PHASE1-FW] JSON'daki eski adlar (yeni adlar onceliklidir)
        d("risk_caution_thr", cfg.risk_thresholds.caution);
        d("risk_pause_thr",   cfg.risk_thresholds.pause);
        d("risk_threshold_allow",   cfg.risk_thresholds.allow);
        d("risk_threshold_caution", cfg.risk_thresholds.caution);
        d("risk_threshold_pause",   cfg.risk_thresholds.pause);
    }

    uint64_t file_mtime() const noexcept {
        struct stat st{};
        if (::stat(path_.c_str(), &st) != 0) return 0;
        return static_cast<uint64_t>(st.st_mtime);
    }

    std::string path_;
    uint64_t    last_mtime_ = 0;
};

} // namespace kusak::config