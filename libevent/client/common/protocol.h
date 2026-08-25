#pragma once
/* 16 字节网络协议（C++11 版）：
 *   "CAM0" (4) + ctl_code (4) + err_code (4) + nDataLen (4) = 16 字节。
 *   字节序与旧版一致：本机字节序直接 memcpy（x86/ARM 均为小端）。
 *   头 + 数据作为一个整体写入 socket，保证不被打断。 */

#include <cstdint>
#include <cstring>

namespace cam {

static constexpr int kHeadLen = 16;

/* 控制码（协议帧 ctl_code 字段） */
enum class Ctl : int32_t {
    HeartbeatReq   = 1,  /* client -> server：请求心跳 */
    HeartbeatResp  = 2,  /* server -> client：心跳应答（带 4 字节数据） */
    ReqImage       = 3,  /* client -> server：请求传输图像 */
    StopImage      = 4,  /* client -> server：停止传输图像 */
    ImgData        = 5,  /* server -> client：图像数据（nDataLen 字节） */
    CtrlDev        = 6,  /* client -> server：请求控制设备（暂未实现） */
    PauseSource    = 7,  /* client -> server：暂停图像源（全局冻结） */
    ResumeSource   = 8,  /* client -> server：恢复图像源 */
};

/* 错误码（协议帧 err_code 字段） */
enum class Err : int32_t {
    Ok               = 0,  /* 正常 */
    ImgTimeout       = 1,  /* 等待 img_ready 超时（5 秒） */
    ServerFull       = 2,  /* 服务器客户端已满（最多 10 个） */
    UnknownCtl       = 3,  /* 未知控制码 */
    CtrlDevNoSupport = 4,  /* 设备控制未实现 */
};

/* 16 字节协议头 */
class FrameHeader {
public:
    char    flag[4];   /* "CAM0" */
    int32_t ctl;
    int32_t err;
    int32_t len;

    FrameHeader() : flag{'C', 'A', 'M', '0'}, ctl(0), err(0), len(0) {}
    FrameHeader(Ctl c, Err e, int32_t dataLen)
        : flag{'C', 'A', 'M', '0'}, ctl(static_cast<int32_t>(c)),
          err(static_cast<int32_t>(e)), len(dataLen) {}

    static constexpr int kSize = kHeadLen;

    /* 是否 16 字节的合法 "CAM0" 头 */
    static bool isValid(const uint8_t* bytes)
    {
        return bytes != nullptr &&
               bytes[0] == 'C' && bytes[1] == 'A' && bytes[2] == 'M' && bytes[3] == '0';
    }

    /* 序列化到 16 字节缓冲区（本机字节序） */
    void toBytes(uint8_t* out) const
    {
        std::memcpy(out, this, kSize);
    }

    /* 从 16 字节缓冲区解析 */
    static FrameHeader fromBytes(const uint8_t* in)
    {
        FrameHeader h;
        std::memcpy(&h, in, kSize);
        return h;
    }
};

/* 图像参数：640x480 8 位灰度 */
static constexpr int kImgWidth  = 640;
static constexpr int kImgHeight = 480;
static constexpr int kImgDataLen = kImgWidth * kImgHeight;

/* 心跳数据长度（字节） */
static constexpr int kHeartbeatDataLen = 4;

/* 服务器默认端口 / 最大客户端数 */
static constexpr int kServerPort = 9995;
static constexpr int kMaxClients = 10;

/* 超时参数（毫秒） */
static constexpr int kImgReadyTimeoutMs         = 5000;
static constexpr int kImgGenIntervalMs          = 20;
static constexpr int kClientReadTimeoutMs       = 3000;
static constexpr int kClientHeartbeatIntervalMs = 3000;
static constexpr int kClientReconnectDelayMs    = 2000;

}  /* namespace cam */
