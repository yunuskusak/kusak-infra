#pragma once
// DOSYA YOLU: include/app/research/optimize_runner.hpp

#include "app/research/research_options.hpp"

namespace kusak::app {

int run_optimize(const ResearchCliOptions& opts);
int run_genetic (const ResearchCliOptions& opts);
int run_wfo     (const ResearchCliOptions& opts);

} // namespace kusak::app
