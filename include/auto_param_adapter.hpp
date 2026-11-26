// ============================================================================
// KUSAK V9 — RESEARCH FRAMEWORK
// auto_param_adapter.hpp  [HFT REFACTOR]
//
// Çözülen:
// 1. enum ParamFlags silindi — FLAG_* research_types.hpp static constexpr'dan gelir
// 2. MarketRegime: TREND→TRENDING, RANGE→RANGING, TOXIC→VOLATILE, DEAD→THIN
// ============================================================================

#pragma once

#include "research_types.hpp"
#include "regime_detector.hpp"

#include <atomic>
#include <array>
#include <cstring>
#include <limits>
#include <type_traits>

#if defined(__GNUC__) || defined(__clang__)
#   define APA_INLINE __attribute__((always_inline)) inline
#else
#   define APA_INLINE inline
#endif

namespace kusak::research {

// ============================================================================
// §1  Constants
// ============================================================================

static constexpr size_t kCacheLine   = 64;
static constexpr size_t kRegimeCount = 5;   // UNKNOWN, TRENDING, RANGING, VOLATILE, THIN

// ============================================================================
// §2  RegimeParamEntry — per-regime overrides (NaN = inherit base)
// ============================================================================

struct RegimeParamEntry {
    double vpin_threshold      = std::numeric_limits<double>::quiet_NaN();
    double entry_threshold     = std::numeric_limits<double>::quiet_NaN();
    double exit_threshold      = std::numeric_limits<double>::quiet_NaN();
    double stop_loss           = std::numeric_limits<double>::quiet_NaN();
    double take_profit         = std::numeric_limits<double>::quiet_NaN();
    double imbalance_thresh    = std::numeric_limits<double>::quiet_NaN();
    double position_size       = std::numeric_limits<double>::quiet_NaN();

    bool disable_buys  = false;
    bool disable_sells = false;
    bool sniper_mode   = false;
};

static constexpr bool apa_is_nan(double v) noexcept { return v != v; }

// ============================================================================
// §3  Snapshot — single atomic publish unit
// ============================================================================

struct Snapshot {
    ResearchParams p{};
    MarketRegime   regime = MarketRegime::UNKNOWN;
    uint8_t        flags  = 0;
};

static constexpr size_t kSnapshotRaw  = sizeof(Snapshot);
static constexpr size_t kSlotStride   =
    ((kSnapshotRaw + kCacheLine - 1) / kCacheLine) * kCacheLine;
static constexpr size_t kSlotPadBytes =
    kSlotStride > kSnapshotRaw ? kSlotStride - kSnapshotRaw : 0;

struct alignas(kCacheLine) SlotPad {
    Snapshot snap{};
    uint8_t  _pad[kSlotPadBytes > 0 ? kSlotPadBytes : 1]{};
};

static_assert(sizeof(SlotPad) % kCacheLine == 0,
              "SlotPad must be a multiple of cache-line size");
static_assert(alignof(SlotPad) == kCacheLine,
              "SlotPad must be cache-line aligned");

// ============================================================================
// §4  default_regime_table
// ============================================================================

inline std::array<RegimeParamEntry, kRegimeCount>
default_regime_table(const ResearchParams& base) noexcept
{
    std::array<RegimeParamEntry, kRegimeCount> t{};

    // UNKNOWN (0) — base passthrough

    // TRENDING (1)
    t[1].entry_threshold  = base.entry_threshold * 0.90;
    t[1].sniper_mode      = false;

    // RANGING (2)
    t[2].entry_threshold  = base.entry_threshold;
    t[2].imbalance_thresh = base.imbalance_thresh;

    // VOLATILE (3)
    t[3].disable_buys     = false;
    t[3].entry_threshold  = base.entry_threshold * 2.0;
    t[3].vpin_threshold   = base.vpin_threshold;

    // THIN (4)
    t[4].entry_threshold  = base.entry_threshold * 1.5;
    t[4].imbalance_thresh = base.imbalance_thresh * 1.3;

    return t;
}

// ============================================================================
// §5  AutoParamAdapter
// ============================================================================

class AutoParamAdapter {
public:
    explicit AutoParamAdapter(const ResearchParams& base) noexcept
        : base_(base)
    {
        tables_[0] = default_regime_table(base_);
        tables_[1] = tables_[0];
        table_idx_.store(0, std::memory_order_release);

        slots_[0].snap.p      = base_;
        slots_[0].snap.regime = MarketRegime::UNKNOWN;
        slots_[0].snap.flags  = 0;
        slots_[1]             = slots_[0];

        active_idx_.store(0, std::memory_order_release);
    }

    // ── HOT-PATH — engine thread, zero locks ─────────────────────────────────

    APA_INLINE const Snapshot& snapshot() const noexcept {
        return slots_[active_idx_.load(std::memory_order_acquire)].snap;
    }

