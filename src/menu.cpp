#include "serialspark/menu.hpp"
#include "serialspark/cli.hpp"
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
std::string select_profile(Form& f,const std::string& key="profile") {
    auto choices=profiles();
    if(choices.empty())throw Error(Exit::usage,"还没有保存的串口配置；请使用手动连接或先保存配置");
    return f.choose(key,"选择已保存的串口配置",choices,choices.front().first,[](const auto& name){profile_config(read_json(named_path("profiles",name)));});
}
SerialConfig communication(Form& f,Plan& plan,const Json& ports) {
    SerialConfig config;
    auto source=f.choose("connection","如何设置连接",{{"manual","选择串口并填写参数"},{"profile","复用已保存的串口配置"}},"manual");
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
                     <<"2. 发送一次数据\n3. 进入交互收发\n4. 接收数据并保存记录\n0. 退出\n"
                     <<"引导中：回车接受默认值，:back 返回上一步，:cancel 返回菜单，:clear 清空可选项。\n"
                     <<"输入以 :: 开头可发送字面量冒号命令，例如 ::cancel。\n> "<<std::flush;
            auto selected=line();if(selected=="0")return 0;
            if(selected=="1") {task([](Form&){Plan p;p.args.push_back("ports");return p;});continue;}
            if(selected=="2"||selected=="3"||selected=="4") {
                auto mode=selected=="2"?"send":selected=="3"?"session":"capture";
                auto ports=discover_ports();task([&](Form& f){return live(f,mode,ports);});continue;
            }
            std::cout<<"请选择菜单中的编号。\n";
        } catch(const Cancel&) {
            if(menu_signal){auto signal=menu_signal;if(signal==SIGTERM)return 128+SIGTERM;menu_signal=0;}
            std::cout<<"\n已取消，返回主菜单。\n";
        } catch(const EndInput&) {std::cout<<"\n输入结束，退出菜单。\n";return 0;}
        catch(const std::exception& e) {std::cout<<"操作失败："<<visible(e.what())<<"；返回主菜单。\n";}
    }
}
}
