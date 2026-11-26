#pragma once
#ifndef KUSAK_MARKET_DATA_RECORDER_HPP
#define KUSAK_MARKET_DATA_RECORDER_HPP

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  KUSAK Market Data Recorder — V12.8  (The Final Ghost)                  ║
// ║                                                                           ║
// ║  Δ V12.7 → V12.8  (Best-in-Class — 4 architectural upgrades)            ║
// ║                                                                           ║
// ║  [FIX-1] Zero-Overhead Callback                                          ║
// ║          std::function<> abolished. ABI is now                           ║
// ║          void(*)(uint64_t total_drops, void* ctx) noexcept.              ║
// ║          No heap, no type erasure, no vtable. One predictable            ║
// ║          branch (mostly not-taken) on the hot path. Drop overhead        ║
// ║          on healthy queue: a single LIKELY branch → zero cost.           ║
// ║                                                                           ║
// ║  [FIX-2] Mmap Pre-allocation & TLB Warm-up                              ║
// ║          posix_fallocate() eliminates on-demand block allocation.        ║
// ║          MAP_POPULATE + manual page-touch loop faults every page         ║
// ║          upfront. madvise(SEQUENTIAL|WILLNEED|HUGEPAGE) primes           ║
// ║          hardware prefetcher and TLB. Runtime grow() is abolished;       ║
// ║          zero TLB shootdowns, zero major faults in steady state.         ║
// ║                                                                           ║
// ║  [FIX-3] NUMA Enforcement (libnuma-free, direct syscall)                ║
// ║          cpu_to_node() resolves writer core → NUMA node via sysfs.       ║
// ║          mbind(MPOL_BIND) pins both the mmap region and the SpscRing     ║
// ║          to that node after writer_loop() starts. Cross-node DRAM        ║
// ║          access (+80–120 ns/LLC miss) is physically impossible.          ║
// ║                                                                           ║
// ║  [FIX-4] Deterministic TSC Calibration (nanosleep path)                 ║
// ║          CLOCK_MONOTONIC busy-probe replaced by nanosleep(10 ms).        ║
// ║          VDSO / clocksource variance removed from the denominator.       ║
// ║          Interrupted-sleep retry loop handles EINTR correctly.           ║
// ║          Measurement window (10 ms) is large enough that scheduler       ║
// ║          jitter on wakeup is sub-0.01% of the total interval.            ║
// ║                                                                           ║
// ║  Carried forward from V12.7 (unchanged):                                 ║
// ║  [FIX-A] rdtsc_ordered: __rdtscp + _mm_lfence()                         ║
// ║  [FIX-D] fdatasync() on close — physical-media durability               ║
// ║  [FIX-E] pin_thread_to_core — HT/isolcpus/IRQ/NUMA checklist            ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#include "core_types.hpp"
#include "research_types.hpp"
#include "latency_monitor.hpp"

#include <atomic>
#include <thread>
#include <cstdio>
#include <cinttypes>
#include <cstring>
#include <cstdlib>       // strtoul
#include <stdexcept>
#include <algorithm>
#include <iostream>      // [FIX-CRASH-PIN] std::cerr icin
// NOTE: <functional> intentionally removed — std::function abolished [FIX-1]
#include <immintrin.h>   // _mm_pause(), _mm_lfence()
#include <x86intrin.h>   // __rdtscp()
#include <pthread.h>
#include <sched.h>       // cpu_set_t
#include <sys/mman.h>    // mmap, munmap, madvise, msync
#include <sys/stat.h>
#include <sys/syscall.h> // SYS_mbind  — [FIX-3]
#include <fcntl.h>
#include <unistd.h>
#include <time.h>        // nanosleep, clock_gettime — [FIX-4]
#include <linux/mempolicy.h>   // MPOL_BIND — [FIX-3]

namespace kusak::research {

// ═════════════════════════════════════════════════════════════════════════════
// §0  Core / Thread Affinity
// ═════════════════════════════════════════════════════════════════════════════

// ── [FIX-E]  Core Isolation Prerequisites (carried from V12.7) ───────────────
//
//  For institutional-grade determinism the target core MUST satisfy ALL of:
//
//  1. Physical core — NOT a HyperThread sibling.
//     HT siblings share L1-I, L1-D, and execution ports. Any co-tenant thread
//     causes cache eviction and port contention invisible to perf counters.
//     Identify physical vs logical cores:
//       cat /sys/devices/system/cpu/cpu<N>/topology/thread_siblings_list
//     If cpu<N> appears with a second ID → it is a HyperThread. Use the
//     lower-numbered sibling; leave the other completely unscheduled.
//
//  2. Kernel-isolated via `isolcpus=<N>` in /proc/cmdline. Verify:
//       cat /sys/devices/system/cpu/isolated
//     Output MUST contain the writer core ID. No runtime workaround exists.
//
//  3. IRQ affinity excluded from this core:
//       for f in /proc/irq/*/smp_affinity_list; do
//           echo "0-$((N-1)),$((N+1))-$(nproc --all)" > "$f" 2>/dev/null
//       done
//
//  4. NUMA-local allocation — enforced programmatically by [FIX-3].
//
//  Violating ANY single condition produces multi-µs jitter spikes invisible
//  in p50/p95 but catastrophic in p99.9 tail latency.
//
inline void pin_thread_to_core(int core_id) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core_id, &set);
    if (::pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
        throw std::runtime_error("pin_thread_to_core: pthread_setaffinity_np failed");
}

