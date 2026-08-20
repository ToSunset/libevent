#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "client.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/thread.h>

#include "../common/logger.h"

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <cerrno>
#include <ctime>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cam {

/* BMP 文件头/信息头：布局与 Windows BITMAPFILEHEADER/BITMAPINFOHEADER 一致 */
#pragma pack(push, 1)
struct BmpFileHeader {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
};
struct BmpInfoHeader {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
};
#pragma pack(pop)

Client::Client(std::string ip, int port, bool autoImage)
    : ip_(std::move(ip)), port_(port), autoImage_(autoImage)
{
}

Client::~Client()
{
    running_.store(0);
    if (base_) event_base_loopbreak(base_);
}

uint32_t Client::nowMs()
{
#ifdef _WIN32
    return static_cast<uint32_t>(GetTickCount());
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(ts.tv_sec) * 1000u +
         ts.tv_nsec / 1000000u) & 0xFFFFFFFFu);
#endif
}

/* ---- 收发状态机 ---- */

void Client::sendHead(Ctl ctl, int32_t len)
{
    uint8_t buf[kHeadLen];
    FrameHeader h(ctl, Err::Ok, len);
    h.toBytes(buf);
    if (bev_) bufferevent_write(bev_, buf, kHeadLen);
}

void Client::setReadTimeout(long ms)
{
    if (!bev_) return;
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    bufferevent_set_timeouts(bev_, &tv, nullptr);   /* 0 表示禁用 */
}

void Client::recvReset()
{
    wantHeader_  = true;
    hdrGot_      = 0;
    payloadGot_  = 0;
    payloadLen_  = 0;
}

int Client::expectPayload(size_t len)
{
    if (len > 16u * 1024u * 1024u) {   /* 上限保护，防异常长度 */
        LOG_WARN("[client] 数据长度过大 (%zu)，放弃当前连接\n", len);
        return -1;
    }
    if (len > payload_.size()) payload_.resize(len);
    payloadLen_ = len;
    payloadGot_ = 0;
    wantHeader_ = false;
    return 0;
}

void Client::onHeader(const FrameHeader& h)
{
    auto bad = [this] {
        state_ = State::Disconnected;
        scheduleReconnect();
    };

    if (state_ == State::WaitHb) {
        if (h.ctl != static_cast<int32_t>(Ctl::HeartbeatResp)) {
            LOG_WARN("[client] 心跳阶段收到意外控制码 %d，重连\n", h.ctl);
            bad();
            return;
        }
        if (expectPayload(kHeartbeatDataLen) < 0) bad();
        return;
    }

    if (state_ == State::Image) {
        if (h.ctl != static_cast<int32_t>(Ctl::ImgData)) {
            LOG_WARN("[client] 收图阶段收到意外控制码 %d，重连\n", h.ctl);
            bad();
            return;
        }
        if (h.err != static_cast<int32_t>(Err::Ok)) {
            LOG_WARN("[client] 服务器错误码 %d（如图像超时），继续等下一帧\n", h.err);
            recvReset();
            return;
        }
        if (h.len == 0) {
            recvReset();
            return;
        }
        if (expectPayload(static_cast<size_t>(h.len)) < 0) bad();
        return;
    }

    LOG_WARN("[client] 状态 %d 下收到意外数据，重连\n", static_cast<int>(state_));
    bad();
}

void Client::onPayload()
{
    if (state_ == State::WaitHb) {
        int32_t val;
        std::memcpy(&val, payload_.data(), kHeartbeatDataLen);
        hbCount_++;
        LOG_DEBUG("[client] 心跳应答 #%u：%d\n", hbCount_, static_cast<int>(val));
        state_ = State::Idle;
        recvReset();
        setReadTimeout(0);
        armHbTimer();
        return;
    }

    if (state_ == State::Image) {
        if (payloadLen_ != static_cast<size_t>(kImgDataLen)) {
            LOG_WARN("[client] 图像长度异常：%zu (期望 %d)\n",
                   payloadLen_, kImgDataLen);
        } else {
            frameCount_++;
            secondFrames_++;
            preview_.showFrame(payload_.data(), kImgWidth, kImgHeight);
            if (saving_.load() && frameCount_ % saveEvery_ == 0)
                saveFrame(payload_.data(), payloadLen_, frameCount_);
            LOG_DEBUG("[client] 收到图像 #%u：%zu 字节\n",
                   frameCount_, payloadLen_);
        }
        recvReset();   /* 继续收下一帧 */
    }
}

