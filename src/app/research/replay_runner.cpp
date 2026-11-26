// FILE: src/app/research/replay_runner.cpp
// V13 uyarlamasi: VPINCalculator -> VPINVelocityTracker, TickRecord yeni alanlari,
// .tick dosyasi = FileHeader(256B) + DiskRecord[] -> TickDataLoader ile okunur.

#include "app/research/replay_runner.hpp"

#include "research_types.hpp"
#include "backtester.hpp"            // TickDataLoader
#include "regime_detector.hpp"
#include "auto_param_adapter.hpp"
#include "vpin_velocity_tracker.hpp"
#include "adverse_selection_tracker.hpp"
#include "latency_monitor.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

extern std::atomic<bool> g_stop;  // research_main.cpp'de tanimli (file-scope)

namespace {

// Eski MmapTickView arayuzu korunuyor; icerik artik gercek disk formatini okuyor.
class MmapTickView {
public:
    void open(const char* path) {
        loader_ = std::make_unique<kusak::research::TickDataLoader>(std::string(path));
    }
    [[nodiscard]] const kusak::research::TickRecord* begin() const noexcept {
        return loader_ ? loader_->data() : nullptr;
    }
    [[nodiscard]] const kusak::research::TickRecord* end() const noexcept {
        return loader_ ? loader_->data() + loader_->size() : nullptr;
    }
    [[nodiscard]] size_t size() const noexcept { return loader_ ? loader_->size() : 0; }
private:
    std::unique_ptr<kusak::research::TickDataLoader> loader_;
};

static kusak::research::RegimeSnapshot make_snapshot(
    const kusak::research::TickRecord& rec, double vpin, int32_t vpin_x1e6, double atr_val) noexcept
{
    const double bid_d = kusak::research::tick_bid_depth(rec, 5);
    const double ask_d = kusak::research::tick_ask_depth(rec, 5);
    const double total     = bid_d + ask_d;
    const double imbalance = (total > 1e-12) ? (bid_d - ask_d) / total : 0.0;

    kusak::research::RegimeSnapshot snap{};
    snap.vpin       = vpin;
    snap.vpin_x1e6  = vpin_x1e6;
    snap.atr        = atr_val;
    snap.spread_bps = kusak::research::tick_spread_bps(rec);
    snap.imbalance  = imbalance;
    snap.volume_1m  = 0.0;
    snap.mid_price  = kusak::research::tick_mid_price(rec);
    snap.ts_us      = rec.ts_exchange;
    return snap;
}

struct LivePipelineState {
    kusak::research::RegimeDetectorV2        regime;
    kusak::research::VPINVelocityTracker     vpin_vel;
    kusak::research::AdverseSelectionTracker adverse;
    kusak::research::LatencyMonitor          latency;
    kusak::research::ATRCalculator           atr_calc{14};
    kusak::research::SyntheticBarBuilder     bar_builder{60000};
    kusak::research::AutoParamAdapter        param_adapter;

    uint64_t tick_count     = 0;
    uint64_t trade_count    = 0;
    uint64_t alarm_count    = 0;
    uint64_t cancel_count   = 0;
    uint64_t regime_changes = 0;

    explicit LivePipelineState(const kusak::research::ResearchParams& base,
                               bool measure_latency = false,
                               kusak::research::LatencyMonitor::Config lm_cfg = {})
        : vpin_vel(kusak::research::make_research_vpin_config(base.vpin_buckets, 0.8))
        , latency(lm_cfg)
        , param_adapter(base)
    {
        vpin_vel.set_close_callback([this](kusak::research::VPINAlarm, int64_t) {
            ++alarm_count;
        });
        adverse.set_cancel_callback([this](double) {
            ++cancel_count;
        });
        (void)measure_latency;
    }
};

static void process_tick(const kusak::research::TickRecord& rec,
                         LivePipelineState& s,
                         bool measure_latency)
{
    using kusak::research::TickType;
    const uint64_t t_start = measure_latency ? kusak::mono_ns() : 0;

    ++s.tick_count;

    if (kusak::research::tick_is_trade(rec)) {
        ++s.trade_count;

        s.vpin_vel.update_trade_f(rec.trade_qty, rec.trade_price,
                                  kusak::research::tick_aggressor_is_buy(rec),
                                  rec.ts_exchange);

        const int64_t ts_ms = static_cast<int64_t>(rec.ts_exchange / 1000);
        auto bar = s.bar_builder.onTrade(rec.trade_price, rec.trade_qty, ts_ms);
        if (bar.has_value())
            s.atr_calc.onBar(bar->high, bar->low, bar->close);

        if (rec.bid_count > 0 && rec.ask_count > 0)
            s.adverse.update_mid(rec.ts_exchange, kusak::research::tick_mid_price(rec));
    }

    if (kusak::research::tick_is_book(rec) && rec.bid_count > 0 && rec.ask_count > 0)
    {
        // [DEBUG] VPIN izleme kancasi (Faz 4: log korlugunu gidermek icin gecici)
        const double vpin_now = s.vpin_vel.vpin_f();
        {
            const auto old_flags = std::cout.flags();
            const auto old_prec  = std::cout.precision();
            std::cout << "[DEBUG] VPIN_F() Değeri: "
                      << std::fixed << std::setprecision(6) << vpin_now << '\n';
            std::cout.flags(old_flags);
            std::cout.precision(old_prec);
        }

        const auto   snap = make_snapshot(rec,
                                          vpin_now,
                                          static_cast<int32_t>(s.vpin_vel.vpin_x1e6()),
                                          s.atr_calc.value());

        const kusak::research::MarketRegime regime = s.regime.update(snap);

        if (s.regime.regime_changed()) {
            ++s.regime_changes;
            s.param_adapter.set_regime(regime);
        }
    }

    if (measure_latency)
        s.latency.record(kusak::research::LatencyMonitor::Tag::REPLAY_DISPATCH,
                         kusak::mono_ns() - t_start);
}

// LatencyMonitor::Config'te dosya alani yok -> CSV burada yazilir.
static void write_latency_csv(const kusak::research::LatencyMonitor& lm, const std::string& path)
{
    std::ofstream f(path);
    if (!f) return;
    f << "tag,count,avg_us,min_us,max_us\n";
    for (int i = 0; i < kusak::research::LatencyMonitor::N; ++i) {
        const auto tag = static_cast<kusak::research::LatencyMonitor::Tag>(i);
        const auto& b  = lm.bucket(tag);
        const uint64_t c = b.count.load(std::memory_order_relaxed);
        if (c == 0) continue;
        f << i << ',' << c << ',' << b.avg_us() << ',' << b.min_us() << ',' << b.max_us() << '\n';
    }
}

} // anonymous namespace

