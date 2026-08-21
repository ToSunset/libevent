#include "web_server.h"

#include <cstdio>
#include <cstdlib>

#include "../common/logger.h"
#include "../common/protocol.h"
#include "client_manager.h"
#include "image_source.h"

namespace cam {

namespace {
constexpr int kWebPort    = 8080;
constexpr int kWebPollMs  = 50;   /* 事件轮询周期，与浏览器页面刷新周期一致 */
}

WebServer::WebServer(ImageSource* source, ClientManager* clients)
    : source_(source), clients_(clients)
{
}

WebServer::~WebServer()
{
    stop();
}

void WebServer::requestStop()
{
    run_.store(false);
}

bool WebServer::start()
{
    if (thread_.joinable()) return true;

    if (!loadIndexHtml())
        LOG_WARN("[server] 警告：读取 index.html 失败，浏览器首页将报错");

    mg_mgr_init(&mgr_);
    if (!mg_http_listen(&mgr_, "http://0.0.0.0:8080", onEvent, this)) {
        LOG_ERROR("[server] 浏览器界面监听 %d 失败", kWebPort);
        mg_mgr_free(&mgr_);
        return false;
    }

    run_.store(true);
    try {
        thread_ = std::thread(threadMain, this);
    } catch (...) {
        run_.store(false);
        mg_mgr_free(&mgr_);
        return false;
    }
    LOG_INFO("[server] 浏览器界面已启动: http://<本机IP>:%d", kWebPort);
    return true;
}

void WebServer::stop()
{
    if (!thread_.joinable()) return;
    run_.store(false);
    thread_.join();
    mg_mgr_free(&mgr_);
    LOG_INFO("[server] 浏览器界面已停止");
}

void WebServer::threadMain(WebServer* self)
{
    while (self->run_.load())
        mg_mgr_poll(&self->mgr_, kWebPollMs);
}

bool WebServer::loadIndexHtml()
{
    FILE* f = std::fopen("index.html", "rb");
    if (!f) return false;
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return false; }
    const long n = std::ftell(f);
    if (n <= 0) { std::fclose(f); return false; }
    if (std::fseek(f, 0, SEEK_SET) != 0) { std::fclose(f); return false; }

    std::string buf;
    buf.resize(static_cast<size_t>(n) + 1);
    if (std::fread(&buf[0], 1, static_cast<size_t>(n), f) !=
        static_cast<size_t>(n)) {
        std::fclose(f);
        return false;
    }
    std::fclose(f);
    buf[static_cast<size_t>(n)] = '\0';
    indexHtml_ = std::move(buf);
    return true;
}

void WebServer::replyJson(struct mg_connection* c, const char* body)
{
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
}

void WebServer::serveIndex(struct mg_connection* c)
{
    if (indexHtml_.empty()) {
        mg_http_reply(c, 500,
                      "Content-Type: text/plain; charset=utf-8\r\n",
                      "%s",
                      "index.html 未加载：请确认该文件与 server 在同一目录");
        return;
    }
    mg_http_reply(c, 200,
                  "Content-Type: text/html; charset=utf-8\r\n"
                  "Cache-Control: no-cache\r\n",
                  "%s", indexHtml_.c_str());
}

void WebServer::serveFrame(struct mg_connection* c, struct mg_http_message* hm)
{
    std::vector<uint8_t> png;
    uint32_t seq = 0;

    if (source_->isRunning()) {
        /* 运行中：取最新一帧编码为 PNG，并同步到冻结缓存 */
        std::vector<uint8_t> gray(static_cast<size_t>(kImgDataLen));
        if (!source_->copyLatestFrame(gray.data(), gray.size(), &seq)) {
            mg_http_reply(c, 503, "", "%s", "{\"error\":\"no frame\"}");
            return;
        }
        png = png_.encodeGray(gray.data(), kImgWidth, kImgHeight);
        if (png.empty()) {
            mg_http_reply(c, 500, "", "%s", "{\"error\":\"png encode\"}");
            return;
        }
        frozen_    = png;
        frozenSeq_ = seq;
    } else {
        /* 暂停：返回冻结帧（200，不 503），页面不闪烁 */
        if (frozen_.empty()) {
            mg_http_reply(c, 503, "", "%s", "{\"error\":\"no frame yet\"}");
            return;
        }
        seq = frozenSeq_;
        png = frozen_;
    }

    /* ETag = 帧号：帧未变时回 304，浏览器保留当前图像 */
    char etag[48];
    std::snprintf(etag, sizeof(etag), "\"frame-%u\"", seq);
    struct mg_str* inm = mg_http_get_header(hm, "If-None-Match");
    if (inm != nullptr && mg_match(*inm, mg_str(etag), nullptr)) {
        mg_http_reply(c, 304, "", "%s", "");
        return;
    }
    mg_printf(c,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: image/png\r\n"
              "Cache-Control: no-cache\r\n"
              "ETag: %s\r\n"
              "Content-Length: %u\r\n"
              "Connection: close\r\n"
              "\r\n",
              etag, static_cast<unsigned>(png.size()));
    mg_send(c, png.data(), png.size());
    c->is_resp = 0;   /* 手动写完整响应，避免 mongoose 自动补 body */
}

