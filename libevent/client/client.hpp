#pragma once
/* 图像客户端：单线程 libevent 事件循环（网络收发/定时器/键盘动作都在主循环），
 * 唯一的额外线程是控制台键盘线程（getchar 阻塞，跨线程交给主循环处理）。
 *
 * 状态机：DISCONNECTED -> CONNECTING -> IDLE --心跳--> WAIT_HB
 *                                    IDLE --按 s--> IMAGE
 * 断线/读超时统一走 teardownConnection() + scheduleReconnect()，2 秒后自动重连。 */

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <event2/util.h>

#include "../common/protocol.hpp"
#include "preview_window.hpp"

struct bufferevent;
struct event;
struct event_base;

namespace cam {

class Client {
public:
    Client(std::string ip, int port, bool autoImage);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    int run();

private:
    enum class State { Disconnected, Connecting, Idle, WaitHb, Image };

    /* ---- 跨平台小工具 ---- */
    static uint32_t nowMs();

    /* ---- 收发状态机 ---- */
    void sendHead(Ctl ctl, int32_t len);
    void setReadTimeout(long ms);
    void recvReset();
    int  expectPayload(size_t len);
    void onHeader(const FrameHeader& h);
    void onPayload();
    void doConnect();
    void teardownConnection();
    void scheduleReconnect();

    /* ---- 动作 ---- */
    void armHbTimer();
    void disarmHbTimer();
    void requestImage();
    void stopImage();

    /* ---- 保存 BMP ---- */
    void initSaveDir();
    void saveFrame(const uint8_t* data, size_t len, uint32_t idx);

    /* ---- 回调（static，用 arg 传 this） ---- */
    static void readCb(struct bufferevent* bev, void* arg);
    static void eventCb(struct bufferevent* bev, short events, void* arg);
    static void hbTimerCb(evutil_socket_t fd, short what, void* arg);
    static void reconnectTimerCb(evutil_socket_t fd, short what, void* arg);
    static void keyControlCb(evutil_socket_t fd, short what, void* arg);
    static void uiTimerCb(evutil_socket_t fd, short what, void* arg);
    static void stdinThreadMain(Client* self);

    /* ---- 状态 ---- */
    struct event_base* base_ = nullptr;
    struct bufferevent* bev_ = nullptr;
    State state_ = State::Disconnected;
    std::atomic<int> running_{1};
    std::atomic<int> reconnectPending_{0};
    std::atomic<int> keyAction_{0};

    std::string ip_;
    int         port_;
    bool        autoImage_;

    /* ---- 帧解析（头 -> 载荷两阶段） ---- */
    bool     wantHeader_ = true;
    uint8_t  hdr_[kHeadLen];
    int      hdrGot_ = 0;
    std::vector<uint8_t> payload_;
    size_t   payloadLen_ = 0;
    size_t   payloadGot_ = 0;

    /* ---- 统计 ---- */
    uint32_t frameCount_ = 0;
    uint32_t hbCount_ = 0;
    uint32_t reconnectCount_ = 0;
    int      saveEvery_ = 30;
    std::atomic<int> saving_{0};
    std::string saveDir_;

    /* ---- 定时器 ---- */
    struct event* hbTimer_ = nullptr;
    struct event* reconnectTimer_ = nullptr;
    struct event* keyEvent_ = nullptr;
    struct event* uiTimer_ = nullptr;

    /* ---- fps ---- */
    uint32_t secondFrames_ = 0;
    uint32_t fps_ = 0;
    uint32_t fpsStart_ = 0;

    PreviewWindow preview_;
    std::thread   stdinThread_;
};

}  /* namespace cam */
