#include "serialspark/core.hpp"
#include <chrono>
#include <fstream>
#include <cctype>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace ss {
Bytes unhex(const std::string& text) {
    Bytes result;
    int high=-1;
    for (unsigned char ch : text) {
        if (std::isspace(ch)) continue;
        int digit=ch>='0'&&ch<='9' ? ch-'0' : ch>='a'&&ch<='f' ? ch-'a'+10 : ch>='A'&&ch<='F' ? ch-'A'+10 : -1;
        if (digit<0) throw Error(Exit::usage,"十六进制包含非法字符；请使用成对的 00～FF 字节");
        if (high<0) high=digit; else { result.push_back(static_cast<std::uint8_t>(high*16+digit)); high=-1; }
    }
    if (high>=0) throw Error(Exit::usage,"十六进制必须包含偶数个数字");
    return result;
}
std::string hex(std::span<const std::uint8_t> bytes) {
    constexpr char digits[]="0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size()*2);
    for (auto b:bytes) {out+=digits[b>>4];out+=digits[b&15];}
    return out;
}
std::string escaped(std::span<const std::uint8_t> bytes) {
    std::string out;
    for (std::size_t i=0;i<bytes.size();) {
        auto b=bytes[i];
        if (b=='\n') {out+="\\n"; ++i;}
        else if(b=='\r') {out+="\\r"; ++i;}
        else if(b=='\t') {out+="\\t"; ++i;}
        else if(b=='\\') {out+="\\\\"; ++i;}
        else if(b>=32&&b<127) {out+=static_cast<char>(b); ++i;}
        else {
            std::size_t len=b>=0xC2&&b<=0xDF?2:b>=0xE0&&b<=0xEF?3:b>=0xF0&&b<=0xF4?4:0;
            bool valid=len&&i+len<=bytes.size();
            for(std::size_t j=1;valid&&j<len;++j) valid=(bytes[i+j]&0xC0)==0x80;
            if(valid&&len>=3) valid=!(b==0xE0&&bytes[i+1]<0xA0)&&!(b==0xED&&bytes[i+1]>=0xA0);
            if(valid&&len==4) valid=!(b==0xF0&&bytes[i+1]<0x90)&&!(b==0xF4&&bytes[i+1]>=0x90);
            // Unicode C1 controls must remain visible bytes, even in valid UTF-8.
            if(valid&&len==2&&b==0xC2&&bytes[i+1]<=0x9F)valid=false;
            if(valid) {out.append(reinterpret_cast<const char*>(bytes.data()+i),len);i+=len;}
            else {out+="\\x"+hex(bytes.subspan(i,1));++i;}
        }
    }
    return out;
}
std::int64_t wall_ns() {return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
std::int64_t mono_ns() {return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
Json read_json(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);
    if(!in) throw Error(Exit::usage,"无法打开配置: "+path.string());
    std::string contents(1024*1024+1,'\0');in.read(contents.data(),contents.size());
    auto count=static_cast<std::size_t>(in.gcount());
    if(count>1024*1024) throw Error(Exit::usage,"配置文件超过 1 MiB 上限");
    if(!in.eof()&&in.fail())throw Error(Exit::io,"配置文件读取失败: "+path.string());
    contents.resize(count);
    try {return Json::parse(contents);} catch(const Json::exception& e) {throw Error(Exit::usage,"JSON 配置错误: "+std::string(e.what()));}
}
void write_json(const std::filesystem::path& path,const Json& j,bool replace) {
    auto contents=j.dump(2)+"\n";
    if(contents.size()>1024*1024) throw Error(Exit::usage,"配置文件超过 1 MiB 上限");
    auto pattern=path.string()+".tmp.XXXXXX";std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);
    int fd=mkstemp(name.data());if(fd<0)throw Error(Exit::io,"无法创建配置临时文件: "+std::string(strerror(errno)));
    struct Cleanup {int fd;std::string path;~Cleanup(){if(fd>=0)::close(fd);::unlink(path.c_str());}} cleanup{fd,name.data()};
    std::size_t offset=0;
    while(offset<contents.size()) {
        auto n=::write(fd,contents.data()+offset,contents.size()-offset);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)throw Error(Exit::io,"配置写入失败: "+std::string(strerror(errno)));
        offset+=n;
    }
    if(fsync(fd)<0)throw Error(Exit::io,"配置同步失败");
    int rc=replace ? ::rename(cleanup.path.c_str(),path.c_str()) : ::link(cleanup.path.c_str(),path.c_str());
    if(rc<0)throw Error(errno==EEXIST?Exit::usage:Exit::io,"无法发布配置（默认不覆盖已有文件）: "+std::string(strerror(errno)));
    auto parent=path.parent_path();if(parent.empty())parent=".";
    int directory=::open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(directory<0)throw Error(Exit::io,"配置已发布，但无法打开目录进行同步");
    rc=fsync(directory);::close(directory);if(rc<0)throw Error(Exit::io,"配置已发布，但目录同步失败");
}
Json event_json(const RawEvent& e) {return {{"schema_version",1},{"kind",e.kind},{"seq",e.seq},{"wall_ns",e.wall},{"mono_ns",e.mono},{"direction",e.direction},{"raw_hex",hex(e.bytes)},{"byte_count",e.bytes.size()},{"detail",e.detail}};}
RawEvent event_from_json(const Json& j) {return {j.at("seq"),j.at("wall_ns"),j.at("mono_ns"),j.at("kind"),j.at("direction"),unhex(j.at("raw_hex")),j.at("detail")};}
}
