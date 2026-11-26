// FILE: src/app/research/optimize_runner.cpp
// Turn 3: run_optimize + run_genetic + run_wfo research_main.cpp'den tasindi.
// Mantik degistirilmedi.

#include "app/research/optimize_runner.hpp"

#include "research_types.hpp"
#include "backtester.hpp"
#include "slippage_models.hpp"
#include "parameter_optimizer.hpp"
#include "genetic_optimizer.hpp"
#include "walk_forward_optimizer.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Orijinal research_main.cpp §2 MmapTickView — aynen kopyalandi
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
        if (map_ == MAP_FAILED) {
            map_ = nullptr;
            throw std::runtime_error("MmapTickView: mmap failed");
        }

        ::madvise(map_, len_, MADV_SEQUENTIAL);
        ::madvise(map_, len_, MADV_WILLNEED);

        ptr_   = reinterpret_cast<const kusak::research::TickRecord*>(map_);
        count_ = len_ / sizeof(kusak::research::TickRecord);
    }

    [[nodiscard]] const kusak::research::TickRecord* data()  const noexcept { return ptr_;   }
    [[nodiscard]] size_t                             size()  const noexcept { return count_; }
    [[nodiscard]] const kusak::research::TickRecord* begin() const noexcept { return ptr_;   }
    [[nodiscard]] const kusak::research::TickRecord* end()   const noexcept { return ptr_ + count_; }

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

// Orijinal research_main.cpp §4 print_backtest_result — aynen kopyalandi
// (run_genetic tarafindan kullaniliyor; ortaklastirma yapilmadi)
static void print_backtest_result(const kusak::research::BacktestResult& r, double elapsed_s) {
    if (!r.valid) {
        std::cout << "[Backtest] GEÇERSİZ — yetersiz işlem ("
                  << (int)r.total_trades << ")\n";
        return;
    }

    auto pct = [](double v) -> std::string {
        std::ostringstream s;
        s << std::fixed << std::setprecision(2) << v << "%";
        return s.str();
    };

    std::cout << "\n╔══════════════════════════════════════════════════════════════════╗\n"
                 "║                 BACKTEST SONUÇ RAPORU                           ║\n"
                 "╚══════════════════════════════════════════════════════════════════╝\n"
              << std::fixed
              << "\n── Temel Sonuçlar ─────────────────────────────────────────────────\n"
              << "  Toplam İşlem    : " << (int)r.total_trades << "\n"
              << "  Net PnL          : " << std::setprecision(4) << r.net_pnl << " USDT\n"
              << "  Win Rate         : " << pct(r.win_rate * 100.0) << "\n"
              << "  Ort. İşlem PnL   : " << r.avg_trade_pnl << " USDT\n"
              << "  Max Drawdown     : " << r.max_drawdown << " USDT\n"
              << "  Fill Rate        : " << pct(r.fill_rate * 100.0) << "\n"
              << "\n── Risk/Getiri Oranları ───────────────────────────────────────────\n"
              << "  Sharpe Ratio     : " << std::setprecision(4) << r.sharpe_ratio << "\n"
              << "  Sortino Ratio    : " << r.sortino_ratio << "\n"
              << "  Calmar Ratio     : " << r.calmar_ratio << "\n"
              << "\n── Genişletilmiş Metrikler ────────────────────────────────────────\n"
              << "  Profit Factor    : " << std::setprecision(3) << r.profit_factor
              << (r.profit_factor >= 2.0 ? "  (Çok İyi)" :
                  r.profit_factor >= 1.5 ? "  (Kabul Edilebilir)" : "  (Zayıf)") << "\n"
              << "  Maks Kazanç Seri : " << r.max_win_streak << "\n"
              << "  Maks Kayıp Seri  : " << r.max_loss_streak << "\n"
              << "  Ort. Kazanan     : " << std::setprecision(4) << r.avg_win_pnl << " USDT\n"
              << "  Ort. Kaybeden    : " << r.avg_loss_pnl << " USDT\n"
              << "  Exposure %%       : " << std::setprecision(1) << r.exposure_pct << "%%\n"
              << "\n── Buy-and-Hold Benchmark ─────────────────────────────────────────\n"
              << "  Buy&Hold         : " << std::setprecision(4) << r.buy_hold_return << " USDT\n"
              << "  Strateji         : " << r.net_pnl << " USDT\n"
              << "  ALFA             : " << r.alpha
              << (r.alpha > 0 ? "  (Piyasayı yendi)" : "  (Buy&Hold altında)") << "\n";

    if (r.kelly_fraction_est > 0.0)
        std::cout << "\n── Kelly Pozisyon Önerisi ─────────────────────────────────────────\n"
                  << "  Kelly %%          : " << std::setprecision(1)
                  << r.kelly_fraction_est * 100.0 << "%%\n";

    if (r.total_slippage_cost > 0.0)
        std::cout << "\n── Slippage ───────────────────────────────────────────────────────\n"
                  << "  Toplam Slippage  : " << std::setprecision(4) << r.total_slippage_cost << " USDT\n"
                  << "  Ort. Slippage    : " << std::setprecision(3) << r.avg_slippage_bps << " bps\n";

    std::cout << "\n── Meta ───────────────────────────────────────────────────────────\n"
              << "  Backtest Süresi  : " << std::setprecision(2) << elapsed_s << " s\n"
              << "  Objective Skoru  : " << std::setprecision(6) << r.objective_score() << "\n"
              << std::string(67, '-') << "\n\n";
}

} // anonymous namespace

