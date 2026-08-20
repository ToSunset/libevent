/* server.c —— libevent 图像服务器（跨平台：Linux / Windows-MinGW）
 *
 * 【架构讲解】
 *   这是一个"一对多"的图像服务器：一台服务器同时向最多 10 个客户端推送
 *   实时生成的灰度测试图。为了让网络收发不阻塞图像生成，程序拆成三类线程：
 *
 *   1) 图像生成线程（全局唯一）
 *      用 libevent 的 EV_PERSIST 定时器按 20ms 周期（50fps）生成一帧
 *      640x480 灰度图，写入全局缓冲 g_img_buf 并递增帧序号，然后广播
 *      "有新帧"的条件变量。
 *
 *   2) 控制线程（每客户端一个）
 *      每个客户端连接后单独开一个线程，线程内拥有独立的 event_base 和
 *      bufferevent，负责解析 16 字节协议头（心跳/请求图像/停止图像），
 *      并启停本客户端的图像发送线程。
 *
 *   3) 图像发送线程（每客户端一个，按需创建）
 *      阻塞在"新帧"条件变量上，一有新帧就把最新一帧发给对应客户端。
 *
 *   线程间协作：
 *      - g_img_lock + g_img_cond：生产（生成线程）与消费（发送线程）之间的
 *        生产者-消费者模型，用条件变量做广播通知；
 *      - 每客户端 send_lock：保证"16 字节头 + 图像数据"连续写出；
 *      - evthread_use_pthreads()/evthread_use_windows_threads() 开启
 *        libevent 内部线程安全。
 *
 * 【跨平台说明】
 *   本文件是纯 POSIX C，不依赖 Windows API（无 windows.h / winsock2.h）：
 *     - 线程/锁/条件变量：pthread（Linux 原生；Windows 用 MinGW 的 winpthreads）
 *     - 原子操作：C11 <stdatomic.h>
 *     - 定时器：libevent 事件定时器（替代 Windows CreateWaitableTimer）
 *     - 网络：全部走 libevent 跨平台接口（evconnlistener / bufferevent）
 *   编译：
 *     Linux   : gcc server.c -o server -levent -lpthread
 *     Windows : MSYS2/MinGW-w64 下  gcc server.c -o server.exe -levent -lpthread
 *               （libevent 也要用 MinGW 版：pacman -S mingw-w64-x86_64-libevent）
 *
 * 功能：
 *   1. 图像生成线程：每 20ms 生成一帧 640x480 8 位灰度测试图，广播 img_ready
 *   2. 监听客户端连接，最多 10 个
 *   3. 每客户端一个控制线程：处理心跳 / 请求图像 / 停止图像 / 设备控制
 *   4. 每客户端一个图像发送线程：等待 img_ready（5 秒超时）后发送
 *      16 字节头 + 640x480 图像数据
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>

#ifdef _WIN32
#include <winsock2.h>   /* 仅提供 struct sockaddr_in 等类型（MinGW 下不含 windows.h） */
#else
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

/* libevent 头文件：event_base（事件循环）、bufferevent（带缓冲的 socket
 * 封装）、listener（监听器）。libevent 本身跨平台，这套头在 Linux/Windows
 * 下完全一致。 */
#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/thread.h>

#include "protocol.h"
#include "mongoose.h"

/* ---------------- 全局：图像生成 ----------------
 * 生产者-消费者模型：
 *   g_img_buf 保存"最新的一帧"，g_img_seq 是它的帧序号。
 *   生成线程持锁写入并递增序号；各发送线程在锁上等条件变量，序号一变就
 *   把新帧拷走。用"帧序号"而不是"标志位"，可以精确判断"有没有新帧"，
 *   避免丢帧或重复处理。 */
static pthread_mutex_t    g_img_lock;   /* 保护 g_img_buf / g_img_seq */
static pthread_cond_t     g_img_cond;   /* img_ready 广播信号 */
static uint8_t            g_img_buf[IMG_DATA_LEN];  /* 最新一帧 */
static uint32_t           g_img_seq = 0;            /* 帧序号 */
static volatile sig_atomic_t g_running = 1;         /* Ctrl+C 置 0 退出 */
static pthread_t          g_gen_thread = 0;         /* 图像生成线程 */
static struct event_base *g_gen_base = NULL;        /* 生成线程的定时器事件循环 */
static struct event      *g_frame_ev = NULL;        /* 20ms 周期定时器事件 */
static int                g_gen_tick = 0;           /* 帧计数（驱动动画位置） */
static atomic_int          g_gen_interval_ms = IMG_GEN_INTERVAL_MS; /* 浏览器可调帧间隔 */
static int                 g_cur_gen_interval_ms = IMG_GEN_INTERVAL_MS; /* 当前生效间隔 */
static atomic_int          g_web_running = 1;    /* 图像源运行开关：1=生成中 0=暂停（全局冻结） */

/* ---------------- 全局：客户端管理 ---------------- */
typedef struct client_s client_t;
static pthread_mutex_t g_clients_lock;
static client_t        *g_clients[MAX_CLIENTS];
static atomic_int      g_client_count = 0;   /* 当前在线客户端数 */

struct client_s
{
    evutil_socket_t fd;           /* 已接受的 socket，交给 bufferevent 接管 */
    struct event_base  *base;     /* 本客户端专属事件循环（控制线程内创建/销毁） */
    struct bufferevent *bev;      /* 本客户端的收发封装 */

    pthread_mutex_t send_lock;    /* 串行化本客户端的所有发送（头+数据不被打断） */

    pthread_t ctrl_thread;        /* 控制线程 */
    pthread_t send_thread;        /* 图像发送线程（未启动为 0） */
    atomic_int send_stop;         /* 停止发送线程标志 */
    uint32_t last_seq;            /* 发送线程已发送的最新帧序号 */