namespace kusak::app {

int run_replay(const ResearchCliOptions& opts) {
    const std::string& tick_file  = opts.file;
    const double       speed      = opts.speed;
    const bool         with_mon   = opts.with_monitor;
    const auto         rm         = opts.replay_step
                                    ? research::ReplayMode::STEP
                                    : research::ReplayMode::FAST;
    (void)rm;  // hiz kontrolu bu turn'de orijinaldeki gibi uygulanmiyor

    std::cout << "[Replay] Yükleniyor (mmap): " << tick_file << "\n"
              << "[Replay] Hız=" << speed << "x"
              << "  Monitor=" << (with_mon ? "aktif" : "kapalı") << "\n\n";

    MmapTickView mv;
    mv.open(tick_file.c_str());
    std::cout << "[Replay] " << mv.size() << " tick işlenecek\n";

    research::ResearchParams default_params{};
    LivePipelineState pipeline(default_params, with_mon);

    const auto t0 = std::chrono::steady_clock::now();
    for (const research::TickRecord* p = mv.begin(); p != mv.end(); ++p) {
        if (g_stop.load(std::memory_order_acquire)) break;
        process_tick(*p, pipeline, with_mon);
    }
    const double el = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    std::cout << "\n[Replay] Tamamlandı.\n"
              << "  İşlenen tick      : " << pipeline.tick_count    << "\n"
              << "  İşlenen ticaret   : " << pipeline.trade_count   << "\n"
              << "  VPIN alarmları    : " << pipeline.alarm_count   << "\n"
              << "  Ters seçim iptal  : " << pipeline.cancel_count  << "\n"
              << "  Rejim geçişleri   : " << pipeline.regime_changes << "\n"
              << "  Süre              : " << std::fixed << std::setprecision(2) << el << " s\n"
              << "  Verim             : " << (el > 0.0 ? pipeline.tick_count / el / 1e6 : 0.0) << " M tick/s\n";

    if (with_mon) {
        std::cout << "\n";
        pipeline.latency.print_to_stdout();
        pipeline.adverse.print_summary();
        pipeline.regime.print_diagnostics();
    }
    return 0;
}

int run_monitor(const ResearchCliOptions& opts) {
    const std::string& tick_file  = opts.file;
    const std::string& output_dir = opts.output_dir;

    std::cout << "[Monitor] Yükleniyor (mmap): " << tick_file << "\n";
    MmapTickView mv;
    mv.open(tick_file.c_str());
    std::cout << "[Monitor] " << mv.size() << " tick — gecikme + ters seçim izleme\n\n";

    fs::create_directories(output_dir);

    research::LatencyMonitor::Config lm_cfg;
    lm_cfg.log_to_stdout  = false;
    lm_cfg.report_every_n = 50000;
    const std::string latency_csv = output_dir + "/latency_monitor.csv";

    research::ResearchParams default_params{};
    LivePipelineState pipeline(default_params, true, lm_cfg);

    research::AdverseSelectionConfig as_cfg;
    as_cfg.cancel_threshold_bps = 7.0;
    as_cfg.report_path          = output_dir + "/adverse_selection.csv";
    as_cfg.verbose              = false;
    pipeline.adverse             = research::AdverseSelectionTracker(as_cfg);
    pipeline.adverse.set_cancel_callback([&pipeline](double) { ++pipeline.cancel_count; });

    const auto t0 = std::chrono::steady_clock::now();
    size_t i = 0;
    for (const research::TickRecord* p = mv.begin(); p != mv.end(); ++p, ++i) {
        if (g_stop.load(std::memory_order_acquire)) break;
        process_tick(*p, pipeline, true);

        if (i % 200 == 0 && p->bid_count > 0 && p->ask_count > 0)
            pipeline.adverse.record_fill(p->ts_exchange,
                                         p->bids[0].price, research::tick_mid_price(*p),
                                         0.01, true, true);

        if (i % 100'000 == 0 && i > 0) {
            const double el = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t0).count();
            std::cout << "[Monitor] " << i / 1000 << "k tick"
                      << "  süre=" << std::fixed << std::setprecision(1) << el << "s"
                      << "  alarmlar=" << pipeline.alarm_count << "\n";
        }
    }
    const double total_el = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    std::cout << "\n[Monitor] Tamamlandı (" << total_el << " s)\n\n";
    pipeline.latency.print_to_stdout();
    write_latency_csv(pipeline.latency, latency_csv);
    pipeline.adverse.print_summary();
    pipeline.adverse.write_report();
    pipeline.regime.print_diagnostics();

    std::cout << "\n[Monitor] Çıktılar:\n"
              << "  " << latency_csv       << "\n"
              << "  " << as_cfg.report_path << "\n";
    return 0;
}

} // namespace kusak::app
