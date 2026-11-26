// FILE: src/app/research/research_cli.cpp
//
// Turn 2 : print_banner, print_help, CLI parse buraya tasindi.
// Turn 2 duzeltme: argv[1] mode parse da buraya tasindi.
//   opts.mode_str -> argv[1] raw string
//   opts.mode     -> ResearchMode enum eslemesi
// Sonraki turn'lerde bu dosyaya dokunulmayacak.

#include "app/research/research_cli.hpp"
#include <iostream>

namespace kusak::app {

void print_research_banner() {
    std::cout
        << "\n╔══════════════════════════════════════════════════════════════════╗\n"
           "║  KUSAK V9 — QUANT RESEARCH FRAMEWORK  v4.0 (Throughput Refactor) ║\n"
           "║  C++17 · AVX2 · WFO · HMM Rejim · Latency P99 · TCA            ║\n"
           "║  mmap I/O · zero-alloc hot-path · 10M+ tick/s                  ║\n"
           "╚══════════════════════════════════════════════════════════════════╝\n\n";
}

void print_research_help(const char* program_name) {
    std::cout
        << "Kullanım: " << program_name << " <mod> [seçenekler]\n\n"
        << "Modlar:\n"
        << "  --record                     Canlı WS verisini kaydet\n"
        << "  --replay   <dosya.ticks>     Replay (RegimeV2 + VPIN + isteğe bağlı monitor)\n"
        << "  --backtest <dosya.ticks>     Tek backtest\n"
        << "  --optimize <dosya.ticks>     Grid + Bayes optimizasyon\n"
        << "  --genetic  <dosya.ticks>     Genetik/Evrimsel optimizasyon\n"
        << "  --paper    <dosya.ticks>     Paper Trading simülasyonu\n"
        << "  --wfo      <dosya.ticks>     Walk-Forward Optimization\n"
        << "  --regime   <dosya.ticks>     RegimeDetectorV2 analiz raporu\n"
        << "  --monitor  <dosya.ticks>     Latency + Ters Seçim izleme\n\n"
        << "Seçenekler:\n"
        << "  --speed    <N>   Replay hız çarpanı (varsayılan 100)\n"
        << "  --output   <D>   Çıktı dizini (varsayılan data/)\n"
        << "  --workers  <N>   Optimizer iş parçacığı sayısı\n"
        << "  --bayes    <N>   Bayes iterasyon sayısı (varsayılan 50)\n"
        << "  --population <N> Genetic: popülasyon büyüklüğü (varsayılan 50)\n"
        << "  --generations <N>Genetic: nesil sayısı (varsayılan 30)\n"
        << "  --wfo-train <N>  WFO eğitim penceresi tick (varsayılan 100000)\n"
        << "  --wfo-test  <N>  WFO test penceresi tick   (varsayılan 25000)\n"
        << "  --wfo-step  <N>  WFO kaydırma adımı tick   (varsayılan 25000)\n"
        << "  --monitor        --replay ile birlikte: latency+TCA ölçümü ekler\n"
        << "  --slippage <model>  Slippage modeli: ticks|bps|volume|volatility\n"
        << "  --kelly <frac>   Kelly fraksiyonu (0.0-1.0, varsayılan kapalı)\n"
        << "  --buy-pct <pct>  Sermaye yüzdesi ile işlem (0.0-1.0)\n"
        << "  --cb-drawdown <p> Circuit breaker max drawdown yüzdesi (varsayılan 0.20)\n\n";
}

ResearchCliOptions parse_research_cli(int argc, char** argv) {
    ResearchCliOptions opts{};

    if (argc < 2) {
        opts.show_help = true;
        return opts;
    }

    // argv[1] -> mode_str + mode enum
    opts.mode_str = argv[1];

    if      (opts.mode_str == "--backtest")    opts.mode = ResearchMode::Backtest;
    else if (opts.mode_str == "--replay")      opts.mode = ResearchMode::Replay;
    else if (opts.mode_str == "--replay-step") { opts.mode = ResearchMode::Replay; opts.replay_step = true; }
    else if (opts.mode_str == "--paper")       opts.mode = ResearchMode::Paper;
    else if (opts.mode_str == "--optimize")   opts.mode = ResearchMode::Optimize;
    else if (opts.mode_str == "--genetic")    opts.mode = ResearchMode::Genetic;
    else if (opts.mode_str == "--wfo")        opts.mode = ResearchMode::WalkForward;
    else if (opts.mode_str == "--regime")     opts.mode = ResearchMode::RegimeReport;
    else if (opts.mode_str == "--record")     opts.mode = ResearchMode::Record;
    else if (opts.mode_str == "--monitor")    opts.mode = ResearchMode::Monitor;
    else                                      opts.mode = ResearchMode::Unknown;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if      (arg == "--speed"       && i+1<argc) opts.speed              = std::stod(argv[++i]);
        else if (arg == "--workers"     && i+1<argc) opts.workers            = std::stoi(argv[++i]);
        else if (arg == "--bayes"       && i+1<argc) opts.bayes_iterations   = std::stoi(argv[++i]);
        else if (arg == "--output"      && i+1<argc) opts.output_dir         = argv[++i];
        else if (arg == "--wfo-train"   && i+1<argc) opts.wfo_train          = std::stoull(argv[++i]);
        else if (arg == "--wfo-test"    && i+1<argc) opts.wfo_test           = std::stoull(argv[++i]);
        else if (arg == "--wfo-step"    && i+1<argc) opts.wfo_step           = std::stoull(argv[++i]);
        else if (arg == "--population"  && i+1<argc) opts.population         = std::stoi(argv[++i]);
        else if (arg == "--generations" && i+1<argc) opts.generations        = std::stoi(argv[++i]);
        else if (arg == "--slippage"    && i+1<argc) opts.slippage_model_str = argv[++i];
        else if (arg == "--kelly"       && i+1<argc) opts.kelly_frac         = std::stod(argv[++i]);
        else if (arg == "--buy-pct"     && i+1<argc) opts.buy_pct            = std::stod(argv[++i]);
        else if (arg == "--cb-drawdown" && i+1<argc) opts.cb_drawdown        = std::stod(argv[++i]);
        else if (arg == "--env"         && i+1<argc) opts.env_path           = argv[++i];
        else if (arg == "--monitor")                 opts.with_monitor       = true;
        else if (arg[0] != '-')                      opts.file               = arg;
    }

    return opts;
}

} // namespace kusak::app
