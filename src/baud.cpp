#include "serialspark/core.hpp"
#include <asm/termbits.h>
#include <sys/ioctl.h>
#include <cerrno>
#include <cstring>
namespace ss {
void extended_baud(int fd,int baud) {
    termios2 t{};
    if(ioctl(fd,TCGETS2,&t)<0) throw Error(Exit::connect,"驱动无法读取扩展波特率: "+std::string(strerror(errno)));
    t.c_cflag=(t.c_cflag&~CBAUD)|BOTHER;
    t.c_ispeed=t.c_ospeed=baud;
    if(ioctl(fd,TCSETS2,&t)<0) throw Error(Exit::connect,"驱动不支持该波特率: "+std::string(strerror(errno)));
    if(ioctl(fd,TCGETS2,&t)<0||t.c_ispeed!=static_cast<unsigned>(baud)||t.c_ospeed!=static_cast<unsigned>(baud))
        throw Error(Exit::connect,"驱动波特率回读不一致");
}
}
