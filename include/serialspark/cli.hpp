#pragma once
#include <cstdint>
#include <string>

namespace ss {
// Each invocation builds fresh CLI state; arguments are never interpreted by a shell.
int run_cli(int argc,char** argv);
std::int64_t milliseconds(const std::string& input,bool allow_zero=false);
}
