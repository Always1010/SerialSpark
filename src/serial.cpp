#include "serialspark/serial.hpp"
#include <libudev.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>

namespace ss {
Json SerialConfig::json() const {return {{"port",port},{"baud",baud},{"data_bits",data_bits},{"parity",parity},{"stop_bits",stop_bits},{"flow",flow}};}
SerialConfig SerialConfig::from_json(const Json& j) {
    if(!j.is_object()) throw Error(Exit::usage,"通信配置必须是对象");
    for(auto it=j.begin();it!=j.end();++it) if(it.key()!="port"&&it.key()!="baud"&&it.key()!="data_bits"&&it.key()!="parity"&&it.key()!="stop_bits"&&it.key()!="flow") throw Error(Exit::usage,"未知通信参数: "+it.key());
    SerialConfig c;
    c.port=j.value("port",c.port);c.baud=j.value("baud",c.baud);c.data_bits=j.value("data_bits",c.data_bits);c.parity=j.value("parity",c.parity);c.stop_bits=j.value("stop_bits",c.stop_bits);c.flow=j.value("flow",c.flow);c.validate(false);return c;
}
void SerialConfig::validate(bool require_port) const {
    if(require_port&&port.empty()) throw Error(Exit::usage,"请指定 --port 或保存的 --profile");
    if(baud<1||baud>4000000) throw Error(Exit::usage,"波特率范围为 1～4000000；实际支持取决于驱动");
    if(data_bits<5||data_bits>8) throw Error(Exit::usage,"数据位必须为 5～8");
    if(parity!="none"&&parity!="odd"&&parity!="even") throw Error(Exit::usage,"校验位必须为 none、odd 或 even");
    if(stop_bits!=1&&stop_bits!=2) throw Error(Exit::usage,"停止位必须为 1 或 2");
    if(flow!="none"&&flow!="rtscts"&&flow!="xonxoff") throw Error(Exit::usage,"流控必须为 none、rtscts 或 xonxoff");
}
static std::string system_error(const std::string& action,int e) {
    std::string reason=e==EACCES||e==EPERM?"权限不足":e==EBUSY?"设备忙":e==ENOENT||e==ENODEV?"设备不存在或已断开":"系统错误";
    return action+": "+reason+" ("+strerror(e)+")";
}
SerialPort::SerialPort(const SerialConfig& c) {
    c.validate();
    fd_=::open(c.port.c_str(),O_RDWR|O_NOCTTY|O_NONBLOCK|O_CLOEXEC);
    if(fd_<0) throw Error(Exit::connect,system_error("连接 "+c.port,errno));
    try {
        if(ioctl(fd_,TIOCEXCL)<0) throw Error(Exit::connect,system_error("请求排他访问",errno));
        if(tcgetattr(fd_,&saved_)<0) throw Error(Exit::connect,system_error("读取串口参数",errno));
        saved_valid_=true;
        termios t=saved_;
        cfmakeraw(&t);
        t.c_iflag&=~(INPCK|IGNPAR|PARMRK|IXON|IXOFF|IXANY);
        t.c_cflag&=~(CSIZE|PARENB|PARODD|CSTOPB|CRTSCTS);
#ifdef CMSPAR
        t.c_cflag&=~CMSPAR;
#endif
        t.c_cflag|=CLOCAL|CREAD;
        t.c_cflag|=c.data_bits==5?CS5:c.data_bits==6?CS6:c.data_bits==7?CS7:CS8;
        if(c.parity!="none") t.c_cflag|=PARENB;
        if(c.parity=="odd") t.c_cflag|=PARODD;
        if(c.stop_bits==2) t.c_cflag|=CSTOPB;
        if(c.flow=="rtscts") t.c_cflag|=CRTSCTS;
        if(c.flow=="xonxoff") t.c_iflag|=IXON|IXOFF;
        t.c_cc[VMIN]=1;t.c_cc[VTIME]=0;
        const std::map<int,speed_t> rates={{50,B50},{75,B75},{110,B110},{300,B300},{600,B600},{1200,B1200},{2400,B2400},{4800,B4800},{9600,B9600},{19200,B19200},{38400,B38400},{57600,B57600},{115200,B115200},{230400,B230400},{460800,B460800},{500000,B500000},{576000,B576000},{921600,B921600},{1000000,B1000000},{1152000,B1152000},{1500000,B1500000},{2000000,B2000000},{2500000,B2500000},{3000000,B3000000},{3500000,B3500000},{4000000,B4000000}};
        auto rate=rates.find(c.baud);
        auto speed=rate==rates.end()?B38400:rate->second;
        if(cfsetispeed(&t,speed)<0||cfsetospeed(&t,speed)<0||tcsetattr(fd_,TCSANOW,&t)<0) throw Error(Exit::connect,system_error("应用串口参数",errno));
        if(rate==rates.end()) extended_baud(fd_,c.baud);
        termios actual{};
        if(tcgetattr(fd_,&actual)<0) throw Error(Exit::connect,system_error("回读串口参数",errno));
        tcflag_t mask=CSIZE|PARENB|PARODD|CSTOPB|CRTSCTS|CLOCAL|CREAD;
        if((actual.c_cflag&mask)!=(t.c_cflag&mask)||(actual.c_iflag&(IXON|IXOFF|IXANY))!=(t.c_iflag&(IXON|IXOFF|IXANY))||(rate!=rates.end()&&(cfgetispeed(&actual)!=speed||cfgetospeed(&actual)!=speed)))
            throw Error(Exit::connect,"串口参数回读不一致；驱动可能不支持请求的组合: "+c.json().dump());
        actual_=c.json();actual_["verification"]="driver_readback";
    } catch(...) {close();throw;}
}
SerialPort::~SerialPort() {close();}
void SerialPort::close() {
    if(fd_>=0) {
        if(saved_valid_) tcsetattr(fd_,TCSANOW,&saved_);
        ioctl(fd_,TIOCNXCL);::close(fd_);fd_=-1;
    }
}
Json discover_ports() {
    std::unique_ptr<udev,decltype(&udev_unref)> context(udev_new(),udev_unref);
    if(!context) throw Error(Exit::io,"无法初始化 udev");
    std::unique_ptr<udev_enumerate,decltype(&udev_enumerate_unref)> enumerate(udev_enumerate_new(context.get()),udev_enumerate_unref);
    if(!enumerate) throw Error(Exit::io,"无法枚举设备");
    udev_enumerate_add_match_subsystem(enumerate.get(),"tty");udev_enumerate_scan_devices(enumerate.get());
    Json result=Json::array();
    udev_list_entry* entry;
    udev_list_entry_foreach(entry,udev_enumerate_get_list_entry(enumerate.get())) {
        std::unique_ptr<udev_device,decltype(&udev_device_unref)> d(udev_device_new_from_syspath(context.get(),udev_list_entry_get_name(entry)),udev_device_unref);
        if(!d) continue;
        const char* node=udev_device_get_devnode(d.get());
        const char* name=udev_device_get_sysname(d.get());
        if(!node||!name) continue;
        std::string n=name;
        if(n=="tty"||n=="console"||n=="ptmx"||(n.starts_with("tty")&&n.size()>3&&std::isdigit(static_cast<unsigned char>(n[3])))) continue;
        Json item={{"port",node},{"sys_path",udev_device_get_syspath(d.get())},{"information",Json::object()},{"aliases",Json::array()}};
        for(const char* key:{"ID_VENDOR","ID_MODEL","ID_VENDOR_ID","ID_MODEL_ID","ID_SERIAL_SHORT","ID_SERIAL","ID_PATH"}) {const char* v=udev_device_get_property_value(d.get(),key);if(v) item["information"][key]=v;}
        for(auto p=d.get();p;p=udev_device_get_parent(p)) {const char* v=udev_device_get_driver(p);if(v){item["information"]["driver"]=v;break;}}
        udev_list_entry* link;
        udev_list_entry_foreach(link,udev_device_get_devlinks_list_entry(d.get())) item["aliases"].push_back(udev_list_entry_get_name(link));
        int r=access(node,R_OK|W_OK);int access_error=errno;
        item["access"]={{"precheck",r==0?"read_write":"unavailable"},{"reason",r==0?"打开连接时确认访问与参数支持":system_error("访问预检查",access_error)}};
        item["holders"]=Json::array();
        bool complete=true;
        struct stat device_stat{};
        if(stat(node,&device_stat)!=0) complete=false;
        else {
            std::error_code ec;
            for(std::filesystem::directory_iterator p("/proc",std::filesystem::directory_options::skip_permission_denied,ec),end;p!=end;p.increment(ec)) {
                if(ec) {complete=false;ec.clear();continue;}
                std::string pid=p->path().filename();
                if(pid.empty()||pid.find_first_not_of("0123456789")!=std::string::npos) continue;
                auto fdpath=p->path()/"fd";
                std::error_code fd_ec;
                std::filesystem::directory_iterator f(fdpath,fd_ec),fend;
                if(fd_ec) {if(fd_ec.value()!=ENOENT) complete=false;continue;}
                for(;f!=fend;f.increment(fd_ec)) {
                    if(fd_ec) {complete=false;break;}
                    struct stat s{};
                    if(stat(f->path().c_str(),&s)==0&&S_ISCHR(s.st_mode)&&s.st_rdev==device_stat.st_rdev) {
                        std::ifstream comm(p->path()/"comm");std::string process;std::getline(comm,process);
                        item["holders"].push_back({{"pid",std::stoi(pid)},{"process",process}});break;
                    }
                }
            }
            if(ec) complete=false;
        }
        item["occupancy"]=item["holders"].empty()?(complete?"no_visible_holder":"unknown"):"visible_holder";
        item["occupancy_complete"]=false;
        item["occupancy_note"]="占用查询是瞬时快照，权限与竞争可能使结果不完整；未发现打开者不保证空闲";
        result.push_back(item);
    }
    return result;
}
}
