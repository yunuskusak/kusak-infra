#pragma once
// DOSYA YOLU: include/app/research/research_cli.hpp

#include "app/research/research_options.hpp"

namespace kusak::app {

ResearchCliOptions parse_research_cli(int argc, char** argv);
void               print_research_banner();
void               print_research_help(const char* program_name);

} // namespace kusak::app
