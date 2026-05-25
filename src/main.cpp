#include "serialspark/session.hpp"
#include "serialspark/protocol.hpp"
#include "serialspark/record.hpp"
#include "serialspark/commands.hpp"
#include <CLI/CLI.hpp>
#include <iostream>
#include <fstream>
#include <charconv>
#include <csignal>
#include <sys/stat.h>

namespace {
using namespace ss;
std::filesystem::path config_dir(const std::string& category) {
    const char* xdg=getenv("XDG_CONFIG_HOME");const char* home=getenv("HOME");
    if((!xdg||!*xdg)&&(!home||!*home))throw Error(Exit::usage,"请设置 HOME 或 XDG_CONFIG_HOME");
    auto root=xdg&&*xdg?std::filesystem::path(xdg):std::filesystem::path(home)/".config";
    return root/"serialspark"/category;
}
std::filesystem::path named_path(const std::string& category,const std::string& name) {
    if(name.empty()||name.size()>128||name=="."||name==".."||name.find('/')!=std::string::npos||name.find('\\')!=std::string::npos)throw Error(Exit::usage,"名称必须为 1～128 字节，不能含路径分隔符");
    for(unsigned char b:name)if(b<32||b==127)throw Error(Exit::usage,"名称含控制字符");
    return config_dir(category)/(name+".json");
}
void save_named(const std::string& category,const std::string& name,const Json& j,bool replace) {
    auto path=named_path(category,name);std::filesystem::create_directories(path.parent_path());
    if(!replace&&std::filesystem::exists(path))throw Error(Exit::usage,"名称已存在；明确使用 --replace 才能覆盖");
    write_json(path,j);
}
Json profile_config(const Json& j) {
    if(!j.is_object()||j.value("schema_version",0)!=1||!j.contains("config"))throw Error(Exit::usage,"通信配置应为 schema_version:1 和 config 对象");
    for(auto i=j.begin();i!=j.end();++i)if(i.key()!="schema_version"&&i.key()!="config")throw Error(Exit::usage,"未知配置键: "+i.key());
    return SerialConfig::from_json(j["config"]).json();
}
Json list_named(const std::string& category) {
    Json names=Json::array();auto path=config_dir(category);if(!std::filesystem::exists(path))return names;
    for(const auto& f:std::filesystem::directory_iterator(path))if(f.is_regular_file()&&f.path().extension()==".json")names.push_back(f.path().stem().string());
    std::sort(names.begin(),names.end());return names;
}
std::int64_t milliseconds(const std::string& input,bool allow_zero=false) {
    std::size_t n=input.find_first_not_of("0123456789");auto digits=input.substr(0,n);auto unit=n==std::string::npos?std::string("ms"):input.substr(n);
    std::uint64_t value=0;auto [end,ec]=std::from_chars(digits.data(),digits.data()+digits.size(),value);
    std::uint64_t multiplier=unit=="ms"?1:unit=="s"?1000:unit=="m"?60000:unit=="h"?3600000:0;
    if(ec!=std::errc{}||end!=digits.data()+digits.size()||!multiplier||value>1000000000ull/multiplier||(!value&&!allow_zero))throw Error(Exit::usage,"时长必须为整数 ms/s/m/h，最大 1000000000ms；超时须大于零");
    return value*multiplier;
}
struct Communication {
    SerialConfig requested;
    std::string profile;
    CLI::Option *p=nullptr,*b=nullptr,*d=nullptr,*y=nullptr,*s=nullptr,*f=nullptr;
    void attach(CLI::App* app) {
        app->add_option("--profile",profile,"复用命名配置；显式参数覆盖配置");
        p=app->add_option("--port",requested.port,"串口节点或稳定别名");b=app->add_option("--baud",requested.baud,"波特率，默认 115200");
        d=app->add_option("--data-bits",requested.data_bits,"数据位 5～8，默认 8");
        y=app->add_option("--parity",requested.parity,"none/odd/even，默认 none")->check(CLI::IsMember({"none","odd","even"}));
        s=app->add_option("--stop-bits",requested.stop_bits,"停止位 1/2，默认 1");
        f=app->add_option("--flow",requested.flow,"none/rtscts/xonxoff，默认 none")->check(CLI::IsMember({"none","rtscts","xonxoff"}));
    }
    SerialConfig resolve() const {
        SerialConfig c=profile.empty()?SerialConfig{}:SerialConfig::from_json(profile_config(read_json(named_path("profiles",profile))));
        if(p->count())c.port=requested.port;
        if(b->count())c.baud=requested.baud;
        if(d->count())c.data_bits=requested.data_bits;
        if(y->count())c.parity=requested.parity;
        if(s->count())c.stop_bits=requested.stop_bits;
        if(f->count())c.flow=requested.flow;
        c.validate();return c;
    }
};
struct LiveArgs {
    Communication communication;
    SessionOptions options;
    std::string text,hex_value,reply_hex,write_timeout="2s",reply_timeout="1s",duration="0ms";
    CLI::Option *text_option=nullptr,*hex_option=nullptr,*file_option=nullptr,*stdin_option=nullptr,*reply_option=nullptr;
    void attach(CLI::App* app,const std::string& mode) {
        options.mode=mode;communication.attach(app);
        app->add_option("--format",options.format,"human/jsonl；json 为 JSONL 别名")->check(CLI::IsMember({"human","json","jsonl"}));
        app->add_option("--display",options.display,"hex/text/both，默认 both")->check(CLI::IsMember({"hex","text","both"}));
        app->add_option("--record",options.record,"新建原始记录，不覆盖已有文件");app->add_option("--protocol",options.protocol,"实时解析的协议 JSON 文件");
        app->add_option("--rotate-bytes",options.rotate_bytes,"记录分段阈值，字节；默认 1 GiB");app->add_option("--quota-bytes",options.quota_bytes,"磁盘配额，0 不限制；不删除历史");
        app->add_option("--duration",duration,"会话最大时长，整数 ms/s/m/h；0 为无限");
        if(mode!="capture") {
            app->add_option("--newline",options.newline,"文本追加 none/lf/cr/crlf，默认 none")->check(CLI::IsMember({"none","lf","cr","crlf"}));
            app->add_option("--write-timeout",write_timeout,"写入无进展与排空超时，默认 2s");
        }
        if(mode=="send") {
            text_option=app->add_option("--text",text,"发送文本，不解释反斜杠转义");hex_option=app->add_option("--hex",hex_value,"发送十六进制字节");
            file_option=app->add_option("--file",options.file,"分块发送文件原始内容");stdin_option=app->add_flag("--stdin",options.stdin_source,"读取标准输入原始字节");
            app->add_flag("--wait-reply",options.wait_reply,"等待首个 RX 数据；配置协议时等待首个有效 RX 帧");
            reply_option=app->add_option("--reply-hex",reply_hex,"等待匹配字节串，可跨接收块；隐含 --wait-reply");
            app->add_option("--reply-timeout",reply_timeout,"完整写入后等待回复，默认 1s");app->add_flag("--drain",options.drain,"等待驱动报告输出队列排空");
        }
    }
    int run() {
        options.config=communication.resolve();options.duration_ms=milliseconds(duration,true);
        options.write_timeout_ms=milliseconds(write_timeout);options.reply_timeout_ms=milliseconds(reply_timeout);
        if(options.mode=="send") {
            if(text_option->count()+hex_option->count()+file_option->count()+stdin_option->count()!=1)throw Error(Exit::usage,"必须且只能选择 --text、--hex、--file、--stdin 之一");
            if(text_option->count()) {
                options.payload=Bytes(text.begin(),text.end());if(options.newline=="lf"||options.newline=="crlf"){if(options.newline=="crlf")options.payload.push_back('\r');options.payload.push_back('\n');}else if(options.newline=="cr")options.payload.push_back('\r');
            } else if(hex_option->count())options.payload=unhex(hex_value);
            if(options.payload.size()>1024*1024)throw Error(Exit::usage,"内联发送超过 1 MiB，请使用文件");
            if(reply_option->count()) {options.reply_pattern=unhex(reply_hex);if(options.reply_pattern.empty()||options.reply_pattern.size()>65536)throw Error(Exit::usage,"回复匹配字节长度必须为 1～65536");options.wait_reply=true;}
        }
        return run_session(options);
    }
};
}
int main(int argc,char** argv) {
    umask(0077);signal(SIGPIPE,SIG_IGN);
    CLI::App app{"SerialSpark 串口通信与协议调试工具；完整帮助见各子命令 --help"};
    app.set_version_flag("--version","SerialSpark 0.1.0");app.require_subcommand(1);
    std::string format="human";
    auto ports=app.add_subcommand("ports","列出串口、权限预检查与可见占用（不打开设备）");ports->add_option("--format",format,"human/json/jsonl")->check(CLI::IsMember({"human","json","jsonl"}));
    ports->callback([&]{Output out(format);out.emit({{"ports",discover_ports()}});out.finish();});
    auto profile=app.add_subcommand("profile","管理命名通信配置");profile->require_subcommand(1);
    Communication saved;std::string save_name;bool replace=false;
    auto save=profile->add_subcommand("save","保存参数，无需连接设备");saved.attach(save);save->add_option("name",save_name)->required();save->add_flag("--replace",replace,"覆盖同名配置");
    save->callback([&]{auto c=saved.resolve();save_named("profiles",save_name,{{"schema_version",1},{"config",c.json()}},replace);std::cout<<named_path("profiles",save_name)<<'\n';});
    auto listing=profile->add_subcommand("list","列出名称");listing->callback([&]{std::cout<<list_named("profiles").dump(2)<<'\n';});
    std::string show_name;auto show=profile->add_subcommand("show","查看参数");show->add_option("name",show_name)->required();show->callback([&]{std::cout<<read_json(named_path("profiles",show_name)).dump(2)<<'\n';});
    std::string delete_name;auto remove=profile->add_subcommand("delete","删除配置");remove->add_option("name",delete_name)->required();remove->callback([&]{if(!std::filesystem::remove(named_path("profiles",delete_name)))throw Error(Exit::usage,"配置不存在");});
    std::string import_name,import_file;bool import_replace=false;auto import=profile->add_subcommand("import","导入 JSON");import->add_option("name",import_name)->required();import->add_option("file",import_file)->required();import->add_flag("--replace",import_replace);import->callback([&]{auto c=profile_config(read_json(import_file));save_named("profiles",import_name,{{"schema_version",1},{"config",c}},import_replace);});
    std::string export_name,export_file;auto export_profile=profile->add_subcommand("export","导出 JSON");export_profile->add_option("name",export_name)->required();export_profile->add_option("file",export_file)->required();export_profile->callback([&]{if(std::filesystem::exists(export_file))throw Error(Exit::usage,"导出目标已存在");write_json(export_file,read_json(named_path("profiles",export_name)));});
    LiveArgs send_args,session_args,capture_args;int result=0;
    auto send=app.add_subcommand("send","单次发送文本/十六进制/文件/管道；成功表示驱动接受，回复另行确认");send_args.attach(send,"send");send->callback([&]{result=send_args.run();});
    auto session=app.add_subcommand("session","交互收发；普通行发送文本，:send hex/file，:status，:connect，:disconnect，:quit");session_args.attach(session,"session");session->callback([&]{result=session_args.run();});
    auto capture=app.add_subcommand("capture","只接收、显示与记录；Ctrl+C 停止");capture_args.attach(capture,"capture");capture->callback([&]{result=capture_args.run();});
    add_analysis_commands(app,result);
    try {app.parse(argc,argv);return result;}
    catch(const CLI::ParseError& e){app.exit(e);return e.get_exit_code()==0?0:static_cast<int>(Exit::usage);}
    catch(const Error& e){std::cerr<<e.what()<<'\n';return static_cast<int>(e.code);}
    catch(const Json::exception& e){std::cerr<<"配置或数据格式错误: "<<e.what()<<'\n';return static_cast<int>(Exit::usage);}
    catch(const std::exception& e){std::cerr<<"操作失败: "<<e.what()<<'\n';return static_cast<int>(Exit::io);}
}
