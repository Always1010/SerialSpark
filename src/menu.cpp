#include "serialspark/menu.hpp"
#include "serialspark/cli.hpp"
#include "serialspark/commands.hpp"
#include "serialspark/record.hpp"
#include "serialspark/profiles.hpp"
#include "serialspark/protocol.hpp"
#include "serialspark/serial.hpp"
#include <unistd.h>
#include <algorithm>
#include <csignal>
#include <cerrno>
#include <charconv>
#include <functional>
#include <iostream>
#include <map>
#include <limits>
#include <ctime>
#include <set>

namespace ss {
namespace {
struct Cancel {};
struct EndInput {};
struct Back {};
volatile sig_atomic_t menu_signal=0;
void menu_interrupt(int signal) { menu_signal=signal; }
struct MenuSignals {
    struct sigaction old_int{},old_term{};
    MenuSignals() {
        menu_signal=0;
        struct sigaction action{};
        action.sa_handler=menu_interrupt;sigemptyset(&action.sa_mask);
        sigaction(SIGINT,&action,&old_int);sigaction(SIGTERM,&action,&old_term);
    }
    ~MenuSignals() { sigaction(SIGINT,&old_int,nullptr);sigaction(SIGTERM,&old_term,nullptr); }
};
std::string visible(const std::string& text) { return escaped(Bytes(text.begin(),text.end())); }
std::string line() {
    // Read only this line: run_session also reads stdin directly and must not lose buffered input.
    std::string text;bool oversized=false;
    for(;;) {
        if(menu_signal)throw Cancel{};
        char ch;auto count=::read(STDIN_FILENO,&ch,1);
        if(count==0)throw EndInput{};
        if(count<0) {if(errno==EINTR)continue;throw Error(Exit::io,"读取菜单输入失败");}
        if(ch=='\n')break;
        if(text.size()<1024*1024)text+=ch;else oversized=true;
    }
    if(oversized)throw Error(Exit::usage,"输入超过 1 MiB，请改用文件");
    if(text.find('\0')!=std::string::npos)throw Error(Exit::usage,"输入不能含 NUL 字节");
    return text;
}
std::uint64_t integer(const std::string& value,std::uint64_t min,std::uint64_t max) {
    std::uint64_t result=0;
    auto [end,ec]=std::from_chars(value.data(),value.data()+value.size(),result);
    if(ec!=std::errc{}||end!=value.data()+value.size()||result<min||result>max)
        throw Error(Exit::usage,"请输入 "+std::to_string(min)+"～"+std::to_string(max)+" 范围的整数");
    return result;
}
using Choices=std::vector<std::pair<std::string,std::string>>;
using Check=std::function<void(const std::string&)>;
struct Answer {std::string key,label,value,display;};
struct Form {
    std::vector<Answer> accepted;
    std::map<std::string,std::string> remembered;
    std::size_t cursor=0;
    std::string ask(const std::string& key,const std::string& label,const std::string& fallback="",
                    Check check={},bool optional=false,const Choices& choices={}) {
        auto validate=[&](const std::string& value) {
            if(value.empty()&&!optional)throw Error(Exit::usage,"此项不能为空");
            if(!choices.empty()&&std::none_of(choices.begin(),choices.end(),[&](const auto& c){return c.first==value;}))
                throw Error(Exit::usage,"请选择列表中的编号");
            if(check)check(value);
        };
        if(cursor<accepted.size()&&accepted[cursor].key==key) {
            try {validate(accepted[cursor].value);return accepted[cursor++].value;}
            catch(const std::exception& e){std::cout<<"需要重新填写："<<visible(e.what())<<'\n';}
        }
        accepted.resize(cursor);
        auto default_value=remembered.contains(key)?remembered.at(key):fallback;
        if(!choices.empty()&&std::none_of(choices.begin(),choices.end(),[&](const auto& c){return c.first==default_value;}))default_value=fallback;
        auto display=[&](const std::string& v) {
            for(const auto& c:choices)if(c.first==v)return c.second;
            return v.empty()?std::string("无"):v;
        };
        for(;;) {
            std::cout<<"\n"<<label<<'\n';
            for(std::size_t n=0;n<choices.size();++n)std::cout<<n+1<<". "<<visible(choices[n].second)<<'\n';
            std::cout<<"默认："<<visible(display(default_value))<<"\n> "<<std::flush;
            try {
                auto value=line();
                bool use_default=value.empty();
                if(value==":cancel"||(!choices.empty()&&value=="0"))throw Cancel{};
                if(value==":back") {
                    if(cursor==0){std::cout<<"已经是第一步；可输入 :cancel 返回菜单。\n";continue;}
                    accepted.resize(cursor-1);throw Back{};
                }
                if(value==":clear")value.clear();
                else if(value.empty())value=default_value;
                else if(value.starts_with("::"))value.erase(0,1);
                if(!use_default&&!choices.empty()&&!value.empty()&&value.find_first_not_of("0123456789")==std::string::npos)
                    value=choices.at(integer(value,1,choices.size())-1).first;
                validate(value);
                remembered[key]=value;
                accepted.push_back({key,label,value,display(value)});++cursor;
                return value;
            } catch(const std::exception& e) {std::cout<<"输入有误："<<visible(e.what())<<'\n';}
        }
    }
    std::string choose(const std::string& key,const std::string& label,const Choices& choices,const std::string& fallback,Check check={}) {
        return ask(key,label,fallback,check,false,choices);
    }
    bool yes(const std::string& key,const std::string& label,bool fallback=false) {
        return choose(key,label,{{"no","否"},{"yes","是"}},fallback?"yes":"no")=="yes";
    }
    std::string number(const std::string& key,const std::string& label,const std::string& fallback,std::uint64_t min,std::uint64_t max) {
        return ask(key,label,fallback,[=](const auto& v){integer(v,min,max);});
    }
    std::string duration(const std::string& key,const std::string& label,const std::string& fallback,bool zero) {
        return ask(key,label,fallback,[=](const auto& v){milliseconds(v,zero);});
    }
};
void input_file(const std::string& value) {
    if(!std::filesystem::is_regular_file(value)||access(value.c_str(),R_OK)!=0)
        throw Error(Exit::usage,"请选择存在且可读取的普通文件");
}
void output_file(const std::string& value) {
    if(value.empty())return;
    if(std::filesystem::exists(std::filesystem::symlink_status(value)))throw Error(Exit::usage,"目标已存在，请换一个文件名");
    auto parent=std::filesystem::path(value).parent_path();if(parent.empty())parent=".";
    if(!std::filesystem::is_directory(parent)||access(parent.c_str(),W_OK)!=0)throw Error(Exit::usage,"输出目录不存在或不可写");
}
void protocol_file(const std::string& value) {
    if(value.empty())return;
    input_file(value);Protocol::validate(read_json(value));
}
void hex_input(const std::string& value,std::size_t max) {
    auto bytes=unhex(value);
    if(bytes.empty()||bytes.size()>max)throw Error(Exit::usage,"十六进制字节数必须为 1～"+std::to_string(max));
}
struct Plan {
    std::vector<std::string> args{"serialspark"};
    std::vector<std::string> notes;
    void option(const std::string& name,const std::string& value) {args.push_back(name);args.push_back(value);}
    void optional(const std::string& name,const std::string& value) {if(!value.empty())option(name,value);}
    void flag(const std::string& name,bool enable) {if(enable)args.push_back(name);}
};
Choices profiles() {
    Choices result;
    for(const auto& name:list_named("profiles"))result.push_back({name.get<std::string>(),name.get<std::string>()});
    return result;
}
std::string select_profile(Form& f,const std::string& key="profile",bool validate=true) {
    auto choices=profiles();
    if(choices.empty())throw Error(Exit::usage,"还没有保存的串口配置；请使用手动连接或先保存配置");
    return f.choose(key,"选择已保存的串口配置",choices,choices.front().first,[=](const auto& name){if(validate)profile_config(read_json(named_path("profiles",name)));});
}
SerialConfig communication(Form& f,Plan& plan,const Json& ports) {
    SerialConfig config;
    auto source=f.choose("connection","如何设置连接",{{"manual","选择串口并填写参数"},{"profile","复用已保存的串口配置"}},"manual",[](const auto& v){if(v=="profile"&&profiles().empty())throw Error(Exit::usage,"还没有保存的配置，请选择手动设置连接");});
    std::string key="manual.";
    if(source=="profile") {
        auto name=select_profile(f);
        config=SerialConfig::from_json(profile_config(read_json(named_path("profiles",name))));
        plan.option("--profile",name);key=name+".";
    } else {
        Choices choices{{"manual","手动输入串口路径或稳定别名"}};
        for(const auto& port:ports) {
            auto path=port.at("port").get<std::string>();
            auto label=path+" — "+port.at("access").value("reason","");
            if(!port.at("holders").empty())label+="；有可见占用";
            choices.push_back({path,label});
        }
        auto port=f.choose("port.select","选择串口（列表仅为预检查，连接时确认可用性）",choices,"manual");
        if(port=="manual")port=f.ask("port.path","串口路径，例如 /dev/ttyUSB0","",[](const auto& v){SerialConfig c;c.port=v;c.validate();});
        config.port=port;plan.option("--port",port);
    }
    config.baud=static_cast<int>(integer(f.number(key+"baud","波特率（请按设备要求填写，默认值不代表自动识别）",std::to_string(config.baud),1,4000000),1,4000000));
    plan.option("--baud",std::to_string(config.baud));
    auto basic="当前："+std::to_string(config.data_bits)+" 数据位，校验 "+config.parity+"，"+std::to_string(config.stop_bits)+" 停止位，流控 "+config.flow+"；是否修改";
    if(f.yes(key+"serial.advanced",basic)) {
        config.data_bits=static_cast<int>(integer(f.number(key+"bits","数据位",std::to_string(config.data_bits),5,8),5,8));
        config.parity=f.choose(key+"parity","校验位",{{"none","无校验"},{"odd","奇校验"},{"even","偶校验"}},config.parity);
        config.stop_bits=static_cast<int>(integer(f.number(key+"stops","停止位",std::to_string(config.stop_bits),1,2),1,2));
        config.flow=f.choose(key+"flow","流控",{{"none","无流控"},{"rtscts","RTS/CTS 硬件流控"},{"xonxoff","XON/XOFF 软件流控"}},config.flow);
    }
    config.validate();
    plan.option("--data-bits",std::to_string(config.data_bits));plan.option("--parity",config.parity);
    plan.option("--stop-bits",std::to_string(config.stop_bits));plan.option("--flow",config.flow);
    plan.notes.push_back("连接："+config.port+"；"+std::to_string(config.baud)+"，"+std::to_string(config.data_bits)+" 数据位，校验 "+config.parity+"，"+std::to_string(config.stop_bits)+" 停止位，流控 "+config.flow);
    return config;
}
Plan live(Form& f,const std::string& mode,const Json& ports) {
    Plan plan;plan.args.push_back(mode);communication(f,plan,ports);
    if(mode=="send") {
        auto type=f.choose("source","发送方式",{{"text","文本（不解释反斜杠转义）"},{"hex","十六进制字节"},{"file","文件原始字节"}},"text");
        if(type=="text") {
            plan.option("--text",f.ask("text","发送文本"));
        } else if(type=="hex") {
            plan.option("--hex",f.ask("hex","发送十六进制，例如 01 02 FF","",[](const auto& v){hex_input(v,1024*1024);}));
        } else plan.option("--file",f.ask("file","发送文件路径","",input_file));
        if(type=="text")plan.option("--newline",f.choose("newline","文本末尾追加的换行",{{"none","不追加"},{"lf","LF"},{"cr","CR"},{"crlf","CRLF"}},"none"));
    } else if(mode=="session") {
        plan.option("--newline",f.choose("newline","每次发送文本末尾追加的换行",{{"none","不追加"},{"lf","LF"},{"cr","CR"},{"crlf","CRLF"}},"none"));
        plan.notes.push_back("进入会话后普通行发送文本；:send hex/file 发送其他内容，:quit 返回菜单。");
    }
    auto protocol=f.ask("protocol","协议 JSON 路径（可留空，不启用实时解析）","",protocol_file,true);plan.optional("--protocol",protocol);
    if(mode=="send") {
        auto reply=f.choose("reply","回复条件",{{"none","无需等待回复"},{"first",protocol.empty()?"等待首个收到的数据":"等待首个有效 RX 协议帧"},{"hex","等待匹配的十六进制字节串"}},"none");
        plan.flag("--wait-reply",reply=="first");
        if(reply=="hex")plan.option("--reply-hex",f.ask("reply.hex","回复匹配字节串（可以跨接收块）","",[](const auto& v){hex_input(v,65536);}));
        if(reply!="none")plan.option("--reply-timeout",f.duration("reply.timeout","回复超时，例如 1s 或 500ms","1s",false));
        plan.notes.push_back("发送成功表示驱动接受数据；设备回复和协议解析结果另行展示。");
    }
    auto record=f.ask("record",mode=="capture"?"保存原始记录到新的 .ssr 文件":"原始记录路径（可留空，不保存）","",output_file,mode!="capture");plan.optional("--record",record);
    plan.option("--display",f.choose("display","数据展示",{{"both","同时显示十六进制与文本"},{"hex","十六进制"},{"text","文本"}},"both"));
    plan.option("--duration",f.duration("duration","最长运行时长，例如 30s；0 表示不限，Ctrl+C 停止后返回菜单","0",true));
    if(f.yes("advanced","是否调整高级设置（超时、记录分段和配额）")) {
        if(mode!="capture")plan.option("--write-timeout",f.duration("write.timeout","写入无进展与排空超时","2s",false));
        if(mode=="send")plan.flag("--drain",f.yes("drain","是否等待驱动输出队列排空（不能证明设备已收到）"));
        if(!record.empty()) {
            plan.option("--rotate-bytes",f.number("rotate","记录分段阈值，字节（默认 1 GiB）","1073741824",65536,std::numeric_limits<std::uint64_t>::max()));
            plan.option("--quota-bytes",f.number("quota","记录磁盘配额，字节；0 表示不限，达到配额时停止采集","0",0,std::numeric_limits<std::uint64_t>::max()));
        }
    }
    return plan;
}
std::string profile_name(Form& f) {
    return f.ask("profile.name","保存后的配置名称（只保存串口通信参数）","",[](const auto& v){named_path("profiles",v);});
}
void replacement(Form& f,Plan& plan,const std::string& name) {
    if(std::filesystem::exists(named_path("profiles",name))) {
        if(!f.yes("profile.replace","同名配置已存在，是否覆盖"))throw Cancel{};
        plan.flag("--replace",true);
    }
}
Plan profile_task(Form& f) {
    Plan plan;plan.args.push_back("profile");
    auto operation=f.choose("profile.operation","管理常用串口配置",{{"list","列出保存的配置"},{"show","查看配置内容"},{"save","保存一组串口参数"},{"import","导入配置 JSON"},{"export","导出配置 JSON"},{"delete","删除保存的配置"}},"list");
    plan.args.push_back(operation);
    plan.notes.push_back("配置管理不打开设备；保存内容只包含串口参数。");
    if(operation=="list")return plan;
    if(operation=="save") {
        communication(f,plan,discover_ports());auto name=profile_name(f);replacement(f,plan,name);
        plan.args.push_back("--");plan.args.push_back(name);
    } else if(operation=="import") {
        auto path=f.ask("profile.input","要导入的配置 JSON 路径","",[](const auto& v){input_file(v);profile_config(read_json(v));});
        auto name=profile_name(f);replacement(f,plan,name);
        plan.args.push_back("--");plan.args.push_back(name);plan.args.push_back(std::filesystem::absolute(path).string());
    } else {
        auto name=select_profile(f,"profile.selected",false);
        plan.args.push_back("--");plan.args.push_back(name);
        if(operation=="export")plan.args.push_back(std::filesystem::absolute(f.ask("profile.output","配置导出到新的 JSON 文件路径","",output_file)).string());
        if(operation=="delete")plan.notes.push_back("执行后删除保存的配置："+name+"；不会删除记录或设备数据。");
    }
    return plan;
}
std::int64_t timestamp(const std::string& value) {
    // Explicit offsets avoid interpreting business times using the host's timezone.
    if(value.size()<20||value[4]!='-'||value[7]!='-'||value[10]!='T'||value[13]!=':'||value[16]!=':')
        throw Error(Exit::usage,"时间格式为 YYYY-MM-DDTHH:MM:SS[.小数]Z 或末尾 +08:00 等时区偏移");
    std::tm parsed{};
    parsed.tm_year=static_cast<int>(integer(value.substr(0,4),1970,9999))-1900;
    parsed.tm_mon=static_cast<int>(integer(value.substr(5,2),1,12))-1;
    parsed.tm_mday=static_cast<int>(integer(value.substr(8,2),1,31));
    parsed.tm_hour=static_cast<int>(integer(value.substr(11,2),0,23));
    parsed.tm_min=static_cast<int>(integer(value.substr(14,2),0,59));
    parsed.tm_sec=static_cast<int>(integer(value.substr(17,2),0,59));
    auto original=parsed;auto seconds=timegm(&parsed);
    if(parsed.tm_year!=original.tm_year||parsed.tm_mon!=original.tm_mon||parsed.tm_mday!=original.tm_mday)
        throw Error(Exit::usage,"日期不存在");
    std::size_t zone=19;std::int64_t fraction=0;
    if(value[zone]=='.') {
        auto end=value.find_first_of("Z+-",zone+1);
        if(end==std::string::npos||end==zone+1||end-zone-1>9)throw Error(Exit::usage,"秒的小数部分须为 1～9 位数字，随后填写时区");
        fraction=integer(value.substr(zone+1,end-zone-1),0,999999999);
        for(std::size_t n=end-zone-1;n<9;++n)fraction*=10;
        zone=end;
    }
    if(value.substr(zone)!="Z") {
        if(value.size()!=zone+6||(value[zone]!='+'&&value[zone]!='-')||value[zone+3]!=':')throw Error(Exit::usage,"请填写 Z 或 +08:00 等明确时区");
        auto offset=integer(value.substr(zone+1,2),0,23)*3600+integer(value.substr(zone+4,2),0,59)*60;
        seconds+=value[zone]=='+'?-static_cast<std::int64_t>(offset):static_cast<std::int64_t>(offset);
    }
    if(seconds<0||seconds>(INT64_MAX-fraction)/1000000000)throw Error(Exit::usage,"时间超出可表示的非负 Unix 纳秒范围");
    return seconds*1000000000+fraction;
}
std::string field_condition(Form& f,const std::string& protocol) {
    auto mode=f.choose("filter.field.mode","是否按解析字段筛选",{{"none","不筛选字段"},{"guided","选择字段、比较方式和值"},{"expression","直接填写高级比较表达式"}},"none");
    if(mode=="none")return "";
    if(mode=="expression")return f.ask("filter.expression","字段表达式，例如 temperature > 24","",validate_field_filter);
    auto config=read_json(protocol);std::set<std::string> names;
    auto collect=[&](const Json& j){if(j.contains("fields"))for(const auto& field:j.at("fields"))names.insert(field.at("name").get<std::string>());};
    collect(config);for(const auto& direction:{"tx","rx"})if(config.contains(direction))collect(config.at(direction));
    Choices fields;for(const auto& name:names)fields.push_back({name,name});
    std::string name;
    if(fields.empty())name=f.ask("filter.field.name","协议未声明字段，请填写要比较的字段名");
    else name=f.choose("filter.field.name","选择解析字段",fields,fields.front().first);
    auto op=f.choose("filter.field.op","比较方式",{{"==","等于"},{"!=","不等于"},{">","大于"},{">=","大于等于"},{"<","小于"},{"<=","小于等于"}},"==");
    auto prefix=Json(name).dump()+" "+op+" ";
    auto value=f.ask("filter.field.value","比较值：JSON 数字、布尔值或带双引号的字符串；64 位整数使用精确十进制字符串","",[&](const auto& v){validate_field_filter(prefix+v);});
    return prefix+value;
}
Plan logs(Form& f) {
    Plan plan;plan.args.push_back("log");
    auto operation=f.choose("log.operation","如何处理已有记录",{{"info","查看摘要、完整性和分段信息"},{"view","查看原始事件或解析结果"},{"parse","按协议重新解析"},{"export","导出到新文件"}},"info");
    plan.args.push_back(operation);
    auto path=f.ask("log.path","记录首段 .ssr 文件路径","",[](const auto& v){input_file(v);log_info(v);});
    plan.args.push_back(path);
    plan.notes.push_back("读取已有记录，不打开串口；原始记录保持不变。");
    if(operation=="info")return plan;
    std::string format="human";
    if(operation=="export")format=f.choose("log.format","导出格式",{{"jsonl","JSONL：保留事件结构或解析结果"},{"csv","CSV：解析字段表格（需要协议）"},{"bin","BIN：单方向原始字节（不包含时间，不解析协议）"}},"jsonl");
    std::string protocol;
    if(format!="bin") {
        bool required=operation=="parse"||format=="csv";
        protocol=f.ask("log.protocol",required?"用于解析的协议 JSON 路径":"协议 JSON 路径（留空查看或导出原始事件）","",protocol_file,!required);
        plan.optional("--protocol",protocol);
    }
    plan.option("--format",format);
    auto direction=f.choose("filter.direction","数据方向",format=="bin"?Choices{{"rx","RX：收到的数据"},{"tx","TX：驱动接受的发送数据"}}:Choices{{"all","全部方向"},{"rx","RX：收到的数据"},{"tx","TX：驱动接受的发送数据"}},format=="bin"?"rx":"all");
    plan.option("--direction",direction);
    auto content=f.choose("filter.content","是否按原始字节内容筛选",{{"none","不筛选内容"},{"text","包含指定 UTF-8 文本"},{"hex","包含指定十六进制字节"}},"none");
    if(content=="text")plan.option("--contains-text",f.ask("filter.text","包含的文本","",[](const auto& v){if(v.size()>65536)throw Error(Exit::usage,"内容筛选超过 64 KiB 上限");}));
    if(content=="hex")plan.option("--contains-hex",f.ask("filter.hex","包含的十六进制字节","",[](const auto& v){hex_input(v,65536);}));
    if(f.yes("filter.time","是否限制时间范围（包含边界）")) {
        auto from=f.ask("filter.from","开始时间，YYYY-MM-DDTHH:MM:SS[.小数]+08:00 或 Z（可留空）","",[](const auto& v){if(!v.empty())timestamp(v);},true);
        auto start=from.empty()?std::int64_t(0):timestamp(from);
        auto to=f.ask("filter.to","结束时间，同样填写明确时区（可留空）","",[&](const auto& v){if(!v.empty()&&timestamp(v)<start)throw Error(Exit::usage,"结束时间不能早于开始时间");},true);
        if(!from.empty())plan.option("--from-ns",std::to_string(start));
        if(!to.empty())plan.option("--to-ns",std::to_string(timestamp(to)));
    }
    if(!protocol.empty())plan.optional("--where",field_condition(f,protocol));
    if(operation=="export")plan.option("--output",f.ask("log.output","导出到新的文件路径","",output_file));
    else plan.option("--display",f.choose("log.display","数据展示",{{"both","同时显示十六进制与文本"},{"hex","十六进制"},{"text","文本"}},"both"));
    plan.notes.push_back(protocol.empty()?"内容筛选作用于单个原始事件。":"先按完整字节流组帧，再按消息、内容与字段筛选。");
    if(format=="bin")plan.notes.push_back("BIN 仅导出所选方向的原始数据字节，不保留事件时间。");
    return plan;
}
std::string quote(const std::string& text) {
    bool control=std::any_of(text.begin(),text.end(),[](unsigned char ch){return ch<32||ch==127;});
    if(control) {
        // Bash/zsh ANSI-C quoting keeps control bytes visible and the copied argument exact.
        std::string result="$'";
        constexpr char digits[]="0123456789abcdef";
        for(unsigned char ch:text) {
            if(ch<32||ch==127){result+="\\x";result+=digits[ch>>4];result+=digits[ch&15];}
            else {if(ch=='\\'||ch=='\'')result+='\\';result+=static_cast<char>(ch);}
        }
        return result+"'";
    }
    std::string result="'";for(char ch:text){if(ch=='\'')result+="'\\''";else result+=ch;}return result+"'";
}
void command(const Plan& plan) {
    std::cout<<"\n等价命令（仅展示，执行不经过 Shell）：\nserialspark";
    for(std::size_t n=1;n<plan.args.size();++n)std::cout<<' '<<quote(plan.args[n]);
    std::cout<<'\n';
}
std::string action(const std::string& prompt,const Choices& choices,const std::string& fallback) {
    Form f;return f.choose("action",prompt,choices,fallback);
}
void task(const std::function<Plan(Form&)>& build) {
    Form f;
    for(;;) {
        try {
            f.cursor=0;auto plan=build(f);
            f.accepted.resize(f.cursor);
            for(;;) {
                std::cout<<"\n执行前摘要\n";
                for(const auto& note:plan.notes)std::cout<<visible(note)<<'\n';
                for(const auto& a:f.accepted)std::cout<<a.label<<"："<<visible(a.display)<<'\n';
                auto next=action("请选择下一步",{{"run","执行"},{"edit","返回修改"},{"command","查看等价命令"},{"cancel","取消并返回菜单"}},"run");
                if(next=="cancel")throw Cancel{};
                if(next=="command"){command(plan);continue;}
                if(next=="edit") {
                    Choices fields;for(std::size_t n=0;n<f.accepted.size();++n)fields.push_back({f.accepted[n].key,f.accepted[n].label});
                    if(fields.empty())continue;
                    auto key=action("选择要修改的步骤",fields,fields.front().first);
                    auto it=std::find_if(f.accepted.begin(),f.accepted.end(),[&](const auto& a){return a.key==key;});
                    f.accepted.erase(it,f.accepted.end());throw Back{};
                }
                std::cout.flush();
                auto args=plan.args;std::vector<char*> argv;
                for(auto& arg:args)argv.push_back(arg.data());
                argv.push_back(nullptr);
                auto result=run_cli(static_cast<int>(args.size()),argv.data());
                std::cout<<"\n操作结束，退出码："<<result;
                if(result==130)std::cout<<"（已停止）";
                std::cout<<"\n";
                auto again=action("接下来",{{"home","返回主菜单"},{"again","复用本次填写内容，再次操作"}},"home");
                if(again=="home")return;
                f.accepted.clear();throw Back{};
            }
        } catch(const Back&) {continue;}
    }
}
}
int run_menu() {
    if(!isatty(STDIN_FILENO)||!isatty(STDOUT_FILENO))throw Error(Exit::usage,"菜单模式需要交互终端；脚本或管道请使用原有子命令");
    MenuSignals signals;
    for(;;) {
        try {
            std::cout<<"\nSerialSpark — 任务菜单\n"
                     <<"1. 查看串口与连接条件（不打开设备）\n"
                     <<"2. 发送一次数据\n3. 进入交互收发\n4. 接收数据并保存记录\n5. 查看、解析或导出已有记录\n6. 管理常用串口配置\n0. 退出\n"
                     <<"引导中：回车接受默认值，:back 返回上一步，:cancel 返回菜单，:clear 清空可选项。\n"
                     <<"输入以 :: 开头可发送字面量冒号命令，例如 ::cancel。\n> "<<std::flush;
            auto selected=line();if(selected=="0")return 0;
            if(selected=="1") {task([](Form&){Plan p;p.args.push_back("ports");return p;});continue;}
            if(selected=="2"||selected=="3"||selected=="4") {
                auto mode=selected=="2"?"send":selected=="3"?"session":"capture";
                auto ports=discover_ports();task([&](Form& f){return live(f,mode,ports);});continue;
            }
            if(selected=="5"){task(logs);continue;}
            if(selected=="6"){task(profile_task);continue;}
            std::cout<<"请选择菜单中的编号。\n";
        } catch(const Cancel&) {
            if(menu_signal){auto signal=menu_signal;if(signal==SIGTERM)return 128+SIGTERM;menu_signal=0;}
            std::cout<<"\n已取消，返回主菜单。\n";
        } catch(const EndInput&) {std::cout<<"\n输入结束，退出菜单。\n";return 0;}
        catch(const std::exception& e) {std::cout<<"操作失败："<<visible(e.what())<<"；返回主菜单。\n";}
    }
}
}
