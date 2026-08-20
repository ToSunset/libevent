#include "server.h"
#include "../common/logger.h"

int main()
{
    /* 日志级别：kDebug 全量 / kInfo 常规 / kWarn / kError / kOff */
    cam::Logger::instance().setLevel(cam::LogLevel::kInfo);
    return cam::Server().run();
}
