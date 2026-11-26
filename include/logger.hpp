// ============================================================================
// KUSAK V12 — RESEARCH FRAMEWORK
// logger.hpp — Zaman Damgalı Yapısal Log
//
// KAYNAK: Mimari Revizyon ve Canlıya Geçiş Planı — §6 Entegrasyon
//
//   Rapor: "Zaman damgalı log yazma"
//           CSV formatı: ID,tickRecv,decisionTime,sendTime,ackTime,fillTime
//
//   Seviyeler : DEBUG < INFO < WARN < ERROR < CRITICAL
//   Çıktı     : stdout + opsiyonel dosya
//   Thread safe: Mutex ile korunmuş (logger birden fazla thread'den çağrılabilir)
//   Format    : [2026-03-02 12:00:01.123456] [WARN] [module] mesaj
// ============================================================================

#pragma once

#include "research_types.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace kusak::log {

// ============================================================================
// §1  Log Seviyesi
// ============================================================================

enum class Level : uint8_t {
    DEBUG    = 0,
    INFO     = 1,
    WARN     = 2,
    ERR      = 3,
    CRITICAL = 4,
};

inline const char* level_name(Level l) noexcept {
    switch (l) {
        case Level::DEBUG:    return "DEBUG";
        case Level::INFO:     return "INFO ";
        case Level::WARN:     return "WARN ";
        case Level::ERR:      return "ERROR";
        case Level::CRITICAL: return "CRIT ";
        default:              return "?????";
    }
}

// ============================================================================
// §2  Logger — Rapor §6 uyumlu zaman damgalı log
// ============================================================================

class Logger {
public:
    // Config: Logger yapılandırması
    struct Config {
        Level       min_level    = Level::INFO;
        bool        to_stdout    = true;
        bool        to_file      = false;
        std::string file_path    = "kusak.log";
        bool        color_stdout = true;
    };

    explicit Logger() = default;
    explicit Logger(Config cfg) : cfg_(std::move(cfg)) {
        if (cfg_.to_file && !cfg_.file_path.empty()) {
            file_.open(cfg_.file_path, std::ios::app);
        }
    }

    ~Logger() {
        if (file_.is_open()) file_.close();
    }

    // ── Ana log metodları ─────────────────────────────────────────────

