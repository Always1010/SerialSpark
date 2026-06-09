#include "serialspark/protocol.hpp"
#include <cassert>
#include <iostream>
#include <random>

using namespace ss;
namespace {
    RawEvent event(Bytes b,std::int64_t seq=1,std::int64_t mono=0,std::string dir="rx") {return {seq,10,mono,"data",std::move(dir),std::move(b),Json::object()};
    }
    Json cfg(Json framing) {return {{"schema_version",1},{"framing",std::move(framing)}};
    }
    void rejects(const Json& c) {bool yes=false;
        try {Protocol::validate(c);
        }catch(const Error& e){yes=e.code==Exit::usage;
        }assert(yes);
    }
    std::vector<Json> semantic(std::vector<Json> out) {for(auto& j:out){j.erase("sources");
            j.erase("wall_ns");
            j.erase("mono_ns");
        }return out;
    }
    void partition_property(const Json& c,const Bytes& input) {
        Protocol whole(c);
        auto expected=whole.feed(event(input));
        auto tail=whole.finish("end");
        expected.insert(expected.end(),tail.begin(),tail.end());
        std::mt19937 random(17);
        for(int run=0;run<80;++run) {
            Protocol split(c);
            std::vector<Json> actual;
            std::vector<RawEvent> events;
            std::size_t pos=0;
            while(pos<input.size()){auto n=std::min(input.size()-pos,std::size_t(1+random()%7));
                events.push_back(event(Bytes(input.begin()+pos,input.begin()+pos+n),events.size()+1));
                pos+=n;
                auto frames=split.feed(events.back());
                actual.insert(actual.end(),frames.begin(),frames.end());
            }
            auto rest=split.finish("end");
            actual.insert(actual.end(),rest.begin(),rest.end());
            assert(semantic(actual)==semantic(expected));
            Bytes recovered;
            for(auto& frame:actual){Bytes raw;
                for(auto& s:frame.at("sources")){auto& source=events.at(s.at("event").get<std::size_t>()-1);
                    auto off=s.at("offset").get<std::size_t>(),n=s.at("length").get<std::size_t>();
                    assert(n&&off+n<=source.bytes.size());
                    raw.insert(raw.end(),source.bytes.begin()+off,source.bytes.begin()+off+n);
                }assert(hex(raw)==frame.at("raw_hex").get<std::string>());
                recovered.insert(recovered.end(),raw.begin(),raw.end());
            }assert(recovered==input);
        }
    }
    void framing() {
        auto lines=cfg({{"mode","newline"},{"newline","CRLF"},{"max_frame_bytes",12}});
        lines["fields"]=Json::array({{{"name","value"},{"type","i16"},{"index",0}}});
        Protocol p(lines);
        assert(p.feed(event({'4','2','\r'})).empty());
        auto out=p.feed(event({'\n','-','8','\r','\n'},2));
        assert(out.size()==2&&out[0]["fields"]["value"]==42&&out[1]["fields"]["value"]==-8);
        assert(out[0]["sources"].size()==2&&out[0]["raw_hex"]=="34320D0A");
        partition_property(lines,{'4','2','\r','\n','-','8','\r','\n','x'});
        partition_property(cfg({{"mode","delimiter"},{"delimiter_hex","00 FF"},{"max_frame_bytes",6}}),{1,2,0,255,3,0,255,9});
        partition_property(cfg({{"mode","fixed"},{"length",3}}),{1,2,3,4,5,6,7});
        partition_property(cfg({{"mode","header_fixed"},{"header_hex","AA 55"},{"length",4}}),unhex("00 AA 00 AA 55 01 02 AA 55 03 04 AA"));
        partition_property(cfg({{"mode","header_fixed"},{"header_hex","AA AA AB"},{"length",5}}),unhex("AA AA AA AA AB 01 02 AA AA AA AB 03 04"));
        partition_property(cfg({{"mode","delimiter"},{"delimiter_hex","AA AA AB"},{"max_frame_bytes",16}}),unhex("AA AA AA AA AB 01 02 AA AA AA AB 03 04"));
        auto lengths=cfg({{"mode","header_length"},{"header_hex","AA 55"},{"length_field",{{"offset",2},{"type","u8"}}},{"max_frame_bytes",16}});
        partition_property(lengths,unhex("01 AA 55 FF AA 55 04 09 AA 55 05 0A 0B AA"));
        Protocol badlen(lengths);
        out=badlen.feed(event(unhex("AA 55 FF AA 55 04 09")));
        assert(out.back()["status"]=="ok"&&out.back()["raw_hex"]=="AA550409");
        assert(out.front()["errors"][0]["code"]=="invalid_length");
        Protocol oversized(cfg({{"mode","newline"},{"max_frame_bytes",4}}));
        out=oversized.feed(event({'a','b','c','d','e','\n'}));
        assert(out.size()==2&&out[0]["status"]=="error"&&out[1]["raw_hex"]=="650A");
        auto huge=cfg({{"mode","header_length"},{"header_hex","AA"},{"length_field",{{"offset",1},{"type","u32"},{"endian","big"},{"unit_bytes",65536},{"adjustment",65536}}},{"max_frame_bytes",32}});
        Protocol malicious(huge);
        out=malicious.feed(event(unhex("AA FFFFFFFF")));
        assert(out.front()["status"]=="error");
    }
    void fields() {
        auto c=cfg({{"mode","fixed"},{"length",27}});
        c["emit_raw_fields"]=true;
        c["fields"]=Json::array({{{"name","u64"},{"type","u64"},{"offset",0},{"endian","big"}},{{"name","i64"},{"type","i64"},{"offset",8},{"endian","little"}},{{"name","temperature"},{"type","i16"},{"offset",16},{"endian","little"},{"scale",0.1},{"bias",1},{"unit","C"}},{{"name","float"},{"type","float32"},{"offset",18},{"endian","big"}},{{"name","state"},{"type","u8"},{"offset",22},{"enum",{{"1","on"}}}},{{"name","utf8"},{"type","utf8"},{"offset",23},{"length",3}},{{"name","bytes"},{"type","bytes"},{"offset",26},{"length",1}}});
        Protocol p(c);
        auto out=p.feed(event(unhex("FFFFFFFFFFFFFFFF FEFFFFFFFFFFFFFF 9CFF 3FC00000 01 E4B8AD 00")));
        assert(out.size()==1);
        auto& f=out[0]["fields"];
        assert(out[0]["status"]=="ok");
        assert(f["u64"]=="18446744073709551615"&&f["i64"]=="-2");
        assert(f["temperature"]==-9.0&&out[0]["units"]["temperature"]=="C"&&out[0]["raw_fields"]["temperature"]==-100);
        assert(f["float"]==1.5&&f["state"]=="on"&&f["utf8"]=="中"&&f["bytes"]=="00");
        auto text=cfg({{"mode","newline"}});
        text["split_separator"]="|";
        text["fields"]=Json::array({{{"name","count"},{"type","u64"},{"index",0}},{{"name","temperature"},{"type","float64"},{"index",1}}});
        Protocol tp(text);
        std::string good="18446744073709551615|1.25\n";
        out=tp.feed(event(Bytes(good.begin(),good.end())));
        assert(out[0]["status"]=="ok"&&out[0]["fields"]["count"]=="18446744073709551615");
        for(const std::string bad:{"18446744073709551616|1.25\n","1x|1.25\n","1|NaN\n","-1|1.25\n","1|1.25junk\n"}){out=tp.feed(event(Bytes(bad.begin(),bad.end())));
            assert(out[0]["status"]=="error"&&!out[0]["errors"].empty()&&!out[0]["raw_hex"].get<std::string>().empty());
        }
        auto transform=cfg({{"mode","fixed"},{"length",10}});
        transform["emit_raw_fields"]=true;
        transform["fields"]=Json::array({
            {{"name","negative"},{"type","i8"},{"offset",0},{"enum",{{"-1","minus"}}}},
            {{"name","scale"},{"type","i8"},{"offset",1},{"scale",-2},{"bias",3}},
            {{"name","large"},{"type","i64"},{"offset",2},{"endian","big"},{"enum",{{"-2","two"}}}}
        });
        Protocol transformp(transform);
        out=transformp.feed(event(unhex("FF FE FFFFFFFFFFFFFFFE")));
        assert(out[0]["fields"]["negative"]=="minus" && out[0]["fields"]["scale"]==7 && out[0]["fields"]["large"]=="two");
        assert(out[0]["raw_fields"]["large"]=="-2");
        auto invalid=cfg({{"mode","fixed"},{"length",2}});
        invalid["fields"]=Json::array({{{"name","text"},{"type","utf8"},{"offset",0},{"length",2}}});
        Protocol ip(invalid);
        assert(ip.feed(event(unhex("C080")))[0]["status"]=="error");
    }
    void checksums() {
        struct Vector {const char* alg;
            const char* suffix;
            int length;
        };
        for(auto v:{Vector{"sum8","DD",10},Vector{"xor8","31",10},Vector{"crc16-modbus","374B",11},Vector{"crc16-ccitt-false","B129",11},Vector{"crc32","2639F4CB",13}}) {
            auto c=cfg({{"mode","fixed"},{"length",v.length}});
            c["checksum"]={{"algorithm",v.alg},{"result_endian","little"}};
            Bytes data={'1','2','3','4','5','6','7','8','9'};
            auto suffix=unhex(v.suffix);
            data.insert(data.end(),suffix.begin(),suffix.end());
            Protocol p(c);
            auto out=p.feed(event(data));
            assert(out[0]["status"]=="ok");
            data.back()^=1;
            out=p.feed(event(data));
            assert(out[0]["status"]=="error"&&out[0]["errors"][0]["code"]=="checksum");
        }
        auto c=cfg({{"mode","fixed"},{"length",12}});
        c["checksum"]={{"algorithm","crc16-ccitt-false"},{"result_endian","big"},{"offset_from_end",2},{"coverage",{{"start",1},{"end_from_end",2}}}};
        Protocol p(c);
        assert(p.feed(event(unhex("00 313233343536373839 29B1")))[0]["status"]=="ok");
    }
    void clocks_and_directions() {
        auto c=cfg({{"mode","fixed"},{"length",3},{"assembly_timeout_ms",10}});
        Protocol p(c);
        assert(p.feed(event({1},1,0)).empty());
        assert(p.feed(event({2},2,9000000)).empty());
        auto clock=event({},3,11000000);
        clock.kind="clock";
        assert(p.feed(clock).empty());
        clock.mono=19000000;
        auto out=p.feed(clock);
        assert(out.size()==1&&out[0]["status"]=="incomplete"&&out[0]["raw_hex"]=="0102");
        assert(p.feed(event({3},4,20000000,"rx")).empty());
        assert(p.feed(event({7,8},5,20000000,"tx")).empty());
        auto dis=event({},6,21000000);
        dis.kind="disconnect";
        out=p.feed(dis);
        assert(out.size()==2&&out[0]["direction"]=="tx"&&out[1]["direction"]=="rx");
        out=p.feed(event({4,5,6},7,22000000));
        assert(out.size()==1&&out[0]["raw_hex"]=="040506");
        assert(p.finish("end").empty());
        auto nested=Json{{"schema_version",1},{"tx",{{"framing",{{"mode","fixed"},{"length",2}}}}},{"rx",{{"framing",{{"mode","newline"}}}}}};
        Protocol separate(nested);
        assert(separate.feed(event({1,2},1,0,"tx")).size()==1);
        assert(separate.feed(event({'a','\n'},2)).size()==1);
    }
    void validation() {
        rejects({{"schema_version",2},{"framing",{{"mode","fixed"},{"length",1}}}});
        rejects(cfg({{"mode","fixed"},{"length",0}}));
        rejects(cfg({{"mode","fixed"},{"length",65537}}));
        rejects(cfg({{"mode","fixed"},{"length",1},{"max_frame_bytes",18446744073709551615ull}}));
        rejects(cfg({{"mode","newline"},{"length",1}}));
        rejects(cfg({{"mode","delimiter"},{"delimiter_hex","0"}}));
        rejects(cfg({{"mode","newline"},{"assembly_timeout_ms",18446744073709551615ull}}));
        auto base=cfg({{"mode","fixed"},{"length",8}});
        base["bogus"]=true;
        rejects(base);
        base.erase("bogus");
        base["fields"]=Json::array({{{"name","value"},{"type","u32"},{"offset",0}}});
        rejects(base);
        base["fields"][0]["endian"]="little";
        base["fields"][0]["offset"]=18446744073709551615ull;
        rejects(base);
        base["fields"][0]["offset"]=65535;
        rejects(base);
        base["fields"][0]["offset"]=0;
        base["fields"][0]["script"]="bad";
        rejects(base);
        base=cfg({{"mode","fixed"},{"length",1}});
        base["checksum"]={{"algorithm","crc32"},{"result_endian","little"},{"offset_from_end",1}};
        rejects(base);
        base=cfg({{"mode","fixed"},{"length",8}});
        base["fields"]=Json::array({{{"name","large"},{"type","i64"},{"offset",0},{"endian","little"},{"scale",-2}}});
        rejects(base);
        base=cfg({{"mode","fixed"},{"length",1}});
        base["checksum"]={{"algorithm","sum8"},{"bogus",true}};
        rejects(base);
        base=cfg({{"mode","fixed"},{"length",1},{"nonsense",0}});
        base["tx"]={{"framing",{{"mode","fixed"},{"length",1}}}};
        base["rx"]=base["tx"];
        rejects(base);
        base=cfg({{"mode","fixed"},{"length",1}});base["fields"]=Json::array();
        for(int i=0;i<257;++i)base["fields"].push_back({{"name","f"+std::to_string(i)},{"type","u8"},{"offset",0}});
        rejects(base);base["fields"].erase(base["fields"].end()-1);
        Protocol bounded(base);assert(bounded.feed(event({1},1))[0]["fields"].size()==256);
        base=cfg({{"mode","fixed"},{"length",65536}});base["fields"]=Json::array();
        for(int i=0;i<17;++i)base["fields"].push_back({{"name","f"+std::to_string(i)},{"type","bytes"},{"offset",0},{"length",65536}});
        rejects(base);
    }
}
int main(){framing();
    fields();
    checksums();
    clocks_and_directions();
    validation();
    std::cout<<"protocol tests passed\n";
}