namespace kusak::app {

// Orijinal research_main.cpp §7 run_optimize — aynen kopyalandi
int run_optimize(const ResearchCliOptions& opts) {
    const std::string& tick_file  = opts.file;
    const int          n_workers  = opts.workers;
    const int          bayes_iters= opts.bayes_iterations;
    const std::string& output_dir = opts.output_dir;

    std::cout << "[Optimizer] Yükleniyor (mmap): " << tick_file << "\n";
    MmapTickView mv;
    mv.open(tick_file.c_str());
    std::cout << "[Optimizer] " << mv.size() << " tick"
              << "  workers=" << n_workers
              << "  bayes=" << bayes_iters << "\n";

    // measure_latency=false: optimizer iteration'larında latency kaydı gereksiz
    auto obj_fn = [&](const research::ResearchParams& p) -> research::BacktestResult {
        research::Backtester bt(mv.data(), mv.size(), research::Backtester::Config{});
        return bt.run(p);
    };

    research::ParameterOptimizer::Config cfg;
    cfg.n_workers        = n_workers;
    cfg.bayes_iterations = bayes_iters;
    cfg.verbose          = true;

    fs::create_directories(output_dir);
    cfg.results_csv = output_dir + "/opt_results.csv";

    research::ParameterOptimizer optimizer(obj_fn, cfg);
    auto result = optimizer.optimize();

    std::string params_path = output_dir + "/best_params.txt";
    std::ofstream f(params_path);
    f << "# KUSAK V9 Grid+Bayes Optimized Parameters\n"
      << "# Skor: " << result.best_score << "\n"
      << "entry_threshold  = " << result.best_params.entry_threshold  << "\n"
      << "exit_threshold   = " << result.best_params.exit_threshold   << "\n"
      << "stop_loss        = " << result.best_params.stop_loss        << "\n"
      << "take_profit      = " << result.best_params.take_profit      << "\n"
      << "vpin_threshold   = " << result.best_params.vpin_threshold   << "\n"
      << "imbalance_thresh = " << result.best_params.imbalance_thresh << "\n"
      << "position_size    = " << result.best_params.position_size    << "\n"
      << "depth_levels     = " << result.best_params.depth_levels     << "\n";

    std::cout << "[Optimizer] Parametreler → " << params_path     << "\n"
              << "[Optimizer] Tüm sonuçlar → " << cfg.results_csv << "\n";
    return 0;
}

// Orijinal research_main.cpp §8 run_genetic — aynen kopyalandi
int run_genetic(const ResearchCliOptions& opts) {
    const std::string& tick_file  = opts.file;
    const int          n_workers  = opts.workers;
    const int          population = opts.population;
    const int          generations= opts.generations;
    const std::string& output_dir = opts.output_dir;

    std::cout << "[GeneticOpt] Yükleniyor (mmap): " << tick_file << "\n";
    MmapTickView mv;
    mv.open(tick_file.c_str());
    std::cout << "[GeneticOpt] " << mv.size() << " tick"
              << "  Popülasyon=" << population
              << "  Nesil=" << generations
              << "  Workers=" << n_workers << "\n";

    // measure_latency=false: genetic fitness fonksiyonlarında latency kaydı yok
    auto fitness_fn = [&](const research::ResearchParams& p) -> research::BacktestResult {
        research::Backtester bt(mv.data(), mv.size(), research::Backtester::Config{});
        return bt.run(p);
    };

    research::GeneticConfig gcfg;
    gcfg.population_size = population;
    gcfg.max_generations = generations;
    gcfg.n_workers       = n_workers;
    gcfg.verbose         = true;

    research::GeneticOptimizer gopt(gcfg);
    auto best = gopt.run(fitness_fn);

    std::cout << "\n[GeneticOpt] En iyi parametrelerle backtest yapılıyor...\n";
    research::Backtester::Config bt_cfg;
    bt_cfg.compute_benchmark = true;
    bt_cfg.initial_capital   = 1000.0;
    research::Backtester bt(mv.data(), mv.size(), bt_cfg);

    auto t0 = std::chrono::steady_clock::now();
    research::BacktestResult r = bt.run(best.params);
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    print_backtest_result(r, el);

    fs::create_directories(output_dir);
    std::string path = output_dir + "/genetic_best_params.txt";
    std::ofstream f(path);
    f << "# KUSAK V9 Genetic Optimized Parameters\n"
      << "# Fitness: " << best.fitness << "\n"
      << "entry_threshold  = " << best.params.entry_threshold  << "\n"
      << "exit_threshold   = " << best.params.exit_threshold   << "\n"
      << "stop_loss        = " << best.params.stop_loss        << "\n"
      << "take_profit      = " << best.params.take_profit      << "\n"
      << "vpin_threshold   = " << best.params.vpin_threshold   << "\n"
      << "imbalance_thresh = " << best.params.imbalance_thresh << "\n"
      << "position_size    = " << best.params.position_size    << "\n"
      << "depth_levels     = " << best.params.depth_levels     << "\n";

    std::cout << "[GeneticOpt] Parametreler → " << path << "\n";
    return 0;
}

// Orijinal research_main.cpp §10 run_wfo — aynen kopyalandi
int run_wfo(const ResearchCliOptions& opts) {
    const std::string& tick_file  = opts.file;
    const int          n_workers  = opts.workers;
    const int          bayes_iters= opts.bayes_iterations;
    const size_t       train_ticks= static_cast<size_t>(opts.wfo_train);
    const size_t       test_ticks = static_cast<size_t>(opts.wfo_test);
    const size_t       step_ticks = static_cast<size_t>(opts.wfo_step);
    const std::string& output_dir = opts.output_dir;

    (void)n_workers; (void)bayes_iters;

    std::cout << "[WFO] Yükleniyor (mmap): " << tick_file << "\n";
    MmapTickView mv;
    mv.open(tick_file.c_str());

    std::cout << "[WFO] " << mv.size() << " tick"
              << "  eğitim=" << train_ticks
              << "  test="   << test_ticks
              << "  adım="   << step_ticks
              << "  workers=" << n_workers << "\n\n";

    if (mv.size() < train_ticks + test_ticks) {
        std::cerr << "[WFO] HATA: Yeterli tick yok (en az "
                  << (train_ticks + test_ticks) << " gerekli, "
                  << mv.size() << " var)\n";
        return 1;
    }

    fs::create_directories(output_dir);

    // measure_latency=false: WFO iteration'larında latency gereksiz
    research::WFOObjectiveFn obj = [&](const research::ResearchParams& p,
                                       const research::TickRecord* in_ptr,  size_t in_len,
                                       const research::TickRecord* out_ptr, size_t out_len) -> research::BacktestResult
    {
        (void)in_ptr; (void)in_len;   // in-sample optimizasyon ParameterOptimizer'a devredilir
        research::Backtester bt(out_ptr, out_len, research::Backtester::Config{});
        return bt.run(p);
    };

    research::WFOConfig wfo_cfg;
    wfo_cfg.in_sample_ticks  = train_ticks;
    wfo_cfg.out_sample_ticks = test_ticks;
    wfo_cfg.step_ticks       = step_ticks;

    research::WalkForwardOptimizer wfo(mv.data(), mv.size(), obj, wfo_cfg);
    std::vector<research::WFOFoldResult> folds = wfo.run();

    if (folds.empty()) {
        std::cerr << "[WFO] UYARI: Hiç fold üretilemedi (yeterli tick yok?).\n";
        return 1;
    }

    // ── Fold özeti ──────────────────────────────────────────────────────
    double total_oos_sharpe = 0.0;
    double total_oos_pnl    = 0.0;
    std::cout << "\n[WFO] " << folds.size() << " fold tamamlandı:\n";
    std::cout << std::left
              << std::setw(6)  << "Fold"
              << std::setw(12) << "OOS PnL"
              << std::setw(12) << "OOS Sharpe"
              << std::setw(8)  << "Trades" << "\n"
              << std::string(38, '-') << "\n";
    for (size_t i = 0; i < folds.size(); ++i) {
        const auto& f = folds[i];
        total_oos_sharpe += f.out_result.sharpe;
        total_oos_pnl    += f.out_result.net_pnl;
        std::cout << std::setw(6)  << i + 1
                  << std::setw(12) << std::fixed << std::setprecision(4) << f.out_result.net_pnl
                  << std::setw(12) << std::setprecision(4) << f.out_result.sharpe
                  << std::setw(8)  << f.out_result.total_trades << "\n";
    }
    const double avg_sharpe = total_oos_sharpe / static_cast<double>(folds.size());
    std::cout << std::string(38, '-') << "\n"
              << "  Ort. OOS Sharpe : " << std::setprecision(4) << avg_sharpe << "\n"
              << "  Top. OOS PnL    : " << std::setprecision(4) << total_oos_pnl << "\n\n";

    // ── En iyi fold'un parametrelerini kaydet ───────────────────────────
    const auto best_fold = std::max_element(folds.begin(), folds.end(),
        [](const research::WFOFoldResult& a, const research::WFOFoldResult& b) {
            return a.out_result.sharpe < b.out_result.sharpe;
        });

    std::string params_path = output_dir + "/wfo_best_params.txt";
    std::ofstream f(params_path);
    f << "# KUSAK V9 Walk-Forward Optimized Parameters\n"
      << "# Folds          : " << folds.size()  << "\n"
      << "# Avg OOS Sharpe : " << avg_sharpe    << "\n"
      << "entry_threshold  = " << best_fold->best_params.entry_threshold  << "\n"
      << "exit_threshold   = " << best_fold->best_params.exit_threshold   << "\n"
      << "stop_loss        = " << best_fold->best_params.stop_loss        << "\n"
      << "take_profit      = " << best_fold->best_params.take_profit      << "\n"
      << "vpin_threshold   = " << best_fold->best_params.vpin_threshold   << "\n"
      << "imbalance_thresh = " << best_fold->best_params.imbalance_thresh << "\n"
      << "position_size    = " << best_fold->best_params.position_size    << "\n"
      << "depth_levels     = " << best_fold->best_params.depth_levels     << "\n";
    std::cout << "[WFO] En iyi parametreler → " << params_path << "\n";
    return 0;
}

} // namespace kusak::app