void Client::doConnect()
{
    struct sockaddr_in sin;

    teardownConnection();   /* 确保旧连接已清理 */

    bev_ = bufferevent_socket_new(base_, -1, BEV_OPT_CLOSE_ON_FREE);
    if (!bev_) {
        scheduleReconnect();
        return;
    }
    bufferevent_setcb(bev_, readCb, nullptr, eventCb, this);
    bufferevent_enable(bev_, EV_READ | EV_WRITE);

    std::memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port   = htons(static_cast<unsigned short>(port_));
    if (evutil_inet_pton(AF_INET, ip_.c_str(), &sin.sin_addr) != 1) {
        LOG_WARN("[client] IP 地址无效：%s\n", ip_.c_str());
        teardownConnection();
        scheduleReconnect();
        return;
    }

    state_ = State::Connecting;
    if (bufferevent_socket_connect(bev_, reinterpret_cast<struct sockaddr*>(&sin),
                                   sizeof(sin)) < 0) {
        LOG_WARN("[client] 连接失败，稍后重试\n");
        teardownConnection();
        scheduleReconnect();
    }
}

void Client::teardownConnection()
{
    if (bev_) {
        bufferevent_free(bev_);
        bev_ = nullptr;
    }
    disarmHbTimer();
    state_ = State::Disconnected;
    recvReset();
}

void Client::scheduleReconnect()
{
    struct timeval tv;
    if (!running_.load() || reconnectPending_.load()) return;
    reconnectPending_.store(1);
    tv.tv_sec  = kClientReconnectDelayMs / 1000;
    tv.tv_usec = (kClientReconnectDelayMs % 1000) * 1000;
    event_add(reconnectTimer_, &tv);
}

/* ---- 定时器 ---- */

void Client::armHbTimer()
{
    if (!hbTimer_) return;
    struct timeval tv;
    tv.tv_sec  = kClientHeartbeatIntervalMs / 1000;
    tv.tv_usec = (kClientHeartbeatIntervalMs % 1000) * 1000;
    event_add(hbTimer_, &tv);
}

void Client::disarmHbTimer()
{
    if (hbTimer_) event_del(hbTimer_);
}

void Client::requestImage()
{
    if (state_ != State::Idle) {
        LOG_WARN("[client] 当前状态 %d，需空闲状态才能开始收图\n",
               static_cast<int>(state_));
        return;
    }
    LOG_INFO("[client] 发送请求图像...\n");
    sendHead(Ctl::ReqImage, 0);
    disarmHbTimer();
    state_ = State::Image;
    recvReset();
    setReadTimeout(kClientReadTimeoutMs);
}

void Client::stopImage()
{
    if (state_ != State::Image) {
        LOG_WARN("[client] 当前不在收图状态\n");
        return;
    }
    saving_.store(0);   /* 演示停止，保存同步关闭 */
    LOG_INFO("[client] 发送停止图像...\n");
    sendHead(Ctl::StopImage, 0);
    state_ = State::Idle;
    recvReset();
    setReadTimeout(0);
    if (bev_) evbuffer_drain(bufferevent_get_input(bev_), -1);
    armHbTimer();
}

/* ---- 保存 BMP ---- */