void WebServer::serveStats(struct mg_connection* c)
{
    char buf[256];
    const bool running = source_->isRunning();
    const int  interval = source_->intervalMs();
    const int  hb       = heartbeat_.load();

    std::snprintf(buf, sizeof(buf),
                  "{\"seq\":%u,\"clients\":%d,\"running\":%s,"
                  "\"interval_ms\":%d,\"fps\":%d,\"heartbeat\":%d,"
                  "\"disp_w\":%d,\"disp_h\":%d}",
                  source_->seq(), clients_->count(),
                  running ? "true" : "false", interval,
                  interval > 0 ? 1000 / interval : 0, hb,
                  dispW_, dispH_);
    replyJson(c, buf);
}

void WebServer::handleFps(struct mg_connection* c, struct mg_http_message* hm)
{
    char ms[16] = "0";
    char reply[64];

    if (hm->body.len > 0 && hm->body.len < sizeof(ms))
        std::snprintf(ms, sizeof(ms), "%.*s", static_cast<int>(hm->body.len),
                      hm->body.buf);
    const int val = std::atoi(ms);
    source_->setIntervalMs(val);
    std::snprintf(reply, sizeof(reply), "{\"ok\":true,\"interval_ms\":%d}",
                  source_->intervalMs());
    replyJson(c, reply);
}

void WebServer::handleDisplay(struct mg_connection* c, struct mg_http_message* hm)
{
    const long w = mg_json_get_long(hm->body, "$.w", -1);
    const long h = mg_json_get_long(hm->body, "$.h", -1);
    char reply[64];

    if (w < 160 || w > 1920 || h < 120 || h > 1080) {
        mg_http_reply(c, 400, "", "%s",
            "{\"ok\":false,\"error\":\"invalid size (w:160-1920, h:120-1080)\"}");
        return;
    }
    dispW_ = static_cast<int>(w);
    dispH_ = static_cast<int>(h);
    LOG_INFO("[server] 浏览器界面显示尺寸设为 %dx%d", dispW_, dispH_);
    std::snprintf(reply, sizeof(reply), "{\"ok\":true,\"w\":%d,\"h\":%d}",
                  dispW_, dispH_);
    replyJson(c, reply);
}

void WebServer::handleHttp(struct mg_connection* c, struct mg_http_message* hm)
{
    if (mg_match(hm->uri, mg_str("/"), nullptr) ||
        mg_match(hm->uri, mg_str("/index.html"), nullptr)) {
        serveIndex(c);
    } else if (mg_match(hm->uri, mg_str("/frame"), nullptr)) {
        serveFrame(c, hm);
    } else if (mg_match(hm->uri, mg_str("/api/stats"), nullptr)) {
        serveStats(c);
    } else if (mg_match(hm->uri, mg_str("/api/start"), nullptr)) {
        source_->resume();
        LOG_INFO("[server] 浏览器界面恢复出图");
        replyJson(c, "{\"ok\":true,\"running\":true}");
    } else if (mg_match(hm->uri, mg_str("/api/stop"), nullptr)) {
        source_->pause();
        LOG_INFO("[server] 浏览器界面暂停出图（返回冻结帧）");
        replyJson(c, "{\"ok\":true,\"running\":false}");
    } else if (mg_match(hm->uri, mg_str("/api/heartbeat"), nullptr)) {
        const int n = heartbeat_.fetch_add(1) + 1;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "{\"ok\":true,\"heartbeat\":%d}", n);
        replyJson(c, buf);
    } else if (mg_match(hm->uri, mg_str("/api/fps"), nullptr)) {
        handleFps(c, hm);
    } else if (mg_match(hm->uri, mg_str("/api/display"), nullptr)) {
        handleDisplay(c, hm);
    } else {
        mg_http_reply(c, 404, "", "%s", "{\"error\":\"not found\"}");
    }
}

void WebServer::onEvent(struct mg_connection* c, int ev, void* evData)
{
    if (ev != MG_EV_HTTP_MSG) return;
    auto* self = static_cast<WebServer*>(c->fn_data);
    if (self) self->handleHttp(c, static_cast<struct mg_http_message*>(evData));
}

}  /* namespace cam */
