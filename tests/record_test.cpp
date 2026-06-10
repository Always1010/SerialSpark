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
        std::int64_t tx=0,rx=0,payload=0;
        for(const auto& segment:info["segments"]) {
            tx+=segment["tx_data_bytes"].get<std::int64_t>();rx+=segment["rx_data_bytes"].get<std::int64_t>();
            payload+=segment["event_payload_bytes"].get<std::int64_t>();
            if(segment["data_bytes"]!=segment["tx_data_bytes"].get<std::int64_t>()+segment["rx_data_bytes"].get<std::int64_t>())throw std::runtime_error("segment data bytes");
        }
        if(tx!=10*16384||rx!=10*16384||info["tx_data_bytes"]!=tx||info["rx_data_bytes"]!=rx||info["data_bytes"]!=tx+rx||info["event_payload_bytes"]!=payload||info["raw_bytes"]!=payload)throw std::runtime_error("rotated byte totals");
        auto sample=dir/"sample.ssr";
        {ss::Recorder log(sample,ss::Json::object());
            log.append({1,1,1,"send_intent","tx",ss::Bytes(6),ss::Json::object()});
            log.append({2,2,2,"data","tx",ss::Bytes(6),ss::Json::object()});
            log.append({3,3,3,"data","rx",ss::Bytes(18),ss::Json::object()});log.close();}
        auto stats=ss::log_info(sample);
        if(stats["raw_bytes"]!=30||stats["event_payload_bytes"]!=30||stats["tx_data_bytes"]!=6||stats["rx_data_bytes"]!=18||stats["data_bytes"]!=24)throw std::runtime_error("send intent counted as data");
        auto failed=dir/"failed.ssr";
        {ss::Recorder log(failed,ss::Json::object());
            log.append({1,1,1,"send_intent","tx",ss::Bytes(6),ss::Json::object()});
            log.append({2,2,2,"send_end","tx",{},{{"status","failed"}}});log.close();}
        stats=ss::log_info(failed);
        if(stats["event_payload_bytes"]!=6||stats["tx_data_bytes"]!=0||stats["rx_data_bytes"]!=0||stats["data_bytes"]!=0)throw std::runtime_error("failed send counted as data");
        bool exists=false;try{ss::Recorder duplicate(path,ss::Json::object());}catch(const ss::Error&){exists=true;}if(!exists)throw std::runtime_error("overwrote log");
        ss::Recorder bounded(dir/"bounded.ssr",ss::Json::object(),65536,0,512);
        bool limited=false;try{bounded.append({1,1,1,"data","rx",ss::Bytes(1024),ss::Json::object()});}catch(const ss::Error&){limited=true;}
        bool latched=false;try{bounded.append({2,1,1,"data","rx",{1},ss::Json::object()});}catch(const ss::Error&){latched=true;}
        try{bounded.close();}catch(const ss::Error&){}
        if(!limited||!latched||ss::log_info(dir/"bounded.ssr")["complete"]!=false)throw std::runtime_error("overflow not terminal/incomplete");
        ss::Recorder quota(dir/"quota.ssr",ss::Json::object(),65536,1000);
        quota.append({1,1,1,"data","rx",{1},ss::Json::object()});
        bool exceeded=false;try{quota.close();}catch(const ss::Error&){exceeded=true;}if(!exceeded)throw std::runtime_error("quota ignored");
        auto corrupt=dir/"corrupt.ssr";
        {ss::Recorder log(corrupt,ss::Json::object());for(int i=1;i<=3;++i)log.append({i,i,i,"data","rx",{1},ss::Json::object()});log.close();}
        sqlite3* database=nullptr;sqlite3_open(corrupt.c_str(),&database);sqlite3_exec(database,"DELETE FROM events WHERE seq=2",nullptr,nullptr,nullptr);sqlite3_close(database);
        auto integrity=ss::log_info(corrupt);
        if(integrity["complete"]!=false||integrity["segments"][0]["sequence_valid"]!=false)throw std::runtime_error("sequence gap marked complete");
        bool damaged=false;try{ss::read_log(corrupt,[](const ss::RawEvent&){});}catch(const ss::Error&){damaged=true;}if(!damaged)throw std::runtime_error("gap accepted");
        sqlite3_open(corrupt.c_str(),&database);sqlite3_exec(database,"INSERT INTO metadata VALUES('oversized',zeroblob(1048577))",nullptr,nullptr,nullptr);sqlite3_close(database);
        bool oversized=false;try{ss::log_info(corrupt);}catch(const ss::Error&){oversized=true;}if(!oversized)throw std::runtime_error("oversized metadata accepted");
        std::filesystem::remove_all(dir);
    } catch(const std::exception& e) {std::filesystem::remove_all(dir);std::cerr<<e.what()<<'\n';return 1;}
}