    int slot;                     /* 全局表中的下标，-1 表示未登记（仅锁内访问） */
};

/* ---------------- 工具函数 ---------------- */
/* 构造 16 字节协议头：固定标志 "CAM0" + 控制码 + 错误码 + 数据长度。
 * 所有交互都以协议头开始，两端靠 flag=="CAM0" 判断字节流是否对齐。 */
static void build_head(uint8_t *out, int32_t ctl, int32_t err, int32_t len)
{
    ImgHead_t *h = (ImgHead_t *)out;
    h->flag[0] = 'C'; h->flag[1] = 'A'; h->flag[2] = 'M'; h->flag[3] = '0';
    h->ctl_code = ctl;
    h->err_code = err;
    h->nDataLen = len;
}

/* 向客户端发送数据（加锁，保证头与数据不被打断）。
 * bufferevent_write 只是把数据拷进 libevent 的输出缓冲，真正的网络发送
 * 由事件循环异步完成，所以这里不会阻塞；加锁是为了防止"头+数据"之间
 * 被同一客户端的其他发送插队，破坏协议帧的连续性。 */
static void send_client(client_t *c, const void *data, int len)
{
    pthread_mutex_lock(&c->send_lock);
    if (c->bev)
        bufferevent_write(c->bev, data, len);
    pthread_mutex_unlock(&c->send_lock);
}

/* ---------------- 图像发送线程（每客户端一个） ----------------
 * 工作循环：
 *   等"新帧"条件变量 -> 有新帧就把最新一帧 memcpy 到私有缓冲
 *   -> 在锁外发送（16 字节头 + 图像数据）。
 * 关键设计：
 *   1) 条件变量必须配锁使用：pthread_cond_timedwait 在等待时自动释放锁、
 *      被唤醒后自动重新持锁，期间不会漏掉"新帧"信号；
 *   2) 只拷"最新一帧"而不是逐帧排队：客户端跟不上时自然跳帧，
 *      保证延迟最低；
 *   3) 网络发送在锁外进行：某个客户端发送慢不会卡住图像生成线程。
 */
static void *send_image_thread(void *arg)
{
    client_t *c = (client_t *)arg;
    uint8_t *frame = (uint8_t *)malloc(IMG_DATA_LEN);
    uint8_t  hdr[IMG_HEAD_LEN];
    int      timed_out = 0;

    if (!frame)
        return (void *)1;

    while (g_running && !atomic_load(&c->send_stop)) {
        pthread_mutex_lock(&g_img_lock);

        /* 等待 img_ready 广播信号，超时 5 秒（pthread_cond_timedwait 用
         * CLOCK_REALTIME 绝对时间，超时返回 ETIMEDOUT） */
        while (g_running && !atomic_load(&c->send_stop) &&
               g_img_seq == c->last_seq) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += IMG_READY_TIMEOUT_MS / 1000;
            ts.tv_nsec += (long)(IMG_READY_TIMEOUT_MS % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            if (pthread_cond_timedwait(&g_img_cond, &g_img_lock, &ts) ==
                ETIMEDOUT) {
                timed_out = 1;   /* 等待超时 */
                break;
            }
        }

        if (!g_running || atomic_load(&c->send_stop)) {
            pthread_mutex_unlock(&g_img_lock);
            break;
        }

        if (timed_out && g_img_seq == c->last_seq) {
            /* 5 秒内没有新图：发一个错误头，然后继续等 */
            pthread_mutex_unlock(&g_img_lock);
            timed_out = 0;
            build_head(hdr, CTL_IMG_DATA, ERR_IMG_TIMEOUT, 0);
            send_client(c, hdr, IMG_HEAD_LEN);
            printf("[server] 客户端 %lld 图像等待超时，已发错误头\n",
                   (long long)c->fd);
            continue;
        }
        timed_out = 0;

        /* 有新图：在锁内拷贝最新一帧（发送在锁外进行） */
        c->last_seq = g_img_seq;
        memcpy(frame, g_img_buf, IMG_DATA_LEN);
        pthread_mutex_unlock(&g_img_lock);

        /* 先发 16 字节数据头，再发 640x480 图像 */
        build_head(hdr, CTL_IMG_DATA, ERR_OK, IMG_DATA_LEN);
        send_client(c, hdr, IMG_HEAD_LEN);
        send_client(c, frame, IMG_DATA_LEN);
    }

    free(frame);
    return 0;
}

/* ---------------- 控制命令处理 ---------------- */
/* 心跳应答：回 4 字节数据（当前单调时钟毫秒数的截断值），证明链路通畅。
 * 客户端空闲时每 3 秒发一次心跳，这里只做应答，不改变任何状态。 */
static void handle_heartbeat(client_t *c)
{
    uint8_t buf[IMG_HEAD_LEN + HEARTBEAT_DATA_LEN];
    struct timespec ts;
    int32_t val;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    val = (int32_t)((ts.tv_sec * 1000 + ts.tv_nsec / 1000000) & 0x7FFFFFFF);

    build_head(buf, CTL_HEARTBEAT_RESP, ERR_OK, HEARTBEAT_DATA_LEN);
    memcpy(buf + IMG_HEAD_LEN, &val, HEARTBEAT_DATA_LEN);
    send_client(c, buf, sizeof(buf));
    printf("[server] 客户端 %lld 心跳应答 (%d)\n", (long long)c->fd, (int)val);
}

/* 请求图像：启动本客户端的发送线程。若已在传输则忽略重复请求；
 * 先把 send_stop 清零（允许发送线程工作），再创建线程。 */
