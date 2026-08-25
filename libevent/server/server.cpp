#include "server.h"

#include <csignal>
#include <cstdio>
#include <cstring>

#include <event2/event.h>
#include <event2/listener.h>
#include <event2/thread.h>

#include "common/logger.h"
#include "common/protocol.h"
#include "client_manager.h"
#include "client_session.h"
#include "image_source.h"
#include "web_server.h"


#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
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
    Server* s = instance();
    if (s) s->stop();
}

void Server::onAccept(struct evconnlistener* lst, evutil_socket_t fd,
                      struct sockaddr* sa, int socklen, void* arg)
{
    auto* self = static_cast<Server*>(arg);
    (void)lst; (void)sa; (void)socklen;

    ClientSession* s = self->clients_.add(fd);
    if (!s) {
        LOG_WARN("[SRV] 拒绝连接：客户端已满（%d/%d）", kMaxClients, kMaxClients);
        evutil_closesocket(fd);
        return;
    }
    char peer[64] = "unknown";
    if (sa) {
        const struct sockaddr_in* sin =
            reinterpret_cast<const struct sockaddr_in*>(sa);
        char ip[INET_ADDRSTRLEN] = "?";
#ifdef _WIN32
        InetNtopA(AF_INET, const_cast<struct in_addr*>(&sin->sin_addr),
                  ip, static_cast<DWORD>(sizeof(ip)));
#else
        inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
#endif
        std::snprintf(peer, sizeof(peer), "%s:%d", ip, ntohs(sin->sin_port));
    }
    LOG_INFO("[SRV] 接受客户端 %s (fd=%lld)，当前 %d/%d",
             peer, static_cast<long long>(fd),
             self->clients_.count(), kMaxClients);

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
            LOG_ERROR("[SRV] WSAStartup 初始化失败");
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
        LOG_ERROR("[SRV] 初始化事件循环失败");
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
        LOG_ERROR("[SRV] 监听端口 %d 失败", kServerPort);
        return 1;
    }

    if (!source_.start()) {
        LOG_ERROR("[SRV] 图像生成线程启动失败");
        return 1;
    }

    std::signal(SIGINT, onSigint);
    std::signal(SIGTERM, onSigint);

    /* 浏览器界面启动失败只告警，不影响 C 客户端 */
    if (!web_.start())
        LOG_WARN("[SRV] 浏览器界面启动失败，C 客户端功能不受影响");

    std::printf("=============================================\n");
    std::printf(" libevent 图像服务器 (C++11)\n");
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
    LOG_INFO("[SRV] 服务器退出中...");
    source_.stop();
    web_.stop();
    if (listener_) {
        evconnlistener_free(listener_);
        listener_ = nullptr;
    }
    event_base_free(base_);
    base_ = nullptr;
    LOG_INFO("[SRV] 服务器已退出");

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}

}  /* namespace cam */
