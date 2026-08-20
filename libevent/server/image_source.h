#pragma once
/* 图像源：独立线程 + libevent 定时器，按固定间隔生成 640x480 8bit 灰度测试图，
 * 并广播"新帧"条件变量。暂停=全局冻结（恢复后从原位置继续）。 */

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <event2/util.h>

struct event;
struct event_base;

namespace cam {

class ImageSource {
public:
    ImageSource();
    ~ImageSource();

    bool start();              /* 启动生成线程，成功返回 true */
    void stop();               /* 停止生成线程并 join（可重复调用） */

    void pause();              /* 暂停生成（全局冻结） */
    void resume();             /* 恢复生成 */
    bool isRunning() const { return running_.load(); }
    bool stopped() const { return stopped_.load(); }

    void setIntervalMs(int ms);   /* 浏览器可调帧间隔（10-2000ms） */
    int  intervalMs() const { return intervalMs_.load(); }

    /* 取最新一帧到 out（cap 需 >= kImgDataLen），成功返回 true */
    uint32_t seq();    /* 当前最新帧号 */
    bool copyLatestFrame(uint8_t* out, size_t cap, uint32_t* seq);

    /* 等"新帧"（发送线程用）：返回 kNew / kTimeout / kStopped */
    enum class WaitResult { kNew, kTimeout, kStopped };
    WaitResult waitForNewFrame(uint32_t lastSeq, int timeoutMs,
                               const std::atomic<bool>& stop);

    void wakeAll();            /* 唤醒所有等待者（停止/断开时调用） */
    void breakLoop();          /* 打断生成线程的事件循环（信号处理用） */

private:
    static void threadMain(void* arg);
    static void onFrameTimer(evutil_socket_t fd, short what, void* arg);
    void onTick();
    void genOneFrame();
    void drawFrameNumber(uint8_t* buf, int seqNo);
    static const char* font5x7(char ch);

    std::mutex              lock_;
    std::condition_variable cond_;
    std::vector<uint8_t>    frame_;        /* 最新一帧（kImgDataLen 字节） */
    uint32_t                seq_ = 0;      /* 已发布帧号 */
    uint32_t                genSeq_ = 0;   /* 本地帧计数 */
    int                     tick_ = 0;

    std::atomic<int>        intervalMs_{20};
    int                     curIntervalMs_ = 20;
    std::atomic<bool>       running_{true};
    std::atomic<bool>       stopped_{false};

    std::thread             thread_;
    struct event_base*      base_ = nullptr;
    struct event*           frameEv_ = nullptr;

    std::vector<uint8_t>    bg_;           /* 静态背景（懒构建） */
    bool                    bgReady_ = false;
};

}  /* namespace cam */
