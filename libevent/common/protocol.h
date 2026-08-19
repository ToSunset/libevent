#ifndef LIBEVENT_IMG_PROTOCOL_H
#define LIBEVENT_IMG_PROTOCOL_H

#include <stdint.h>

/* 16 字节数据头（4+4+4+4，默认对齐下正好 16 字节） */
typedef struct _img_head
{
    char     flag[4];     /* 传输结构头标志："CAM0" */
    int32_t  ctl_code;    /* 控制码 */
    int32_t  err_code;    /* 错误码 */
    int32_t  nDataLen;    /* 后面要传输的数据长度 */
} ImgHead_t;

/* 编译期校验：头必须正好 16 字节 */
typedef char assert_img_head_16[(sizeof(ImgHead_t) == 16) ? 1 : -1];

#define IMG_HEAD_LEN  16

/* 控制码 */
#define CTL_HEARTBEAT_REQ   1   /* 客户端 -> 服务器：请求心跳 */
#define CTL_HEARTBEAT_RESP  2   /* 服务器 -> 客户端：心跳应答（带 4 字节数据） */
#define CTL_REQ_IMAGE       3   /* 客户端 -> 服务器：请求传输图像 */
#define CTL_STOP_IMAGE      4   /* 客户端 -> 服务器：停止传输图像 */
#define CTL_IMG_DATA        5   /* 服务器 -> 客户端：图像数据（nDataLen 字节） */
#define CTL_CTRL_DEV        6   /* 客户端 -> 服务器：请求控制设备（暂未实现） */

/* 错误码 */
#define ERR_OK                  0   /* 正常 */
#define ERR_IMG_TIMEOUT         1   /* 等待 img_ready 超时（5 秒） */
#define ERR_SERVER_FULL         2   /* 服务器客户端已满（最多 10 个） */
#define ERR_UNKNOWN_CTL         3   /* 未知控制码 */
#define ERR_CTRL_DEV_NOSUPPORT  4   /* 设备控制未实现 */

/* 图像参数：640x480 8 位灰度 */
#define IMG_WIDTH      640
#define IMG_HEIGHT     480
#define IMG_DATA_LEN   (IMG_WIDTH * IMG_HEIGHT)

/* 心跳数据长度（字节） */
#define HEARTBEAT_DATA_LEN  4

/* 服务器默认端口 / 最大客户端数 */
#define SERVER_PORT   9995
#define MAX_CLIENTS   10

/* 超时参数（毫秒） */
#define IMG_READY_TIMEOUT_MS         5000  /* 发送线程等待 img_ready */
#define IMG_GEN_INTERVAL_MS         20    /* 图像生成间隔（50 帧/秒） */
#define CLIENT_READ_TIMEOUT_MS       3000  /* 客户端读超时 */
#define CLIENT_HEARTBEAT_INTERVAL_MS 3000  /* 客户端心跳间隔 */
#define CLIENT_RECONNECT_DELAY_MS    2000  /* 客户端断线重连延时 */

#endif /* LIBEVENT_IMG_PROTOCOL_H */