static void handle_req_image(client_t *c)
{
    if (c->send_thread) {
        printf("[server] 客户端 %lld 已在传输图像，忽略重复请求\n",
               (long long)c->fd);
        return;
    }
    atomic_store(&c->send_stop, 0);
    pthread_create(&c->send_thread, NULL, send_image_thread, c);
    printf("[server] 客户端 %lld 开始传输图像\n", (long long)c->fd);
}

/* 停止图像：置 send_stop=1，并广播唤醒可能正阻塞在"等新帧"上的发送线程
 * （其他客户端会跟着空醒一次，但检查条件后仍会继续等待，无害），
 * 然后等发送线程真正退出并回收线程句柄。 */
static void handle_stop_image(client_t *c)
{
    if (!c->send_thread) {
        printf("[server] 客户端 %lld 当前没有图像传输\n", (long long)c->fd);
        return;
    }
    atomic_store(&c->send_stop, 1);
    /* 唤醒可能正在等待 img_ready 的发送线程（其他客户端会空醒一次，无害） */
    pthread_mutex_lock(&g_img_lock);
    pthread_cond_broadcast(&g_img_cond);
    pthread_mutex_unlock(&g_img_lock);

    pthread_join(c->send_thread, NULL);
    c->send_thread = 0;
    printf("[server] 客户端 %lld 停止传输图像\n", (long long)c->fd);
}

/* ---------------- 控制线程 ---------------- */
/* 控制线程的读回调：只要输入缓冲里还有数据就循环处理。
 * 本协议的控制请求都只有 16 字节头、没有载荷，所以按"整头"读取：
 *   心跳 -> 应答；请求/停止图像 -> 启停发送线程；
 *   设备控制/未知码 -> 回错误头。
 * flag 校验失败说明字节流错位（例如上一帧数据没对齐），直接断开。 */
static void ctrl_read_cb(struct bufferevent *bev, void *arg)
{
    client_t *c = (client_t *)arg;
    uint8_t hdr[IMG_HEAD_LEN];

    while (evbuffer_get_length(bufferevent_get_input(bev)) >= IMG_HEAD_LEN) {
        if (bufferevent_read(bev, hdr, IMG_HEAD_LEN) != IMG_HEAD_LEN)
            break;
        {
            ImgHead_t *h = (ImgHead_t *)hdr;
            if (h->flag[0] != 'C' || h->flag[1] != 'A' ||
                h->flag[2] != 'M' || h->flag[3] != '0') {
                printf("[server] 客户端 %lld 发来非法头，断开\n",
                       (long long)c->fd);
                event_base_loopexit(c->base, NULL);
                return;
            }
            switch (h->ctl_code) {
            case CTL_HEARTBEAT_REQ:
                handle_heartbeat(c);
                break;
            case CTL_REQ_IMAGE:
                handle_req_image(c);
                break;
            case CTL_STOP_IMAGE:
                handle_stop_image(c);
                break;
            case CTL_PAUSE_SOURCE:
                /* 暂停图像源（全局冻结）：只改标志，不回包 */
                atomic_store(&g_web_running, 0);
                printf("[server] 客户端 %lld 暂停图像源（全局冻结）\n",
                       (long long)c->fd);
                break;
            case CTL_RESUME_SOURCE:
                /* 恢复图像源：只改标志，不回包 */
                atomic_store(&g_web_running, 1);
                printf("[server] 客户端 %lld 恢复图像源\n",
                       (long long)c->fd);
                break;
            case CTL_CTRL_DEV:
                /* 设备控制暂未实现：应答"未支持" */
                build_head(hdr, CTL_CTRL_DEV, ERR_CTRL_DEV_NOSUPPORT, 0);
                send_client(c, hdr, IMG_HEAD_LEN);
                printf("[server] 客户端 %lld 请求设备控制（未实现）\n",
                       (long long)c->fd);
                break;
            default:
                build_head(hdr, CTL_CTRL_DEV, ERR_UNKNOWN_CTL, 0);
                send_client(c, hdr, IMG_HEAD_LEN);
                printf("[server] 客户端 %lld 未知控制码 %d\n",
                       (long long)c->fd, h->ctl_code);
                break;
            }
            /* 本协议请求均不带载荷，如带则忽略（仅提示） */
            if (h->nDataLen > 0)
                printf("[server] 客户端 %lld 请求带 %d 字节载荷，已忽略\n",
                       (long long)c->fd, h->nDataLen);
        }
    }
}

/* 事件回调：读超时 / 客户端断开 / 网络错误时，结束本客户端的事件循环，
 * 控制线程随后统一走清理流程（停发送线程、摘除全局表、释放资源）。 */
static void ctrl_event_cb(struct bufferevent *bev, short events, void *arg)
{
    client_t *c = (client_t *)arg;

    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
        const char *why = "连接断开";
        if (events & BEV_EVENT_TIMEOUT)
            why = "读超时";
        else if (events & BEV_EVENT_EOF)
            why = "客户端关闭连接";
        else
            why = "网络错误";
        printf("[server] 客户端 %lld %s\n", (long long)c->fd, why);
        event_base_loopexit(c->base, NULL);
    }
}

/* 控制线程主函数：每个客户端一个独立线程 + 独立 event_base。
 * 为什么不用全局一个事件循环？—— 各客户端并行处理互不阻塞；配合
 * evthread_use_pthreads()/evthread_use_windows_threads()，libevent 内部
 * 结构在线程间是安全的。
 * event_base_dispatch() 一直运行到回调里调用 event_base_loopexit() 为止，
 * 返回后即开始清理本客户端的资源。 */
