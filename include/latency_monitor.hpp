#pragma once
#ifndef KUSAK_LATENCY_MONITOR_HPP
#define KUSAK_LATENCY_MONITOR_HPP

// Çözülen:
// 1. FORCE_INLINE artık core_types.hpp'tan geliyor — sıralama bağımlılığı sıfır
// 2. market_data_recorder.hpp dahil herhangi bir header bu dosyadan ÖNCE include edilse dahi
//    FORCE_INLINE / now_ns / now_us tanımlı gelir (core_types'ın #pragma once garantisi)

#include "core_types.hpp"

#include <atomic>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <numeric>
#include <cmath>

namespace kusak::research {

// ═════════════════════════════════════════════════════════════════════════════
// TickToTradeTimer — measures tick-received → order-sent latency
// ═════════════════════════════════════════════════════════════════════════════
class TickToTradeTimer {
public:
    void mark_tick_received() noexcept { tick_ns_ = mono_ns(); }
    void mark_order_sent()    noexcept { send_ns_ = mono_ns(); }
    double tick_to_trade_us() const noexcept {
        return (send_ns_ > tick_ns_)
            ? static_cast<double>(send_ns_ - tick_ns_) / 1000.0
            : 0.0;
    }
private:
    uint64_t tick_ns_ = 0;
    uint64_t send_ns_ = 0;
};

// ═════════════════════════════════════════════════════════════════════════════
// Percentile hesabı (sort-based, small N için yeterli)
// ═════════════════════════════════════════════════════════════════════════════
template<typename T, std::size_t N>
FORCE_INLINE T percentile_sorted(std::array<T, N> arr, std::size_t count, double pct) noexcept {
    if (count == 0) return T{};
    const std::size_t n = std::min(count, N);
    std::sort(arr.begin(), arr.begin() + n);
    const std::size_t idx = static_cast<std::size_t>(pct * 0.01 * (n - 1));
    return arr[idx];
}

// ═════════════════════════════════════════════════════════════════════════════
// LatencyBucket — tek bir gecikme ölçüm kategorisi
// ═════════════════════════════════════════════════════════════════════════════
struct LatencyBucket {
    std::atomic<uint64_t> count   {0};
    std::atomic<uint64_t> sum_ns  {0};
    std::atomic<uint64_t> min_ns  {UINT64_MAX};
    std::atomic<uint64_t> max_ns  {0};

    LatencyBucket() = default;
    LatencyBucket(const LatencyBucket&)            = delete;
    LatencyBucket& operator=(const LatencyBucket&) = delete;

    FORCE_INLINE void record(uint64_t ns) noexcept {
        count.fetch_add(1, std::memory_order_relaxed);
        sum_ns.fetch_add(ns, std::memory_order_relaxed);
        // min CAS
        uint64_t cur_min = min_ns.load(std::memory_order_relaxed);
        while (ns < cur_min &&
               !min_ns.compare_exchange_weak(cur_min, ns,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed));
        // max CAS
        uint64_t cur_max = max_ns.load(std::memory_order_relaxed);
        while (ns > cur_max &&
               !max_ns.compare_exchange_weak(cur_max, ns,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed));
    }

    FORCE_INLINE double avg_us() const noexcept {
        const uint64_t c = count.load(std::memory_order_relaxed);
        if (c == 0) return 0.0;
        return static_cast<double>(sum_ns.load(std::memory_order_relaxed))
               / static_cast<double>(c) / 1'000.0;
    }

    FORCE_INLINE double min_us() const noexcept {
        const uint64_t v = min_ns.load(std::memory_order_relaxed);
        return (v == UINT64_MAX) ? 0.0 : static_cast<double>(v) / 1'000.0;
    }

    FORCE_INLINE double max_us() const noexcept {
        return static_cast<double>(max_ns.load(std::memory_order_relaxed)) / 1'000.0;
    }

