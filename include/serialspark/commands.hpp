#pragma once
#include <CLI/CLI.hpp>

namespace ss {
void add_analysis_commands(CLI::App& app,int& result);
void validate_field_filter(const std::string& expression);
}
