#include "serialspark/serial.hpp"
#include <pty.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <iostream>
int main() {
    int master=-1,slave=-1;
    try {
        char path[256];
        if(openpty(&master,&slave,path,nullptr,nullptr)) throw std::runtime_error("openpty");
        close(slave);slave=-1;
        ss::SerialConfig c;c.port=path;
        ss::SerialPort port(c);
        ss::Bytes bytes;for(int n=0;n<256;++n) bytes.push_back(n);
        if(write(master,bytes.data(),bytes.size())!=256) throw std::runtime_error("write");
        pollfd p{port.fd(),POLLIN,0};if(poll(&p,1,500)!=1) throw std::runtime_error("poll");
        ss::Bytes recv(256);if(read(port.fd(),recv.data(),256)!=256||recv!=bytes) throw std::runtime_error("raw bytes corrupted");
        int competing=open(path,O_RDWR|O_NOCTTY);if(competing>=0){close(competing);throw std::runtime_error("exclusive open not enforced");}
        c.baud=123456;c.flow="bad";
        bool failed=false;try{c.validate();}catch(const ss::Error&){failed=true;}if(!failed) throw std::runtime_error("invalid flow");
        port.close();c.flow="none";ss::SerialPort custom(c);
        if(custom.actual().at("baud")!=123456) throw std::runtime_error("custom baud readback");
        close(master);master=-1;
    } catch(const std::exception& e) {if(master>=0)close(master);if(slave>=0)close(slave);std::cerr<<e.what()<<'\n';return 1;}
}
