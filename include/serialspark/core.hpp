#pragma once
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace ss {
using Json = nlohmann::json;
using Bytes = std::vector<std::uint8_t>;
enum class Exit : int { ok=0, usage=2, connect=3, io=4, partial=5, timeout=6, record=7, protocol=8, cancelled=130 };
class Error : public std::runtime_error {
public:
    Exit code;
    Error(Exit code, const std::string& message) : std::runtime_error(message), code(code) {}
};
Bytes unhex(const std::string& text);
std::string hex(std::span<const std::uint8_t> bytes);
std::string escaped(std::span<const std::uint8_t> bytes);
std::int64_t wall_ns();
std::int64_t mono_ns();
Json read_json(const std::filesystem::path& path);
void write_json(const std::filesystem::path& path, const Json& json, bool replace=false);
struct RawEvent {
    std::int64_t seq=0;
    std::int64_t wall=0;
    std::int64_t mono=0;
    std::string kind="data";
    std::string direction;
    Bytes bytes;
    Json detail=Json::object();
};
Json event_json(const RawEvent& event);
RawEvent event_from_json(const Json& json);
}