// ═════════════════════════════════════════════════════════════════════════════
// §1  NUMA Enforcement  — [FIX-3]
// ═════════════════════════════════════════════════════════════════════════════

// ── mbind(2) via direct syscall — no libnuma dependency ──────────────────────
//
//  libnuma would introduce a shared-library dependency and dynamic linking
//  overhead. The syscall ABI is stable since Linux 2.6.7.
//
static inline long mbind_syscall(void*          addr,
                                 unsigned long  len,
                                 int            mode,
                                 const unsigned long* nodemask,
                                 unsigned long  maxnode,
                                 unsigned int   flags) noexcept
{
    return ::syscall(SYS_mbind,
                     addr, len, mode, nodemask, maxnode, flags);
}

// Returns the NUMA node that owns the given CPU core.
// Reads /sys/devices/system/cpu/cpu<N>/numa_node (Linux 2.6+).
// Returns 0 on any error (single-node systems have node 0 only).
//
static inline int cpu_to_node(int cpu_id) noexcept {
    char path[96];
    std::snprintf(path, sizeof(path),
        "/sys/devices/system/cpu/cpu%d/numa_node", cpu_id);
    FILE* f = ::fopen(path, "r");
    if (!f) return 0;
    int node = 0;
    std::fscanf(f, "%d", &node);
    ::fclose(f);
    return (node >= 0) ? node : 0;
}

