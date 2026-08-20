#pragma once
/* 服务器组合根：持有 ImageSource / ClientManager / WebServer 三大子系统，
 * 主线程跑监听事件循环（9995 端口），Ctrl+C 统一走 stop()。 */

#include <atomic>

#include "image_source.h"
#include "client_manager.h"
#include "web_server.h"

#include <event2/util.h>

struct event_base;
struct evconnlistener;

namespace cam {

class Server {
public:
    Server();
    ~Server();

    int run();      /* 启动并运行服务器，返回进程退出码 */
    void stop();    /* 置停止标志并打断各事件循环 */

    static Server* instance() { return instance_; }

private:
    static void onAccept(struct evconnlistener* lst, evutil_socket_t fd,
                         struct sockaddr* sa, int socklen, void* arg);
    static void onSigint(int sig);

    ImageSource        source_;
    ClientManager      clients_;
    WebServer          web_;

    struct event_base*      base_ = nullptr;
    struct evconnlistener*  listener_ = nullptr;
    std::atomic<bool>       running_{true};

    static Server* instance_;
};

}  /* namespace cam */
