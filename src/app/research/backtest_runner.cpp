// FILE: src/app/research/backtest_runner.cpp
// Turn 3: run_backtest + print_backtest_result research_main.cpp'den tasindi.
// Mantik degistirilmedi.

#include "app/research/backtest_runner.hpp"

#include "research_types.hpp"
#include "backtester.hpp"
#include "slippage_models.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Eski MmapTickView arayuzu korunuyor; .tick = FileHeader(256B) + DiskRecord[]
// oldugundan okuma TickDataLoader (backtester.hpp) uzerinden yapilir.
class MmapTickView {
public:
    void open(const char* path) {
        loader_ = std::make_unique<kusak::research::TickDataLoader>(std::string(path));
    }
    [[nodiscard]] const kusak::research::TickRecord* data()  const noexcept { return loader_ ? loader_->data() : nullptr; }
    [[nodiscard]] size_t                             size()  const noexcept { return loader_ ? loader_->size() : 0; }
private:
    std::unique_ptr<kusak::research::TickDataLoader> loader_;
};

// Orijinal research_main.cpp §4 print_backtest_result — aynen kopyalandi
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
              << "  Exposure %        : " << std::setprecision(1) << r.exposure_pct << "%\n"
              << "\n── Buy-and-Hold Benchmark ─────────────────────────────────────────\n"
              << "  Buy&Hold         : " << std::setprecision(4) << r.buy_hold_return << " USDT\n"
              << "  Strateji         : " << r.net_pnl << " USDT\n"
              << "  ALFA             : " << r.alpha
              << (r.alpha > 0 ? "  (Piyasayı yendi)" : "  (Buy&Hold altında)") << "\n";

    if (r.kelly_fraction_est > 0.0)
        std::cout << "\n── Kelly Pozisyon Önerisi ─────────────────────────────────────────\n"
                  << "  Kelly %           : " << std::setprecision(1)
                  << r.kelly_fraction_est * 100.0 << "%\n";

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

// Orijinal research_main.cpp §6 run_backtest — aynen kopyalandi.
// Orijinal imza: run_backtest(tick_file, params, bt_cfg)
// Yeni imza: run_backtest(opts) — opts'tan ayni degerler uretiliyor.
int run_backtest(const ResearchCliOptions& opts) {
    const std::string& tick_file = opts.file;

    // slip_cfg: yalnizca model adi/raporlama + FIXED_BASIS_POINTS icin bps degeri.
    // V13 Backtester tek bir slippage_bps alani kullanir (diger modeller simule edilmez).
    research::SlippageConfig slip_cfg;
    if      (opts.slippage_model_str == "bps" || opts.slippage_model_str == "fixed_bps")
                                                      slip_cfg.model = research::SlippageModel::FIXED_BASIS_POINTS;
    else if (opts.slippage_model_str == "volume")     slip_cfg.model = research::SlippageModel::VOLUME_SHARE;
    else if (opts.slippage_model_str == "volatility") slip_cfg.model = research::SlippageModel::VOLATILITY_VOLUME;
    else                                              slip_cfg.model = research::SlippageModel::FIXED_TICKS;

    research::Backtester::Config bt_cfg;   // BacktesterConfig: commission_bps/slippage_bps/slippage/position_size
    if (slip_cfg.model == research::SlippageModel::FIXED_BASIS_POINTS)
        bt_cfg.slippage_bps = slip_cfg.basis_points.basis_points;

    // V13 Backtester'da olmayan ayarlar (kelly_frac, buy_pct, circuit breaker,
    // initial_capital, benchmark) runner seviyesinde tutulur.
    constexpr double kInitialCapital = 1000.0;
    (void)opts.kelly_frac; (void)opts.buy_pct; (void)opts.cb_drawdown;

    research::ResearchParams default_params{};

    std::cout << "[Backtest] Yükleniyor (mmap): " << tick_file << "\n";
    MmapTickView mv;
    mv.open(tick_file.c_str());
    std::cout << "[Backtest] " << mv.size() << " tick"
              << "  Slippage=" << research::slippage_model_name(slip_cfg.model) << "\n";

    research::Backtester bt(mv.data(), mv.size(), bt_cfg);
    auto t0 = std::chrono::steady_clock::now();
    research::BacktestResult r = bt.run(default_params);
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // Turetilmis metrikler (valid, sharpe_ratio, sortino, calmar, streak, kelly ...)
    r.finalize_derived(5);

    // Buy&Hold benchmark: ilk ve son gecerli mid fiyat, position_size adet
    if (mv.size() > 0) {
        double first_mid = 0.0, last_mid = 0.0;
        const research::TickRecord* d = mv.data();
        for (size_t i = 0; i < mv.size() && first_mid <= 0.0; ++i)
            first_mid = research::tick_mid_price(d[i]);
        for (size_t i = mv.size(); i-- > 0 && last_mid <= 0.0; )
            last_mid = research::tick_mid_price(d[i]);
        if (first_mid > 0.0 && last_mid > 0.0)
            r.buy_hold_return = (last_mid - first_mid) * default_params.position_size;
        r.alpha = r.net_pnl - r.buy_hold_return;
    }
    (void)kInitialCapital;

    print_backtest_result(r, el);
    return r.valid ? 0 : 1;
}

} // namespace kusak::app
