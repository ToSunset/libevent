#include "image_source.hpp"

#include <chrono>
#include <cstring>

#include <event2/event.h>

#include "../common/logger.hpp"
#include "../common/protocol.hpp"

namespace cam {

namespace {
constexpr int kImgGenIntervalDefault = 20;   /* 与 protocol.hpp 的默认值一致 */
}

ImageSource::ImageSource()
{
    frame_.resize(static_cast<size_t>(kImgDataLen));
    intervalMs_.store(kImgGenIntervalDefault);
    curIntervalMs_ = kImgGenIntervalDefault;
}

ImageSource::~ImageSource()
{
    stop();
}

bool ImageSource::start()
{
    if (thread_.joinable()) return true;
    stopped_.store(false);
    try {
        thread_ = std::thread(threadMain, this);
    } catch (...) {
        return false;
    }
    return true;
}

void ImageSource::stop()
{
    stopped_.store(true);
    if (base_) event_base_loopbreak(base_);
    wakeAll();
    if (thread_.joinable()) thread_.join();
}

void ImageSource::breakLoop()
{
    if (base_) event_base_loopbreak(base_);
}

void ImageSource::pause()
{
    running_.store(false);
}

void ImageSource::resume()
{
    running_.store(true);
}

void ImageSource::setIntervalMs(int ms)
{
    if (ms < 10) ms = 10;
    if (ms > 2000) ms = 2000;
    intervalMs_.store(ms);
}

bool ImageSource::copyLatestFrame(uint8_t* out, size_t cap, uint32_t* seqOut)
{
    std::lock_guard<std::mutex> lk(lock_);
    if (cap < frame_.size()) return false;
    std::memcpy(out, frame_.data(), frame_.size());
    if (seqOut) *seqOut = seq_;
    return true;
}

uint32_t ImageSource::seq()
{
    std::lock_guard<std::mutex> lk(lock_);
    return seq_;
}

ImageSource::WaitResult ImageSource::waitForNewFrame(uint32_t lastSeq,
                                                     int timeoutMs,
                                                     const std::atomic<bool>& stop)
{
    std::unique_lock<std::mutex> lk(lock_);
    cond_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] {
        return stop.load() || stopped_.load() || seq_ != lastSeq;
    });
    if (stop.load() || stopped_.load()) return WaitResult::kStopped;
    if (seq_ != lastSeq) return WaitResult::kNew;   /* 超时瞬间恰好来了新帧 */
    return WaitResult::kTimeout;
}

void ImageSource::wakeAll()
{
    std::lock_guard<std::mutex> lk(lock_);
    cond_.notify_all();
}

void ImageSource::threadMain(void* arg)
{
    auto* self = static_cast<ImageSource*>(arg);

    self->base_ = event_base_new();
    if (!self->base_) return;
    self->frameEv_ = event_new(self->base_, -1, EV_PERSIST, onFrameTimer, self);
    if (!self->frameEv_) {
        event_base_free(self->base_);
        self->base_ = nullptr;
        return;
    }
    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = kImgGenIntervalDefault * 1000;
    event_add(self->frameEv_, &tv);
    event_base_dispatch(self->base_);

    event_free(self->frameEv_);
    self->frameEv_ = nullptr;
    event_base_free(self->base_);
    self->base_ = nullptr;
}

void ImageSource::onFrameTimer(evutil_socket_t fd, short what, void* arg)
{
    (void)fd; (void)what;
    static_cast<ImageSource*>(arg)->onTick();
}

void ImageSource::onTick()
{
    struct timeval tv;
    const int want = intervalMs_.load();

    /* 帧间隔变化时先重挂定时器再生成一帧 */
    if (want != curIntervalMs_) {
        curIntervalMs_ = want;
        tv.tv_sec  = 0;
        tv.tv_usec = want * 1000;
        if (frameEv_) {
            event_del(frameEv_);
            event_add(frameEv_, &tv);
        }
    }
    /* 暂停时冻结图像源：不生成、不推进 tick，恢复后从原位置继续 */
    if (!running_.load()) return;
    genOneFrame();
}