    template<typename... Args>
    void debug(std::string_view module, Args&&... args) {
        log(Level::DEBUG, module, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void info(std::string_view module, Args&&... args) {
        log(Level::INFO, module, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void warn(std::string_view module, Args&&... args) {
        log(Level::WARN, module, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void error(std::string_view module, Args&&... args) {
        log(Level::ERR, module, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void critical(std::string_view module, Args&&... args) {
        log(Level::CRITICAL, module, std::forward<Args>(args)...);
    }

    // ── OrderLifecycle CSV log (Rapor §3 formatı) ─────────────────────
    //   "ID,tickRecvNS,decisionNS,sendOrderNS,ackNS,fillNS,
    //    decisionLat_ns,sendLat_ns,roundTrip_ns"
    void log_order_lifecycle(const std::string& order_id,
                              const kusak::research::OrderLifecycle& lc) {
        std::lock_guard<std::mutex> g(mu_);
        if (!lifecycle_file_.is_open()) {
            lifecycle_file_.open("order_lifecycle.csv", std::ios::app);
            if (lifecycle_file_.is_open())
                lifecycle_file_ << "order_id,tickRecvNS,decisionNS,sendOrderNS,"
                                   "ackNS,fillNS,decision_lat_ns,send_lat_ns,"
                                   "round_trip_ns,tick_to_fill_ns\n";
        }
        if (!lifecycle_file_.is_open()) return;
        lifecycle_file_
            << order_id << ","
            << lc.tickRecvNS   << ","
            << lc.decisionNS   << ","
            << lc.sendOrderNS  << ","
            << lc.ackNS        << ","
            << lc.fillNS       << ","
            << static_cast<uint64_t>(lc.tick_to_decision_us() * 1000.0) << ","
            << static_cast<uint64_t>(lc.decision_to_send_us() * 1000.0) << ","
            << static_cast<uint64_t>(lc.send_to_ack_us()     * 1000.0) << ","
            << static_cast<uint64_t>(lc.tick_to_trade_us()   * 1000.0) << "\n";
    }

    // ── Adverse selection log satırı (Rapor §4 formatı) ──────────────
    //   "[2026-03-02 12:00:01.123456] WARN: Order ABC cancelled at X, mid=Y; adverseMove=Z%"
    void log_adverse_move(const std::string& order_id,
                           double cancel_price,
                           double mid_now,
                           double adverse_bps) {
        std::ostringstream ss;
        ss << "Order " << order_id
           << " cancelled at " << std::fixed << std::setprecision(6) << cancel_price
           << ", mid=" << mid_now
           << "; adverseMove=" << std::setprecision(4) << adverse_bps / 100.0 << "%";
        warn("ADVERSE", ss.str());
    }

    void set_min_level(Level l) noexcept { cfg_.min_level = l; }

    uint64_t total_errors()    const noexcept { return errors_.load(std::memory_order_relaxed); }
    uint64_t total_warnings()  const noexcept { return warnings_.load(std::memory_order_relaxed); }
    uint64_t total_messages()  const noexcept { return messages_.load(std::memory_order_relaxed); }

private:
    template<typename... Args>
    void log(Level lvl, std::string_view module, Args&&... args) {
        if (lvl < cfg_.min_level) return;

        messages_.fetch_add(1, std::memory_order_relaxed);
        if (lvl >= Level::ERR)  errors_.fetch_add(1, std::memory_order_relaxed);
        if (lvl == Level::WARN) warnings_.fetch_add(1, std::memory_order_relaxed);

        std::ostringstream body;
        (body << ... << std::forward<Args>(args));

        std::string line = format_line(lvl, module, body.str());

        std::lock_guard<std::mutex> g(mu_);
        if (cfg_.to_stdout) {
            if (cfg_.color_stdout) {
                std::cout << color_for(lvl) << line << "\033[0m\n";
            } else {
                std::cout << line << "\n";
            }
        }
        if (cfg_.to_file && file_.is_open()) {
            file_ << line << "\n";
        }
    }

    static std::string format_line(Level lvl, std::string_view module,
                                    const std::string& msg) {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        auto us  = std::chrono::duration_cast<std::chrono::microseconds>(
                       now.time_since_epoch()).count() % 1'000'000;

        char timebuf[32];
        std::tm tm_info{};
#ifdef _WIN32
        localtime_s(&tm_info, &t);
#else
        localtime_r(&t, &tm_info);
#endif
        std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_info);

        std::ostringstream ss;
        ss << "[" << timebuf << "."
           << std::setw(6) << std::setfill('0') << us << "] "
           << "[" << level_name(lvl) << "] "
           << "[" << module << "] "
           << msg;
        return ss.str();
    }

    static const char* color_for(Level l) noexcept {
        switch (l) {
            case Level::DEBUG:    return "\033[37m";    // gri
            case Level::INFO:     return "\033[32m";    // yeşil
            case Level::WARN:     return "\033[33m";    // sarı
            case Level::ERR:      return "\033[31m";    // kırmızı
            case Level::CRITICAL: return "\033[1;31m";  // koyu kırmızı
            default:              return "";
        }
    }

    Config      cfg_;
    std::mutex  mu_;
    std::ofstream file_;
    std::ofstream lifecycle_file_;

    std::atomic<uint64_t> messages_ {0};
    std::atomic<uint64_t> errors_   {0};
    std::atomic<uint64_t> warnings_ {0};
};

// ============================================================================
// §3  Global Logger Singleton (opsiyonel kolaylık)
// ============================================================================

inline Logger& global_logger() {
    static Logger inst;
    return inst;
}

// Kısa makrolar (global logger kullanır)
#define KLOG_DEBUG(module, ...)    ::kusak::log::global_logger().debug(module, __VA_ARGS__)
#define KLOG_INFO(module, ...)     ::kusak::log::global_logger().info(module, __VA_ARGS__)
#define KLOG_WARN(module, ...)     ::kusak::log::global_logger().warn(module, __VA_ARGS__)
#define KLOG_ERROR(module, ...)    ::kusak::log::global_logger().error(module, __VA_ARGS__)
#define KLOG_CRIT(module, ...)     ::kusak::log::global_logger().critical(module, __VA_ARGS__)

} // namespace kusak::log
