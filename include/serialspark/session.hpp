#pragma once
#include "serialspark/serial.hpp"
#include <deque>
#include <functional>

namespace ss {
class Output {
public:
    Output(std::string format="human",std::string display="both",const std::string& path="",bool live=false);
    ~Output();
    void emit(const Json& json);
    void display(const std::string& value) {display_=value;}
    void raw(std::span<const std::uint8_t> bytes);
    void flush();
    void finish();
private:
    void enqueue(std::string text);
    int fd_=1,old_flags_=-1;
    bool owned_=false,live_=false;
    std::string format_,display_;
    std::deque<std::string> queue_;
    std::size_t queued_=0,offset_=0;
    std::int64_t start_=0;
};
struct SessionOptions {
    SerialConfig config;
    std::string mode="send",format="human",display="both",newline="none";
    std::string protocol,record,file;
    Bytes payload,reply_pattern;
    bool stdin_source=false,wait_reply=false,drain=false;
    int write_timeout_ms=2000,reply_timeout_ms=1000;
    std::int64_t duration_ms=0;
    std::uint64_t rotate_bytes=1024ull*1024*1024,quota_bytes=0;
};
int run_session(const SessionOptions& options);
}