static void *ctrl_thread_main(void *arg)
{
    client_t *c = (client_t *)arg;

    c->base = event_base_new();
    if (!c->base)
        goto fail;

    c->bev = bufferevent_socket_new(c->base, c->fd,
                              BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE);
    if (!c->bev)
        goto fail;

    bufferevent_setcb(c->bev, ctrl_read_cb, NULL, ctrl_event_cb, c);
    bufferevent_enable(c->bev, EV_READ | EV_WRITE);
    event_base_dispatch(c->base);

    /* ---- 客户端断开，清理 ----
     * 清理顺序很重要：先通知发送线程停止并等它退出，再释放 bufferevent，
     * 避免发送线程还在使用已被释放的资源；最后摘除全局表并减计数。 */
    atomic_store(&c->send_stop, 1);
    pthread_mutex_lock(&g_img_lock);
    pthread_cond_broadcast(&g_img_cond);
    pthread_mutex_unlock(&g_img_lock);

    if (c->send_thread) {
        pthread_join(c->send_thread, NULL);
        c->send_thread = 0;
    }

    pthread_mutex_lock(&g_clients_lock);
    if (c->slot >= 0) {
        g_clients[c->slot] = NULL;
        c->slot = -1;
    }
    pthread_mutex_unlock(&g_clients_lock);
    atomic_fetch_add(&g_client_count, -1);
    printf("[server] 客户端 %lld 已清理，当前 %d/%d\n",
           (long long)c->fd, atomic_load(&g_client_count), MAX_CLIENTS);

    if (c->bev)
        bufferevent_free(c->bev);
    if (c->base)
        event_base_free(c->base);
    pthread_mutex_destroy(&c->send_lock);
    free(c);
    return 0;

fail:
    evutil_closesocket(c->fd);
    if (c->bev)
        bufferevent_free(c->bev);
    if (c->base)
        event_base_free(c->base);
    pthread_mutex_destroy(&c->send_lock);
    pthread_mutex_lock(&g_clients_lock);
    if (c->slot >= 0) {
        g_clients[c->slot] = NULL;
        c->slot = -1;
    }
    pthread_mutex_unlock(&g_clients_lock);
    atomic_fetch_add(&g_client_count, -1);
    free(c);
    return (void *)1;
}

/* ---------------- 图像生成 ---------------- */
static const char *font5x7(char ch)
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

/* 在测试图上绘制帧号：5x7 点阵放大 6 倍，白字黑描边 */
static void draw_frame_number(uint8_t *buf, int seq_no)
{
    char text[16];
    const int scale = 6;
    int ox = 16;
    int oy = IMG_HEIGHT - 64;

    snprintf(text, sizeof(text), "FRAME %05d", seq_no);

    for (int i = 0; text[i] != '\0' && i < 15; i++) {
        const char *g = font5x7(text[i]);
        for (int r = 0; r < 7; r++) {
            for (int c = 0; c < 5; c++) {
                if (g[r * 5 + c] != '1')
                    continue;
                /* 先画 1 像素黑色描边 */
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int xx = ox + c * scale + dx;
                        int yy = oy + r * scale + dy;
                        if (xx >= 0 && xx < IMG_WIDTH && yy >= 0 && yy < IMG_HEIGHT)
                            buf[yy * IMG_WIDTH + xx] = 0;
                    }
                }
                /* 再填白色块 */
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++)
                        buf[(oy + r * scale + dy) * IMG_WIDTH + ox + c * scale + dx] = 255;
                }
            }
        }
        ox += 6 * scale;
    }
}

/* 生成一帧测试图（由 20ms 定时器回调驱动）。
 * 节奏控制：libevent EV_PERSIST 定时器固定 20ms/帧，跨平台且足够稳定。
 * 生成策略：静态背景（水平渐变 + 顶部阶梯条）只计算一次，之后每帧
 * 先 memcpy，再叠加 3 个动态元素（上下扫动的亮带、移动方块、帧号），
 * 最后递增帧序号并广播条件变量。对 g_img_buf 的写入全程持锁。 */
static void gen_one_frame(void)
{
    static uint8_t bg[IMG_DATA_LEN];   /* 静态背景：水平渐变 + 顶部阶梯条 */
    static int     bg_ready = 0;
    static uint32_t seq = 0;
    int tick;

    if (!bg_ready) {
        for (int y = 0; y < IMG_HEIGHT; y++) {
            for (int x = 0; x < IMG_WIDTH; x++)
                bg[y * IMG_WIDTH + x] = (uint8_t)(x * 255 / (IMG_WIDTH - 1));
        }
        for (int y = 0; y < 48; y++) {
            for (int x = 0; x < IMG_WIDTH; x++) {
                int step = x / (IMG_WIDTH / 10);
                bg[y * IMG_WIDTH + x] = (uint8_t)(step * 255 / 9);
            }
        }
        bg_ready = 1;
    }
    tick = ++g_gen_tick;

    pthread_mutex_lock(&g_img_lock);
    memcpy(g_img_buf, bg, sizeof(bg));
    /* 动态元素：1.全屏宽亮带上下扫动  2.移动白色方块  3.帧号 */
    /* 1. 全屏宽亮带上下扫动（24 像素高，平滑往返） */
    {
        const int half = IMG_HEIGHT - 24;
        int band_y = (tick * 8) % (2 * half);
        if (band_y > half)
            band_y = 2 * half - band_y;
        for (int y = band_y; y < band_y + 24 && y < IMG_HEIGHT; y++)
            memset(g_img_buf + y * IMG_WIDTH, 255, IMG_WIDTH);
    }

    /* 2. 移动白色方块（40x40，沿对角线循环移动） */
    {
        const int BS = 40;
        int bx = (tick * 6) % (IMG_WIDTH - BS);
        int by = (tick * 6) % (IMG_HEIGHT - BS);
        for (int y = by; y < by + BS; y++) {
            for (int x = bx; x < bx + BS; x++)
                g_img_buf[y * IMG_WIDTH + x] = 255;
        }
    }

    /* 3. 左下角大号帧号 */
    draw_frame_number(g_img_buf, (int)(seq + 1));

    g_img_seq = ++seq;
    pthread_mutex_unlock(&g_img_lock);

    pthread_cond_broadcast(&g_img_cond);   /* 广播 img_ready */
}

