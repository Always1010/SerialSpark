#include "serialspark/record.hpp"
#include <iostream>
#include <unistd.h>
int main() {
    auto dir=std::filesystem::temp_directory_path()/("serialspark-test-"+std::to_string(getpid()));
    try {
        std::filesystem::create_directory(dir);
        auto path=dir/"log.ssr";
        ss::Recorder recorder(path,{{"test",true}},65536);
        for(int n=1;n<=20;++n) recorder.append({n,n,n,"data",n%2?"tx":"rx",ss::Bytes(16384,n),{{"request",n}}});
        recorder.close();
        int seen=0;
        ss::read_log(path,[&](const ss::RawEvent& e){++seen;if(e.seq!=seen||e.bytes!=ss::Bytes(16384,seen))throw std::runtime_error("record bytes/order");});
        auto info=ss::log_info(path);
        if(seen!=20||info["segments"].size()<2||info["complete"]!=true) throw std::runtime_error("rotation metadata");
        bool exists=false;try{ss::Recorder duplicate(path,ss::Json::object());}catch(const ss::Error&){exists=true;}if(!exists)throw std::runtime_error("overwrote log");
        ss::Recorder bounded(dir/"bounded.ssr",ss::Json::object(),65536,0,512);
        bool limited=false;try{bounded.append({1,1,1,"data","rx",ss::Bytes(1024),ss::Json::object()});}catch(const ss::Error&){limited=true;}bounded.close();if(!limited)throw std::runtime_error("unbounded queue");
        ss::Recorder quota(dir/"quota.ssr",ss::Json::object(),65536,1000);
        quota.append({1,1,1,"data","rx",{1},ss::Json::object()});
        bool exceeded=false;try{quota.close();}catch(const ss::Error&){exceeded=true;}if(!exceeded)throw std::runtime_error("quota ignored");
        std::filesystem::remove_all(dir);
    } catch(const std::exception& e) {std::filesystem::remove_all(dir);std::cerr<<e.what()<<'\n';return 1;}
}
