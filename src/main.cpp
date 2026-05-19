#include "serialspark/core.hpp"
#include <CLI/CLI.hpp>
#include <iostream>
int main(int argc,char** argv) {
    CLI::App app{"SerialSpark 串口通信与协议调试工具"};
    app.set_version_flag("--version","SerialSpark 0.1.0");
    try {app.parse(argc,argv);std::cout<<app.help();}
    catch(const CLI::ParseError& e) {return app.exit(e);}
}