void Client::initSaveDir()
{
    char dir[64];
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::snprintf(dir, sizeof(dir), "frames_%04u%02u%02u_%02u%02u%02u_%u",
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond,
                  static_cast<unsigned>(GetCurrentProcessId()));
    if (CreateDirectoryA(dir, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) {
        saveDir_ = dir;
        LOG_INFO("保存目录 : %s/\n", saveDir_.c_str());
    } else {
        LOG_WARN("保存目录 : 创建失败（错误码 %lu），退回当前目录\n",
               static_cast<unsigned long>(GetLastError()));
    }
#else
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    std::snprintf(dir, sizeof(dir), "frames_%04d%02d%02d_%02d%02d%02d_%d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, static_cast<int>(getpid()));
    if (mkdir(dir, 0755) == 0 || errno == EEXIST) {
        saveDir_ = dir;
        LOG_INFO("保存目录 : %s/\n", saveDir_.c_str());
    } else {
        LOG_WARN("保存目录 : 创建失败（错误码 %d），退回当前目录\n", errno);
    }
#endif
}

void Client::saveFrame(const uint8_t* data, size_t len, uint32_t idx)
{
    if (len < static_cast<size_t>(kImgDataLen)) return;
    char name[64];
    if (!saveDir_.empty())
        std::snprintf(name, sizeof(name), "%s/frame_%04u.bmp",
                      saveDir_.c_str(), idx);
    else
        std::snprintf(name, sizeof(name), "frame_%04u.bmp", idx);

    FILE* fp = std::fopen(name, "wb");
    if (!fp) return;

    BmpFileHeader bf{};
    BmpInfoHeader bi{};
    const int row = kImgWidth;   /* 8bpp，640 字节，无需行填充 */

    bf.bfType    = 0x4D42;                       /* "BM" */
    bf.bfOffBits = static_cast<uint32_t>(sizeof(bf) + sizeof(bi) + 256u * 4u);
    bf.bfSize    = bf.bfOffBits + static_cast<uint32_t>(row * kImgHeight);

    bi.biSize        = static_cast<uint32_t>(sizeof(bi));
    bi.biWidth       = kImgWidth;
    bi.biHeight      = kImgHeight;
    bi.biPlanes      = 1;
    bi.biBitCount    = 8;
    bi.biCompression = 0;                        /* BI_RGB */
    bi.biSizeImage   = static_cast<uint32_t>(row * kImgHeight);
    bi.biClrUsed     = 256;

    std::fwrite(&bf, 1, sizeof(bf), fp);
    std::fwrite(&bi, 1, sizeof(bi), fp);
    for (int i = 0; i < 256; i++) {
        uint8_t pal[4] = { static_cast<uint8_t>(i), static_cast<uint8_t>(i),
                           static_cast<uint8_t>(i), 0 };
        std::fwrite(pal, 1, sizeof(pal), fp);
    }
    /* BMP 行自下而上存储 */
    for (int y = kImgHeight - 1; y >= 0; y--)
        std::fwrite(data + static_cast<size_t>(y) * kImgWidth, 1, kImgWidth, fp);

    std::fclose(fp);
    LOG_INFO("[client] 已保存 %s\n", name);
}

/* ---- 回调 ---- */

void Client::hbTimerCb(evutil_socket_t fd, short what, void* arg)
{
    auto* self = static_cast<Client*>(arg);
    if (self->state_ != State::Idle) return;
    LOG_DEBUG("[client] 发送心跳请求\n");
    self->sendHead(Ctl::HeartbeatReq, 0);
    self->disarmHbTimer();
    self->state_ = State::WaitHb;
    self->recvReset();
    self->setReadTimeout(kClientReadTimeoutMs);
}

void Client::reconnectTimerCb(evutil_socket_t fd, short what, void* arg)
{
    auto* self = static_cast<Client*>(arg);
    self->reconnectPending_.store(0);
    self->reconnectCount_++;
    LOG_WARN("[client] 第 %u 次重连 %s:%d ...\n",
           self->reconnectCount_, self->ip_.c_str(), self->port_);
    self->doConnect();
}

void Client::readCb(struct bufferevent* bev, void* arg)
{
    auto* self = static_cast<Client*>(arg);
    struct evbuffer* in = bufferevent_get_input(bev);

    /* 空闲状态不应收到任何数据：残留的帧数据直接丢弃，不解析、不重连 */
    if (self->state_ == State::Idle) {
        evbuffer_drain(in, -1);
        return;
    }

    while (evbuffer_get_length(in) > 0) {
        if (self->wantHeader_) {
            const int need = kHeadLen - self->hdrGot_;
            const int n = static_cast<int>(
                bufferevent_read(bev, self->hdr_ + self->hdrGot_,
                                 static_cast<size_t>(need)));
            if (n <= 0) break;
            self->hdrGot_ += n;
            if (self->hdrGot_ < kHeadLen) break;
            self->hdrGot_ = 0;

            if (!FrameHeader::isValid(self->hdr_)) {
                LOG_WARN("[client] 非法头标志，重连\n");
                self->scheduleReconnect();
                return;
            }
            self->onHeader(FrameHeader::fromBytes(self->hdr_));
            if (!self->running_.load() || self->state_ == State::Disconnected)
                return;
        } else {
            const int need = static_cast<int>(self->payloadLen_ - self->payloadGot_);
            const int n = static_cast<int>(
                bufferevent_read(bev, self->payload_.data() + self->payloadGot_,
                                 static_cast<size_t>(need)));
            if (n <= 0) break;
            self->payloadGot_ += static_cast<size_t>(n);
            if (self->payloadGot_ >= self->payloadLen_) {
                self->onPayload();
                if (!self->running_.load() || self->state_ == State::Disconnected)
                    return;
            } else {
                break;
            }
        }
    }
}

void Client::eventCb(struct bufferevent* bev, short events, void* arg)
{
    auto* self = static_cast<Client*>(arg);

    if (events & BEV_EVENT_CONNECTED) {
        LOG_INFO("[client] 连接成功 (%s:%d)\n", self->ip_.c_str(), self->port_);
        self->state_ = State::Idle;
        self->recvReset();
        self->setReadTimeout(0);
        self->armHbTimer();
        if (self->autoImage_) self->requestImage();
        return;
    }

    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
        const char* why;
        if (events & BEV_EVENT_TIMEOUT) why = "读超时";
        else if (events & BEV_EVENT_EOF) why = "服务器断开";
        else why = "网络错误";
        LOG_WARN("[client] 连接中断：%s，准备重连...\n", why);
        self->teardownConnection();
        self->scheduleReconnect();
    }
}

void Client::keyControlCb(evutil_socket_t fd, short what, void* arg)
{
    auto* self = static_cast<Client*>(arg);
    const int act = self->keyAction_.exchange(0);

    switch (act) {
    case 's': case 'S':
        /* 开始：恢复图像源（若被暂停）并开始收图，同时打开监控窗口 */
        if (self->bev_) self->sendHead(Ctl::ResumeSource, 0);
        self->requestImage();
        self->preview_.setVisible(true);
        break;
    case 'q': case 'Q':
        /* 暂停：停止本端收图并冻结图像源（全局），恢复后从原位置继续 */
        if (self->state_ == State::Image) self->stopImage();
        if (self->bev_) self->sendHead(Ctl::PauseSource, 0);
        break;
    case 'h': case 'H':
        if (self->state_ == State::Idle) self->hbTimerCb(0, 0, self);
        break;
    case 'b': case 'B':
        if (self->state_ != State::Image) {
            LOG_WARN("[client] 请先按 s 开始图像演示\n");
            break;
        }
        if (self->saveDir_.empty()) self->initSaveDir();
        self->saving_.store(1);
        LOG_INFO("[client] 开始保存图片（每 %d 帧存一张 BMP）\n", self->saveEvery_);
        break;
    case 'e': case 'E':
        self->saving_.store(0);
        LOG_INFO("[client] 停止保存图片（演示继续）\n");
        break;
    case 'x': case 'X':
        /* 关闭监控：只隐藏窗口，程序与收图继续（与网页端 x 一致） */
        self->preview_.setVisible(false);
        LOG_INFO("[client] 关闭监控（窗口隐藏，程序继续运行）\n");
        break;
    case 'o': case 'O':
        /* 打开监控：重新显示窗口，立即刷新最新一帧 */
        self->preview_.setVisible(true);
        LOG_INFO("[client] 打开监控（窗口显示）\n");
        break;
    case 'l': case 'L':
        /* 循环切换日志级别：调试->信息->警告->错误->关闭->调试 */
        {
            const int nxt = (static_cast<int>(Logger::instance().level()) + 1) % 5;
            Logger::instance().setLevel(static_cast<LogLevel>(nxt));
            const char* cn = "关闭";
            switch (static_cast<LogLevel>(nxt)) {
                case LogLevel::kDebug: cn = "调试"; break;
                case LogLevel::kInfo:  cn = "信息"; break;
                case LogLevel::kWarn:  cn = "警告"; break;
                case LogLevel::kError: cn = "错误"; break;
                default:               cn = "关闭"; break;
            }
            printf("[client] 日志级别: %s\n", cn);
        }
        break;
    default:
        break;
    }
}

void Client::uiTimerCb(evutil_socket_t fd, short what, void* arg)
{
    auto* self = static_cast<Client*>(arg);

    if (!self->preview_.pumpMessages()) {
        self->running_.store(0);
        event_base_loopbreak(self->base_);
        return;
    }
    if (!self->preview_.hasWindow()) return;

    const uint32_t now = nowMs();
    if (now - self->fpsStart_ >= 1000) {
        self->fps_ = self->secondFrames_;
        self->secondFrames_ = 0;
        self->fpsStart_ = now;
        self->preview_.setTitle(kImgWidth, kImgHeight,
                                self->frameCount_, self->fps_);
    }
}

void Client::stdinThreadMain(Client* self)
{
    int ch;
    while (self->running_.load() && (ch = getchar()) != EOF) {
        if (ch == '\r' || ch == '\n') continue;
        self->keyAction_.store(ch);
        event_active(self->keyEvent_, EV_READ, 0);
    }
}

/* ---- 主流程 ---- */

int Client::run()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOG_ERROR("WSAStartup 失败\n");
        return 1;
    }
    SetConsoleOutputCP(CP_UTF8);
    evthread_use_windows_threads();   /* libevent 内部锁使用 Windows 线程原语 */