/* 20ms 周期定时器回调（EV_PERSIST，自动重复触发） */
static void on_frame_timer(evutil_socket_t fd, short what, void *arg)
{
    struct timeval tv;
    int want = atomic_load(&g_gen_interval_ms);
    (void)fd; (void)what; (void)arg;

    /* 浏览器界面可动态调整帧间隔：变化时先重挂定时器再生成一帧 */
    if (want != g_cur_gen_interval_ms) {
        g_cur_gen_interval_ms = want;
        tv.tv_sec = 0;
        tv.tv_usec = want * 1000;
        event_del(g_frame_ev);
        event_add(g_frame_ev, &tv);
    }
    /* 暂停时冻结图像源：不生成、不推进 g_gen_tick，恢复后从原位置继续 */
    if (!atomic_load(&g_web_running))
        return;
    gen_one_frame();
}

/* 图像生成线程：跑一个只含定时器事件的 libevent 循环。
 * 替代 Windows CreateWaitableTimer，完全跨平台；SIGINT 时 loopbreak 退出。 */
static void *gen_image_thread(void *arg)
{
    struct timeval tv;
    (void)arg;

    g_gen_base = event_base_new();
    if (!g_gen_base)
        return (void *)1;
    g_frame_ev = event_new(g_gen_base, -1, EV_PERSIST, on_frame_timer, NULL);
    if (!g_frame_ev) {
        event_base_free(g_gen_base);
        g_gen_base = NULL;
        return (void *)1;
    }
    tv.tv_sec = 0;
    tv.tv_usec = IMG_GEN_INTERVAL_MS * 1000;
    event_add(g_frame_ev, &tv);
    event_base_dispatch(g_gen_base);

    event_free(g_frame_ev);
    g_frame_ev = NULL;
    event_base_free(g_gen_base);
    g_gen_base = NULL;
    return 0;
}

/* ---------------- 浏览器界面（mongoose HTTP + 极简 PNG 编码） ----------------
 * 在 8080 端口提供浏览器页面，与 C 客户端并行工作：
 *   /              -> index.html（启动时读入内存）
 *   /frame         -> 最新一帧 PNG（ETag=帧号，帧未变回 304）
 *   /api/stats     -> JSON：帧号/客户端数/运行状态/帧间隔/帧率/心跳数
 *   /api/start     -> 恢复出图
 *   /api/stop      -> 暂停出图：之后 /frame 返回冻结帧，页面不闪烁
 *   /api/heartbeat -> 浏览器心跳计数
 *   /api/fps       -> POST body 为毫秒数（20/50/100/200），动态调整帧间隔
 * PNG 编码器不依赖任何第三方库（无 stb、无 zlib）：8bit 灰度 PNG，
 * IDAT 用 zlib stored 块（无压缩），完全自包含、跨平台。 */

#define WEB_PORT       8080
#define WEB_POLL_MS    50          /* 事件轮询周期，与浏览器页面刷新周期一致 */
#define PNG_BUF_CAP    (IMG_DATA_LEN + IMG_HEIGHT + 512)

/* 以下状态只在 web 线程（mg_mgr_poll 回调）内访问，无需加锁 */
static uint8_t           *g_web_frozen = NULL;   /* 暂停时冻结的 PNG 帧 */
static size_t             g_web_frozen_len = 0;
static size_t             g_web_frozen_cap = 0;
static uint32_t           g_web_frozen_seq = 0;
static atomic_int         g_web_heartbeat = 0;   /* 心跳计数 */
static volatile sig_atomic_t g_web_run = 1;      /* web 线程退出标志 */
static pthread_t          g_web_thread = 0;
static struct mg_mgr      g_web_mgr;
static char              *g_index_html = NULL;

/* ---- 极简 PNG 编码（8bit 灰度，stored deflate 块，无外部依赖） ---- */
static uint32_t g_png_crc_tab[256];
static int      g_png_crc_ready = 0;

static void png_crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_png_crc_tab[n] = c;
    }
    g_png_crc_ready = 1;
}

