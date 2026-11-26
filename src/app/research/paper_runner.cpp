// FILE: src/app/research/paper_runner.cpp
// Turn 3: run_paper research_main.cpp'den tasindi.
// Mantik degistirilmedi.

#include "app/research/paper_runner.hpp"

#include "research_types.hpp"
#include "backtester.hpp"
#include "slippage_models.hpp"
#include "paper_trader.hpp"
#include "notification.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
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

// g_stop: paper_runner'in tick loop'u sinyal kontrolu yapiyor.
// Orijinal research_main.cpp'deki g_stop ile ayni semantik:
// SIGINT/SIGTERM geldiginde loop'u kesiyor.
// Bu extern, research_main.cpp'deki anonymous namespace'teki g_stop'a baglanir.
extern std::atomic<bool> g_stop;  // research_main.cpp §1'de tanimli

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
// (run_paper tarafindan kullaniliyor; ortaklastirma yapilmadi)
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

// Orijinal research_main.cpp §9 run_paper — aynen kopyalandi
int run_paper(const ResearchCliOptions& opts) {
    const std::string& tick_file = opts.file;
    const std::string& env_path  = opts.env_path;

    research::ResearchParams params{};

    std::cout << "[PaperTrader] Yükleniyor (mmap): " << tick_file << "\n";
    MmapTickView mv;
    mv.open(tick_file.c_str());
    std::cout << "[PaperTrader] " << mv.size() << " tick\n"
              << "[PaperTrader] NOT: Gerçek emirler GÖNDERILMEZ.\n\n";

    kusak::notify::NotificationConfig notify_cfg;
    if (!env_path.empty()) {
        notify_cfg = kusak::notify::load_notification_config_from_env(env_path);
        if (notify_cfg.telegram.enabled)
            std::cout << "[PaperTrader] Telegram bildirimleri aktif\n";
        if (notify_cfg.slack.enabled)
            std::cout << "[PaperTrader] Slack bildirimleri aktif\n";
    }
    kusak::notify::Notifier notifier(notify_cfg);

    research::PaperTraderConfig cfg;
    cfg.params             = params;
    cfg.initial_capital    = 1000.0;
    cfg.buy_pct            = 0.50;
    cfg.kelly_fraction     = 0.25;
    cfg.tp_atr_mult        = 1.5;
    cfg.sl_atr_mult        = 1.0;
    cfg.max_hold_ticks     = 2000;
    cfg.max_drawdown_pct   = 0.20;
    cfg.max_orders_per_day = 200;
    cfg.report_every_n     = 0;      // hot-path içi raporlama kapalı
    cfg.verbose            = false;  // per-tick I/O yok
    cfg.slippage.model     = research::SlippageModel::FIXED_TICKS;

    research::PaperTrader trader(cfg, &notifier);
    const auto t0 = std::chrono::steady_clock::now();

    // Tick loop: measure_latency=false, progress I/O yok
    for (const research::TickRecord* p = mv.begin(); p != mv.end(); ++p) {
        if (g_stop.load(std::memory_order_acquire)) break;
        trader.on_tick(*p);
        if (trader.circuit_broken()) {
            std::cout << "\n[PaperTrader] Circuit Breaker — simülasyon durduruldu.\n";
            break;
        }
    }

    const double el = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    std::cout << "\n[PaperTrader] Tamamlandı.\n";
    trader.print_summary();

    research::BacktestResult r = trader.get_result();
    std::ostringstream extra;
    extra << std::fixed << std::setprecision(1)
          << "Toplam İşlem=" << (int)r.total_trades
          << " WinRate=" << r.win_rate * 100.0 << "%%"
          << " MaxDD=" << r.max_drawdown;
    notifier.send_paper_summary(trader.cum_pnl(), extra.str());

    print_backtest_result(r, el);
    return 0;
}

} // namespace kusak::app
