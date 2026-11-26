#pragma once
#ifndef KUSAK_INFRA_TIME_HPP
#define KUSAK_INFRA_TIME_HPP

// infra/time.hpp — V13 Ghost Engine — TSC Kalibrasyon + Zaman Yardımcıları
// ─────────────────────────────────────────────────────────────────────────────
// main.cpp satır 72–146'dan taşındı. Orijinal kod değiştirilmeden kopyalandı.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <immintrin.h>   // _mm_pause, _mm_lfence
#include <x86intrin.h>   // __rdtsc, __rdtscp

#ifdef _WIN32
#  include <Windows.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Compiler hints
// ─────────────────────────────────────────────────────────────────────────────
#ifndef FORCE_INLINE
#  define FORCE_INLINE __attribute__((always_inline)) inline
#endif
#ifndef LIKELY
#  define LIKELY(x)   __builtin_expect(!!(x), 1)
#  define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

// ─────────────────────────────────────────────────────────────────────────────
// [V13-CLOCK → TIER-1 TSC] Kalibrasyon + tsc_ns()
// mono_ns() SİLİNDİ — hot-path'te vDSO/syscall yok
// ─────────────────────────────────────────────────────────────────────────────
extern uint64_t g_tsc_ns_mult;  // tanım main.cpp'de: uint64_t g_tsc_ns_mult = 0;

struct TscCalibration {
    uint64_t tsc_per_ns;
    uint64_t ns_mult;
};

[[nodiscard]] inline TscCalibration calibrate_tsc() noexcept {
    constexpr uint64_t CALIBRATION_NS = 100'000'000ULL; // 100 ms
    unsigned aux = 0;
    uint64_t start_tsc = __rdtscp(&aux);
    _mm_lfence();
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    uint64_t start_ns = uint64_t(ts.tv_sec) * 1'000'000'000ULL + uint64_t(ts.tv_nsec);

    struct timespec rem = {0, 100'000'000}; // 100 ms
#ifdef _WIN32
    Sleep(100);
#else
    nanosleep(&rem, nullptr);
#endif

    uint64_t end_tsc = __rdtscp(&aux);
    _mm_lfence();
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    uint64_t end_ns = uint64_t(ts.tv_sec) * 1'000'000'000ULL + uint64_t(ts.tv_nsec);

    uint64_t delta_tsc = end_tsc - start_tsc;
    uint64_t delta_ns  = end_ns  - start_ns;

    uint64_t ns_mult = (delta_ns << 32) / delta_tsc;
    return { delta_tsc / delta_ns, ns_mult };
}

// [FIX-P0-01] 64-bit carpim tasmasi giderildi: 128-bit ara carpim.
FORCE_INLINE uint64_t tsc_to_ns(uint64_t tsc, uint64_t mult) noexcept {
    return static_cast<uint64_t>((static_cast<unsigned __int128>(tsc) * mult) >> 32);
}

[[nodiscard]] FORCE_INLINE uint64_t tsc_ns() noexcept {
    unsigned aux = 0;
    uint64_t tsc = __rdtscp(&aux);
    _mm_lfence();
    return tsc_to_ns(tsc, g_tsc_ns_mult);
}

// ─────────────────────────────────────────────────────────────────────────────
// [V13-BACKOFF] TSC tabanlı yoğun bekleme — ~500ns spin için syscallsız
// ─────────────────────────────────────────────────────────────────────────────
FORCE_INLINE uint64_t rdtsc_ordered() noexcept {
    unsigned aux = 0;
    const uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}

FORCE_INLINE void tsc_pause_cycles(uint32_t cycles) noexcept {
    const uint64_t end = rdtsc_ordered() + cycles;
    while (rdtsc_ordered() < end) _mm_pause();
}

// ─────────────────────────────────────────────────────────────────────────────
// panic_critical — g_stop main.cpp'de tanımlı, extern ile erişilir
// ─────────────────────────────────────────────────────────────────────────────
extern std::atomic<bool> g_stop;

FORCE_INLINE void panic_critical(const char* msg) noexcept {
    std::fputs(msg, stderr);
    std::fputc('\n', stderr);
    g_stop.store(true, std::memory_order_release);
}

#endif // KUSAK_INFRA_TIME_HPP