static uint32_t png_crc32(const uint8_t *data, size_t len, uint32_t crc)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++)
        crc = g_png_crc_tab[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static uint32_t png_adler32(const uint8_t *data, size_t len)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static void png_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* 写一个 PNG chunk：长度 + 类型 + 数据 + CRC32，返回写入字节数 */
static size_t png_write_chunk(uint8_t *buf, const char type[4],
                              const uint8_t *data, uint32_t len)
{
    uint32_t crc;
    png_put_u32(buf, len);
    memcpy(buf + 4, type, 4);
    if (len > 0)
        memcpy(buf + 8, data, len);
    crc = png_crc32((const uint8_t *)type, 4, 0);
    if (len > 0)
        crc = png_crc32(data, len, crc);
    png_put_u32(buf + 8 + len, crc);
    return 8 + len + 4;
}

/* 640x480 8bit 灰度 -> PNG（stored 块无压缩）。out 容量需 >= PNG_BUF_CAP，
 * 成功返回 PNG 字节数，失败返回 0。只在 web 线程内调用。 */
static size_t gray_to_png(const uint8_t *gray, size_t w, size_t h,
                          uint8_t *out, size_t cap)
{
    static uint8_t raw[IMG_DATA_LEN + IMG_HEIGHT];  /* 行首补 0 filter 字节 */
    static const uint8_t sig[8] = {0x89, 0x50, 0x4E, 0x47,
                                   0x0D, 0x0A, 0x1A, 0x0A};
    uint8_t ihdr[13];
    const size_t raw_line = w + 1;
    const size_t raw_len = raw_line * h;
    const size_t blk_cnt = raw_len / 65535 + 1;
    const size_t zlen = 2 + raw_len + blk_cnt * 5 + 4;
    const size_t need = 8 + 25 + (8 + zlen + 4) + 12;
    size_t pos = 0, raw_pos = 0;
    uint32_t adler;

    if (cap < need)
        return 0;
    if (!g_png_crc_ready)
        png_crc_init();

    /* 拼接原始扫描线：每行前加 1 字节 filter=0（None） */
    for (size_t y = 0; y < h; y++) {
        raw[raw_pos++] = 0;
        memcpy(raw + raw_pos, gray + y * w, w);
        raw_pos += w;
    }

    memcpy(out, sig, 8);
    pos = 8;

    png_put_u32(ihdr, (uint32_t)w);
    png_put_u32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;    /* bit depth */
    ihdr[9] = 0;    /* color type: 0=grayscale */
    ihdr[10] = 0;   /* compression: deflate */
    ihdr[11] = 0;   /* filter: adaptive */
    ihdr[12] = 0;   /* interlace: none */
    pos += png_write_chunk(out + pos, "IHDR", ihdr, sizeof(ihdr));

    /* IDAT：先写 chunk 头（长度+类型），再把 zlib 流直接写入数据区，最后补 CRC */
    {
        size_t data_start;
        uint32_t crc;
        png_put_u32(out + pos, (uint32_t)zlen);
        memcpy(out + pos + 4, "IDAT", 4);
        pos += 8;
        data_start = pos;
        out[pos++] = 0x78;
        out[pos++] = 0x01;
        raw_pos = 0;
        while (raw_pos < raw_len) {
            size_t n = raw_len - raw_pos;
            size_t left = n > 65535 ? 65535 : n;
            out[pos++] = (uint8_t)((raw_pos + left >= raw_len) ? 0x01 : 0x00);
            out[pos++] = (uint8_t)(left & 0xFF);
            out[pos++] = (uint8_t)((left >> 8) & 0xFF);
            out[pos++] = (uint8_t)(~left & 0xFF);
            out[pos++] = (uint8_t)((~left >> 8) & 0xFF);
            memcpy(out + pos, raw + raw_pos, left);
            pos += left;
            raw_pos += left;
        }
        adler = png_adler32(raw, raw_len);
        png_put_u32(out + pos, adler);
        pos += 4;
        crc = png_crc32((const uint8_t *)"IDAT", 4, 0);
        crc = png_crc32(out + data_start, pos - data_start, crc);
        png_put_u32(out + pos, crc);
        pos += 4;
    }

    pos += png_write_chunk(out + pos, "IEND", NULL, 0);
    return pos;
}

/* ---- 浏览器界面接口（原 web_server.c 的功能，现并入 server.c） ---- */

/* 拷贝最新一帧灰度图：成功返回 0，cap 不够返回 -1 */
static int server_get_latest_frame(uint8_t *out, size_t cap, uint32_t *seq)
{
    pthread_mutex_lock(&g_img_lock);
    if (cap >= IMG_DATA_LEN) {
        memcpy(out, g_img_buf, IMG_DATA_LEN);
        if (seq)
            *seq = g_img_seq;
        pthread_mutex_unlock(&g_img_lock);
        return 0;
    }
    pthread_mutex_unlock(&g_img_lock);
    return -1;
}

static uint32_t server_get_frame_seq(void)
{
    uint32_t s;
    pthread_mutex_lock(&g_img_lock);
    s = g_img_seq;
    pthread_mutex_unlock(&g_img_lock);
    return s;
}

static int server_get_client_count(void)
{
    return atomic_load(&g_client_count);
}

static void server_web_set_gen_interval_ms(int ms)
{
    if (ms < 10) ms = 10;
    if (ms > 2000) ms = 2000;
    atomic_store(&g_gen_interval_ms, ms);
    printf("[server] 浏览器界面调整帧间隔为 %dms\n", ms);
}

static int server_web_get_gen_interval_ms(void)
{
    return atomic_load(&g_gen_interval_ms);
}

/* ---- HTTP 路由 ---- */

static void web_reply_json(struct mg_connection *c, const char *body)
{
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
}

static void web_serve_index(struct mg_connection *c)
{
    if (!g_index_html) {
        mg_http_reply(c, 500,
                      "Content-Type: text/plain; charset=utf-8\r\n",
                      "%s",
                      "index.html 未加载：请确认该文件与 server 在同一目录");
        return;
    }
    mg_http_reply(c, 200,
                  "Content-Type: text/html; charset=utf-8\r\n"
                  "Cache-Control: no-cache\r\n",
                  "%s", g_index_html);
}

static void web_serve_frame(struct mg_connection *c, struct mg_http_message *hm)
{
    static uint8_t gray[IMG_DATA_LEN];
    static uint8_t png[PNG_BUF_CAP];
    char etag[48];
    struct mg_str *inm;
    size_t len;
    uint32_t seq;

    if (atomic_load(&g_web_running)) {
        /* 运行中：取最新一帧编码为 PNG，并同步到冻结缓存 */
        if (server_get_latest_frame(gray, sizeof(gray), &seq) != 0) {
            mg_http_reply(c, 503, "", "%s", "{\"error\":\"no frame\"}");
            return;
        }
        len = gray_to_png(gray, IMG_WIDTH, IMG_HEIGHT, png, sizeof(png));
        if (len == 0) {
            mg_http_reply(c, 500, "", "%s", "{\"error\":\"png encode\"}");
            return;
        }
        if (g_web_frozen_cap < len) {
            uint8_t *nb = (uint8_t *)realloc(g_web_frozen, len);
            if (!nb) {
                mg_http_reply(c, 500, "", "%s", "{\"error\":\"oom\"}");
                return;
            }
            g_web_frozen = nb;
            g_web_frozen_cap = len;
        }
        memcpy(g_web_frozen, png, len);
        g_web_frozen_len = len;
        g_web_frozen_seq = seq;
    } else {
        /* 暂停：返回冻结帧（200，不 503），页面不闪烁 */
        if (g_web_frozen_len == 0) {
            mg_http_reply(c, 503, "", "%s", "{\"error\":\"no frame yet\"}");
            return;
        }
        seq = g_web_frozen_seq;
        len = g_web_frozen_len;
        memcpy(png, g_web_frozen, len);
    }

    /* ETag = 帧号：帧未变时回 304，浏览器保留当前图像 */
    snprintf(etag, sizeof(etag), "\"frame-%u\"", seq);
    inm = mg_http_get_header(hm, "If-None-Match");
    if (inm != NULL && mg_match(*inm, mg_str(etag), NULL)) {
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
              etag, (unsigned)len);
    mg_send(c, png, len);
    c->is_resp = 0;   /* ???????mongoose ????????? */
}

static void web_serve_stats(struct mg_connection *c)
{
    char buf[192];
    int interval = server_web_get_gen_interval_ms();
    int running = atomic_load(&g_web_running);
    int hb = atomic_load(&g_web_heartbeat);

    snprintf(buf, sizeof(buf),
             "{\"seq\":%u,\"clients\":%d,\"running\":%s,"
             "\"interval_ms\":%d,\"fps\":%d,\"heartbeat\":%d}",
             server_get_frame_seq(), server_get_client_count(),
             running ? "true" : "false", interval,
             interval > 0 ? 1000 / interval : 0, hb);
    web_reply_json(c, buf);
}

static void web_handle_fps(struct mg_connection *c, struct mg_http_message *hm)
{
    char ms[16] = "0";
    char reply[64];
    int val;

    if (hm->body.len > 0 && hm->body.len < sizeof(ms))
        snprintf(ms, sizeof(ms), "%.*s", (int)hm->body.len, hm->body.buf);
    val = atoi(ms);
    server_web_set_gen_interval_ms(val);
    snprintf(reply, sizeof(reply), "{\"ok\":true,\"interval_ms\":%d}",
             server_web_get_gen_interval_ms());
    web_reply_json(c, reply);
}

static void web_ev_handler(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        if (mg_match(hm->uri, mg_str("/"), NULL) ||
            mg_match(hm->uri, mg_str("/index.html"), NULL)) {
            web_serve_index(c);
        } else if (mg_match(hm->uri, mg_str("/frame"), NULL)) {
            web_serve_frame(c, hm);
        } else if (mg_match(hm->uri, mg_str("/api/stats"), NULL)) {
            web_serve_stats(c);
        } else if (mg_match(hm->uri, mg_str("/api/start"), NULL)) {
            atomic_store(&g_web_running, 1);
            printf("[server] 浏览器界面恢复出图\n");
            web_reply_json(c, "{\"ok\":true,\"running\":true}");
        } else if (mg_match(hm->uri, mg_str("/api/stop"), NULL)) {
            atomic_store(&g_web_running, 0);
            printf("[server] 浏览器界面暂停出图（返回冻结帧）\n");
            web_reply_json(c, "{\"ok\":true,\"running\":false}");
        } else if (mg_match(hm->uri, mg_str("/api/heartbeat"), NULL)) {
            int n = atomic_fetch_add(&g_web_heartbeat, 1) + 1;
            char buf[64];
            snprintf(buf, sizeof(buf), "{\"ok\":true,\"heartbeat\":%d}", n);
            web_reply_json(c, buf);
        } else if (mg_match(hm->uri, mg_str("/api/fps"), NULL)) {
            web_handle_fps(c, hm);
        } else {
            mg_http_reply(c, 404, "", "%s", "{\"error\":\"not found\"}");
        }
    }
}

