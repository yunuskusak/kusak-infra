#pragma once
// DOSYA YOLU: include/app/research/replay_runner.hpp

#include "app/research/research_options.hpp"

namespace kusak::app {

int run_replay (const ResearchCliOptions& opts);
int run_monitor(const ResearchCliOptions& opts);

} // namespace kusak::app
