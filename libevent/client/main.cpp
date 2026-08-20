#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "client.h"
#include "../common/logger.h"

int main(int argc, char** argv)
{
    std::string ip = "127.0.0.1";
    int port = cam::kServerPort;
    bool autoImage = false;

    /* 参数：client [服务器IP] [端口] [-s] */
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-s") == 0) {
            autoImage = true;
        } else if (i == 1 && argv[i][0] != '-') {
            ip = argv[i];
        } else if (i == 2 && argv[i][0] != '-') {
            port = std::atoi(argv[i]);
        }
    }

    /* 日志级别：kDebug 全量 / kInfo 常规 / kWarn / kError / kOff */
    cam::Logger::instance().setLevel(cam::LogLevel::kInfo);
    return cam::Client(ip, port, autoImage).run();
}
