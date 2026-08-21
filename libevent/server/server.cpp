#include "server.hpp"

#include <csignal>
#include <cstdio>
#include <cstring>

#include <event2/event.h>
#include <event2/listener.h>
#include <event2/thread.h>

#include "../common/logger.hpp"
#include "../common/protocol.hpp"
#include "client_manager.hpp"
#include "client_session.hpp"
#include "image_source.hpp"
#include "web_server.hpp"


#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

namespace cam {

Server* Server::instance_ = nullptr;

Server::Server()
    : clients_(&source_), web_(&source_, &clients_)
{
    instance_ = this;
}

Server::~Server()
{
    stop();
    if (listener_) evconnlistener_free(listener_);
    if (base_) event_base_free(base_);
}

void Server::stop()
{
    running_.store(false);
    web_.requestStop();
    if (base_) event_base_loopbreak(base_);
    source_.breakLoop();
}

void Server::onSigint(int sig)
{
    (void)sig;
    if (Server* s = instance())
        s->stop();
}

void Server::onAccept(struct evconnlistener* lst, evutil_socket_t fd,
                      struct sockaddr* sa, int socklen, void* arg)
{
    auto* self = static_cast<Server*>(arg);
    (void)lst; (void)sa; (void)socklen;

    ClientSession* s = self->clients_.add(fd);
    if (!s) {
        evutil_closesocket(fd);
        return;
    }
    LOG_INFO("[server] 接受客户端 %lld，当前 %d/%d",
             static_cast<long long>(fd), self->clients_.count(), kMaxClients);

    /* 控制线程不 join：创建后立即分离，线程结束自动清理资源 */
    s->start();
}

int Server::run()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);   /* 重定向时也实时输出日志 */

#ifdef _WIN32
    /* Windows 控制台默认 GBK：把输出代码页切到 UTF-8，避免中文乱码 */
    SetConsoleOutputCP(CP_UTF8);

    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            std::printf("WSAStartup 失败\n");
            return 1;
        }
    }
#endif

    /* 让 libevent 内部线程安全：Windows 用 Windows 线程原语，Linux 用 pthread */
#ifdef _WIN32
    evthread_use_windows_threads();
#else
    evthread_use_pthreads();
#endif

    base_ = event_base_new();
    if (!base_) {
        std::printf("event_base_new 失败\n");
        return 1;
    }

    struct sockaddr_in sin;
    std::memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(kServerPort);

    listener_ = evconnlistener_new_bind(base_, onAccept, this,
        LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1,
        reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin));
    if (!listener_) {
        std::printf("监听端口 %d 失败\n", kServerPort);
        return 1;
    }

    if (!source_.start()) {
        std::printf("图像生成线程启动失败\n");
        return 1;
    }

    std::signal(SIGINT, onSigint);
    std::signal(SIGTERM, onSigint);

    /* 浏览器界面启动失败只告警，不影响 C 客户端 */
    if (!web_.start())
        std::printf("[server] 警告：浏览器界面启动失败，C 客户端功能不受影响\n");

    std::printf("=============================================\n");
    std::printf(" libevent 图像服务器 (C++17)\n");
    std::printf(" 监听端口   : %d\n", kServerPort);
    std::printf(" 最大客户端 : %d\n", kMaxClients);
    std::printf(" 图像       : %dx%d 8bit 灰度，%dms/帧\n",
                kImgWidth, kImgHeight, kImgGenIntervalMs);
    std::printf(" 按 Ctrl+C 退出\n");
    std::printf(" 浏览器界面 : http://<本机IP>:8080\n");
    std::printf("=============================================\n");

    event_base_dispatch(base_);

    /* ---- 退出清理：唤醒生成线程并 join，再停 web、释放监听器/事件循环。
     * 各客户端的控制线程是独立线程，进程退出时随进程一起结束。 */
    std::printf("\n服务器退出中...\n");
    source_.stop();
    web_.stop();
    if (listener_) {
        evconnlistener_free(listener_);
        listener_ = nullptr;
    }
    event_base_free(base_);
    base_ = nullptr;
    std::printf("服务器已退出\n");

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}

}  /* namespace cam */
