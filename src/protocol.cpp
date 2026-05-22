#include "serialspark/protocol.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <deque>
#include <limits>
#include <set>

namespace ss {
    namespace {
        constexpr std::size_t limit = 65536;
        [[noreturn]] void bad(const std::string& s) { throw Error(Exit::usage,"协议配置: "+s);
        }
        void keys(const Json& j, std::initializer_list<const char*> allowed) {
            if (!j.is_object()) bad("必须为对象");
            for (auto it=j.begin();it!=j.end();++it) {
                bool found=false;
                for(auto k:allowed) if(it.key()==k) found=true;
                if(!found) bad("未知选项 "+it.key());
            }
        }
        std::int64_t integer(const Json& j,const std::string& k,std::int64_t def,std::int64_t low,std::int64_t high) {
            if(!j.contains(k)) return def;
            const auto& v=j.at(k);
            if(!v.is_number_integer() || (v.is_number_unsigned()&&v.get<std::uint64_t>()>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))) bad(k+" 必须为有界整数");
            auto n=v.get<std::int64_t>();
            if(n<low||n>high) bad(k+" 超出范围");
            return n;
        }
        std::string string(const Json& j,const std::string& k,const std::string& def="") {
            if(!j.contains(k)) return def;
            if(!j.at(k).is_string()) bad(k+" 必须为字符串");
            return j.at(k).get<std::string>();
        }
        bool boolean(const Json& j,const std::string& k,bool def) {
            if(!j.contains(k)) return def;
            if(!j.at(k).is_boolean()) bad(k+" 必须为布尔值");
            return j.at(k).get<bool>();
        }
        std::size_t width(const std::string& type) {
            if(type=="u8"||type=="i8") return 1;
            if(type=="u16"||type=="i16") return 2;
            if(type=="u32"||type=="i32"||type=="float32") return 4;
            if(type=="u64"||type=="i64"||type=="float64") return 8;
            return 0;
        }
        bool little(const Json& j,const std::string& key,std::size_t w) {
            auto s=string(j,key);
            if(s.empty()&&w<=1) return true;
            if(s!="little"&&s!="big") bad(key+" 必须显式为 little 或 big");
            return s=="little";
        }
        Bytes bytes(const Json& j,const std::string& k) {
            auto s=string(j,k);
            Bytes b;
            try {b=unhex(s);
            } catch(const Error&) {bad(k+" 十六进制无效");
            }
            if(b.empty()||b.size()>limit) bad(k+" 字节长度无效");
            return b;
        }
        struct Field {std::string name,type,unit;
            std::size_t offset=0,length=0,index=0;
            bool text=false,le=true;
            double scale=1,bias=0;
            Json enums=Json::object();
        };
        struct Spec {
            std::string mode;
            Bytes marker;
            std::vector<std::size_t> failure;
            std::size_t length=0,min=1,max=limit,lenoff=0,lenwidth=0,unit=1;
            std::int64_t adjustment=0,timeout=0;
            bool include=false,lenle=true,raw=false;
            std::string separator=",";
            std::vector<Field> fields;
            std::string checksum;
            std::size_t checkwidth=0,start=0,end=0,checkoff=0;
            bool checkle=true;
        };
        Spec parse(const Json& j) {
            keys(j,{"framing","fields","checksum","emit_raw_fields","split_separator"});
            Spec s;
            s.raw=boolean(j,"emit_raw_fields",false);
            s.separator=string(j,"split_separator",",");
            if(s.separator.empty()) bad("split_separator 不可为空");
            if(!j.contains("framing")) bad("缺少 framing");
            const auto& f=j.at("framing");
            keys(f,{"mode","newline","delimiter_hex","include_delimiter","length","header_hex","length_field","min_frame_bytes","max_frame_bytes","assembly_timeout_ms"});
            s.timeout=integer(f,"assembly_timeout_ms",0,0,std::numeric_limits<std::int64_t>::max()/1000000);
            s.mode=string(f,"mode");
            s.min=integer(f,"min_frame_bytes",1,1,limit);
            s.max=integer(f,"max_frame_bytes",limit,1,limit);
            if(s.min>s.max) bad("min_frame_bytes 大于 max_frame_bytes");
            auto modekey=[&](const char* key,bool ok){if(f.contains(key)&&!ok) bad(std::string(key)+" 不适用于 "+s.mode);
            };
            modekey("newline",s.mode=="newline");
            modekey("delimiter_hex",s.mode=="delimiter");
            modekey("include_delimiter",s.mode=="newline"||s.mode=="delimiter");
            modekey("length",s.mode=="fixed"||s.mode=="header_fixed");
            modekey("header_hex",s.mode=="header_fixed"||s.mode=="header_length");
            modekey("length_field",s.mode=="header_length");
            if(s.mode=="newline") {auto n=string(f,"newline","LF");
                if(n=="LF")s.marker={10};
                else if(n=="CRLF")s.marker={13,10};
                else if(n=="CR")s.marker={13};
                else bad("newline 必须为 LF/CRLF/CR");
            }
            else if(s.mode=="delimiter")s.marker=bytes(f,"delimiter_hex");
            else if(s.mode!="fixed"&&s.mode!="header_fixed"&&s.mode!="header_length")bad("未知 framing.mode");
            s.include=boolean(f,"include_delimiter",false);
            if(s.mode=="header_fixed"||s.mode=="header_length")s.marker=bytes(f,"header_hex");
            if(s.marker.size()>s.max)bad("分隔符或帧头超过 max_frame_bytes");
            if(s.mode=="fixed"||s.mode=="header_fixed") {
                if(!f.contains("length"))bad("缺少 length");
                s.length=integer(f,"length",0,s.min,s.max);
                if(s.marker.size()>s.length)bad("帧头超过 length");
            }
            if(s.mode=="header_length") {
                if(!f.contains("length_field"))bad("缺少 length_field");
                const auto& l=f.at("length_field");
                keys(l,{"offset","type","endian","unit_bytes","adjustment"});
                auto t=string(l,"type");
                if(t!="u8"&&t!="u16"&&t!="u32")bad("length_field.type 必须为 u8/u16/u32");
                s.lenwidth=width(t);
                s.lenle=little(l,"endian",s.lenwidth);
                s.lenoff=integer(l,"offset",0,0,limit);
                s.unit=integer(l,"unit_bytes",1,1,limit);
                s.adjustment=integer(l,"adjustment",0,-static_cast<std::int64_t>(limit),limit);
                if(s.lenoff+s.lenwidth>s.max)bad("长度字段超过 max_frame_bytes");
            }
            if(j.contains("fields")) {
                if(!j.at("fields").is_array())bad("fields 必须为数组");
                std::set<std::string> names;
                for(const auto& v:j.at("fields")) {
                    keys(v,{"name","type","offset","length","endian","index","scale","bias","unit","enum"});
                    Field a;
                    a.name=string(v,"name");
                    if(a.name.empty()||!names.insert(a.name).second)bad("字段名为空或重复");
                    a.type=string(v,"type");
                    auto w=width(a.type);
                    bool blob=a.type=="bytes"||a.type=="ascii"||a.type=="utf8";
                    if(!w&&!blob)bad("未知字段类型 "+a.type);
                    a.text=v.contains("index");
                    if(a.text) {a.index=integer(v,"index",0,0,limit);
                        if(v.contains("offset")||v.contains("length")||v.contains("endian")||a.type=="bytes")bad("文本字段不支持 offset/length/endian/bytes");
                    }
                    else {if(!v.contains("offset"))bad("二进制字段缺少 offset");
                        a.offset=integer(v,"offset",0,0,limit);
                        a.length=w;
                        if(blob){if(!v.contains("length"))bad("文本/字节字段缺少 length");
                            a.length=integer(v,"length",0,1,limit);
                            if(v.contains("endian"))bad("文本/字节字段不支持 endian");
                        }
                        else {if(v.contains("length"))bad("数值字段不支持 length");
                            a.le=little(v,"endian",w);
                        }
                        if(a.offset+a.length>s.max)bad("字段超过 max_frame_bytes");
                    }
                    for(auto k:{"scale","bias"})if(v.contains(k)&&(!v.at(k).is_number()||!std::isfinite(v.at(k).get<double>())))bad(std::string(k)+" 必须为有限数值");
                    a.scale=v.value("scale",1.0);
                    a.bias=v.value("bias",0.0);
                    a.unit=string(v,"unit");
                    if(blob&&(v.contains("scale")||v.contains("bias")||v.contains("enum")))bad("文本/字节字段不支持数值变换");
                    if(w==8&&(a.type=="u64"||a.type=="i64")&&(a.scale!=1||a.bias!=0))bad("64 位整数字段不支持浮点缩放");
                    if(v.contains("enum")){a.enums=v.at("enum");
                        if(!a.enums.is_object())bad("enum 必须为对象");
                        for(auto it=a.enums.begin();it!=a.enums.end();++it)if(!it.value().is_string())bad("enum 值必须为字符串");
                    }
                    s.fields.push_back(std::move(a));
                }
            }
            if(j.contains("checksum")) {
                const auto& c=j.at("checksum");
                keys(c,{"algorithm","coverage","offset_from_end","result_endian"});
                s.checksum=string(c,"algorithm");
                if(s.checksum=="sum8"||s.checksum=="xor8")s.checkwidth=1;
                else if(s.checksum=="crc16-modbus"||s.checksum=="crc16-ccitt-false")s.checkwidth=2;
                else if(s.checksum=="crc32")s.checkwidth=4;
                else bad("未知 checksum.algorithm");
                s.checkle=little(c,"result_endian",s.checkwidth);
                s.checkoff=integer(c,"offset_from_end",s.checkwidth,s.checkwidth,s.max);
                s.end=s.checkoff;
                if(c.contains("coverage")){const auto& cov=c.at("coverage");
                    keys(cov,{"start","end_from_end"});
                    s.start=integer(cov,"start",0,0,s.max);
                    s.end=integer(cov,"end_from_end",s.checkoff,0,s.max);
                }
                if(s.start+s.end>s.max)bad("校验覆盖范围超出 max_frame_bytes");
            }
            s.failure.resize(s.marker.size());
            for (std::size_t i=1, matched=0; i<s.marker.size(); ++i) {
                while (matched && s.marker[i]!=s.marker[matched]) matched=s.failure[matched-1];
                if (s.marker[i]==s.marker[matched]) ++matched;
                s.failure[i]=matched;
            }
            return s;
        }
        std::pair<Spec,Spec> specs(const Json& j) {
            keys(j,{"schema_version","framing","fields","checksum","emit_raw_fields","split_separator","tx","rx"});
            if(!j.contains("schema_version")||integer(j,"schema_version",0,1,1)!=1)bad("schema_version 必须为 1");
            Json common=j;
            common.erase("schema_version");
            common.erase("tx");
            common.erase("rx");
            if(!common.empty()){auto check=common;
                if(!check.contains("framing"))check["framing"]={{"mode","fixed"},{"length",1}};
                (void)parse(check);
            }
            auto fordir=[&](const char* dir){auto d=common;
                if(j.contains(dir)){keys(j.at(dir),{"framing","fields","checksum","emit_raw_fields","split_separator"});
                    for(auto it=j.at(dir).begin();it!=j.at(dir).end();++it)d[it.key()]=it.value();
                }return parse(d);
            };
            return {fordir("tx"),fordir("rx")};
        }
        std::uint64_t number(std::span<const std::uint8_t> b,bool le) {std::uint64_t n=0;
            for(std::size_t i=0;i<b.size();++i)n|=static_cast<std::uint64_t>(b[i])<<(8*(le?i:b.size()-i-1));
            return n;
        }
        bool utf8(std::span<const std::uint8_t> b) {
            for(std::size_t i=0;i<b.size();) {auto c=b[i];
                if(c<128){++i;
                    continue;
                }std::size_t n=c>=0xC2&&c<=0xDF?2:c>=0xE0&&c<=0xEF?3:c>=0xF0&&c<=0xF4?4:0;
                if(!n||i+n>b.size())return false;
                for(std::size_t k=1;k<n;++k)if((b[i+k]&0xC0)!=0x80)return false;
                if(n>=3&&((c==0xE0&&b[i+1]<0xA0)||(c==0xED&&b[i+1]>=0xA0)))return false;
                if(n==4&&((c==0xF0&&b[i+1]<0x90)||(c==0xF4&&b[i+1]>=0x90)))return false;
                i+=n;
            }return true;
        }
        std::uint32_t checksum(const std::string& alg,std::span<const std::uint8_t> b) {
            std::uint32_t c=alg=="crc16-modbus"||alg=="crc16-ccitt-false"?0xffff:alg=="crc32"?0xffffffff:0;
            for(auto v:b) {
                if(alg=="sum8") c=(c+v)&255;
                else if(alg=="xor8")c^=v;
                else if(alg=="crc16-ccitt-false"){c^=static_cast<std::uint32_t>(v)<<8;
                    for(int i=0;i<8;++i)c=((c<<1)^((c&0x8000)?0x1021:0))&0xffff;
                }
                else {c^=v;
                    for(int i=0;i<8;++i)c=(c>>1)^((c&1)?(alg=="crc32"?0xedb88320:0xa001):0);
                }
            }return alg=="crc32"?c^0xffffffff:c;
        }
        std::vector<std::string> split(const Bytes& b,const std::string& sep) {
            std::string str(b.begin(),b.end());
            std::vector<std::string> out;
            std::size_t start=0;
            for(;;){auto p=str.find(sep,start);
                out.push_back(str.substr(start,p==std::string::npos?p:p-start));
                if(p==std::string::npos)break;
                start=p+sep.size();
            }return out;
        }
        Json numeric(const Field& f,std::uint64_t n) {
            auto w=width(f.type);
            if(f.type=="float32")return std::bit_cast<float>(static_cast<std::uint32_t>(n));
            if(f.type=="float64")return std::bit_cast<double>(n);
            if(f.type[0]=='i') {if(w<8&&(n&(std::uint64_t(1)<<(w*8-1))))n|=(~std::uint64_t(0))<<(w*8);
                auto val=std::bit_cast<std::int64_t>(n);
                if(w==8)return std::to_string(val);
                return val;
            }
            if(w==8)return std::to_string(n);
            return n;
        }
        Json textvalue(const Field& f,const std::string& t) {
            if(f.type=="ascii"||f.type=="utf8")return t;
            if(t.empty())throw std::runtime_error("空数值");
            auto first=t.data(),last=first+t.size();
            if (f.type=="float32") {
                float n=0;
                auto r=std::from_chars(first,last,n);
                if (r.ec!=std::errc() || r.ptr!=last || !std::isfinite(n)) throw std::runtime_error("无效文本数值");
                return n;
            }
            if (f.type=="float64") {
                double n=0;
                auto r=std::from_chars(first,last,n);
                if (r.ec!=std::errc() || r.ptr!=last || !std::isfinite(n)) throw std::runtime_error("无效文本数值");
                return n;
            }
            auto bits=width(f.type)*8;
            if(f.type[0]=='i'){std::int64_t n=0;
                auto r=std::from_chars(first,last,n);
                if(r.ec!=std::errc()||r.ptr!=last||(bits<64&&(n<-(std::int64_t(1)<<(bits-1))||n>(std::int64_t(1)<<(bits-1))-1)))throw std::runtime_error("无效或溢出的文本整数");
                if(bits==64)return std::to_string(n);
                return n;
            }
            std::uint64_t n=0;
            auto r=std::from_chars(first,last,n);
            if(r.ec!=std::errc()||r.ptr!=last||(bits<64&&n> (std::uint64_t(1)<<bits)-1))throw std::runtime_error("无效或溢出的文本整数");
            if(bits==64)return std::to_string(n);
            return n;
        }
        void decode(Json& out,const Bytes& b,const Spec& s) {
            auto view=std::span<const std::uint8_t>(b);
            auto tokens=split(b,s.separator);
            for(const auto& f:s.fields)try {
                Json val;
                Bytes raw;
                if(f.text){if(f.index>=tokens.size())throw std::runtime_error("缺少文本字段");
                    raw.assign(tokens[f.index].begin(),tokens[f.index].end());
                    if((f.type=="utf8"&&!utf8(raw))||(f.type=="ascii"&&std::any_of(raw.begin(),raw.end(),[](auto v){return v>127;})))throw std::runtime_error("文本编码无效");
                    val=textvalue(f,tokens[f.index]);
                }
                else {if(f.offset>b.size()||f.length>b.size()-f.offset)throw std::runtime_error("字段超出帧范围");
                    auto part=view.subspan(f.offset,f.length);
                    raw.assign(part.begin(),part.end());
                    if(f.type=="bytes")val=hex(part);
                    else if(f.type=="ascii"||f.type=="utf8"){if((f.type=="ascii"&&std::any_of(part.begin(),part.end(),[](auto v){return v>127;}))||(f.type=="utf8"&&!utf8(part)))throw std::runtime_error("文本编码无效");
                        val=std::string(part.begin(),part.end());
                    }else val=numeric(f,number(part,f.le));
                }
                if(val.is_number_float()&&!std::isfinite(val.get<double>()))throw std::runtime_error("非有限浮点数");
                if(s.raw)out["raw_fields"][f.name]=val;
                auto enumkey=val.is_string()?val.get<std::string>():val.dump();
                if(f.enums.contains(enumkey))val=f.enums.at(enumkey);
                else if(val.is_number()&&(f.scale!=1||f.bias!=0)){auto v=val.get<double>()*f.scale+f.bias;
                    if(!std::isfinite(v))throw std::runtime_error("缩放溢出");
                    val=v;
                }
                out["fields"][f.name]=val;
                if(!f.unit.empty())out["units"][f.name]=f.unit;
            }catch(const std::exception& e){out["errors"].push_back({{"code","field"},{"field",f.name},{"message",e.what()}});
            }
            if(!s.checksum.empty()) {
                if(s.start>b.size()||s.end>b.size()-s.start||s.checkoff>b.size())out["errors"].push_back({{"code","checksum_range"},{"message","校验位置或覆盖范围超出帧"}});
                else {auto expected=checksum(s.checksum,view.subspan(s.start,b.size()-s.end-s.start));
                    auto actual=number(view.subspan(b.size()-s.checkoff,s.checkwidth),s.checkle);
                    if(expected!=actual)out["errors"].push_back({{"code","checksum"},{"expected",expected},{"actual",actual}});
                }
            }
            if(!out["errors"].empty())out["status"]="error";
        }
        struct Cell {std::uint8_t byte;
            std::int64_t event,wall,mono;
            std::size_t offset;
        };
        struct Stream {Spec spec;
            std::deque<Cell> buffer;
            std::int64_t last=0;
            std::size_t prefix=0;
            bool header_ready=false;
        };
    }
    struct Protocol::Impl {
        Json config;
        Stream tx,rx;
        explicit Impl(const Json& c):config(c){auto pair=specs(c);
            tx.spec=std::move(pair.first);
            rx.spec=std::move(pair.second);
        }
        Json take(Stream& st,const std::string& dir,std::size_t count,const std::string& status,const std::string& reason="",std::size_t trim=0) {
            Json out={{"schema_version",1},{"kind","frame"},{"direction",dir},{"status",status},{"fields",Json::object()},{"errors",Json::array()},{"sources",Json::array()}};
            Bytes data;
            data.reserve(count);
            for(std::size_t i=0;i<count;++i){auto c=st.buffer.front();
                st.buffer.pop_front();
                data.push_back(c.byte);
                out["wall_ns"]=c.wall;
                out["mono_ns"]=c.mono;
                auto& sources=out["sources"];
                if(!sources.empty()&&sources.back()["event"]==c.event&&sources.back()["offset"].get<std::size_t>()+sources.back()["length"].get<std::size_t>()==c.offset)sources.back()["length"]=sources.back()["length"].get<std::size_t>()+1;
                else sources.push_back({{"event",c.event},{"offset",c.offset},{"length",1}});
            }
            st.prefix=0;
            st.header_ready=false;
            out["raw_hex"]=hex(data);
            if(!reason.empty())out["errors"].push_back({{"code",reason},{"message",reason}});
            if(status=="ok"){data.resize(data.size()-trim);
                if(st.spec.raw)out["raw_fields"]=Json::object();
                decode(out,data,st.spec);
            }return out;
        }
        void drain(Stream& st,const std::string& dir,std::vector<Json>& out) {
            const auto& s=st.spec;
            while(!st.buffer.empty()) {
                auto n=st.buffer.size();
                if (s.mode=="newline" || s.mode=="delimiter") {
                    // drain is called once for each appended byte; prefix carries
                    // the delimiter automaton across arbitrary input partitions.
                    const auto byte=st.buffer.back().byte;
                    while (st.prefix && byte!=s.marker[st.prefix]) st.prefix=s.failure[st.prefix-1];
                    if (byte==s.marker[st.prefix]) ++st.prefix;
                    if (st.prefix==s.marker.size()) {
                        out.push_back(take(st,dir,n,n<s.min?"error":"ok",n<s.min?"invalid_length":"",s.include?0:s.marker.size()));
                        continue;
                    }
                    if (n>=s.max) {
                        out.push_back(take(st,dir,n,"error","max_frame_bytes"));
                        continue;
                    }
                    break;
                }
                if ((s.mode=="header_fixed" || s.mode=="header_length") && !st.header_ready) {
                    auto matched=st.prefix;
                    bool found=false;
                    for (std::size_t i=st.prefix; i<n; ++i) {
                        const auto byte=st.buffer[i].byte;
                        while (matched && byte!=s.marker[matched]) matched=s.failure[matched-1];
                        if (byte==s.marker[matched]) ++matched;
                        if (matched==s.marker.size()) {
                            auto noise=i+1-s.marker.size();
                            while (noise--) out.push_back(take(st,dir,1,"noise","header_noise"));
                            st.header_ready=true;
                            found=true;
                            break;
                        }
                    }
                    if (!found) {
                        auto noise=n-matched;
                        while (noise--) out.push_back(take(st,dir,1,"noise","header_noise"));
                        st.prefix=matched;
                        break;
                    }
                    n=st.buffer.size();
                }
                std::size_t target=s.length;
                if(s.mode=="header_length") {
                    if(n<s.lenoff+s.lenwidth)break;
                    Bytes v;
                    for(std::size_t i=0;i<s.lenwidth;++i)v.push_back(st.buffer[s.lenoff+i].byte);
                    auto raw=number(v,s.lenle);
                    auto computed=static_cast<std::int64_t>(raw)*static_cast<std::int64_t>(s.unit)+s.adjustment;
                    if(computed<static_cast<std::int64_t>(std::max(s.min,std::max(s.marker.size(),s.lenoff+s.lenwidth)))||computed>static_cast<std::int64_t>(s.max)){out.push_back(take(st,dir,1,"error","invalid_length"));
                        continue;
                    }target=static_cast<std::size_t>(computed);
                }
                if(n<target)break;
                out.push_back(take(st,dir,target,"ok"));
            }
        }
        void expire(Stream& st,const std::string& dir,std::int64_t now,std::vector<Json>& out) {
            if(!st.buffer.empty()&&st.spec.timeout&&now>=st.last&&static_cast<std::uint64_t>(now)-static_cast<std::uint64_t>(st.last)>=static_cast<std::uint64_t>(st.spec.timeout)*1000000)out.push_back(take(st,dir,st.buffer.size(),"incomplete","assembly_timeout"));
        }
    };
    Protocol::Protocol(const Json& c):impl_(std::make_unique<Impl>(c)){}
    Protocol::~Protocol()=default;
    Protocol::Protocol(Protocol&&) noexcept=default;
    Protocol& Protocol::operator=(Protocol&&) noexcept=default;
    void Protocol::validate(const Json& c){(void)specs(c);
    }
    const Json& Protocol::config()const{return impl_->config;
    }
    std::vector<Json> Protocol::feed(const RawEvent& e) {
        std::vector<Json> out;
        impl_->expire(impl_->tx,"tx",e.mono,out);
        impl_->expire(impl_->rx,"rx",e.mono,out);
        if(e.kind=="disconnect"){auto tail=finish("disconnect");
            out.insert(out.end(),tail.begin(),tail.end());
            return out;
        }
        if(e.kind!="data")return out;
        if(e.direction!="tx"&&e.direction!="rx")throw Error(Exit::protocol,"协议数据方向必须为 tx 或 rx");
        auto& st=e.direction=="tx"?impl_->tx:impl_->rx;
        for(std::size_t i=0;i<e.bytes.size();++i){st.buffer.push_back({e.bytes[i],e.seq,e.wall,e.mono,i});
            st.last=e.mono;
            impl_->drain(st,e.direction,out);
        }return out;
    }
    std::vector<Json> Protocol::finish(const std::string& reason) {
        std::vector<Json> out;
        for(auto dir:{"tx","rx"}){auto& st=std::string(dir)=="tx"?impl_->tx:impl_->rx;
            if(!st.buffer.empty())out.push_back(impl_->take(st,dir,st.buffer.size(),"incomplete",reason));
            st.last=0;
        }return out;
    }
}