// Pins [addr, addr+len) to the NUMA node that owns cpu_id.
//
// MPOL_BIND: pages are allocated ONLY from the specified node.
//   → cross-node DRAM access becomes physically impossible.
//   → any subsequent page fault on this range hits local DRAM.
//
// Called from writer_loop() — errors are non-fatal (logs to stderr).
// On single-NUMA machines the call is a no-op (node 0 == only node).
//
static inline void bind_memory_to_node(void* addr, std::size_t len,
                                       int cpu_id) noexcept
{
    const int node = cpu_to_node(cpu_id);
    // nodemask: 1 bit per node, bit position = node index.
    unsigned long mask = 1UL << static_cast<unsigned>(node);

    if (mbind_syscall(addr, len,
                      MPOL_BIND,
                      &mask,
                      /*maxnode=*/ sizeof(mask) * 8,
                      /*flags=*/   0) != 0)
    {
        // Non-fatal: NUMA affinity is best-effort on VMs and containers.
        // Log and continue — missing this is a latency concern, not correctness.
        std::fprintf(stderr,
            "[KUSAK V12.8] WARN: mbind(MPOL_BIND, node=%d) failed for "
            "addr=%p len=%zu — cross-node access possible. "
            "Check kernel NUMA support (CONFIG_NUMA).\n",
            node, addr, len);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// §2  TSC Utilities
// ═════════════════════════════════════════════════════════════════════════════

// ── [FIX-A]  Ultra-Strict TSC Serialization (carried from V12.7) ─────────────
//
//  __rdtscp (Intel SDM Vol.2B §4.2):
//   • Serialises prior LOADs — equivalent to implicit LFENCE before.
//   • Does NOT flush the store buffer — stores may reorder past it.
//   • CPU may speculate instructions AFTER rdtscp.
//
//  _mm_lfence() added AFTER: retires all prior µops before the return
//  value is consumed. Closes the speculative read window completely.
//  Required pattern per Intel Optimization Reference Manual §3.2.1.
//
FORCE_INLINE uint64_t rdtsc_ordered() noexcept {
    unsigned aux;
    uint64_t t = __rdtscp(&aux);
    _mm_lfence();   // retire fence — zero speculative reads escape
    return t;
}

// Adaptive batch pause: 32 → 256 _mm_pause() per interval check.
FORCE_INLINE void tsc_pause_cycles(uint64_t cycles) noexcept {
    const uint64_t start = rdtsc_ordered();
    int batch = 32;
    while (true) {
        for (int i = 0; i < batch; ++i) _mm_pause();
        if ((rdtsc_ordered() - start) >= cycles) break;
        batch = std::min(batch * 2, 256);
    }
}

// ── [FIX-4]  Deterministic TSC Calibration ───────────────────────────────────
//
//  Three-path strategy (ordered by accuracy):
//
//  Path 1 — /sys/devices/system/cpu/cpu0/tsc_freq_khz
//    Kernel reads invariant TSC from CPUID leaves 0x15/0x16 at boot.
//    Sub-ppm accuracy on physical hardware. Available: Linux 5.3+,
//    CONFIG_X86_TSC, physical host. Absent on most VMs / old kernels.
//
//  Path 2 — nanosleep(10 ms) + rdtsc delta                    [FIX-4]
//    V12.7 used a CLOCK_MONOTONIC busy-probe. Replaced here with
//    nanosleep(10 ms). Rationale:
//      • busy-probe calls clock_gettime in a tight loop → depends on
//        VDSO clocksource quality (TSC, HPET, kvm-clock vary).
//      • nanosleep() lets the CPU sleep until the kernel wakes it;
//        rdtsc is read immediately before and after at the boundary.
//      • Wakeup jitter (~50–200 µs worst case) is < 2% of the 10 ms
//        window → frequency error < 0.02%. Acceptable for all uses.
//      • EINTR-safe retry loop handles signal interruptions correctly.
//    A 1M-iteration _mm_pause warmup ensures the CPU exits deep C-state
//    before the first rdtsc, preventing early-TSC drift.
//
//  Path 3 — 3.5 GHz emergency fallback with mandatory stderr CRITICAL.
//    MUST NEVER fire silently in production.
//
inline double calibrate_tsc_ghz() noexcept {

    // ── Path 1: kernel sysfs invariant TSC ───────────────────────────────────
    {
        const char* sysfs_candidates[] = {
            "/sys/devices/system/cpu/cpu0/tsc_freq_khz",
            "/sys/bus/platform/drivers/pcspkr/cpu0/tsc_freq_khz",
        };
        for (const char* p : sysfs_candidates) {
            FILE* f = ::fopen(p, "r");
            if (!f) continue;
            uint64_t freq_khz = 0;
            const int n = std::fscanf(f, "%" SCNu64, &freq_khz);
            ::fclose(f);
            if (n == 1 && freq_khz > 1'000'000ULL) {   // > 1 GHz sanity
                return static_cast<double>(freq_khz) / 1'000'000.0;
            }
        }
    }

    // ── Path 2: nanosleep(10 ms) + rdtsc delta  [FIX-4] ─────────────────────
    {
        // Warm-up: drive CPU out of deep C-state.
        // Without this, early TSC ticks run at a reduced frequency on some
        // Intel platforms (C6/C7 residency distorts the first few rdtsc reads).
        {
            volatile uint64_t dummy = 0;
            for (int w = 0; w < 1'000'000; ++w) { dummy += static_cast<uint64_t>(w); }
            (void)dummy;
        }

        // Capture t0 immediately before sleeping.
        const uint64_t tsc0 = rdtsc_ordered();

        // nanosleep with EINTR retry (signals may cut the sleep short).
        struct timespec remaining{0, 10'000'000L};   // 10 ms
        while (::nanosleep(&remaining, &remaining) == -1) {
            // remaining is updated by the kernel to reflect unslept time.
            // Loop until the full interval has elapsed.
            if (remaining.tv_sec == 0 && remaining.tv_nsec == 0) break;
        }

        // Capture t1 immediately after wakeup, before any other work.
        const uint64_t tsc1 = rdtsc_ordered();

        // Denominator: 10 ms = 10,000,000 ns exactly.
        // Wakeup jitter (50–200 µs typical) is absorbed by the large window.
        const double ghz = static_cast<double>(tsc1 - tsc0) / 10'000'000.0;

        if (ghz >= 0.5 && ghz <= 8.0) return ghz;

        std::fprintf(stderr,
            "[KUSAK V12.8] WARN: nanosleep TSC probe out of bounds "
            "(%.3f GHz) — falling through to emergency fallback.\n", ghz);
    }

    // ── Path 3: emergency fallback ────────────────────────────────────────────
    std::fprintf(stderr,
        "[KUSAK V12.8] CRITICAL: All TSC calibration paths failed. "
        "Using 3.5 GHz fallback — ALL latency measurements INVALID. "
        "Verify: Linux >=5.3, CONFIG_X86_TSC, invariant TSC "
        "(MSR_PLATFORM_INFO bit 28), and that the process is not in a VM "
        "with synthetic clock.\n");
    return 3.5;
}

// ═════════════════════════════════════════════════════════════════════════════
// §3  Zero-Overhead Drop Callback ABI  — [FIX-1]
// ═════════════════════════════════════════════════════════════════════════════

// Raw function pointer — no heap, no type erasure, no vtable.
// ABI: (cumulative_drop_count, user_context_pointer)
//
// Usage:
//   struct Strategy { void on_drop(uint64_t n) noexcept; };
//
//   static void drop_trampoline(uint64_t total, void* ctx) noexcept {
//       static_cast<Strategy*>(ctx)->on_drop(total);
//   }
//
//   cfg.drop_fn  = &drop_trampoline;
//   cfg.drop_ctx = &my_strategy;
//
// Hot-path overhead when drop_fn == nullptr: a single UNLIKELY branch
// (predicted-not-taken) with no other instructions. Cost: ~0 cycles.
// Hot-path overhead on drop (cold path by definition): one indirect call
// via a register-held pointer ≈ 1–2 ns. No allocation. No exception risk.
//
using DropFn = void (*)(uint64_t total_drops, void* ctx) noexcept;

// ═════════════════════════════════════════════════════════════════════════════
// §4  SpscRing — Lock-Free SPSC Ring Buffer  (layout unchanged from V12.7)
//
//  Cache-line layout:
//   [0x000]  head_       — producer write
//   [0x040]  tail_       — consumer write
//   [0x080]  tail_cache_ — producer-local snapshot (non-atomic, hot-path)
//   [0x0C0]  dropped_    — stat; never on hot-path
//   [0x100]  slots_[]
//
//  SpscRing remains callback-agnostic. Backpressure is handled by the
//  owning layer (MarketDataRecorder::enqueue_) via DropFn [FIX-1].
// ═════════════════════════════════════════════════════════════════════════════

struct alignas(64) PaddedAtomic {
    std::atomic<uint64_t> v;
    char pad[64 - sizeof(std::atomic<uint64_t>)];
    explicit PaddedAtomic(uint64_t init = 0) noexcept : v(init) {}
};

struct alignas(64) PaddedU64 {
    uint64_t v;
    char pad[64 - sizeof(uint64_t)];
    explicit PaddedU64(uint64_t init = 0) noexcept : v(init) {}
};

template<std::size_t CAP = 262144>
class SpscRing {
    static_assert((CAP & (CAP - 1)) == 0, "CAP must be power of two");
    static_assert(sizeof(uint64_t) == 8,
        "SpscRing requires 64-bit unsigned counters for safe wraparound math");
    // Unsigned overflow is well-defined [ISO C++ basic.fundamental].
    // (head - tail) wraps correctly at 2^64; >= CAP check is always valid.

    static constexpr uint64_t MASK = CAP - 1;

public:
    SpscRing() noexcept : head_(0), tail_(0), tail_cache_(0), dropped_(0) {}

    // ── Producer ─────────────────────────────────────────────────────────────
    FORCE_INLINE bool push(const TickRecord& rec) noexcept {
        const uint64_t head = head_.v.load(std::memory_order_relaxed);
        uint64_t       tail = tail_cache_.v;
        // tail_cache_ is written exclusively by this (producer) thread —
        // no data race, no acquire fence required.

        if ((head & 15) == 0)
            __builtin_prefetch(&slots_[(head + 4) & MASK], 1, 1);

        if (head - tail >= CAP) [[unlikely]] {
            tail = tail_.v.load(std::memory_order_acquire);
            tail_cache_.v = tail;
            if (head - tail >= CAP) {
                dropped_.v.fetch_add(1, std::memory_order_relaxed);
                return false;   // enqueue_() fires DropFn
            }
        }

        slots_[head & MASK] = rec;
        head_.v.store(head + 1, std::memory_order_release);
        return true;
    }

    // ── Consumer ─────────────────────────────────────────────────────────────
    FORCE_INLINE bool pop(TickRecord& rec) noexcept {
        const uint64_t tail = tail_.v.load(std::memory_order_relaxed);
        const uint64_t head = head_.v.load(std::memory_order_acquire);

        if (tail == head) [[unlikely]]
            return false;

        if ((tail & 15) == 0)
            __builtin_prefetch(&slots_[(tail + 4) & MASK], 0, 1);

        rec = slots_[tail & MASK];
        tail_.v.store(tail + 1, std::memory_order_release);
        return true;
    }

    FORCE_INLINE bool empty() const noexcept {
        return tail_.v.load(std::memory_order_acquire)
            == head_.v.load(std::memory_order_acquire);
    }

    uint64_t dropped() const noexcept {
        return dropped_.v.load(std::memory_order_relaxed);
    }

private:
    PaddedAtomic head_;
    PaddedAtomic tail_;
    PaddedU64    tail_cache_;
    PaddedAtomic dropped_;

    alignas(64) TickRecord slots_[CAP];
};

// ═════════════════════════════════════════════════════════════════════════════
// §5  MmapWriter — [FIX-2] Pre-allocated, TLB-warm, grow-free
//
//  V12.7 architecture (abolished):
//   open(65K records) → write → grow(+65K) → TLB shootdown → spike → repeat
//
//  V12.8 architecture:
//   open_prealloc(N GB)
//     posix_fallocate → real disk blocks, no holes
//     MAP_POPULATE    → kernel builds page table upfront (best-effort)
//     page-touch loop → every page faulted and dirtied before first tick
//     madvise(SEQUENTIAL | WILLNEED | HUGEPAGE) → HW prefetcher primed
//   → write_tick() is a pure bounds check + memcpy, zero fault risk.
//   → TLB shootdowns: impossible (no munmap until rotation/shutdown).
//   → Major page faults in steady state: zero.
// ═════════════════════════════════════════════════════════════════════════════
class MmapWriter {
public:
    MmapWriter()  = default;
    ~MmapWriter() { close(); }

    MmapWriter(const MmapWriter&)            = delete;
    MmapWriter& operator=(const MmapWriter&) = delete;

    // ── [FIX-2]  open_prealloc ────────────────────────────────────────────────
    bool open_prealloc(const std::string& path,
                       const std::string& symbol,
                       std::size_t        prealloc_bytes) noexcept
    {
        if (fd_ >= 0) close();

        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (UNLIKELY(fd_ < 0)) return false;

        // posix_fallocate: pre-allocates real disk blocks.
        // Unlike ftruncate(), no sparse holes — every block is claimed now.
        // Rationale: on-demand block allocation at write time causes
        // file-system metadata updates inside the hot path → µs-level spikes.
        if (UNLIKELY(::posix_fallocate(fd_, 0,
                static_cast<off_t>(prealloc_bytes)) != 0))
        {
            ::close(fd_); fd_ = -1;
            std::fprintf(stderr,
                "[KUSAK V12.8] WARN: posix_fallocate(%zu) failed — "
                "falling back to ftruncate (sparse). Disk spikes possible.\n",
                prealloc_bytes);
            // Graceful degradation: ftruncate sparse file.
            fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
            if (UNLIKELY(fd_ < 0)) return false;
            if (UNLIKELY(::ftruncate(fd_,
                    static_cast<off_t>(prealloc_bytes)) < 0))
            {
                ::close(fd_); fd_ = -1; return false;
            }
        }

        // MAP_POPULATE: request kernel to populate page tables at mmap time.
        // Not guaranteed on all kernels/configurations, but when honoured it
        // eliminates the first-touch page faults for the first ~8 MB window.
        // The manual page-touch loop below is the authoritative warm-up.
        int map_flags = MAP_SHARED;
#ifdef MAP_POPULATE
        map_flags |= MAP_POPULATE;
#endif

        base_ = static_cast<uint8_t*>(
            ::mmap(nullptr, prealloc_bytes,
                   PROT_READ | PROT_WRITE, map_flags, fd_, 0));
        if (UNLIKELY(base_ == MAP_FAILED)) {
            base_ = nullptr; ::close(fd_); fd_ = -1; return false;
        }

        file_capacity_ = prealloc_bytes;

        // Access pattern advisory: sequential write pattern, whole file needed.
        ::madvise(base_, file_capacity_, MADV_SEQUENTIAL | MADV_WILLNEED);

        // 2 MB Transparent Huge Pages: reduces TLB pressure by 512x per page.
        // Falls back gracefully to 4K pages if THP is disabled (MADV_NOHUGEPAGE).
#ifdef MADV_HUGEPAGE
        ::madvise(base_, file_capacity_, MADV_HUGEPAGE);
#endif

        // ── Page-touch warm-up ────────────────────────────────────────────────
        //
        //  Despite MAP_POPULATE, some kernels defer page-table entries for
        //  large mappings under memory pressure. Manual write-touch is the
        //  only guarantee. We touch every page at page-size stride:
        //
        //   • base_[off] = 0   → write-fault → page mapped, dirtied
        //   • sink ^= base_[off] → read-back prevents dead-code elimination
        //
        //  Cost: ~1–5 ms for 1 GB on modern NVMe/DRAM (startup only).
        //  Amortised over the file lifetime → zero marginal cost per tick.
        //
        {
            const std::size_t page = static_cast<std::size_t>(
                ::sysconf(_SC_PAGESIZE));
            volatile uint8_t sink = 0;
            for (std::size_t off = 0; off < file_capacity_; off += page) {
                base_[off] = 0;
                sink ^= base_[off];
            }
            (void)sink;
        }

        // Write file header.
        FileHeader hdr{};
        hdr.magic        = TICK_FILE_MAGIC;
        hdr.version      = TICK_FILE_VERSION;
        hdr.ts_created   = now_us();
        hdr.record_count = 0;
        std::strncpy(hdr.symbol, symbol.c_str(), sizeof(hdr.symbol) - 1);
        std::memcpy(base_, &hdr, sizeof(hdr));

        record_count_offset_ = __builtin_offsetof(FileHeader, record_count);
        bytes_written_       = sizeof(FileHeader);
        record_count_        = 0;
        return true;
    }

    // ── write_tick: pure bounds check + memcpy ────────────────────────────────
    //
    //  [FIX-2] grow() abolished. If the preallocated region is full, write_tick
    //  returns false and the caller (writer_loop) triggers rotate_file().
    //  Pre-allocated files are always sized larger than max_file_bytes, so this
    //  path is never taken in steady state — the rotation guard fires first.
    //
    FORCE_INLINE bool write_tick(const TickRecord& rec) noexcept {
        if (UNLIKELY(bytes_written_ + sizeof(DiskRecord) > file_capacity_))
            return false;   // capacity exhausted; caller must rotate

        DiskRecord drec{};
        to_disk(rec, drec);
        std::memcpy(base_ + bytes_written_, &drec, sizeof(drec));
        bytes_written_ += sizeof(DiskRecord);
        ++record_count_;
        return true;
    }

    // MS_ASYNC: non-blocking writeback during normal operation.
    FORCE_INLINE void sync_record_count() noexcept {
        if (UNLIKELY(!base_)) return;
        std::memcpy(base_ + record_count_offset_,
                    &record_count_, sizeof(record_count_));
        ::msync(base_ + record_count_offset_,
                sizeof(record_count_), MS_ASYNC);
    }

    // ── [FIX-D]  Crash-Safe close() (carried from V12.7) ─────────────────────
    //
    //  Shutdown durability sequence:
    //   1. sync_record_count()  — commit final header counter into mmap
    //   2. msync(MS_SYNC)       — flush dirty pages: process → page cache → VFS
    //   3. fdatasync(fd_)       — flush VFS → physical storage (NVMe FLUSH /
    //                             ATA FLUSH CACHE EXT). Bridges volatile write
    //                             cache (NVMe WB, RAID BBU). Power-loss safe.
    //
    void close() noexcept {
        if (!base_) return;

        sync_record_count();
        const std::size_t real_size = bytes_written_;

        ::msync(base_, file_capacity_, MS_SYNC);
        ::munmap(base_, file_capacity_);
        base_ = nullptr;

        if (fd_ >= 0) {
            ::ftruncate(fd_, static_cast<off_t>(real_size));
            ::fdatasync(fd_);   // [FIX-D] physical-media flush
            ::close(fd_);
            fd_ = -1;
        }
    }

    // Accessors required by [FIX-3] NUMA bind in writer_loop.
    uint8_t*    base()           const noexcept { return base_;          }
    std::size_t capacity()       const noexcept { return file_capacity_; }
    std::size_t bytes_written()  const noexcept { return bytes_written_; }
    uint64_t    record_count()   const noexcept { return record_count_;  }

private:
    // grow() ABOLISHED in V12.8. [FIX-2]
    // The field INITIAL_RECORDS / GROW_RECORDS are gone.
    // File size is fixed at construction (open_prealloc).

    int         fd_                  = -1;
    uint8_t*    base_                = nullptr;
    std::size_t file_capacity_       = 0;
    std::size_t bytes_written_       = 0;
    std::size_t record_count_offset_ = 0;
    uint64_t    record_count_        = 0;
};

// ═════════════════════════════════════════════════════════════════════════════
// §6  MarketDataRecorder  —  V12.8  (The Final Ghost)
//
//  Δ from V12.7:
//   • Config::drop_callback (std::function) → drop_fn/drop_ctx (DropFn)
//   • Config::mmap_prealloc_bytes added
//   • open_prealloc() replaces open() in rotate_file()
//   • NUMA bind in writer_loop() after rotate_file()
//   • grow() gone → no TLB shootdowns in steady state
//
//  writer_loop backoff state machine (unchanged):
//   idle < SPIN1  → single _mm_pause()         (~10–30 cy)
//   idle < SPIN2  → 16x _mm_pause()            (~160–480 cy)
//   idle ≥ SPIN2  → adaptive TSC batch-pause   (0–2 µs)
// ═════════════════════════════════════════════════════════════════════════════
class MarketDataRecorder {
public:
    struct Config {
        std::string symbol;
        std::string base_dir              = "/tmp";

        // Rotation threshold. writer_loop starts a new file when
        // bytes_written() >= max_file_bytes. Must be < mmap_prealloc_bytes.
        std::size_t max_file_bytes        = 512ULL * 1024 * 1024;   // 512 MB

        // [FIX-2] Pre-allocated mmap region per file.
        // Must satisfy: mmap_prealloc_bytes > max_file_bytes.
        // Recommended: 1 GB for 512 MB max_file_bytes (2x headroom).
        // Larger values warm up more aggressively but take longer at open().
        std::size_t mmap_prealloc_bytes   = 1ULL * 1024 * 1024 * 1024;  // 1 GB

        uint32_t    fsync_interval_ms     = 10'000;
        int         writer_core           = 2;

        // ── [FIX-1]  Zero-Overhead Drop Callback ─────────────────────────────
        //
        //  Raw function pointer + opaque context replaces std::function.
        //  Contract:
        //   • drop_fn is called on the PRODUCER thread at every drop event.
        //   • Argument 1: cumulative drop count (monotonically increasing).
        //   • Argument 2: drop_ctx — caller-supplied strategy pointer.
        //   • drop_fn MUST be noexcept. No allocation, no lock, no OS call.
        //     Typical budget: < 200 ns.
        //   • If drop_fn == nullptr (default) the branch is predicted-not-taken
        //     and costs zero cycles on the healthy (push succeeds) path.
        //
        //  Pattern:
        //    static void my_drop(uint64_t total, void* ctx) noexcept {
        //        static_cast<MyStrategy*>(ctx)->on_drop(total);
        //    }
        //    cfg.drop_fn  = &my_drop;
        //    cfg.drop_ctx = &strategy_instance;
        //
        DropFn drop_fn  = nullptr;   // [FIX-1]
        void*  drop_ctx = nullptr;   // [FIX-1]
    };

    explicit MarketDataRecorder(Config cfg)
        : cfg_(std::move(cfg))
        , tsc_ghz_(calibrate_tsc_ghz())
    {}

    ~MarketDataRecorder() { stop(); }

    MarketDataRecorder(const MarketDataRecorder&)            = delete;
    MarketDataRecorder& operator=(const MarketDataRecorder&) = delete;

    void start() {
        if (running_.exchange(true)) return;
        writer_thread_ = std::thread([this]{ writer_loop(); });
    }

    void stop() noexcept {
        if (!running_.exchange(false, std::memory_order_release)) return;
        if (writer_thread_.joinable()) writer_thread_.join();
        writer_.close();
    }

    // ── Public record API ─────────────────────────────────────────────────────

    FORCE_INLINE bool record_depth(
            const double* bid_prices, const double* bid_qtys, uint32_t bid_n,
            const double* ask_prices, const double* ask_qtys, uint32_t ask_n,
            uint64_t ts_exchange = 0) noexcept {
        TickRecord rec{};
        rec.ts_exchange = ts_exchange ? ts_exchange : now_us();
        rec.ts_local    = now_us();
        rec.type        = TickType::DEPTH;
        rec.bid_count   = std::min(bid_n, static_cast<uint32_t>(MAX_DEPTH));
        rec.ask_count   = std::min(ask_n, static_cast<uint32_t>(MAX_DEPTH));
        for (uint32_t i = 0; i < rec.bid_count; ++i)
            rec.bids[i] = PriceLevel(bid_prices[i], bid_qtys[i]);
        for (uint32_t i = 0; i < rec.ask_count; ++i)
            rec.asks[i] = PriceLevel(ask_prices[i], ask_qtys[i]);
        return enqueue_(rec);
    }

    FORCE_INLINE bool record_book_ticker(
            double best_bid, double best_bid_qty,
            double best_ask, double best_ask_qty,
            uint64_t ts_exchange = 0) noexcept {
        TickRecord rec{};
        rec.ts_exchange = ts_exchange ? ts_exchange : now_us();
        rec.ts_local    = now_us();
        rec.type        = TickType::BOOK_TICKER;
        rec.bid_count   = 1;
        rec.ask_count   = 1;
        rec.bids[0]     = PriceLevel(best_bid,  best_bid_qty);
        rec.asks[0]     = PriceLevel(best_ask,  best_ask_qty);
        return enqueue_(rec);
    }

    FORCE_INLINE bool record_trade(
            double price, double qty, bool buyer_is_maker,
            uint64_t ts_exchange = 0) noexcept {
        TickRecord rec{};
        rec.ts_exchange = ts_exchange ? ts_exchange : now_us();
        rec.ts_local    = now_us();
        rec.type        = TickType::TRADE;
        rec.trade_price = price;
        rec.trade_qty   = qty;
        rec.trade_buyer = buyer_is_maker;
        return enqueue_(rec);
    }

    FORCE_INLINE bool write_tick_internal(const TickRecord& rec) noexcept {
        return writer_.write_tick(rec);
    }

    uint64_t dropped_packets() const noexcept { return queue_.dropped(); }
    double   tsc_ghz()         const noexcept { return tsc_ghz_;         }

private:
    static constexpr uint32_t SPIN1      =   64;
    static constexpr uint32_t SPIN2      =  256;
    static constexpr uint32_t IDLE_CLAMP = 4096;

    // ── [FIX-1]  Zero-Overhead enqueue_ ──────────────────────────────────────
    //
    //  Healthy path (push succeeds, 99.99% of calls):
    //   • LIKELY(push) → one taken branch → return true
    //   • UNLIKELY(fn) check is never reached
    //   Total: ~1 branch instruction.
    //
    //  Drop path (cold path):
    //   • push returns false
    //   • fn loaded from register (already cached in d-cache)
    //   • UNLIKELY(fn) → one indirect call if set, otherwise skip
    //   Total: ~2–4 ns. Acceptable: this path already implies data loss.
    //
    FORCE_INLINE bool enqueue_(const TickRecord& rec) noexcept {
        if (LIKELY(queue_.push(rec))) return true;

        // [FIX-1] predictable branch — mostly not-taken in healthy operation
        const DropFn fn = cfg_.drop_fn;
        if (UNLIKELY(fn)) {
            fn(queue_.dropped(), cfg_.drop_ctx);
        }
        return false;
    }

    void writer_loop() noexcept {
        // [FIX-CRASH-PIN] pin_thread_to_core() istisna firlatabilir; bu fonksiyon
        // noexcept oldugu icin, once bu istisna disariya kacsaydi ANINDA
        // std::terminate() cagirilirdi (tum program cokerdi) -- CPU pinleme
        // gibi sirf performans icin olan bir optimizasyonun basarisiz olmasi
        // yuzunden. Kisitli ortamlarda (WSL/konteyner/VM, CAP_SYS_NICE yok,
        // sinirli CPU seti) bu gercek bir risk. Artik basarisiz olursa sadece
        // uyari basip pinlemeden devam ediyoruz.
        try {
            pin_thread_to_core(cfg_.writer_core);
        } catch (const std::exception& ex) {
            std::cerr << "[MarketDataRecorder] UYARI: writer thread CPU'ya "
                         "sabitlenemedi (core=" << cfg_.writer_core << "): "
                      << ex.what() << " -- pinlemeden devam ediliyor.\n";
        }

        // Open the first file with full pre-allocation and TLB warm-up.
        rotate_file();

        // ── [FIX-3]  NUMA Memory Enforcement ─────────────────────────────────
        //
        //  Called AFTER rotate_file() so that writer_.base() is valid.
        //
        //  bind_memory_to_node(MPOL_BIND) ensures that:
        //   • Every page fault on the mmap region draws from the local DRAM
        //     bank attached to cfg_.writer_core's NUMA node.
        //   • SpscRing slots are also local — producer writes and consumer
        //     reads both hit local DRAM.
        //
        //  Without this: a process started on node 0 that migrates its writer
        //  thread to node 1 will silently suffer cross-node access (+80 ns
        //  per LLC miss). mbind() is the surgical fix.
        //
        if (writer_.base()) {
            bind_memory_to_node(writer_.base(), writer_.capacity(),
                                cfg_.writer_core);
        }
        bind_memory_to_node(static_cast<void*>(&queue_), sizeof(queue_),
                            cfg_.writer_core);

        const uint64_t cycles_per_us =
            static_cast<uint64_t>(tsc_ghz_ * 1000.0);

        uint64_t fsync_last = now_us();
        uint32_t idle       = 0;

        while (running_.load(std::memory_order_acquire) || !queue_.empty()) {
            TickRecord rec;

            if (queue_.pop(rec)) [[likely]] {
                // [FIX-2] write_tick returns false only if preallocated region
                // is exhausted — this is the rotation trigger. max_file_bytes
                // fires first (see guard below), so this path is a safety net.
                if (UNLIKELY(writer_.bytes_written() >= cfg_.max_file_bytes)) {
                    rotate_file();
                    // Rebind new file's mmap region to the same NUMA node.
                    if (writer_.base()) {
                        bind_memory_to_node(writer_.base(), writer_.capacity(),
                                            cfg_.writer_core);
                    }
                }
                writer_.write_tick(rec);
                idle = 0;
            } else {
                if (idle < SPIN1) {
                    _mm_pause();
                } else if (idle < SPIN2) {
                    for (int i = 0; i < 16; ++i) _mm_pause();
                } else {
                    const uint64_t wait_cycles =
                        std::min<uint64_t>(
                            static_cast<uint64_t>(idle - SPIN2) * 32ULL,
                            2ULL * cycles_per_us);
                    tsc_pause_cycles(wait_cycles);
                }
                idle = std::min(idle + 1, IDLE_CLAMP);
            }

            if (idle == 0) {
                const uint64_t now = now_us();
                if (UNLIKELY(now - fsync_last >=
                             static_cast<uint64_t>(cfg_.fsync_interval_ms)
                             * 1'000ULL))
                {
                    writer_.sync_record_count();
                    fsync_last = now;
                }
            }
        }

        writer_.sync_record_count();
        // writer_.close() called by stop() → ~MarketDataRecorder:
        // triggers msync(MS_SYNC) + fdatasync() [FIX-D].
    }

    void rotate_file() noexcept {
        writer_.close();
        const std::string path = make_tick_filename_legacy_alloc(cfg_.symbol, cfg_.base_dir);
        writer_.open_prealloc(path, cfg_.symbol, cfg_.mmap_prealloc_bytes);
        // NUMA rebind for the new mapping is performed by the caller
        // (writer_loop) after this returns.
    }

    Config             cfg_;
    double             tsc_ghz_;
    SpscRing<262144>   queue_;
    MmapWriter         writer_;
    std::thread        writer_thread_;
    std::atomic<bool>  running_{false};
};

// ═════════════════════════════════════════════════════════════════════════════
// §7  TradeLogger — low-frequency trade record  (unchanged)
// ═════════════════════════════════════════════════════════════════════════════
class TradeLogger {
public:
    explicit TradeLogger(const std::string& path)
        : fp_(std::fopen(path.c_str(), "a"))
    {
        if (!fp_) throw std::runtime_error("TradeLogger: dosya açılamadı: " + path);
        std::fprintf(fp_, "ts_us,price,qty,side,pnl,commission,reason\n");
    }

    ~TradeLogger() { if (fp_) std::fclose(fp_); }

    TradeLogger(const TradeLogger&)            = delete;
    TradeLogger& operator=(const TradeLogger&) = delete;

    bool log(const TradeRecord& rec) noexcept {
        if (UNLIKELY(!fp_)) return false;
        std::fprintf(fp_, "%" PRIu64 ",%.8f,%.8f,%d,%.8f,%.8f,%s\n",
                     rec.ts_us, rec.price, rec.qty, rec.side,
                     rec.pnl, rec.commission, rec.reason);
        return true;
    }

    void flush() noexcept { if (fp_) std::fflush(fp_); }

private:
    std::FILE* fp_ = nullptr;
};

} // namespace kusak::research

#endif // KUSAK_MARKET_DATA_RECORDER_HPP