void ImageSource::genOneFrame()
{
    if (!bgReady_) {
        bg_.resize(static_cast<size_t>(kImgDataLen));
        for (int y = 0; y < kImgHeight; y++) {
            for (int x = 0; x < kImgWidth; x++)
                bg_[static_cast<size_t>(y) * kImgWidth + x] =
                    static_cast<uint8_t>(x * 255 / (kImgWidth - 1));
        }
        for (int y = 0; y < 48; y++) {
            for (int x = 0; x < kImgWidth; x++) {
                const int step = x / (kImgWidth / 10);
                bg_[static_cast<size_t>(y) * kImgWidth + x] =
                    static_cast<uint8_t>(step * 255 / 9);
            }
        }
        bgReady_ = true;
    }
    const int tick = ++tick_;

    std::lock_guard<std::mutex> lk(lock_);
    std::memcpy(frame_.data(), bg_.data(), bg_.size());

    /* 1. 全屏宽亮带上下扫动（24 像素高，平滑往返） */
    {
        const int half = kImgHeight - 24;
        int bandY = (tick * 8) % (2 * half);
        if (bandY > half) bandY = 2 * half - bandY;
        for (int y = bandY; y < bandY + 24 && y < kImgHeight; y++)
            std::memset(frame_.data() + static_cast<size_t>(y) * kImgWidth, 255,
                        kImgWidth);
    }

    /* 2. 移动白色方块（40x40，沿对角线循环移动） */
    {
        const int BS = 40;
        const int bx = (tick * 6) % (kImgWidth - BS);
        const int by = (tick * 6) % (kImgHeight - BS);
        for (int y = by; y < by + BS; y++) {
            for (int x = bx; x < bx + BS; x++)
                frame_[static_cast<size_t>(y) * kImgWidth + x] = 255;
        }
    }

    /* 3. 左下角大号帧号 */
    drawFrameNumber(frame_.data(), static_cast<int>(genSeq_ + 1));

    seq_ = ++genSeq_;
    cond_.notify_all();
}

const char* ImageSource::font5x7(char ch)
{
    switch (ch) {
    case '0': return "01110" "10001" "10011" "10101" "11001" "10001" "01110";
    case '1': return "00100" "01100" "00100" "00100" "00100" "00100" "01110";
    case '2': return "01110" "10001" "00001" "00010" "00100" "01000" "11111";
    case '3': return "11111" "00010" "00100" "00010" "00001" "10001" "01110";
    case '4': return "00010" "00110" "01010" "10010" "11111" "00010" "00010";
    case '5': return "11111" "10000" "11110" "00001" "00001" "10001" "01110";
    case '6': return "00110" "01000" "10000" "11110" "10001" "10001" "01110";
    case '7': return "11111" "00001" "00010" "00100" "01000" "01000" "01000";
    case '8': return "01110" "10001" "10001" "01110" "10001" "10001" "01110";
    case '9': return "01110" "10001" "10001" "01111" "00001" "00010" "01100";
    case 'A': return "01110" "10001" "10001" "11111" "10001" "10001" "10001";
    case 'F': return "11111" "10000" "10000" "11110" "10000" "10000" "10000";
    case 'R': return "11110" "10001" "10001" "11110" "10100" "10010" "10001";
    case 'M': return "10001" "11011" "10101" "10101" "10001" "10001" "10001";
    case 'E': return "11111" "10000" "10000" "11110" "10000" "10000" "11111";
    case ' ': return "00000" "00000" "00000" "00000" "00000" "00000" "00000";
    default:  return "00000" "00000" "00000" "00000" "00000" "00000" "00000";
    }
}

void ImageSource::drawFrameNumber(uint8_t* buf, int seqNo)
{
    char text[16];
    constexpr int scale = 6;
    int ox = 16;
    const int oy = kImgHeight - 64;

    std::snprintf(text, sizeof(text), "FRAME %05d", seqNo);

    for (int i = 0; text[i] != '\0' && i < 15; i++) {
        const char* g = font5x7(text[i]);
        for (int r = 0; r < 7; r++) {
            for (int c = 0; c < 5; c++) {
                if (g[r * 5 + c] != '1') continue;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        const int xx = ox + c * scale + dx;
                        const int yy = oy + r * scale + dy;
                        if (xx >= 0 && xx < kImgWidth && yy >= 0 && yy < kImgHeight)
                            buf[static_cast<size_t>(yy) * kImgWidth + xx] = 0;
                    }
                }
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++)
                        buf[static_cast<size_t>(oy + r * scale + dy) * kImgWidth +
                            ox + c * scale + dx] = 255;
                }
            }
        }
        ox += 6 * scale;
    }
}

}  /* namespace cam */
