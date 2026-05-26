#include "serialspark/core.hpp"
#include <iostream>
#include <fstream>
#include <atomic>
#include <thread>
#include <unistd.h>
int main() {
    try {
        ss::Bytes all;for(int n=0;n<256;++n) all.push_back(n);
        if(ss::unhex(ss::hex(all))!=all) throw std::runtime_error("binary roundtrip");
        for(auto input:{"ABC","ZZ","0x01"}) {bool failed=false;try{ss::unhex(input);}catch(const ss::Error&){failed=true;}if(!failed) throw std::runtime_error("bad hex accepted");}
        auto display=ss::escaped(ss::Bytes{0,27,'[','3','1','m',255});
        if(display!="\\x00\\x1B[31m\\xFF") throw std::runtime_error("unsafe display");
        ss::RawEvent e{1,2,3,"data","rx",all,ss::Json::object()};
        if(ss::event_from_json(ss::event_json(e)).bytes!=all) throw std::runtime_error("event roundtrip");
        auto dir=std::filesystem::temp_directory_path()/("serialspark-config-"+std::to_string(getpid()));std::filesystem::create_directory(dir);
        struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code ec;std::filesystem::remove_all(path,ec);}} cleanup{dir};
        auto target=dir/"config.json";std::ofstream(target.string()+".tmp")<<"keep";
        std::atomic<int> won=0;std::vector<std::thread> writers;
        for(int n=0;n<8;++n)writers.emplace_back([&,n]{try{ss::write_json(target,{{"winner",n}});++won;}catch(const ss::Error&){}});
        for(auto& writer:writers)writer.join();
        if(won!=1||!ss::read_json(target).contains("winner"))throw std::runtime_error("configuration publication race");
        std::ifstream previous(target.string()+".tmp");std::string preserved;previous>>preserved;if(preserved!="keep")throw std::runtime_error("temporary file overwritten");
        ss::write_json(target,{{"replaced",true}},true);if(ss::read_json(target)["replaced"]!=true)throw std::runtime_error("explicit replacement");
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
