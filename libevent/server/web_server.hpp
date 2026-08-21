#pragma once
/* 浏览器界面（mongoose HTTP，8080 端口）：
 *   /              -> index.html
 *   /frame         -> 最新一帧 PNG（ETag=帧号，帧未变回 304）
 *   /api/stats     -> JSON：帧号/客户端数/运行状态/帧间隔/帧率/心跳数/显示尺寸
 *   /api/start     -> 恢复出图
 *   /api/stop      -> 暂停出图：之后 /frame 返回冻结帧
 *   /api/heartbeat -> 浏览器心跳计数
 *   /api/fps       -> POST body 为毫秒数，动态调整帧间隔
 *   /api/display   -> POST {"w":..,"h":..} 设置浏览器显示尺寸
 * 状态只在 web 线程（mg_mgr_poll 回调）内访问，无需加锁。 */

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "mongoose.h"
#include "../common/protocol.hpp"
#include "png_encoder.hpp"

namespace cam {

class ClientManager;
class ImageSource;

class WebServer {
public:
    WebServer(ImageSource* source, ClientManager* clients);
    ~WebServer();

    bool start();           /* 启动 web 线程；失败返回 false（不影响 C 客户端） */
    void stop();            /* 停止 web 线程并释放 mgr */
    void requestStop();     /* 只置退出标志（信号处理用） */

private:
    static void threadMain(WebServer* self);
    static void onEvent(struct mg_connection* c, int ev, void* evData);

    void handleHttp(struct mg_connection* c, struct mg_http_message* hm);
    bool loadIndexHtml();
    void serveIndex(struct mg_connection* c);
    void serveFrame(struct mg_connection* c, struct mg_http_message* hm);
    void serveStats(struct mg_connection* c);
    void handleFps(struct mg_connection* c, struct mg_http_message* hm);
    void handleDisplay(struct mg_connection* c, struct mg_http_message* hm);
    static void replyJson(struct mg_connection* c, const char* body);

    ImageSource*  source_;
    ClientManager* clients_;
    PngEncoder    png_;

    std::string         indexHtml_;
    std::vector<uint8_t> frozen_;     /* 暂停时冻结的 PNG 帧 */
    uint32_t            frozenSeq_ = 0;
    std::atomic<int>    heartbeat_{0};
    std::atomic<bool>   run_{false};
    std::thread         thread_;
    struct mg_mgr       mgr_{};
    int                 dispW_ = kImgWidth;
    int                 dispH_ = kImgHeight;
};

}  /* namespace cam */
