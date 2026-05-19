#pragma once
#include "serialspark/core.hpp"
#include <termios.h>

namespace ss {
struct SerialConfig {
    std::string port;
    int baud=115200;
    int data_bits=8;
    std::string parity="none";
    int stop_bits=1;
    std::string flow="none";
    Json json() const;
    static SerialConfig from_json(const Json& json);
    void validate(bool require_port=true) const;
};
Json discover_ports();
class SerialPort {
public:
    explicit SerialPort(const SerialConfig& config);
    ~SerialPort();
    SerialPort(const SerialPort&)=delete;
    SerialPort& operator=(const SerialPort&)=delete;
    int fd() const {return fd_;}
    const Json& actual() const {return actual_;}
    void close();
private:
    int fd_=-1;
    termios saved_{};
    bool saved_valid_=false;
    Json actual_;
};
void extended_baud(int fd,int baud);
}
