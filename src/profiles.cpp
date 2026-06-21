#include "serialspark/profiles.hpp"
#include "serialspark/serial.hpp"
#include <algorithm>

namespace ss {
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
    write_json(path,j,replace);
}
Json profile_config(const Json& j) {
    if(!j.is_object())throw Error(Exit::usage,"通信配置必须为 JSON 对象");
    if(!j.contains("schema_version")||!j.at("schema_version").is_number_integer()||j.at("schema_version")!=1)
        throw Error(Exit::usage,"通信配置 schema_version 必须为整数 1");
    if(!j.contains("config"))throw Error(Exit::usage,"通信配置缺少 config 对象");
    for(auto i=j.begin();i!=j.end();++i)if(i.key()!="schema_version"&&i.key()!="config")throw Error(Exit::usage,"未知配置键: "+i.key());
    return SerialConfig::from_json(j["config"]).json();
}
Json list_named(const std::string& category) {
    Json names=Json::array();auto path=config_dir(category);if(!std::filesystem::exists(path))return names;
    for(const auto& f:std::filesystem::directory_iterator(path))if(f.is_regular_file()&&f.path().extension()==".json")names.push_back(f.path().stem().string());
    std::sort(names.begin(),names.end());return names;
}
}
