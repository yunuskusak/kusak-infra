// ============================================================================
// KUSAK V13 — Replay Harness
//
// [FIX-REPLAY-1] Bu arac, market_data_recorder.hpp ile kaydedilmis .tick
// dosyalarini okuyup, strategy_core.hpp'deki (main.cpp ile BIREBIR AYNI)
// karar mantigini (process_book_ticker / process_agg_trade / maybe_send_order)
// dry_run modunda calistirir. Amac: "canli botun VPIN/OBI/Kelly mantigi
// gercek piyasa verisinde nasil davranir" sorusuna, ayri/paralel bir
// yeniden-uygulama (eski paper_trader.hpp) DEGIL, gercekten canlida
// calisacak kodun kendisiyle cevap vermek.
//
// Kullanim:
//   ./replay_harness <dosya.tick> [symbol] [obi_threshold] [kelly_fraction]
//
// Ag baglantisi GEREKMEZ -- filter_validator, ExchangeInfoManager'in canli
// cektigi degerler yerine burada makul varsayilanlarla elle dolduruluyor
// (asagida FILL_FILTER_CACHE bolumune bakin; gercek sembol icin gercek
// tick/step/minNotional degerlerini girmek ISTEGE BAGLI ama daha dogru
// sonuc verir).
// ============================================================================
#include "strategy_core.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

// [FIX-REPLAY-3] main.cpp'de tanimli olan g_tsc_ns_mult burada da tanimlanmali
// (infra/time.hpp bunu extern bekliyor) -- ve kullanmadan once calibrate_tsc()
// ile doldurulmali, aksi halde tum sure/zaman damgasi hesaplamalari (0 * x = 0)
// bozuk cikar.
uint64_t g_tsc_ns_mult = 0;

using kusak::research::DiskRecord;
using kusak::research::TickRecord;
using kusak::research::from_disk;
using kusak::TickType;
using kusak::FileHeader;
using kusak::TICK_FILE_MAGIC;

