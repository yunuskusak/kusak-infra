#pragma once
#ifndef KUSAK_INFRA_SPSC_RING_HPP
#define KUSAK_INFRA_SPSC_RING_HPP

// infra/spsc_ring.hpp — V13 Ghost Engine — SpscRing + LogRing
// ─────────────────────────────────────────────────────────────────────────────
// main.cpp satır 168–239'dan taşındı. g_log_ring main.cpp'de kaldı.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <cstdint>
#include <cstddef>
#include "engine/market_event.hpp"  // MarketEvent

// ─────────────────────────────────────────────────────────────────────────────
// [FIX-SPSC-2] SpscRing<N> — MarketEvent'e özel SPSC ring (V12'den değişmedi)
// [V13-HOOK]   dropped() takma adı eklendi (monitor_drops kancası için)
// ─────────────────────────────────────────────────────────────────────────────
template<size_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0, "N power of 2");
    alignas(64) std::atomic<uint32_t> head_{0};
    alignas(64) std::atomic<uint32_t> tail_{0};
    alignas(64) std::atomic<uint64_t> drop_count_{0};
    alignas(64) MarketEvent           buf_[N];
public:
    bool try_push(const MarketEvent& e) noexcept {
        const uint32_t h    = head_.load(std::memory_order_relaxed);
        const uint32_t next = (h + 1) & (N - 1);
        if (next == tail_.load(std::memory_order_acquire)) {
            drop_count_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        buf_[h] = e;
        std::atomic_thread_fence(std::memory_order_release);
        head_.store(next, std::memory_order_relaxed);
        return true;
    }
    uint32_t drain(MarketEvent* out, uint32_t max) noexcept {
        const uint32_t h = head_.load(std::memory_order_acquire);
        uint32_t t = tail_.load(std::memory_order_relaxed);
        uint32_t n = 0;
        while (t != h && n < max) {
            out[n++] = buf_[t];
            t = (t + 1) & (N - 1);
        }
        tail_.store(t, std::memory_order_release);
        return n;
    }
    // [V13-HOOK]: monitor_drops() hook'u için dropped() takma adı
    uint64_t dropped() const noexcept {
        return drop_count_.load(std::memory_order_relaxed);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Async Log Ring (V12'den değişmedi)
// ─────────────────────────────────────────────────────────────────────────────
struct LogMsg {
    char    buf[384];
    int     len;
    uint8_t level;
};

template<size_t N>
class LogRing {
    static_assert((N & (N - 1)) == 0);
    alignas(64) std::atomic<uint32_t> head_{0};
    alignas(64) std::atomic<uint32_t> tail_{0};
    alignas(64) LogMsg                buf_[N];
public:
    bool try_push(const LogMsg& m) noexcept {
        const uint32_t h    = head_.load(std::memory_order_relaxed);
        const uint32_t next = (h + 1) & (N - 1);
        if (next == tail_.load(std::memory_order_acquire)) return false;
        buf_[h] = m;
        head_.store(next, std::memory_order_release);
        return true;
    }
    bool try_pop(LogMsg& out) noexcept {
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        out = buf_[t];
        tail_.store((t + 1) & (N - 1), std::memory_order_release);
        return true;
    }
};

// NOT: g_log_ring main.cpp'de kalıyor:
//   static LogRing<8192> g_log_ring;

#endif // KUSAK_INFRA_SPSC_RING_HPP
