#pragma once
#include "serialspark/core.hpp"
#include <memory>

namespace ss {
// Both live and offline callers feed the same chronological RawEvents.
// data: append bytes; clock: apply assembly timeout; disconnect: finish stream.
// Output JSON: kind=frame, direction, status=ok/error/noise/incomplete,
// raw_hex, fields (object), errors (array), sources [{event,offset,length}],
// wall_ns, mono_ns. A source refers only to accepted TX or actual RX bytes.
class Protocol {
public:
    explicit Protocol(const Json& config);
    ~Protocol();
    Protocol(Protocol&&) noexcept;
    Protocol& operator=(Protocol&&) noexcept;
    std::vector<Json> feed(const RawEvent& event);
    std::vector<Json> finish(const std::string& reason);
    const Json& config() const;
    static void validate(const Json& config);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