    void reset() noexcept {
        count  .store(0,          std::memory_order_relaxed);
        sum_ns .store(0,          std::memory_order_relaxed);
        min_ns .store(UINT64_MAX, std::memory_order_relaxed);
        max_ns .store(0,          std::memory_order_relaxed);
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// ScopedTimer — RAII ölçüm yardımcısı
// ═════════════════════════════════════════════════════════════════════════════
class ScopedTimer {
public:
    FORCE_INLINE explicit ScopedTimer(LatencyBucket& bucket) noexcept
        : bucket_(bucket), start_(mono_ns()) {}

    FORCE_INLINE ~ScopedTimer() noexcept {
        bucket_.record(mono_ns() - start_);
    }

    ScopedTimer(const ScopedTimer&)            = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    LatencyBucket& bucket_;
    uint64_t       start_;
};

// ═════════════════════════════════════════════════════════════════════════════
// HdrHistogram — lock-free, power-of-2 bucket histogram
// ═════════════════════════════════════════════════════════════════════════════
class HdrHistogram {
public:
    static constexpr int    N_BUCKETS   = 64;
    static constexpr double BUCKET_BASE = 1.5;

    FORCE_INLINE void record(uint64_t ns) noexcept {
        const int bucket = bucket_for(ns);
        buckets_[bucket].fetch_add(1, std::memory_order_relaxed);
        total_.fetch_add(1, std::memory_order_relaxed);
        sum_.fetch_add(ns, std::memory_order_relaxed);
    }

    // p50/p95/p99/p999 (µs)
    double percentile_us(double pct) const noexcept {
        const uint64_t total = total_.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        const uint64_t target = static_cast<uint64_t>(pct * 0.01 * total);
        uint64_t cumsum = 0;
        for (int i = 0; i < N_BUCKETS; ++i) {
            cumsum += buckets_[i].load(std::memory_order_relaxed);
            if (cumsum >= target) {
                return bucket_upper_ns(i) / 1'000.0;
            }
        }
        return bucket_upper_ns(N_BUCKETS - 1) / 1'000.0;
    }

    double avg_us() const noexcept {
        const uint64_t c = total_.load(std::memory_order_relaxed);
        if (c == 0) return 0.0;
        return static_cast<double>(sum_.load(std::memory_order_relaxed))
               / static_cast<double>(c) / 1'000.0;
    }

    void reset() noexcept {
        for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
        total_.store(0, std::memory_order_relaxed);
        sum_  .store(0, std::memory_order_relaxed);
    }

private:
    FORCE_INLINE static int bucket_for(uint64_t ns) noexcept {
        if (ns == 0) return 0;
        int b = 0;
        uint64_t upper = 1'000ULL; // 1 µs başlangıç
        while (b < N_BUCKETS - 1 && ns > upper) {
            upper = static_cast<uint64_t>(static_cast<double>(upper) * BUCKET_BASE);
            ++b;
        }
        return b;
    }

    FORCE_INLINE static uint64_t bucket_upper_ns(int b) noexcept {
        uint64_t v = 1'000ULL;
        for (int i = 0; i < b; ++i)
            v = static_cast<uint64_t>(static_cast<double>(v) * BUCKET_BASE);
        return v;
    }

    std::array<std::atomic<uint64_t>, N_BUCKETS> buckets_ {};
    std::atomic<uint64_t> total_ {0};
    std::atomic<uint64_t> sum_   {0};
};

// ═════════════════════════════════════════════════════════════════════════════
// LatencyMonitor — tüm bileşenlerin merkezi gecikme takipçisi
// ═════════════════════════════════════════════════════════════════════════════
class LatencyMonitor {
public:
    // ── Config struct for constructor ────────────────────────────────────────
    struct Config {
        uint64_t alert_threshold_us = 500;
        int      report_every_n     = 1000;
        bool     log_to_stdout      = false;
    };

    enum class Tag : int {
        MARKET_DATA_RECV  = 0,
        ORDER_BOOK_UPDATE = 1,
        SIGNAL_COMPUTE    = 2,
        ORDER_SUBMIT      = 3,
        TICK_WRITE        = 4,
        REPLAY_DISPATCH   = 5,
        N_TAGS
    };
    static constexpr int N = static_cast<int>(Tag::N_TAGS);

    LatencyMonitor() noexcept : cfg_() {}
    explicit LatencyMonitor(const Config& cfg) noexcept
        : cfg_(cfg) {}

    FORCE_INLINE void record(Tag tag, uint64_t ns) noexcept {
        buckets_[static_cast<int>(tag)].record(ns);
        histograms_[static_cast<int>(tag)].record(ns);
    }

    [[nodiscard]] FORCE_INLINE
    uint64_t start_measurement() const noexcept { return mono_ns(); }

    FORCE_INLINE void stop_measurement(Tag tag, uint64_t start_ns) noexcept {
        record(tag, mono_ns() - start_ns);
    }

    const LatencyBucket& bucket(Tag tag) const noexcept {
        return buckets_[static_cast<int>(tag)];
    }
    const HdrHistogram& histogram(Tag tag) const noexcept {
        return histograms_[static_cast<int>(tag)];
    }

    void reset_all() noexcept {
        for (auto& b : buckets_)    b.reset();
        for (auto& h : histograms_) h.reset();
        ttt_hist_.reset();
    }

    // ── TickToTrade recording & percentiles ──────────────────────────────────
    void record_tick_to_trade(const TickToTradeTimer& ttt) noexcept {
        double us = ttt.tick_to_trade_us();
        if (us > 0.0) {
            uint64_t ns = static_cast<uint64_t>(us * 1000.0);
            ttt_hist_.record(ns);
        }
    }

    double tick_to_trade_p50() const noexcept {
        return ttt_hist_.percentile_us(50.0);
    }

    double tick_to_trade_p99() const noexcept {
        return ttt_hist_.percentile_us(99.0);
    }

    // ── Reporting ────────────────────────────────────────────────────────────
    void print_to_stdout() const noexcept {
        std::printf("[LatencyMonitor] TTT p50=%.1f us  p99=%.1f us\n",
                    tick_to_trade_p50(), tick_to_trade_p99());
        for (int i = 0; i < N; ++i) {
            auto& b = buckets_[i];
            if (b.count.load(std::memory_order_relaxed) == 0) continue;
            std::printf("  tag[%d]: avg=%.1f us  min=%.1f us  max=%.1f us  n=%lu\n",
                        i, b.avg_us(), b.min_us(), b.max_us(),
                        static_cast<unsigned long>(b.count.load(std::memory_order_relaxed)));
        }
    }

    void dump_report() const noexcept {
        print_to_stdout();
    }

private:
    Config        cfg_;
    LatencyBucket buckets_   [N];
    HdrHistogram  histograms_[N];
    HdrHistogram  ttt_hist_;
};

} // namespace kusak::research

// ═════════════════════════════════════════════════════════════════════════════
// Fix 4: namespace alias so kusak::monitoring:: resolves to kusak::research::
// ═════════════════════════════════════════════════════════════════════════════
namespace kusak { namespace monitoring = kusak::research; }

#endif // KUSAK_LATENCY_MONITOR_HPP
