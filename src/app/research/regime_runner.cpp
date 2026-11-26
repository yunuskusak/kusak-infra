// FILE: src/app/research/regime_runner.cpp
// Turn 4: run_regime_report research_main.cpp §11'den tasindi.
// Mantik degistirilmedi.

#include "app/research/regime_runner.hpp"

#include "research_types.hpp"
#include "regime_detector.hpp"
#include "auto_param_adapter.hpp"
#include "vpin_velocity_tracker.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

extern std::atomic<bool> g_stop;  // research_main.cpp'de tanimli (file-scope)

namespace {

// MmapTickView — orijinal research_main.cpp §2 aynen kopyalandi
class MmapTickView {
public:
    MmapTickView() = default;
    ~MmapTickView() noexcept { close(); }
    MmapTickView(const MmapTickView&)            = delete;
    MmapTickView& operator=(const MmapTickView&) = delete;

    void open(const char* path) {
        close();
        fd_ = ::open(path, O_RDONLY);
        if (fd_ < 0)
            throw std::runtime_error(std::string("MmapTickView: open failed: ") + path);
        struct stat st{};
        if (::fstat(fd_, &st) != 0)
            throw std::runtime_error("MmapTickView: fstat failed");
        if (st.st_size <= 0)
            throw std::runtime_error("MmapTickView: empty file");
        len_ = static_cast<size_t>(st.st_size);
        if (len_ % sizeof(kusak::research::TickRecord) != 0)
            throw std::runtime_error("MmapTickView: file size not aligned to TickRecord");
        ::posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
        ::posix_fadvise(fd_, 0, 0, POSIX_FADV_WILLNEED);
        map_ = ::mmap(nullptr, len_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (map_ == MAP_FAILED) { map_ = nullptr; throw std::runtime_error("MmapTickView: mmap failed"); }
        ::madvise(map_, len_, MADV_SEQUENTIAL);
        ::madvise(map_, len_, MADV_WILLNEED);
        ptr_   = reinterpret_cast<const kusak::research::TickRecord*>(map_);
        count_ = len_ / sizeof(kusak::research::TickRecord);
    }

    [[nodiscard]] const kusak::research::TickRecord* begin() const noexcept { return ptr_; }
    [[nodiscard]] const kusak::research::TickRecord* end()   const noexcept { return ptr_ + count_; }
    [[nodiscard]] size_t                             size()  const noexcept { return count_; }

private:
    void close() noexcept {
        if (map_)     { ::munmap(map_, len_); map_ = nullptr; }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        len_ = 0; ptr_ = nullptr; count_ = 0;
    }
    int                                      fd_    = -1;
    void*                                    map_   = nullptr;
    size_t                                   len_   = 0;
    const kusak::research::TickRecord*       ptr_   = nullptr;
    size_t                                   count_ = 0;
};

// make_snapshot — orijinal research_main.cpp §4 aynen kopyalandi
static kusak::research::RegimeSnapshot make_snapshot(
    const kusak::research::TickRecord& rec, double vpin, double atr_val) noexcept
{
    double bid_d = 0.0, ask_d = 0.0;
    for (int l = 0; l < std::min(5, (int)rec.bid_count); ++l) bid_d += rec.bid_qtys[l];
    for (int l = 0; l < std::min(5, (int)rec.ask_count); ++l) ask_d += rec.ask_qtys[l];
    double total     = bid_d + ask_d;
    double imbalance = (total > 1e-12) ? (bid_d - ask_d) / total : 0.0;

    kusak::research::RegimeSnapshot snap{};
    snap.vpin       = vpin;
    snap.atr        = atr_val;
    snap.spread_bps = rec.spread_bps;
    snap.imbalance  = imbalance;
    snap.volume_1m  = 0.0;
    snap.mid_price  = rec.mid_price;
    snap.ts_us      = rec.ts_exchange;
    return snap;
}

} // anonymous namespace

namespace kusak::app {

// Orijinal research_main.cpp §11 run_regime_report — aynen kopyalandi
int run_regime_report(const ResearchCliOptions& opts) {
    const std::string& tick_file = opts.file;

    std::cout << "[Rejim] Yükleniyor (mmap): " << tick_file << "\n";
    MmapTickView mv;
    mv.open(tick_file.c_str());
    std::cout << "[Rejim] " << mv.size() << " tick — RegimeDetectorV2 analizi\n\n";

    research::RegimeDetectorV2Config cfg;
    cfg.hmm_confidence = 0.80;
    cfg.track_accuracy = true;

    research::RegimeDetectorV2    detector(cfg);
    research::VPINCalculator      vpin_calc;
    research::ATRCalculator       atr_calc{14};
    research::SyntheticBarBuilder bar_builder{60000};

    // O(1) enum-indexed array yerine O(log N) std::map
    // MarketRegime: UNKNOWN=0, NORMAL=1, ELEVATED=2, HIGH_RISK=3, TOXIC=4
    std::array<uint64_t, 5> counts{};  // zero-init, log(N) yok
    std::vector<std::pair<uint64_t, research::MarketRegime>> transitions;
    transitions.reserve(100'000);      // tek seferlik alloc, realloc yok

    for (const research::TickRecord* p = mv.begin(); p != mv.end(); ++p) {
        if (g_stop.load(std::memory_order_acquire)) break;
        const auto& rec = *p;

        if (rec.type == research::TickType::AGG_TRADE) {
            vpin_calc.onTrade(rec.qty, rec.side == 0);
            const int64_t ms = static_cast<int64_t>(rec.ts_exchange / 1000);
            auto bar = bar_builder.onTrade(rec.price, rec.qty, ms);
            if (bar) atr_calc.onBar(bar->high, bar->low, bar->close);
        }

        if ((rec.type != research::TickType::DEPTH_UPDATE &&
             rec.type != research::TickType::BOOK_TICKER) ||
            rec.bid_count == 0 || rec.ask_count == 0) continue;

        const auto snap   = make_snapshot(rec, vpin_calc.currentVPIN(), atr_calc.value());
        const research::MarketRegime regime = detector.update(snap);

        const size_t idx = static_cast<size_t>(regime);
        if (idx < counts.size()) ++counts[idx];

        if (detector.regime_changed() && transitions.size() < 100'000)
            transitions.emplace_back(rec.ts_exchange, regime);
    }

    // ── Sınıflandırma raporu ─────────────────────────────────────────
    static constexpr const char* kRegimeNames[5] = {
        "UNKNOWN", "NORMAL", "ELEVATED", "HIGH_RISK", "TOXIC"
    };
    const uint64_t total = mv.size();
    std::cout << "[Rejim] Sınıflandırma Raporu (" << total << " tick):\n\n";
    for (size_t i = 0; i < counts.size(); ++i) {
        if (counts[i] == 0) continue;
        const double pct = 100.0 * counts[i] / total;
        const std::string bar(static_cast<int>(pct / 2), '#');
        std::cout << "  " << std::left << std::setw(9) << kRegimeNames[i]
                  << std::right << std::setw(8) << counts[i] << " tick  "
                  << std::fixed << std::setprecision(1) << pct << "%  " << bar << "\n";
    }

    std::cout << "\n  Rejim geçiş sayısı: " << transitions.size() << "\n";
    if (!transitions.empty()) {
        std::cout << "  İlk 10 geçiş:\n";
        for (size_t k = 0; k < std::min(size_t(10), transitions.size()); ++k)
            std::cout << "    ts=" << transitions[k].first
                      << "  → " << kRegimeNames[static_cast<size_t>(transitions[k].second)] << "\n";
    }

    std::cout << "\n";
    detector.print_diagnostics();
    return 0;
}

} // namespace kusak::app
