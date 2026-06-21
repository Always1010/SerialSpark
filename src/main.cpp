#include "serialspark/cli.hpp"
#include <csignal>
#include <sys/stat.h>

int main(int argc,char** argv) {
    umask(0077);
    signal(SIGPIPE,SIG_IGN);
    return ss::run_cli(argc,argv);
}
