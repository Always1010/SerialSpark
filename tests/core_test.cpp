#include "serialspark/core.hpp"
#include <iostream>
int main() {
    try {
        ss::Bytes all;for(int n=0;n<256;++n) all.push_back(n);
        if(ss::unhex(ss::hex(all))!=all) throw std::runtime_error("binary roundtrip");
        for(auto input:{"ABC","ZZ","0x01"}) {bool failed=false;try{ss::unhex(input);}catch(const ss::Error&){failed=true;}if(!failed) throw std::runtime_error("bad hex accepted");}
        auto display=ss::escaped(ss::Bytes{0,27,'[','3','1','m',255});
        if(display!="\\x00\\x1B[31m\\xFF") throw std::runtime_error("unsafe display");
        ss::RawEvent e{1,2,3,"data","rx",all,ss::Json::object()};
        if(ss::event_from_json(ss::event_json(e)).bytes!=all) throw std::runtime_error("event roundtrip");
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