/* ---- 启动 / 停止 ---- */

static int load_index_html(void)
{
    FILE *f = fopen("index.html", "rb");
    long n;
    char *buf;

    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    n = ftell(f);
    if (n <= 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return -1;
    }
    buf[n] = '\0';
    fclose(f);
    g_index_html = buf;
    return 0;
}

static void *web_thread_main(void *arg)
{
    (void)arg;
    while (g_web_run)
        mg_mgr_poll(&g_web_mgr, WEB_POLL_MS);
    return 0;
}

/* 启动浏览器界面（8080 端口）。失败不影响 C 客户端功能。 */
static int web_server_start(void)
{
    if (g_web_thread)
        return 0;

    if (load_index_html() != 0)
        printf("[server] 警告：读取 index.html 失败，浏览器首页将报错\n");

    mg_mgr_init(&g_web_mgr);
    if (!mg_http_listen(&g_web_mgr, "http://0.0.0.0:8080",
                        web_ev_handler, NULL)) {
        printf("[server] 浏览器界面监听 %d 失败\n", WEB_PORT);
        mg_mgr_free(&g_web_mgr);
        return -1;
    }
    g_web_run = 1;
    if (pthread_create(&g_web_thread, NULL, web_thread_main, NULL) != 0) {
        mg_mgr_free(&g_web_mgr);
        return -1;
    }
    printf("[server] 浏览器界面已启动: http://<本机IP>:%d\n", WEB_PORT);
    return 0;
}

static void web_server_stop(void)
{
    if (!g_web_thread)
        return;
    g_web_run = 0;
    pthread_join(g_web_thread, NULL);
    g_web_thread = 0;
    mg_mgr_free(&g_web_mgr);
    if (g_web_frozen) {
        free(g_web_frozen);
        g_web_frozen = NULL;
    }
    g_web_frozen_len = g_web_frozen_cap = 0;
    if (g_index_html) {
        free(g_index_html);
        g_index_html = NULL;
    }
    printf("[server] 浏览器界面已停止\n");
}

