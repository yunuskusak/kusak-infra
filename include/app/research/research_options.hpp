// FILE: include/app/research/research_options.hpp
#pragma once

#include <cstdint>
#include <string>

namespace kusak::app {

enum class ResearchMode {
    Backtest,
    Replay,
    Paper,
    Optimize,
    Genetic,
    WalkForward,
    RegimeReport,
    Record,
    Monitor,
    Unknown
};

struct ResearchCliOptions {
    ResearchMode  mode                = ResearchMode::Unknown;
    std::string   mode_str;                              // parse sonrasi sadece hata mesajinda kullanilir
    bool          replay_step         = false;           // --replay-step icin; dispatch string'e bagimli degil
    std::string   file;
    std::string   output_dir          = "data/";
    std::string   env_path            = ".env";
    std::string   slippage_model_str  = "fixed_bps";
    double        speed               = 100.0;
    int           workers             = 4;
    int           bayes_iterations    = 50;
    int           population          = 50;
    int           generations         = 30;
    bool          with_monitor        = false;
    bool          show_help           = false;
    bool          show_banner         = true;
    double        kelly_frac          = 0.0;
    double        buy_pct             = 0.95;
    double        cb_drawdown         = 0.0;
    std::uint64_t wfo_train           = 100000;
    std::uint64_t wfo_test            =  25000;
    std::uint64_t wfo_step            =  25000;
};

} // namespace kusak::app