#else
    evthread_use_pthreads();
#endif

    base_ = event_base_new();
    if (!base_) {
        LOG_ERROR("event_base_new 失败\n");
        return 1;
    }

    hbTimer_       = event_new(base_, -1, EV_PERSIST, hbTimerCb, this);
    reconnectTimer_ = event_new(base_, -1, 0, reconnectTimerCb, this);
    keyEvent_      = event_new(base_, -1, 0, keyControlCb, this);
    uiTimer_       = event_new(base_, -1, EV_PERSIST, uiTimerCb, this);

    try {
        stdinThread_ = std::thread(stdinThreadMain, this);
        stdinThread_.detach();
    } catch (...) {
        LOG_ERROR("[client] 键盘线程创建失败\n");
    }

    if (preview_.create(kImgWidth, kImgHeight)) {
        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 10000;
        event_add(uiTimer_, &tv);
        fpsStart_ = nowMs();
        LOG_INFO("预览窗口 : 已打开，实时显示收到的图像（ESC/关闭按钮=隐藏窗口）\n");
    } else {
        LOG_INFO("预览窗口 : 未打开，当前仅存图模式（Linux 下为正常状态）\n");
    }

    printf("=============================================\n");
    printf(" libevent 图像客户端 (C++11)\n");
    printf(" 服务器 : %s:%d\n", ip_.c_str(), port_);
    printf(" 心跳   : 每 %dms 一次（空闲时）\n", kClientHeartbeatIntervalMs);
    printf(" 读超时 : %dms\n", kClientReadTimeoutMs);
    printf(" 按键   : s=开始/恢复     q=暂停（冻结图像源）\n");
    printf("          o=打开监控      x=关闭监控（隐藏窗口）\n");
    printf("          b=开始保存图片  e=停止保存\n");
    printf("          h=手动心跳      l=切换日志级别\n");
printf("          退出按 Ctrl+C\n");
    printf("=============================================\n");

    doConnect();
    event_base_dispatch(base_);

    LOG_INFO("[client] 程序结束\n");
    teardownConnection();
    if (hbTimer_) event_free(hbTimer_);
    if (reconnectTimer_) event_free(reconnectTimer_);
    if (keyEvent_) event_free(keyEvent_);
    if (uiTimer_) event_free(uiTimer_);
    preview_.destroy();
    event_base_free(base_);
    base_ = nullptr;
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}

}  /* namespace cam */
