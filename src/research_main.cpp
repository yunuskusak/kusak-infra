// ============================================================================
// src/research_main.cpp — V13 research CLI surucusu
// Aktif modlar : --record, --replay, --replay-step, --monitor, --backtest
// Kapali modlar: --optimize, --genetic, --paper, --wfo, --regime
//                (ilgili runner'lar bu derlemede yok; acik hata mesaji verir)
// ============================================================================

#include <csignal>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "research_types.hpp"
#include "market_data_recorder.hpp"   // --record
#include "ws_client.hpp"              // --record

#include "app/research/research_cli.hpp"
#include "app/research/backtest_runner.hpp"
#include "app/research/replay_runner.hpp"

namespace fs       = std::filesystem;
namespace research = ::kusak::research;

std::atomic<bool> g_stop{false};   // replay_runner.cpp extern ile erisir
static void handle_signal(int) { g_stop.store(true, std::memory_order_release); }

int main(int argc, char* argv[]) {
    kusak::app::print_research_banner();

    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    if (argc < 2) { kusak::app::print_research_help(argv[0]); return 1; }

    auto opts = kusak::app::parse_research_cli(argc, argv);

    if (opts.show_help) { kusak::app::print_research_help(argv[0]); return 0; }

    const std::string& file   = opts.file;
    const std::string& output = opts.output_dir;

    [[maybe_unused]] auto need_file = [&](const char* m) -> bool {
        if (!file.empty()) return true;
        std::cerr << "Hata: " << m << " <dosya.ticks> gerekli\n";
        return false;
    };

    try {
        if (opts.mode == kusak::app::ResearchMode::Record) {
            // ── .env discovery ──────────────────────────────────────────────
            auto is_ws_ch = [](char c) noexcept {
                return c == ' ' || c == '\t' || c == '\n' ||
                       c == '\r' || c == '\f' || c == '\v';
            };
            auto ltrim = [&](std::string& s) {
                size_t i = 0;
                while (i < s.size() && is_ws_ch(s[i])) ++i;
                s.erase(0, i);
            };
            auto rtrim = [&](std::string& s) {
                size_t i = s.size();
                while (i > 0 && is_ws_ch(s[i - 1])) --i;
                s.erase(i);
            };
            auto trim = [&](std::string& s) { ltrim(s); rtrim(s); };

            auto unquote = [&](std::string& v) -> std::string& {
                trim(v);
                if (v.size() >= 2) {
                    const char q = v.front();
                    if ((q == '"' || q == '\'') && v.back() == q)
                        v = v.substr(1, v.size() - 2);
                }
                return v;
            };

            auto strip_inline_comment = [&](std::string& v) {
                std::string t = v; trim(t);
                if (!t.empty() && (t.front() == '"' || t.front() == '\'')) {
                    v = unquote(v); return;
                }
                auto pos = v.find('#');
                if (pos != std::string::npos) v = v.substr(0, pos);
                trim(v);
            };

            fs::path env_file_path;
            {
                fs::path dir = fs::current_path();
                for (int depth = 0; depth < 6; ++depth) {
                    fs::path candidate = dir / ".env";
                    if (fs::exists(candidate) && fs::is_regular_file(candidate)) {
                        env_file_path = candidate; break;
                    }
                    if (!dir.has_parent_path()) break;
                    dir = dir.parent_path();
                }
            }

            if (env_file_path.empty()) {
                std::cerr << "[record] HATA: .env bulunamadi.\n";
                return 1;
            }
            std::cout << "[record] .env bulundu: " << env_file_path.string() << "\n";

            std::string api_key, api_secret, symbol_env;
            {
                std::ifstream fin(env_file_path.string());
                if (!fin) {
                    std::cerr << "[record] HATA: .env acilamadi.\n";
                    return 1;
                }
                std::string line;
                while (std::getline(fin, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    trim(line);
                    if (line.empty() || line[0] == '#') continue;
                    static const std::string kExport = "export";
                    if (line.size() > kExport.size() &&
                        line.compare(0, kExport.size(), kExport) == 0 &&
                        is_ws_ch(line[kExport.size()])) {
                        line = line.substr(kExport.size()); trim(line);
                    }
                    const auto eq = line.find('=');
                    if (eq == std::string::npos) continue;
                    std::string key = line.substr(0, eq);
                    std::string val = line.substr(eq + 1);
                    trim(key); strip_inline_comment(val); unquote(val);
                    if      (key == "API_KEY")    api_key    = val;
                    else if (key == "API_SECRET") api_secret = val;
                    else if (key == "SYMBOL")     symbol_env = val;
                }
            }

            auto mask = [](const std::string& s) -> std::string {
                if (s.empty()) return "<empty>";
                if (s.size() <= 6) return std::string(s.size(), '*');
                return s.substr(0, 3) + "..." + s.substr(s.size() - 3);
            };

            if (api_key.empty() || api_secret.empty()) {
                std::cerr << "[record] HATA: API_KEY / API_SECRET eksik."
                          << "  API_KEY=" << mask(api_key)
                          << "  API_SECRET=" << mask(api_secret) << "\n";
                return 1;
            }
            std::cout << "[record] .env yuklendi:"
                      << "  API_KEY=" << mask(api_key)
                      << "  API_SECRET=" << mask(api_secret) << "\n";

            const std::string rec_symbol = symbol_env.empty() ? "BTCUSDT" : symbol_env;
            research::MarketDataRecorder::Config rcfg;
            rcfg.base_dir        = (fs::path(output) / "ticks").string();
            rcfg.symbol          = rec_symbol;
            rcfg.max_file_bytes  = 1024ULL * 1024 * 1024;
            // [FIX-RECORD-ISOLATE] flush_on_rotate artik yok -- yerine
            // zaman-araligina dayali fsync_interval_ms geldi (varsayilan
            // 10s). Kayit oturumu icin veri kaybini en aza indirmek adina
            // biraz daha sik (5s) fsync yapiyoruz; varsayilani da birakabilirdik.
            rcfg.fsync_interval_ms = 5'000;

            try {
                fs::create_directories(rcfg.base_dir);
                const fs::path probe = fs::path(rcfg.base_dir) / ".probe_write";
                { std::ofstream pf(probe.string(), std::ios::out | std::ios::trunc);
                  if (!pf) {
                      std::cerr << "[record] HATA: yazma izni yok: " << rcfg.base_dir << "\n";
                      return 1;
                  }
                  pf << "ok\n"; }
                fs::remove(probe);
            } catch (const std::exception& ex) {
                std::cerr << "[record] HATA: dizin hazirlanamadi: " << ex.what() << "\n";
                return 1;
            }

            // [FIX-STACK-OVERFLOW] MarketDataRecorder ~334 MB (SpscRing<262144>
            // queue_ degeri-turunden gomulu) -- stack'te yerel degisken olarak
            // tanimlamak ANINDA stack tasmasi (SIGSEGV) yaratiyordu. Heap'e
            // tasindi, mantik degismedi.
            auto recorder = std::make_unique<research::MarketDataRecorder>(rcfg);
            recorder->start();
            std::cout << "[record] Recorder basladi."
                      << "  Output=" << rcfg.base_dir
                      << "  Symbol=" << rcfg.symbol << "\n";

            // [FIX-SCALE-2] ws_client.hpp'deki DepthUpdate/BookTicker/AggTrade
            // yapilari V13'te int64_t x1e8 sabit-nokta olarak degistirildi
            // (bkz. canli motorda cozulen ayni sinif hata, [FIX-SCALE-1]).
            // MarketDataRecorder ise hala GERCEK ondalikli double bekliyor.
            // DepthUpdate icin bu bir DERLEME HATASI olarak ortaya cikiyordu
            // (int64_t[]* -> double* donusumu yok), ama BookTicker/AggTrade
            // icin sessiz bir veri bozulmasiydi (int64_t->double ORTUK
            // donusum derleyici hatasi vermez, deger 100 milyon kat yanlis
            // kaydedilirdi) -- ikisini de burada duzeltiyoruz.
            auto on_depth = [&](const kusak::ws::DepthUpdate& d) {
                double bid_p[20], bid_q[20], ask_p[20], ask_q[20];
                const uint32_t bn = static_cast<uint32_t>(d.bid_n);
                const uint32_t an = static_cast<uint32_t>(d.ask_n);
                for (uint32_t i = 0; i < bn && i < 20; ++i) {
                    bid_p[i] = static_cast<double>(d.bid_prices[i]) / 1e8;
                    bid_q[i] = static_cast<double>(d.bid_qtys[i])   / 1e8;
                }
                for (uint32_t i = 0; i < an && i < 20; ++i) {
                    ask_p[i] = static_cast<double>(d.ask_prices[i]) / 1e8;
                    ask_q[i] = static_cast<double>(d.ask_qtys[i])   / 1e8;
                }
                recorder->record_depth(
                    bid_p, bid_q, bn,
                    ask_p, ask_q, an,
                    d.ts_exchange);
            };
            auto on_ticker = [&](const kusak::ws::BookTicker& t) {
                recorder->record_book_ticker(
                    static_cast<double>(t.best_bid)     / 1e8,
                    static_cast<double>(t.best_bid_qty) / 1e8,
                    static_cast<double>(t.best_ask)     / 1e8,
                    static_cast<double>(t.best_ask_qty) / 1e8,
                    t.ts_exchange);
            };
            auto on_trade = [&](const kusak::ws::AggTrade& t) {
                recorder->record_trade(
                    static_cast<double>(t.price) / 1e8,
                    static_cast<double>(t.qty)   / 1e8,
                    t.buyer_is_maker, t.ts_exchange);
            };


            kusak::ws::BinanceWsClient::Config ws_cfg;
            ws_cfg.symbol        = rec_symbol;
            ws_cfg.enable_depth  = true;
            ws_cfg.enable_ticker = true;
            ws_cfg.enable_trade  = true;
            ws_cfg.verbose       = false;

            kusak::ws::BinanceWsClient ws_client(
                ws_cfg,
                std::move(on_depth),
                std::move(on_ticker),
                std::move(on_trade));

            std::cout << "[record] Binance stream baglaniyor... (Ctrl+C ile dur)\n";
            ws_client.run(g_stop);

            std::cout << "[record] Ctrl+C algilandi, recorder durduruluyor...\n";
            try { recorder->stop(); }
            catch (const std::exception& ex) {
                std::cerr << "[record] UYARI: recorder->stop() istisna: " << ex.what() << "\n";
            }
            std::cout << "[record] Recorder durdu.\n";
            return 0;
        }
#ifndef KUSAK_RECORD_ONLY
        else if (opts.mode == kusak::app::ResearchMode::Replay)   { if (!need_file("--replay"))   return 1; return kusak::app::run_replay(opts); }
        else if (opts.mode == kusak::app::ResearchMode::Backtest) { if (!need_file("--backtest")) return 1; return kusak::app::run_backtest(opts); }
        else if (opts.mode == kusak::app::ResearchMode::Monitor)  { if (!need_file("--monitor"))  return 1; return kusak::app::run_monitor(opts); }
        else if (opts.mode == kusak::app::ResearchMode::Optimize     ||
                 opts.mode == kusak::app::ResearchMode::Genetic      ||
                 opts.mode == kusak::app::ResearchMode::Paper        ||
                 opts.mode == kusak::app::ResearchMode::WalkForward  ||
                 opts.mode == kusak::app::ResearchMode::RegimeReport) {
            std::cerr << "[research] HATA: " << opts.mode_str
                      << " modu bu derlemede kapali (yalnizca --record/--replay/--monitor/--backtest).\n";
            return 1;
        }
#else
        else if (opts.mode == kusak::app::ResearchMode::Replay   ||
                 opts.mode == kusak::app::ResearchMode::Backtest ||
                 opts.mode == kusak::app::ResearchMode::Monitor  ||
                 opts.mode == kusak::app::ResearchMode::Optimize ||
                 opts.mode == kusak::app::ResearchMode::Genetic  ||
                 opts.mode == kusak::app::ResearchMode::Paper    ||
                 opts.mode == kusak::app::ResearchMode::WalkForward ||
                 opts.mode == kusak::app::ResearchMode::RegimeReport) {
            std::cerr << "[research_record] HATA: bu mod yalnizca tam 'research' hedefinde mevcut.\n";
            return 1;
        }
#endif
        else { std::cerr << "Bilinmeyen mod\n"; kusak::app::print_research_help(argv[0]); return 1; }
    }
    catch (const std::exception& e) { std::cerr << "\nKRITIK HATA: " << e.what() << "\n"; return 1; }
    catch (...)                     { std::cerr << "\nKRITIK HATA: bilinmeyen istisna\n"; return 1; }
}
