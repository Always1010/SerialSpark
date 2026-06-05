#include "serialspark/commands.hpp"
#include "serialspark/protocol.hpp"
#include "serialspark/record.hpp"
#include "serialspark/session.hpp"
#include <fstream>
#include <set>

namespace ss {
namespace {
struct Filter {
    std::string direction="all",contains_hex,contains_text,where;
    std::int64_t from=0,to=INT64_MAX;
    Bytes pattern;
    std::string field,op;
    Json expected;
    void attach(CLI::App* app) {
        app->add_option("--direction",direction,"all/tx/rx，默认 all")->check(CLI::IsMember({"all","tx","rx"}));
        app->add_option("--from-ns",from,"包含的起始墙上时间，Unix 纳秒");app->add_option("--to-ns",to,"包含的结束墙上时间，Unix 纳秒");
        app->add_option("--contains-hex",contains_hex,"按原始字节内容筛选");app->add_option("--contains-text",contains_text,"按原始 UTF-8 字节内容筛选");
        app->add_option("--where",where,"解析字段比较，例如 temperature > 30 或 state == \"active\"");
    }
    void prepare(bool parsed) {
        if(from<0||to<from)throw Error(Exit::usage,"时间范围必须非负且结束不早于开始");
        if(!contains_hex.empty()&&!contains_text.empty())throw Error(Exit::usage,"内容筛选只能选择十六进制或文本之一");
        pattern=contains_hex.empty()?Bytes(contains_text.begin(),contains_text.end()):unhex(contains_hex);
        if(pattern.size()>65536)throw Error(Exit::usage,"内容筛选超过 64 KiB 上限");
        if(!where.empty()) {
            if(!parsed)throw Error(Exit::usage,"--where 必须配合 --protocol 解析后使用");
            if(where.size()>4096)throw Error(Exit::usage,"字段筛选表达式超过上限");
            auto trim=[](std::string s){auto first=s.find_first_not_of(" \t\r\n");if(first==std::string::npos)return std::string{};auto last=s.find_last_not_of(" \t\r\n");return s.substr(first,last-first+1);};
            auto expression=trim(where);std::string remainder;
            if(!expression.empty()&&expression.front()=='"') {
                bool escape=false;std::size_t end=1;
                for(;end<expression.size();++end){char ch=expression[end];if(escape)escape=false;else if(ch=='\\')escape=true;else if(ch=='"')break;}
                if(end==expression.size())throw Error(Exit::usage,"字段名的 JSON 引号不完整");
                try{field=Json::parse(expression.substr(0,end+1)).get<std::string>();}catch(const Json::exception&){throw Error(Exit::usage,"字段名 JSON 字符串错误");}
                remainder=trim(expression.substr(end+1));
            } else {
                auto end=expression.find_first_of("=!<>");if(end==std::string::npos)throw Error(Exit::usage,"字段筛选缺少比较符");
                field=trim(expression.substr(0,end));remainder=trim(expression.substr(end));
            }
            for(auto candidate:{"==","!=",">=","<=",">","<"})if(remainder.starts_with(candidate)){op=candidate;break;}
            if(field.empty()||op.empty())throw Error(Exit::usage,"字段筛选格式: 字段名 比较符 JSON值");
            try{expected=Json::parse(trim(remainder.substr(op.size())));}catch(const Json::exception&){throw Error(Exit::usage,"字段比较值必须是 JSON 数字、字符串或布尔值");}
            if(!expected.is_primitive()||expected.is_null())throw Error(Exit::usage,"字段比较仅支持数字、字符串或布尔值");
            if(!expected.is_number()&&op!="=="&&op!="!=")throw Error(Exit::usage,"字符串及布尔值只支持 == 或 !=；64 位整数按精确字符串比较");
        }
    }
    bool accepts(const Json& j) const {
        if(direction!="all"&&j.value("direction","")!=direction)return false;
        auto time=j.value("wall_ns",std::int64_t{});if(time<from||time>to)return false;
        if(!pattern.empty()) {auto bytes=unhex(j.value("raw_hex",""));if(std::search(bytes.begin(),bytes.end(),pattern.begin(),pattern.end())==bytes.end())return false;}
        if(field.empty())return true;
        if(!j.contains("fields")||!j["fields"].contains(field))return false;
        const auto& actual=j["fields"][field];
        if(op=="==")return actual==expected;
        if(op=="!=")return actual!=expected;
        if(!actual.is_number())return false;
        auto numeric=[](const Json& v)->long double {if(v.is_number_unsigned())return v.get<std::uint64_t>();if(v.is_number_integer())return v.get<std::int64_t>();return v.get<double>();};
        auto a=numeric(actual),b=numeric(expected);
        return op==">"?a>b:op=="<"?a<b:op==">="?a>=b:a<=b;
    }
};
std::string cell(const Json& value) {
    auto text=value.is_string()?value.get<std::string>():value.is_null()?"":value.dump();
    std::string out="\"";for(char ch:text){if(ch=='"')out+='"';out+=ch;}return out+'"';
}
void csv_row(Output& output,const std::vector<Json>& values) {
    std::string text;for(std::size_t n=0;n<values.size();++n){if(n)text+=',';text+=cell(values[n]);}text+='\n';output.raw(Bytes(text.begin(),text.end()));
}
struct LogArgs {
    std::string path,protocol,format="human",display="both",output;
    Filter filter;
    void attach(CLI::App* app,bool parse,bool export_mode) {
        app->add_option("file",path,"SerialSpark .ssr 首段记录文件")->required();
        auto option=app->add_option("--protocol",protocol,"重新解析使用的协议 JSON 文件");if(parse)option->required();
        app->add_option("--format",format,export_mode?"human/json/jsonl/csv/bin":"human/json/jsonl")->check(export_mode?CLI::IsMember({"human","json","jsonl","csv","bin"}):CLI::IsMember({"human","json","jsonl"}));
        app->add_option("--display",display,"hex/text/both")->check(CLI::IsMember({"hex","text","both"}));
        app->add_option("--output",output,"新建导出文件；默认标准输出，不覆盖");filter.attach(app);
    }
    void run() {
        bool parsed=!protocol.empty();filter.prepare(parsed);
        if(format=="csv"&&!parsed)throw Error(Exit::usage,"CSV 导出需要 --protocol");
        if(format=="bin"&&(parsed||filter.direction=="all"))throw Error(Exit::usage,"BIN 导出不解析协议，必须明确 --direction tx 或 rx；不包含时间元数据");
        std::unique_ptr<Protocol> engine;if(parsed)engine=std::make_unique<Protocol>(read_json(protocol));
        Output out(format,display,output);
        std::vector<std::string> names;
        if(engine) {
            if(format=="csv") {
                std::set<std::string> seen;
                auto add=[&](const Json& j){if(j.contains("fields"))for(const auto& field:j["fields"]){auto name=field.at("name").get<std::string>();if(seen.insert(name).second)names.push_back(name);}};
                add(engine->config());for(auto direction:{"tx","rx"})if(engine->config().contains(direction))add(engine->config()[direction]);
                std::vector<Json> columns={"direction","wall_ns","mono_ns","status","raw_hex","sources","errors","protocol_config"};
                for(const auto& name:names){columns.push_back(name);columns.push_back(name+".unit");}csv_row(out,columns);
            } else out.emit({{"schema_version",1},{"kind","parse_config"},{"config_ref",0},{"parser_version","0.1.0"},{"config",engine->config()}});
        }
        auto frame=[&](Json j) {
            if(!filter.accepts(j))return;
            j["parse_config_ref"]=0;
            if(format!="csv")out.emit(j);
            else {
                std::vector<Json> values={j.value("direction",""),j.value("wall_ns",std::int64_t{}),j.value("mono_ns",std::int64_t{}),j.value("status",""),j["raw_hex"],j["sources"],j["errors"],engine->config()};
                for(const auto& name:names){values.push_back(j["fields"].contains(name)?j["fields"][name]:Json(nullptr));values.push_back(j.contains("units")&&j["units"].contains(name)?j["units"][name]:Json(nullptr));}csv_row(out,values);
            }
        };
        read_log(path,[&](const RawEvent& e) {
            if(engine) {for(auto& j:engine->feed(e))frame(std::move(j));}
            else if(format=="bin") {if(e.kind=="data"&&filter.accepts(event_json(e)))out.raw(e.bytes);}
            else {auto j=event_json(e);if(filter.accepts(j))out.emit(j);}
        });
        if(engine)for(auto& j:engine->finish("end_of_log"))frame(std::move(j));
        out.finish();
    }
};
struct ProtocolTest {
    std::string config_path,input_file,hex_value,format="human",direction="rx";
    CLI::Option* hex_option=nullptr;
    int run() {
        Protocol engine(read_json(config_path));Output output(format);
        if(bool(hex_option->count())==!input_file.empty())throw Error(Exit::usage,"试解析必须且只能选择 --hex 或 --file");
        std::int64_t seq=0;bool errors=false;
        auto show=[&](const std::vector<Json>& frames){for(const auto& f:frames){if(f["status"]=="error"||f["status"]=="incomplete")errors=true;output.emit(f);}};
        auto feed=[&](Bytes b){show(engine.feed({++seq,0,0,"data",direction,std::move(b),Json::object()}));};
        if(hex_option->count()){auto bytes=unhex(hex_value);if(bytes.size()>65536)throw Error(Exit::usage,"试解析十六进制上限为 64 KiB，请改用文件");feed(std::move(bytes));}
        else {
            std::ifstream in(input_file,std::ios::binary);if(!in)throw Error(Exit::io,"无法打开试解析文件");
            char buffer[4096];while(in){in.read(buffer,sizeof(buffer));if(in.gcount())feed(Bytes(buffer,buffer+in.gcount()));}if(!in.eof())throw Error(Exit::io,"试解析文件读取失败");
        }
        show(engine.finish("end_of_input"));output.finish();return errors?static_cast<int>(Exit::protocol):0;
    }
};
}
void add_analysis_commands(CLI::App& app,int& result) {
    auto log=app.add_subcommand("log","只读离线查看、重解析与导出；不会打开串口或发送数据");log->require_subcommand(1);
    auto info=log->add_subcommand("info","查看设备、配置、持久化位置、完整性与分段概要");
    auto info_path=std::make_shared<std::string>();auto info_format=std::make_shared<std::string>("human");
    info->add_option("file",*info_path)->required();info->add_option("--format",*info_format)->check(CLI::IsMember({"human","json","jsonl"}));info->callback([info_path,info_format]{Output out(*info_format);out.emit(log_info(*info_path));out.finish();});
    for(const auto& name:{"view","parse","export"}) {
        auto args=std::make_shared<LogArgs>();bool parse=std::string(name)=="parse",export_mode=std::string(name)=="export";if(export_mode)args->format="jsonl";
        auto command=log->add_subcommand(name,parse?"先组帧解析完整字节流，再按消息与字段筛选":export_mode?"导出 JSONL、CSV 或单方向 BIN；不修改原记录":"查看原始事件；内容筛选作用于单个事件");args->attach(command,parse,export_mode);command->callback([args]{args->run();});
    }
    auto protocol=app.add_subcommand("protocol","验证、试解析与分享声明式协议配置");protocol->require_subcommand(1);
    auto validate_path=std::make_shared<std::string>();auto validate_format=std::make_shared<std::string>("human");auto validate=protocol->add_subcommand("validate","校验协议配置，拒绝未知选项与非法范围");validate->add_option("file",*validate_path)->required();validate->add_option("--format",*validate_format)->check(CLI::IsMember({"human","json","jsonl"}));validate->callback([validate_path,validate_format]{auto j=read_json(*validate_path);Protocol::validate(j);Output out(*validate_format);out.emit({{"valid",true},{"config",j}});out.finish();});
    auto test=protocol->add_subcommand("test","离线试解析原始 HEX 或文件；文件没有采集时间，不模拟帧间超时");auto test_args=std::make_shared<ProtocolTest>();test->add_option("protocol_file",test_args->config_path)->required();test->add_option("--file",test_args->input_file);test_args->hex_option=test->add_option("--hex",test_args->hex_value);test->add_option("--direction",test_args->direction)->check(CLI::IsMember({"tx","rx"}));test->add_option("--format",test_args->format)->check(CLI::IsMember({"human","json","jsonl"}));test->callback([test_args,&result]{result=test_args->run();});
    for(const auto& name:{"import","export"}) {
        auto source=std::make_shared<std::string>(),target=std::make_shared<std::string>();
        auto copy=protocol->add_subcommand(name,"验证并复制协议 JSON 文件；不覆盖目标，可直接分享文件");copy->add_option("source",*source)->required();copy->add_option("target",*target)->required();copy->callback([source,target]{auto j=read_json(*source);Protocol::validate(j);if(std::filesystem::exists(*target))throw Error(Exit::usage,"协议目标文件已存在");write_json(*target,j);});
    }
}
}
