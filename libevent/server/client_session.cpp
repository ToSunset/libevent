#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "client_session.h"

#include <chrono>
#include <cstring>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>

#include "common/logger.h"
#include "common/protocol.h"
#include "client_manager.h"
#include "image_source.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <ctime>
#endif

namespace cam {

ClientSession::ClientSession(ClientManager* mgr, ImageSource* source,
                             evutil_socket_t fd)
    : mgr_(mgr), source_(source), fd_(fd)
{
    sendBuf_.resize(static_cast<size_t>(kHeadLen + kImgDataLen));
}

ClientSession::~ClientSession()
{
    /* 防御性收尾：正常情况下清理已在控制线程内完成 */
    if (sendThread_.joinable()) {
        sendStop_.store(true);
        source_->wakeAll();
        sendThread_.join();
    }
}

void ClientSession::start()
{
    try {
        ctrlThread_ = std::thread(ctrlThreadMain, this);
        ctrlThread_.detach();
    } catch (...) {
        LOG_ERROR("[NET] 客户端 %lld 创建控制线程失败，断开",
                  static_cast<long long>(fd_));
        evutil_closesocket(fd_);
    }
}

void ClientSession::send(const void* data, int len)
{
    std::lock_guard<std::mutex> lk(sendLock_);
    if (bev_) bufferevent_write(bev_, data, len);
}

/* ---- 图像发送线程 ---- */
void ClientSession::sendThreadMain(ClientSession* self)
{
    while (true) {
        const auto res = self->source_->waitForNewFrame(
            self->lastSeq_, kImgReadyTimeoutMs, self->sendStop_);

        if (res == ImageSource::WaitResult::kStopped) break;

        if (res == ImageSource::WaitResult::kTimeout) {
            /* 超时内没有新图：发一个错误头，然后继续等 */
            FrameHeader h(Ctl::ImgData, Err::ImgTimeout, 0);
            uint8_t hdr[kHeadLen];
            h.toBytes(hdr);
            self->send(hdr, kHeadLen);
            LOG_DEBUG("[NET] 客户端 %lld 图像等待超时，已发错误头",
                      static_cast<long long>(self->fd_));
            continue;
        }

        /* 有新图：拷贝最新一帧到"头+数据"缓冲，合并成一次发送 */
        if (self->source_->copyLatestFrame(
                self->sendBuf_.data() + kHeadLen,
                self->sendBuf_.size() - kHeadLen, &self->lastSeq_)) {
            FrameHeader h(Ctl::ImgData, Err::Ok, kImgDataLen);
            h.toBytes(self->sendBuf_.data());
            self->send(self->sendBuf_.data(),
                       kHeadLen + kImgDataLen);
            self->sendCount_++;
            self->sendBytes_ += static_cast<uint64_t>(kHeadLen + kImgDataLen);
        }
    }
}

void ClientSession::startSending()
{
    if (sendThread_.joinable()) {
        LOG_INFO("[NET] 客户端 %lld 已在传输图像，忽略重复请求",
                 static_cast<long long>(fd_));
        return;
    }
    sendStop_.store(false);
    try {
        sendThread_ = std::thread(sendThreadMain, this);
    } catch (...) {
        LOG_ERROR("[NET] 客户端 %lld 创建发送线程失败",
                  static_cast<long long>(fd_));
    }
    LOG_INFO("[NET] 客户端 %lld 开始传输图像",
             static_cast<long long>(fd_));
}

void ClientSession::stopSending()
{
    if (!sendThread_.joinable()) {
        LOG_INFO("[NET] 客户端 %lld 当前没有图像传输",
                 static_cast<long long>(fd_));
        return;
    }
    sendStop_.store(true);
    source_->wakeAll();   /* 唤醒可能正在等待新帧的发送线程 */
    sendThread_.join();
    LOG_INFO("[NET] 客户端 %lld 停止传输图像（已发 %llu 帧 / %llu 字节）",
             static_cast<long long>(fd_),
             static_cast<unsigned long long>(sendCount_),
             static_cast<unsigned long long>(sendBytes_));
}

/* ---- 心跳应答 ---- */
void ClientSession::handleHeartbeat()
{
    int32_t val;
#ifdef _WIN32
    val = static_cast<int32_t>(GetTickCount64() & 0x7FFFFFFF);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    val = static_cast<int32_t>(
        (ts.tv_sec * 1000 + ts.tv_nsec / 1000000) & 0x7FFFFFFF);
#endif

    uint8_t buf[kHeadLen + kHeartbeatDataLen];
    FrameHeader h(Ctl::HeartbeatResp, Err::Ok, kHeartbeatDataLen);
    h.toBytes(buf);
    std::memcpy(buf + kHeadLen, &val, sizeof(val));
    send(buf, sizeof(buf));
    LOG_DEBUG("[NET] 客户端 %lld 心跳应答 (%d)",
              static_cast<long long>(fd_), static_cast<int>(val));
}

void ClientSession::sendError(Ctl c, Err e)
{
    FrameHeader h(c, e, 0);
    uint8_t buf[kHeadLen];
    h.toBytes(buf);
    send(buf, kHeadLen);
}

/* ---- 控制线程 ---- */
void ClientSession::ctrlThreadMain(ClientSession* self)
{
    self->base_ = event_base_new();
    if (!self->base_) goto fail;

    self->bev_ = bufferevent_socket_new(self->base_, self->fd_,
        BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE);
    if (!self->bev_) goto fail;

    bufferevent_setcb(self->bev_, onRead, nullptr, onEvent, self);
    bufferevent_enable(self->bev_, EV_READ | EV_WRITE);
    event_base_dispatch(self->base_);

    /* ---- 客户端断开，清理 ----
     * 顺序很重要：先停发送线程并等它退出，再释放 bufferevent，
     * 最后摘除全局表（销毁本对象）。 */
    self->sendStop_.store(true);
    self->source_->wakeAll();
    if (self->sendThread_.joinable()) self->sendThread_.join();

    if (self->bev_) {
        bufferevent_free(self->bev_);
        self->bev_ = nullptr;
    }
    if (self->base_) {
        event_base_free(self->base_);
        self->base_ = nullptr;
    }
    LOG_INFO("[NET] 客户端 %lld 断开（已发 %llu 帧 / %llu 字节）",
             static_cast<long long>(self->fd_),
             static_cast<unsigned long long>(self->sendCount_),
             static_cast<unsigned long long>(self->sendBytes_));
    self->mgr_->onClosed(self);
    return;

fail:
    evutil_closesocket(self->fd_);
    if (self->bev_) {
        bufferevent_free(self->bev_);
        self->bev_ = nullptr;
    }
    if (self->base_) {
        event_base_free(self->base_);
        self->base_ = nullptr;
    }
    self->mgr_->onClosed(self);
}

void ClientSession::onRead(struct bufferevent* bev, void* arg)
{
    auto* self = static_cast<ClientSession*>(arg);
    uint8_t hdr[kHeadLen];

    while (evbuffer_get_length(bufferevent_get_input(bev)) >= kHeadLen) {
        if (bufferevent_read(bev, hdr, kHeadLen) != kHeadLen) break;

        if (!FrameHeader::isValid(hdr)) {
            LOG_WARN("[NET] 客户端 %lld 发来非法头，断开",
                     static_cast<long long>(self->fd_));
            event_base_loopexit(self->base_, nullptr);
            return;
        }

        const FrameHeader h = FrameHeader::fromBytes(hdr);
        const Ctl ctl = static_cast<Ctl>(h.ctl);
        switch (ctl) {
        case Ctl::HeartbeatReq:
            self->handleHeartbeat();
            break;
        case Ctl::ReqImage:
            self->startSending();
            break;
        case Ctl::StopImage:
            self->stopSending();
            break;
        case Ctl::PauseSource:
            /* 暂停图像源（全局冻结）：只改标志，不回包 */
            self->source_->pause();
            LOG_INFO("[NET] 客户端 %lld 暂停图像源（全局冻结）",
                     static_cast<long long>(self->fd_));
            break;
        case Ctl::ResumeSource:
            self->source_->resume();
            LOG_INFO("[NET] 客户端 %lld 恢复图像源",
                     static_cast<long long>(self->fd_));
            break;
        case Ctl::CtrlDev:
            self->sendError(Ctl::CtrlDev, Err::CtrlDevNoSupport);
            LOG_WARN("[NET] 客户端 %lld 请求设备控制（未实现）",
                     static_cast<long long>(self->fd_));
            break;
        default:
            self->sendError(Ctl::CtrlDev, Err::UnknownCtl);
            LOG_WARN("[NET] 客户端 %lld 未知控制码 %d",
                     static_cast<long long>(self->fd_), h.ctl);
            break;
        }

        if (h.len > 0)
            LOG_DEBUG("[NET] 客户端 %lld 请求带 %d 字节载荷，已忽略",
                      static_cast<long long>(self->fd_), h.len);
    }
}

void ClientSession::onEvent(struct bufferevent* bev, short events, void* arg)
{
    auto* self = static_cast<ClientSession*>(arg);

    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
        const char* why = "连接断开";
        if (events & BEV_EVENT_TIMEOUT) why = "读超时";
        else if (events & BEV_EVENT_EOF) why = "客户端关闭连接";
        else why = "网络错误";
        LOG_INFO("[NET] 客户端 %lld %s",
                 static_cast<long long>(self->fd_), why);
        event_base_loopexit(self->base_, nullptr);
    }
}

}  /* namespace cam */