    APA_INLINE const ResearchParams& read() const noexcept {
        return snapshot().p;
    }

    APA_INLINE MarketRegime current_regime() const noexcept {
        return snapshot().regime;
    }

    // FLAG_* come from research_types.hpp static constexpr uint8_t
    APA_INLINE bool buys_disabled()  const noexcept {
        return (snapshot().flags & FLAG_DISABLE_BUYS)  != 0u;
    }
    APA_INLINE bool sells_disabled() const noexcept {
        return (snapshot().flags & FLAG_DISABLE_SELLS) != 0u;
    }
    APA_INLINE bool sniper_mode()    const noexcept {
        return (snapshot().flags & FLAG_SNIPER_MODE)   != 0u;
    }

    const ResearchParams& base_params() const noexcept { return base_; }

    // ── WRITER-SIDE API ──────────────────────────────────────────────────────

    void set_regime_entry(MarketRegime r, const RegimeParamEntry& entry) noexcept {
        WriterGuard g(writer_gate_);

        const uint32_t cur_ti = table_idx_.load(std::memory_order_relaxed);
        const uint32_t new_ti = cur_ti ^ 1u;

        tables_[new_ti] = tables_[cur_ti];
        tables_[new_ti][static_cast<size_t>(r)] = entry;
        table_idx_.store(new_ti, std::memory_order_release);

        const uint32_t ai = active_idx_.load(std::memory_order_relaxed);
        if (slots_[ai].snap.regime == r)
            publish_snapshot(r);
    }

    bool set_regime(MarketRegime new_regime) noexcept {
        WriterGuard g(writer_gate_);

        const uint32_t ai = active_idx_.load(std::memory_order_relaxed);
        if (slots_[ai].snap.regime == new_regime) return false;

        publish_snapshot(new_regime);
        return true;
    }

    void override_param(double ResearchParams::* field, double value) noexcept {
        WriterGuard g(writer_gate_);

        const uint32_t ai  = active_idx_.load(std::memory_order_relaxed);
        const uint32_t ini = ai ^ 1u;

        slots_[ini].snap          = slots_[ai].snap;
        slots_[ini].snap.p.*field = value;

        active_idx_.store(ini, std::memory_order_release);
    }

private:
    struct WriterGuard {
        std::atomic_flag& f_;
        explicit WriterGuard(std::atomic_flag& f) noexcept : f_(f) {
            while (f_.test_and_set(std::memory_order_acquire)) {}
        }
        ~WriterGuard() noexcept { f_.clear(std::memory_order_release); }
        WriterGuard(const WriterGuard&)            = delete;
        WriterGuard& operator=(const WriterGuard&) = delete;
    };

    void publish_snapshot(MarketRegime r) noexcept {
        const uint32_t ai  = active_idx_.load(std::memory_order_relaxed);
        const uint32_t ini = ai ^ 1u;

        Snapshot& dst = slots_[ini].snap;
        dst.regime    = r;
        apply_entry(r, dst.p, dst.flags);

        active_idx_.store(ini, std::memory_order_release);
    }

    void apply_entry(MarketRegime r,
                     ResearchParams& out,
                     uint8_t& flags) const noexcept
    {
        const uint32_t ti = table_idx_.load(std::memory_order_acquire);
        const RegimeParamEntry& e = tables_[ti][static_cast<size_t>(r)];

        out = base_;

        if (!apa_is_nan(e.vpin_threshold))   out.vpin_threshold   = e.vpin_threshold;
        if (!apa_is_nan(e.entry_threshold))  out.entry_threshold  = e.entry_threshold;
        if (!apa_is_nan(e.exit_threshold))   out.exit_threshold   = e.exit_threshold;
        if (!apa_is_nan(e.stop_loss))        out.stop_loss        = e.stop_loss;
        if (!apa_is_nan(e.take_profit))      out.take_profit      = e.take_profit;
        if (!apa_is_nan(e.imbalance_thresh)) out.imbalance_thresh = e.imbalance_thresh;
        if (!apa_is_nan(e.position_size))    out.position_size    = e.position_size;

        flags  = 0u;
        flags |= e.disable_buys  ? FLAG_DISABLE_BUYS  : 0u;
        flags |= e.disable_sells ? FLAG_DISABLE_SELLS : 0u;
        flags |= e.sniper_mode   ? FLAG_SNIPER_MODE   : 0u;
    }

    ResearchParams base_;
    SlotPad        slots_[2];

    alignas(kCacheLine) std::atomic<uint32_t> active_idx_{0};

    std::array<RegimeParamEntry, kRegimeCount> tables_[2]{};
    alignas(kCacheLine) std::atomic<uint32_t>  table_idx_{0};

    alignas(kCacheLine) mutable std::atomic_flag writer_gate_ = ATOMIC_FLAG_INIT;
};

}  // namespace kusak::research
