#include "serialspark/record.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <memory>
#include <set>

namespace ss {
namespace {
void sql(sqlite3* db,const std::string& command) {
    char* error=nullptr;
    if(sqlite3_exec(db,command.c_str(),nullptr,nullptr,&error)!=SQLITE_OK) {
        std::string message=error?error:sqlite3_errmsg(db);sqlite3_free(error);
        throw Error(Exit::record,"通信记录数据库错误: "+message);
    }
}
void meta(sqlite3* db,const std::string& key,const Json& value) {
    sqlite3_stmt* raw=nullptr;
    if(sqlite3_prepare_v2(db,"INSERT OR REPLACE INTO metadata(key,value) VALUES(?,?)",-1,&raw,nullptr)!=SQLITE_OK) throw Error(Exit::record,sqlite3_errmsg(db));
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> statement(raw,sqlite3_finalize);
    auto text=value.dump();
    sqlite3_bind_text(raw,1,key.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(raw,2,text.c_str(),-1,SQLITE_TRANSIENT);
    if(sqlite3_step(raw)!=SQLITE_DONE) throw Error(Exit::record,sqlite3_errmsg(db));
}
std::uint64_t size_of(const std::filesystem::path& p) {
    std::error_code ec;auto n=std::filesystem::file_size(p,ec);return ec?0:n;
}
using Database=std::unique_ptr<sqlite3,decltype(&sqlite3_close)>;
Database readonly(const std::filesystem::path& path) {
    sqlite3* raw=nullptr;
    int rc=sqlite3_open_v2(path.c_str(),&raw,SQLITE_OPEN_READONLY,nullptr);
    Database db(raw,sqlite3_close);
    if(rc!=SQLITE_OK) throw Error(Exit::record,"无法读取记录: "+path.string());
    sql(raw,"PRAGMA query_only=ON; PRAGMA cache_size=-32768;");return db;
}
Json metadata(sqlite3* db) {
    sqlite3_stmt* raw=nullptr;
    if(sqlite3_prepare_v2(db,"SELECT key,value FROM metadata",-1,&raw,nullptr)!=SQLITE_OK) throw Error(Exit::record,"不是 SerialSpark 记录文件");
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> statement(raw,sqlite3_finalize);
    Json j=Json::object();int rc;
    while((rc=sqlite3_step(raw))==SQLITE_ROW) {
        auto key=reinterpret_cast<const char*>(sqlite3_column_text(raw,0));
        auto value=reinterpret_cast<const char*>(sqlite3_column_text(raw,1));
        if(!key||!value) throw Error(Exit::record,"记录元数据含空值");
        if(sqlite3_column_bytes(raw,1)>1024*1024) throw Error(Exit::record,"记录元数据超过上限");
        try {j[key]=Json::parse(value);}catch(const Json::exception&){throw Error(Exit::record,"记录元数据 JSON 损坏");}
    }
    if(rc!=SQLITE_DONE) throw Error(Exit::record,sqlite3_errmsg(db));
    if(!j.contains("schema_version")||!j["schema_version"].is_number_integer()||j["schema_version"]!=1) throw Error(Exit::record,"记录格式版本不支持");
    return j;
}
template<class F> void segments(const std::filesystem::path& first,F&& callback) {
    auto path=first;std::set<std::string> seen;
    for(std::size_t n=0;n<10000;++n) {
        if(!seen.insert(path.lexically_normal().string()).second) throw Error(Exit::record,"记录分段链接循环");
        auto db=readonly(path);auto m=metadata(db.get());callback(db.get(),m,path);
        auto next=m.value("next_segment",std::string{});
        if(next.empty()) return;
        if(std::filesystem::path(next).filename()!=next) throw Error(Exit::record,"记录分段必须在同一目录");
        path=path.parent_path()/next;
    }
    throw Error(Exit::record,"记录分段超过 10000 个上限");
}
}
Recorder::Recorder(const std::filesystem::path& path,const Json& metadata,std::uint64_t rotate,std::uint64_t quota,std::size_t limit)
    :base_(path),metadata_(metadata),rotate_bytes_(rotate),quota_bytes_(quota),queue_limit_(limit) {
    if(rotate_bytes_<65536) throw Error(Exit::usage,"记录分段大小至少为 65536 字节");
    if(!queue_limit_) throw Error(Exit::usage,"记录队列上限必须大于零");
    try {open_segment(base_);worker_=std::thread([this]{work();});}
    catch(...) {if(insert_)sqlite3_finalize(insert_);if(db_)sqlite3_close(db_);throw;}
}
Recorder::~Recorder() {try{close();}catch(...){} }
void Recorder::open_segment(const std::filesystem::path& path) {
    int fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    if(fd<0) throw Error(Exit::record,"无法创建记录（不覆盖已有文件）: "+path.string());
    ::close(fd);current_=path;
    if(sqlite3_open_v2(path.c_str(),&db_,SQLITE_OPEN_READWRITE,nullptr)!=SQLITE_OK) throw Error(Exit::record,"无法打开记录数据库");
    sql(db_,"PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA cache_size=-32768; PRAGMA busy_timeout=1000;"
            "CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
            "CREATE TABLE events(seq INTEGER PRIMARY KEY,wall_ns INTEGER NOT NULL,mono_ns INTEGER NOT NULL,kind TEXT NOT NULL,direction TEXT NOT NULL,bytes BLOB NOT NULL,detail TEXT NOT NULL);"
            "CREATE INDEX event_direction_time ON events(direction,wall_ns);");
    meta(db_,"schema_version",1);meta(db_,"session",metadata_);meta(db_,"segment",part_);meta(db_,"clean_shutdown",false);meta(db_,"persisted_seq",persisted_.load());
    if(sqlite3_prepare_v2(db_,"INSERT INTO events VALUES(?,?,?,?,?,?,?)",-1,&insert_,nullptr)!=SQLITE_OK) throw Error(Exit::record,sqlite3_errmsg(db_));
}
void Recorder::append(const RawEvent& event) {
    std::lock_guard lock(mutex_);
    if(!error_.empty()) throw Error(Exit::record,error_+"；最后持久化事件="+std::to_string(persisted_.load()));
    if(stopping_) throw Error(Exit::record,"记录已关闭");
    auto size=event.bytes.size()+event.detail.dump().size()+256;
    if(size>queue_limit_||queued_bytes_>queue_limit_-size) {
        error_="记录队列达到上限，停止采集";stopping_=true;cv_.notify_one();
        throw Error(Exit::record,error_+"；最后持久化事件="+std::to_string(persisted_.load()));
    }
    queue_.push_back(event);queued_bytes_+=size;cv_.notify_one();
}
void Recorder::check() {
    std::lock_guard lock(mutex_);
    if(!error_.empty()) throw Error(Exit::record,error_+"；最后持久化事件="+std::to_string(persisted_.load()));
}
void Recorder::commit() {
    if(!transaction_) return;
    meta(db_,"persisted_seq",pending_);sql(db_,"COMMIT");transaction_=false;persisted_=pending_;
    auto current_size=size_of(current_)+size_of(current_.string()+"-wal");
    if(quota_bytes_&&finished_bytes_+current_size>=quota_bytes_) throw Error(Exit::record,"记录磁盘配额达到上限，停止采集");
    if(current_size>=rotate_bytes_) {
        auto next=base_;next+=".part"+std::to_string(++part_)+".ssr";
        // A linked missing segment is intentionally detectable after a failed rotation.
        meta(db_,"next_segment",next.filename().string());finish_segment(true);
        finished_bytes_+=size_of(current_);open_segment(next);
    }
}
void Recorder::finish_segment(bool clean) {
    if(!db_) return;
    meta(db_,"clean_shutdown",clean);
    if(insert_) {sqlite3_finalize(insert_);insert_=nullptr;}
    sql(db_,"PRAGMA wal_checkpoint(TRUNCATE)");
    if(sqlite3_close(db_)!=SQLITE_OK) throw Error(Exit::record,"记录数据库关闭失败");
    db_=nullptr;
}
void Recorder::work() {
    try {
        auto deadline=mono_ns()+100000000;
        std::size_t batch_bytes=0;
        while(true) {
            RawEvent e;bool has=false,stop=false;
            {
                std::unique_lock lock(mutex_);
                cv_.wait_for(lock,std::chrono::milliseconds(100),[&]{return stopping_||!queue_.empty();});
                stop=stopping_;
                if(!queue_.empty()) {e=std::move(queue_.front());queue_.pop_front();queued_bytes_-=e.bytes.size()+e.detail.dump().size()+256;has=true;}
            }
            if(has) {
                if(!transaction_) {sql(db_,"BEGIN IMMEDIATE");transaction_=true;}
                sqlite3_reset(insert_);sqlite3_clear_bindings(insert_);
                sqlite3_bind_int64(insert_,1,e.seq);sqlite3_bind_int64(insert_,2,e.wall);sqlite3_bind_int64(insert_,3,e.mono);
                sqlite3_bind_text(insert_,4,e.kind.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(insert_,5,e.direction.c_str(),-1,SQLITE_TRANSIENT);
                if(e.bytes.empty()) sqlite3_bind_zeroblob(insert_,6,0);else sqlite3_bind_blob(insert_,6,e.bytes.data(),e.bytes.size(),SQLITE_TRANSIENT);
                auto detail=e.detail.dump();sqlite3_bind_text(insert_,7,detail.c_str(),-1,SQLITE_TRANSIENT);
                if(sqlite3_step(insert_)!=SQLITE_DONE) throw Error(Exit::record,sqlite3_errmsg(db_));
                pending_=e.seq;batch_bytes+=e.bytes.size()+detail.size()+256;
            }
            if(mono_ns()>=deadline||batch_bytes>=256*1024||(!has&&stop)) {commit();batch_bytes=0;deadline=mono_ns()+100000000;}
            if(!has&&stop) break;
        }
        bool clean;
        {std::lock_guard lock(mutex_);clean=error_.empty();}
        finish_segment(clean);
    } catch(const std::exception& e) {
        {std::lock_guard lock(mutex_);error_="记录失败: "+std::string(e.what());stopping_=true;queue_.clear();queued_bytes_=0;}
        if(db_) {
            if(transaction_) sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);
            try{finish_segment(false);}catch(...) {if(insert_){sqlite3_finalize(insert_);insert_=nullptr;}sqlite3_close(db_);db_=nullptr;}
        }
    }
}
void Recorder::close() {
    {std::lock_guard lock(mutex_);stopping_=true;cv_.notify_one();}
    if(worker_.joinable()) worker_.join();
    check();
}
void read_log(const std::filesystem::path& path,const std::function<void(const RawEvent&)>& visit) {
    std::int64_t previous=0;
    segments(path,[&](sqlite3* db,const Json&,const auto&) {
        sqlite3_stmt* raw=nullptr;
        if(sqlite3_prepare_v2(db,"SELECT seq,wall_ns,mono_ns,kind,direction,bytes,detail FROM events ORDER BY seq",-1,&raw,nullptr)!=SQLITE_OK) throw Error(Exit::record,sqlite3_errmsg(db));
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> statement(raw,sqlite3_finalize);
        int rc;
        while((rc=sqlite3_step(raw))==SQLITE_ROW) {
            auto text=[&](int column){auto p=reinterpret_cast<const char*>(sqlite3_column_text(raw,column));return std::string(p?p:"");};
            RawEvent e;e.seq=sqlite3_column_int64(raw,0);e.wall=sqlite3_column_int64(raw,1);e.mono=sqlite3_column_int64(raw,2);e.kind=text(3);e.direction=text(4);
            int bytes=sqlite3_column_bytes(raw,5);
            if(bytes>65536||sqlite3_column_bytes(raw,6)>1024*1024) throw Error(Exit::record,"记录事件超过安全读取上限");
            auto p=static_cast<const std::uint8_t*>(sqlite3_column_blob(raw,5));if(bytes)e.bytes.assign(p,p+bytes);
            try {e.detail=Json::parse(text(6));}catch(const Json::exception&){throw Error(Exit::record,"记录事件 JSON 损坏");}
            if(e.seq!=previous+1) throw Error(Exit::record,"记录事件序号存在缺口或重复");
            previous=e.seq;
            visit(e);
        }
        if(rc!=SQLITE_DONE) throw Error(Exit::record,sqlite3_errmsg(db));
    });
}
Json log_info(const std::filesystem::path& path) {
    Json info={{"schema_version",1},{"segments",Json::array()},{"event_count",0},{"raw_bytes",0},{"complete",true}};
    segments(path,[&](sqlite3* db,const Json& m,const auto& p) {
        sqlite3_stmt* raw=nullptr;
        if(sqlite3_prepare_v2(db,"SELECT count(*),coalesce(sum(length(bytes)),0) FROM events",-1,&raw,nullptr)!=SQLITE_OK) throw Error(Exit::record,sqlite3_errmsg(db));
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> statement(raw,sqlite3_finalize);
        if(sqlite3_step(raw)!=SQLITE_ROW) throw Error(Exit::record,sqlite3_errmsg(db));
        auto count=sqlite3_column_int64(raw,0),bytes=sqlite3_column_int64(raw,1);
        info["event_count"]=info["event_count"].get<std::int64_t>()+count;info["raw_bytes"]=info["raw_bytes"].get<std::int64_t>()+bytes;
        bool clean=m.value("clean_shutdown",false);if(!clean) info["complete"]=false;
        info["segments"].push_back({{"path",p.string()},{"metadata",m},{"event_count",count},{"raw_bytes",bytes}});
    });return info;
}
}