int main(int argc, char** argv) {
    // [FIX-REPLAY-3] TSC kalibrasyonu -- yapilmazsa g_tsc_ns_mult=0 kalir ve
    // tsc_ns() hep 0 doner (throttle/cooldown/latency hesaplari bozulur).
    {
        auto cal = calibrate_tsc();
        g_tsc_ns_mult = cal.ns_mult;
    }

    if (argc < 2) {
        std::cerr << "Kullanim: " << argv[0]
                  << " <dosya.tick> [symbol=PENDLEUSDT] [obi_thr=0.35] [kelly=0.0]\n";
        return 1;
    }
    const std::string path   = argv[1];
    const std::string symbol = (argc >= 3) ? argv[2] : "PENDLEUSDT";
    const double obi_thr     = (argc >= 4) ? std::atof(argv[3]) : 0.35;
    const double kelly_frac  = (argc >= 5) ? std::atof(argv[4]) : 0.0;

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::cerr << "HATA: dosya acilamadi: " << path << "\n";
        return 1;
    }

    FileHeader hdr{};
    if (std::fread(&hdr, sizeof(hdr), 1, f) != 1) {
        std::cerr << "HATA: FileHeader okunamadi (dosya cok kucuk?)\n";
        std::fclose(f);
        return 1;
    }
    if (hdr.magic != TICK_FILE_MAGIC) {
        std::cerr << "HATA: gecersiz dosya (magic uyusmuyor) -- gercek bir "
                     ".tick dosyasi mi?\n";
        std::fclose(f);
        return 1;
    }

    std::cout << "[replay] Dosya: "        << path
              << "  Sembol(header): "      << hdr.symbol
              << "  Kayit sayisi: "        << hdr.record_count << "\n";

    // ── StrategyContext kurulumu (dry_run, ag YOK) ──────────────────────────
    kusak::config::KusakConfig cfg;
    kusak::config::ConfigManager config_mgr("kusak_config.json");
    config_mgr.load(cfg);
    cfg.symbol          = symbol;
    cfg.dry_run         = true;
    cfg.testnet         = true;   // sadece bilgi amacli, replay'de kullanilmiyor
    cfg.kelly_fraction  = kelly_frac;
    cfg.obi_threshold   = obi_thr;
    cfg.initial_capital = 10000.0;

    // [FIX-STACK-OVERFLOW-2] StrategyContext ~9.64 MB (order_channel/market_ring
    // gibi buyuk SPSC halkalari degeri-turunden gomulu) -- main.cpp bunu zaten
    // heap'te (std::make_unique) olusturuyor, stack'te degil. Ayni deseni
    // burada da uyguluyoruz; aksi halde ana thread'in varsayilan 8MB stack
    // limiti asilir ve fonksiyon girisinde ANINDA segfault olusur.
    auto ctx_ptr = std::make_unique<StrategyContext>(cfg);
    StrategyContext& ctx = *ctx_ptr;
    ctx.current_capital = cfg.initial_capital;

    // [FIX-REPLAY-2] filter_validator'i AGSIZ elle doldur -- gercek
    // ExchangeInfoManager cagrisi yapilamaz (offline replay). Degerler
    // PENDLEUSDT testnet'ten daha once gozlemlenen gercek degerlerdir;
    // farkli bir sembol icin bunlari kendi degerlerinle degistir.
    {
        kusak::exchange::SymbolFilterCache fc;
        fc.symbol       = symbol;
        fc.tick_size    = 0.001;
        fc.step_size    = 0.1;
        fc.min_notional = 5.0;
        fc.min_qty      = 0.1;
        fc.max_qty      = 9'000'000.0;
        fc.min_price    = 0.0001;
        fc.max_price    = 1'000'000.0;
        fc.valid        = true;
        ctx.filter_validator.update_filter(fc);
    }

    std::cout << "[replay] StrategyContext hazir. dry_run=EVET obi_thr=" << obi_thr
              << " kelly_fraction=" << kelly_frac << "\n";

    // ── Kayitlari sirayla oku ve besle ──────────────────────────────────────
    uint64_t n_book_ticker = 0, n_trade = 0, n_depth_skipped = 0, n_unknown = 0;
    DiskRecord drec;
    while (std::fread(&drec, sizeof(drec), 1, f) == 1) {
        TickRecord tr{};
        from_disk(drec, tr);
        ctx.mock_time_ns = tr.ts_exchange;

        switch (tr.type) {
        case TickType::BOOK_TICKER: {
            if (tr.bid_count == 0 || tr.ask_count == 0) break;
            kusak::ws::BookTicker bt{};
            bt.ts_exchange  = tr.ts_exchange;
            bt.best_bid     = static_cast<int64_t>(tr.bids[0].price    * 1e8);
            bt.best_bid_qty = static_cast<int64_t>(tr.bids[0].quantity * 1e8);
            bt.best_ask     = static_cast<int64_t>(tr.asks[0].price    * 1e8);
            bt.best_ask_qty = static_cast<int64_t>(tr.asks[0].quantity * 1e8);
            process_book_ticker(ctx, bt);
            ++n_book_ticker;
            break;
        }
        case TickType::TRADE:
        case TickType::AGG_TRADE: {
            kusak::ws::AggTrade at{};
            at.ts_exchange     = tr.ts_exchange;
            at.price           = static_cast<int64_t>(tr.trade_price * 1e8);
            at.qty             = static_cast<int64_t>(tr.trade_qty   * 1e8);
            at.buyer_is_maker  = tr.trade_buyer;
            process_agg_trade(ctx, at);
            ++n_trade;
            break;
        }
        case TickType::DEPTH:
            // [NOT] main.cpp'nin canli stratejisi (maybe_send_order) sadece
            // top-of-book (last_bid/last_ask, BOOK_TICKER event'inden) okur,
            // tam derinlik (DEPTH) kullanmiyor -- bu yuzden burada atlaniyor.
            ++n_depth_skipped;
            break;
        default:
            ++n_unknown;
            break;
        }

        enforce_timeouts(ctx, ctx.flat_order_map, ctx.order_channel, symbol);
    }
    std::fclose(f);

    std::cout << "\n[replay] Oynatma bitti.\n"
              << "  BOOK_TICKER islendi : " << n_book_ticker << "\n"
              << "  TRADE islendi       : " << n_trade << "\n"
              << "  DEPTH atlandi       : " << n_depth_skipped << "\n"
              << "  Bilinmeyen          : " << n_unknown << "\n\n";

    // Mevcut telemetry/adverse/latency raporlarini oldugu gibi kullan --
    // main.cpp'nin kapanista bastigi AYNI raporlar.
    std::cout << "=== STRATEJI RAPORU (dry_run replay) ===\n";
    ctx.telemetry.print_summary();
    ctx.latency.dump_report();
    ctx.adverse.print_summary();

    return 0;
}