#pragma once
#include "serialspark/core.hpp"

namespace ss {
std::filesystem::path config_dir(const std::string& category);
std::filesystem::path named_path(const std::string& category,const std::string& name);
void save_named(const std::string& category,const std::string& name,const Json& json,bool replace);
Json profile_config(const Json& json);
Json list_named(const std::string& category);
}