/* ---------------- 监听与主流程 ---------------- */
static struct event_base       *g_base = NULL;
static struct evconnlistener   *g_listener = NULL;

/* 监听回调：有新连接时由监听线程（主循环）调用。
 * 这里只做：限流检查（最多 MAX_CLIENTS）、登记进全局表、创建控制线程。
 * 真正的收发都在控制线程里完成，避免在监听回调里做耗时操作
 * 阻塞后续 accept。 */
static void on_accept(struct evconnlistener *lst, evutil_socket_t fd,
                      struct sockaddr *sa, int socklen, void *arg)
{
    client_t *c;
    int n = atomic_fetch_add(&g_client_count, 1) + 1;
    int i;

    (void)lst; (void)sa; (void)socklen; (void)arg;

    if (n > MAX_CLIENTS) {
        atomic_fetch_add(&g_client_count, -1);
        printf("[server] 客户端数已达 %d，拒绝新连接\n", MAX_CLIENTS);
        evutil_closesocket(fd);
        return;
    }

    c = (client_t *)calloc(1, sizeof(client_t));
    if (!c) {
        atomic_fetch_add(&g_client_count, -1);
        evutil_closesocket(fd);
        return;
    }
    c->fd = fd;
    c->slot = -1;
    pthread_mutex_init(&c->send_lock, NULL);

    pthread_mutex_lock(&g_clients_lock);
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i] == NULL) {
            g_clients[i] = c;
            c->slot = i;
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_lock);

    if (c->slot < 0) {   /* 表满（理论上不会发生） */
        atomic_fetch_add(&g_client_count, -1);
        pthread_mutex_destroy(&c->send_lock);
        evutil_closesocket(fd);
        free(c);
        return;
    }

    printf("[server] 接受客户端 %lld，当前 %d/%d\n",
           (long long)fd, (int)n, MAX_CLIENTS);

    pthread_create(&c->ctrl_thread, NULL, ctrl_thread_main, c);
}

/* Ctrl+C / SIGTERM 统一走这里：置停止标志并打断两个事件循环 */
static void on_sigint(int sig)
{
    (void)sig;
    g_running = 0;
    g_web_run = 0;   /* 让 web 线程尽快退出 */
    if (g_base)
        event_base_loopbreak(g_base);
    if (g_gen_base)
        event_base_loopbreak(g_gen_base);
}

int main(void)
{
    struct sockaddr_in sin;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);   /* 重定向时也实时输出日志 */

    /* 让 libevent 内部线程安全：Windows 用 Windows 线程原语，
     * Linux 用 pthread（这是源码里唯一的平台分支） */
#ifdef _WIN32
    evthread_use_windows_threads();
#else
    evthread_use_pthreads();
#endif

    pthread_mutex_init(&g_img_lock, NULL);
    pthread_cond_init(&g_img_cond, NULL);
    pthread_mutex_init(&g_clients_lock, NULL);
    atomic_init(&g_client_count, 0);
    for (i = 0; i < MAX_CLIENTS; i++)
        g_clients[i] = NULL;

    g_base = event_base_new();
    if (!g_base) {
        printf("event_base_new 失败\n");
        return 1;
    }

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(SERVER_PORT);

    /* 创建监听器：evconnlistener 把 accept 封装进事件循环，有连接到来时
     * 自动调用 on_accept；LEV_OPT_REUSEABLE 让端口可以快速复用。 */
    g_listener = evconnlistener_new_bind(g_base, on_accept, NULL,
        LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1,
        (struct sockaddr *)&sin, sizeof(sin));
    if (!g_listener) {
        printf("监听端口 %d 失败\n", SERVER_PORT);
        return 1;
    }

    /* 启动图像生成线程（内部用 libevent 定时器，20ms/帧） */
    pthread_create(&g_gen_thread, NULL, gen_image_thread, NULL);
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    /* 启动浏览器界面（失败只告警，不影响 C 客户端） */
    if (web_server_start() != 0)
        printf("[server] 警告：浏览器界面启动失败，C 客户端功能不受影响\n");

    printf("=============================================\n");
    printf(" libevent 图像服务器\n");
    printf(" 监听端口   : %d\n", SERVER_PORT);
    printf(" 最大客户端 : %d\n", MAX_CLIENTS);
    printf(" 图像       : %dx%d 8bit 灰度，%dms/帧\n",
           IMG_WIDTH, IMG_HEIGHT, IMG_GEN_INTERVAL_MS);
    printf(" 按 Ctrl+C 退出\n");
    printf(" 浏览器界面 : http://<本机IP>:%d\n", WEB_PORT);
    printf("=============================================\n");

    event_base_dispatch(g_base);

    /* ---- 退出清理 ----
     * 顺序：置 g_running=0 并广播唤醒可能正在等待的发送线程 -> 打断生成
     * 线程的事件循环并 join -> 释放监听器/事件循环。各客户端的控制线程
     * 是独立线程，进程退出时随进程一起结束，无需在这里逐个等待。 */
    printf("\n服务器退出中...\n");
    g_running = 0;
    pthread_mutex_lock(&g_img_lock);
    pthread_cond_broadcast(&g_img_cond);
    pthread_mutex_unlock(&g_img_lock);
    if (g_gen_base)
        event_base_loopbreak(g_gen_base);
    if (g_gen_thread)
        pthread_join(g_gen_thread, NULL);
    web_server_stop();
    if (g_listener)
        evconnlistener_free(g_listener);
    event_base_free(g_base);

    pthread_cond_destroy(&g_img_cond);
    pthread_mutex_destroy(&g_img_lock);
    pthread_mutex_destroy(&g_clients_lock);
    printf("服务器已退出\n");
    return 0;
}
