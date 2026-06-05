#pragma once
#include "serialspark/core.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <sqlite3.h>

namespace ss {
class Recorder {
public:
    Recorder(const std::filesystem::path& path,const Json& metadata,
             std::uint64_t rotate_bytes=1024ull*1024*1024,std::uint64_t quota_bytes=0,
             std::size_t queue_limit=16*1024*1024);
    ~Recorder();
    void append(const RawEvent& event);
    void check();
    void close();
    std::int64_t persisted() const {return persisted_;}
private:
    void work();
    void open_segment(const std::filesystem::path& path);
    void finish_segment(bool clean);
    void commit();
    std::filesystem::path base_, current_;
    Json metadata_;
    sqlite3* db_=nullptr;
    sqlite3_stmt* insert_=nullptr;
    std::uint64_t rotate_bytes_,quota_bytes_,finished_bytes_=0;
    std::size_t part_=0,queue_limit_,queued_bytes_=0;
    std::atomic<std::int64_t> persisted_{0};
    std::int64_t pending_=0;
    std::int64_t last_appended_=0;
    bool transaction_=false;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<RawEvent> queue_;
    bool stopping_=false;
    std::string error_;
    std::thread worker_;
};
void read_log(const std::filesystem::path& path,const std::function<void(const RawEvent&)>& visit);
Json log_info(const std::filesystem::path& path);
}
