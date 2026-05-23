#include "serialspark/session.hpp"
#include "serialspark/protocol.hpp"
#include "serialspark/record.hpp"
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <optional>

namespace ss {
namespace {
volatile sig_atomic_t interrupted=0;
void signal_handler(int) {interrupted=1;}
struct Signals {
    struct sigaction old_int{},old_term{},old_pipe{};
    Signals() {interrupted=0;struct sigaction action{};action.sa_handler=signal_handler;sigemptyset(&action.sa_mask);sigaction(SIGINT,&action,&old_int);sigaction(SIGTERM,&action,&old_term);action.sa_handler=SIG_IGN;sigaction(SIGPIPE,&action,&old_pipe);}
    ~Signals() {sigaction(SIGINT,&old_int,nullptr);sigaction(SIGTERM,&old_term,nullptr);sigaction(SIGPIPE,&old_pipe,nullptr);}
};
struct Input {
    int fd=-1,flags=-1;
    bool own=false;
    Bytes memory;
    std::size_t pos=0;
    ~Input() {if(own&&fd>=0)::close(fd);else if(fd>=0&&flags>=0)fcntl(fd,F_SETFL,flags);}
    void open_file(const std::string& path) {fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NONBLOCK);own=true;if(fd<0)throw Error(Exit::io,"无法打开发送文件: "+path+" ("+strerror(errno)+")");}
    void standard() {fd=STDIN_FILENO;flags=fcntl(fd,F_GETFL);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0)throw Error(Exit::io,"无法设置标准输入");}
};
Bytes text_bytes(const std::string& s,const std::string& newline) {
    Bytes b(s.begin(),s.end());
    if(newline=="lf"||newline=="crlf") {if(newline=="crlf")b.push_back('\r');b.push_back('\n');}
    else if(newline=="cr") b.push_back('\r');
    return b;
}
}
Output::Output(std::string format,std::string display,const std::string& path,bool live):live_(live),format_(std::move(format)),display_(std::move(display)) {
    if(!path.empty()) {fd_=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);owned_=true;if(fd_<0)throw Error(Exit::io,"无法创建输出文件（不覆盖已有文件）: "+path);}
    if(live_) {old_flags_=fcntl(fd_,F_GETFL);if(old_flags_<0||fcntl(fd_,F_SETFL,old_flags_|O_NONBLOCK)<0)throw Error(Exit::io,"无法设置非阻塞输出");}
}
Output::~Output() {if(old_flags_>=0)fcntl(fd_,F_SETFL,old_flags_);if(owned_)::close(fd_);}
void Output::enqueue(std::string text) {
    if(live_&&(text.size()>4*1024*1024||queued_>4*1024*1024-text.size())) throw Error(Exit::io,"输出队列达到 4 MiB 上限，停止会话；原始记录保留到已采集位置");
    queued_+=text.size();queue_.push_back(std::move(text));flush();
}
void Output::flush() {
    while(!queue_.empty()) {
        auto& s=queue_.front();ssize_t n=write(fd_,s.data()+offset_,s.size()-offset_);
        if(n<0) {if(errno==EINTR)continue;if(live_&&(errno==EAGAIN||errno==EWOULDBLOCK))return;throw Error(Exit::io,"输出失败: "+std::string(strerror(errno)));}
        if(n==0) throw Error(Exit::io,"输出没有进展");
        offset_+=n;queued_-=n;
        if(offset_==s.size()) {queue_.pop_front();offset_=0;}
    }
}
void Output::finish() {
    auto deadline=mono_ns()+2000000000;
    while(!queue_.empty()) {
        flush();if(queue_.empty())break;
        if(mono_ns()>=deadline)throw Error(Exit::io,"输出排空超时，部分显示结果未输出");
        pollfd p{fd_,POLLOUT,0};poll(&p,1,50);
    }
}
void Output::raw(std::span<const std::uint8_t> bytes) {enqueue(std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size()));}
void Output::emit(const Json& j) {
    if(format_=="json"||format_=="jsonl") {enqueue(j.dump()+"\n");return;}
    std::ostringstream out;
    if(j.contains("kind")&&j.contains("mono_ns")) {
        auto t=j["mono_ns"].get<std::int64_t>();if(!start_)start_=t;
        out<<'+'<<std::fixed<<std::setprecision(6)<<static_cast<double>(t-start_)/1e9<<"s wall_ns="<<j.value("wall_ns",std::int64_t{})<<' ';
        auto direction=j.value("direction",std::string{});
        if(!direction.empty())out<<(direction=="tx"?"TX":"RX")<<' ';
        out<<j.value("kind",std::string{});
        if(j.contains("seq"))out<<" event="<<j["seq"];
        if(j.contains("raw_hex")) {
            auto bytes=unhex(j["raw_hex"]);out<<" bytes="<<bytes.size();
            if(display_!="text")out<<" HEX="<<hex(bytes);
            if(display_!="hex")out<<" TEXT="<<escaped(bytes);
        }
        if(j.contains("status"))out<<" status="<<j["status"];
        if(j.contains("fields"))out<<" fields="<<j["fields"].dump();
        if(j.contains("errors")&&!j["errors"].empty())out<<" errors="<<j["errors"].dump();
        if(j.contains("detail")&&!j["detail"].empty())out<<" detail="<<j["detail"].dump();
    } else out<<j.dump(2);
    out<<'\n';enqueue(out.str());
}
int run_session(const SessionOptions& options) {
    Signals signals;
    Output output(options.format,options.display,"",true);
    std::unique_ptr<Protocol> protocol;
    if(!options.protocol.empty())protocol=std::make_unique<Protocol>(read_json(options.protocol));
    auto config=options.config;config.validate();
    std::unique_ptr<Recorder> recorder;
    if(!options.record.empty()) {
        Json meta={{"application","SerialSpark"},{"version","0.1.0"},{"requested_config",config.json()},{"protocol",protocol?protocol->config():Json(nullptr)},{"device_discovery",discover_ports()},{"timestamp_scope","application_observed"}};
        recorder=std::make_unique<Recorder>(options.record,meta,options.rotate_bytes,options.quota_bytes);
    }
    std::int64_t seq=0,request=0,started=mono_ns(),first_tx_seq=0;
    std::unique_ptr<SerialPort> port;
    bool reply_seen=false,active=false,source_eof=false,complete=false,quit=false,first_tx=false;
    bool source_read_failed=false;
    std::uint64_t accepted=0,requested=0;
    std::size_t offset=0;
    std::int64_t progress=0,reply_deadline=0,drain_deadline=0;
    Bytes chunk,match_tail;
    std::unique_ptr<Input> source;
    auto frames=[&](const std::vector<Json>& results) {
        for(const auto& frame:results) {
            if(first_tx&&frame.value("direction","")=="rx"&&frame.value("status","")=="ok"&&options.reply_pattern.empty()&&
               std::all_of(frame.at("sources").begin(),frame.at("sources").end(),[&](const Json& source){return source.at("event").get<std::int64_t>()>first_tx_seq;})) reply_seen=true;
            output.emit(frame);
        }
    };
    auto event=[&](std::string kind,std::string direction,Bytes bytes,Json detail) {
        RawEvent e{++seq,wall_ns(),mono_ns(),std::move(kind),std::move(direction),std::move(bytes),std::move(detail)};
        if(recorder)recorder->append(e);
        output.emit(event_json(e));
        if(protocol)frames(protocol->feed(e));
    };
    auto connect=[&] {
        try {port=std::make_unique<SerialPort>(config);event("connect","",{},{{"actual_config",port->actual()},{"flow_note",config.flow=="xonxoff"?"流控字节由驱动处理":"应用取得的原始字节"}});}
        catch(const Error& e) {event("connect_failed","",{},{{"reason",e.what()},{"exit_code",static_cast<int>(e.code)}});throw;}
    };
    auto begin=[&](std::unique_ptr<Input> next) {
        if(active)throw Error(Exit::usage,"当前发送尚未完成");
        if(!port)throw Error(Exit::connect,"设备未连接，请先 :connect");
        source=std::move(next);active=true;complete=false;source_eof=false;source_read_failed=false;first_tx=false;first_tx_seq=0;reply_seen=false;accepted=requested=0;chunk.clear();match_tail.clear();offset=0;progress=mono_ns();++request;
        event("send_begin","tx",{},{{"request",request},{"completion_scope","driver_accepted"}});
    };
    auto end_send=[&](std::string status,std::string reason) {
        event("send_end","tx",{},{{"request",request},{"status",status},{"accepted_bytes",accepted},{"requested_bytes_read",requested},{"source_complete",source_eof},{"reason",reason}});
        active=false;source.reset();chunk.clear();offset=0;
    };
    int result=0;
    try {
        connect();
        Input interactive;
        if(options.mode=="session") {interactive.standard();event("session_help","",{},{{"commands",":send text|hex|file, :display hex|text|both, :status, :disconnect, :connect, :quit"}});}
        if(options.mode=="send") {
            auto input=std::make_unique<Input>();
            if(options.stdin_source)input->standard();else if(!options.file.empty())input->open_file(options.file);else input->memory=options.payload;
            begin(std::move(input));
        }
        std::string line;
        while(!quit) {
            if(interrupted)throw Error(Exit::cancelled,"用户停止了会话");
            if(recorder)recorder->check();
            output.flush();
            if(options.duration_ms&&mono_ns()-started>=options.duration_ms*1000000) {if(active)throw Error(accepted?Exit::partial:Exit::timeout,"会话时限达到，发送尚未完成");break;}
            pollfd descriptors[3]={{port?port->fd():-1,static_cast<short>(POLLIN|(active&&!chunk.empty()?POLLOUT:0)),0},{options.mode=="session"?interactive.fd:active&&source?source->fd:-1,POLLIN,0},{-1,0,0}};
            int rc=poll(descriptors,2,active&&source&&source->fd<0&&chunk.empty()?0:20);
            if(rc<0&&errno!=EINTR)throw Error(Exit::io,"等待串口事件失败");
            if(port&&(descriptors[0].revents&(POLLIN|POLLHUP|POLLERR))) {
                for(int n=0;n<16;++n) {
                    Bytes recv(4096);ssize_t count=read(port->fd(),recv.data(),recv.size());
                    if(count>0) {
                        recv.resize(count);
                        if(first_tx&&!options.reply_pattern.empty()) {
                            match_tail.insert(match_tail.end(),recv.begin(),recv.end());
                            if(std::search(match_tail.begin(),match_tail.end(),options.reply_pattern.begin(),options.reply_pattern.end())!=match_tail.end())reply_seen=true;
                            auto keep=options.reply_pattern.size()-1;if(match_tail.size()>keep)match_tail.erase(match_tail.begin(),match_tail.end()-keep);
                        } else if(first_tx&&!protocol)reply_seen=true;
                        event("data","rx",std::move(recv),Json::object());
                    } else if(count<0&&errno==EINTR)continue;
                    else if(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;
                    else throw Error(Exit::io,"设备已断开或读取失败: "+std::string(count==0?"EOF":strerror(errno)));
                }
                if(descriptors[0].revents&(POLLHUP|POLLERR|POLLNVAL))throw Error(Exit::io,"设备已断开（HUP/ERR）");
            }
            if(options.mode=="session"&&(descriptors[1].revents&(POLLIN|POLLHUP))) {
                char buffer[4096];ssize_t n=read(interactive.fd,buffer,sizeof(buffer));
                if(n==0) {if(active)throw Error(Exit::cancelled,"输入结束，取消未完成发送");quit=true;}
                else if(n>0) {
                    line.append(buffer,n);if(line.size()>65536)throw Error(Exit::usage,"交互输入超过 64 KiB 上限");
                    std::size_t newline;
                    while((newline=line.find('\n'))!=std::string::npos) {
                        auto command=line.substr(0,newline);line.erase(0,newline+1);if(!command.empty()&&command.back()=='\r')command.pop_back();
                        try {
                            if(command==":quit") {quit=true;break;}
                            if(command==":status") {event("status","",{},{{"connected",bool(port)},{"sending",active},{"persisted_seq",recorder?recorder->persisted():0}});continue;}
                            if(command==":disconnect") {if(active)end_send(accepted?"partial":"cancelled","用户断开连接");event("disconnect","",{},{{"reason","user"}});port.reset();continue;}
                            if(command==":connect") {if(!port)connect();continue;}
                            if(command.starts_with(":display ")) {auto value=command.substr(9);if(value!="hex"&&value!="text"&&value!="both")throw Error(Exit::usage,"显示方式必须为 hex、text 或 both");output.display(value);continue;}
                            auto input=std::make_unique<Input>();
                            if(command.starts_with(":send hex "))input->memory=unhex(command.substr(10));
                            else if(command.starts_with(":send file "))input->open_file(command.substr(11));
                            else if(command.starts_with(":send text "))input->memory=text_bytes(command.substr(11),options.newline);
                            else if(command.starts_with(':'))throw Error(Exit::usage,"未知交互命令；发送冒号开头文本请使用 :send text");
                            else input->memory=text_bytes(command,options.newline);
                            begin(std::move(input));
                        } catch(const Error& e) {event("command_error","",{},{{"reason",e.what()}});}
                    }
                } else if(errno!=EAGAIN&&errno!=EINTR)throw Error(Exit::io,"交互输入读取失败");
            }
            if(quit) {if(active)throw Error(Exit::cancelled,"用户结束会话，取消未完成发送");break;}
            if(active&&port) {
                if(chunk.empty()&&!source_eof) {
                    if(source->fd<0) {
                        auto count=std::min<std::size_t>(4096,source->memory.size()-source->pos);
                        chunk.assign(source->memory.begin()+source->pos,source->memory.begin()+source->pos+count);source->pos+=count;
                        if(!count)source_eof=true;
                    } else {
                        pollfd p{source->fd,POLLIN,0};poll(&p,1,0);
                        if(p.revents&(POLLIN|POLLHUP|POLLERR)) {
                            chunk.resize(4096);ssize_t n=read(source->fd,chunk.data(),chunk.size());
                            if(n>0)chunk.resize(n);else {chunk.clear();if(n==0)source_eof=true;else if(errno!=EINTR&&errno!=EAGAIN){source_read_failed=true;throw Error(accepted?Exit::partial:Exit::io,"发送源读取失败");}}
                        }
                    }
                    if(!chunk.empty()) {requested+=chunk.size();offset=0;progress=mono_ns();event("send_intent","tx",chunk,{{"request",request},{"source_offset",requested-chunk.size()}});}
                }
                if(!chunk.empty()) {
                    if(interrupted)throw Error(Exit::cancelled,"用户停止发送");
                    ssize_t n=write(port->fd(),chunk.data()+offset,chunk.size()-offset);
                    if(n>0) {
                        if(!first_tx)first_tx_seq=seq+1;
                        first_tx=true;Bytes sent(chunk.begin()+offset,chunk.begin()+offset+n);offset+=n;accepted+=n;progress=mono_ns();
                        event("data","tx",std::move(sent),{{"request",request},{"request_offset",accepted-static_cast<std::uint64_t>(n)}});
                        if(offset==chunk.size()) {chunk.clear();offset=0;}
                    } else if(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)throw Error(accepted?Exit::partial:Exit::io,"发送失败: "+std::string(strerror(errno)));
                    if(!chunk.empty()&&mono_ns()-progress>=options.write_timeout_ms*1000000ll)throw Error(accepted?Exit::partial:Exit::timeout,"写入超时（可能被流控阻塞）");
                }
                if(source_eof&&chunk.empty()) {
                    if(!accepted)throw Error(Exit::usage,"发送源为空，没有发送字节");
                    end_send("complete","全部读取的字节已被驱动接受");complete=true;
                    reply_deadline=mono_ns()+options.reply_timeout_ms*1000000ll;drain_deadline=mono_ns()+options.write_timeout_ms*1000000ll;
                    if(options.mode=="send"&&!options.wait_reply&&!options.drain)break;
                }
            }
            if(protocol) {
                RawEvent clock{seq+1,wall_ns(),mono_ns(),"clock","",{},Json::object()};auto results=protocol->feed(clock);
                if(!results.empty()) {++seq;if(recorder)recorder->append(clock);output.emit(event_json(clock));frames(results);}
            }
            if(options.mode=="send"&&complete) {
                bool drained=!options.drain;
                if(options.drain) {int count=0;if(ioctl(port->fd(),TIOCOUTQ,&count)<0)throw Error(Exit::io,"驱动无法查询输出队列");drained=count==0;if(!drained&&mono_ns()>=drain_deadline)throw Error(Exit::timeout,"输出队列排空超时");}
                if(options.wait_reply&&!reply_seen&&mono_ns()>=reply_deadline)throw Error(Exit::timeout,"回复超时；发送已完成，但未收到满足条件的回复");
                if(drained&&(!options.wait_reply||reply_seen)) {event("request_result","",{},{{"request",request},{"driver_accepted",true},{"output_queue_empty",options.drain?Json(true):Json(nullptr)},{"reply_matched",options.wait_reply?Json(reply_seen):Json(nullptr)}});break;}
            }
        }
        if(active)end_send(accepted?"partial":"cancelled","用户结束会话");
    } catch(const Error& e) {
        result=static_cast<int>(e.code==Exit::io&&active&&accepted?Exit::partial:e.code);
        try {if(active)end_send(accepted?"partial":"failed",e.what());event("error","",{},{{"reason",e.what()},{"exit_code",result},{"source_read_failed",source_read_failed}});}catch(...){}
        std::cerr<<e.what()<<'\n';
    }
    try {event("disconnect","",{},{{"reason",result?"error":"closed"}});}catch(const Error& e){if(!result)result=static_cast<int>(e.code);std::cerr<<e.what()<<'\n';}
    port.reset();
    if(recorder) {try{recorder->close();output.emit({{"kind","record_status"},{"persisted_seq",recorder->persisted()}});}catch(const Error& e){result=static_cast<int>(e.code);std::cerr<<e.what()<<'\n';}}
    try{output.finish();}catch(const Error& e){if(!result)result=static_cast<int>(e.code);std::cerr<<e.what()<<'\n';}
    return result;
}
}
