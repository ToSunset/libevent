#pragma once
/* 客户端会话：一个 TCP 连接 = 一个对象。
 * 每个会话拥有独立的控制线程（libevent event_base + bufferevent）和
 * 可选的图像发送线程；控制线程结束时通过 ClientManager::onClosed()
 * 把自己从管理表中摘除并销毁。 */

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <event2/util.h>

#include "../common/protocol.h"

struct bufferevent;
struct event_base;

namespace cam {

class ClientManager;
class ImageSource;

class ClientSession {
public:
    ClientSession(ClientManager* mgr, ImageSource* source, evutil_socket_t fd);
    ~ClientSession();

    evutil_socket_t fd() const { return fd_; }
    int slot() const { return slot_; }
    void setSlot(int s) { slot_ = s; }

    void start();          /* 启动控制线程（分离式） */
    void startSending();   /* 请求图像：启动发送线程 */
    void stopSending();    /* 停止图像：停发送线程并 join */

    void send(const void* data, int len);   /* 加锁发送，保证头+数据不被插队 */

private:
    static void ctrlThreadMain(ClientSession* self);
    static void sendThreadMain(ClientSession* self);
    static void onRead(struct bufferevent* bev, void* arg);
    static void onEvent(struct bufferevent* bev, short events, void* arg);

    void handleHeartbeat();
    void sendError(Ctl c, Err e);
    void cleanup();

    ClientManager*    mgr_;
    ImageSource*      source_;
    evutil_socket_t   fd_;
    int               slot_ = -1;

    struct event_base*    base_ = nullptr;
    struct bufferevent*   bev_  = nullptr;
    std::mutex            sendLock_;
    std::thread           ctrlThread_;
    std::thread           sendThread_;
    std::atomic<bool>     sendStop_{true};
    uint32_t              lastSeq_ = 0;
    std::vector<uint8_t>  sendBuf_;   /* 头+图像数据合并缓冲 */
};

}  /* namespace cam */
