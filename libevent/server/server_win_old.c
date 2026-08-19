/* server.c —— libevent 图像服务器
 *
 * 【架构讲解】
 *   这是一个"一对多"的图像服务器：一台服务器同时向最多 10 个客户端推送
 *   实时生成的灰度测试图。为了让网络收发不阻塞图像生成，程序拆成三类线程：
 *
 *   1) 图像生成线程（全局唯一）
 *      用高精度等待定时器按 20ms 周期（50fps）生成一帧 640x480 灰度图，
 *      写入全局缓冲 g_img_buf 并递增帧序号，然后广播"有新帧"的条件变量。
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
 *      - evthread_use_windows_threads() 开启 libevent 内部线程安全。
 *
 * 功能：
 *   1. 图像生成线程：每 20ms 生成一帧 640x480 8 位灰度测试图，广播 img_ready
 *   2. 监听客户端连接，最多 10 个
 *   3. 每客户端一个控制线程：处理心跳 / 请求图像 / 停止图像 / 设备控制
 *   4. 每客户端一个图像发送线程：等待 img_ready（5 秒超时）后发送
 *      16 字节头 + 640x480 图像数据
 */

#define _WIN32_WINNT 0x0600
#define WINVER       0x0600

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>

/* libevent 头文件：event_base（事件循环）、bufferevent（带缓冲的 socket
 * 封装）、listener（监听器）。注意 winsock2.h 必须包含在 windows.h 之前，
 * 这是 Windows SDK 的固定包含顺序要求。 */
#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/thread.h>

#include "protocol.h"

/* ---------------- 全局：图像生成 ----------------
 * 生产者-消费者模型：
 *   g_img_buf 保存"最新的一帧"，g_img_seq 是它的帧序号。
 *   生成线程持锁写入并递增序号；各发送线程在锁上等条件变量，序号一变就
 *   把新帧拷走。用"帧序号"而不是"标志位"，可以精确判断"有没有新帧"，
 *   避免丢帧或重复处理。 */
static CRITICAL_SECTION   g_img_lock;   /* 保护 g_img_buf / g_img_seq */
static CONDITION_VARIABLE g_img_cond;   /* img_ready 广播信号 */
static uint8_t            g_img_buf[IMG_DATA_LEN];  /* 最新一帧 */
static uint32_t           g_img_seq = 0;            /* 帧序号 */
static volatile LONG      g_running = 1;
static HANDLE             g_gen_thread = NULL;
static HANDLE             g_frame_timer = NULL;   /* 高精度周期定时器（20ms/帧） */

/* ---------------- 全局：客户端管理 ---------------- */
typedef struct client_s client_t;
static CRITICAL_SECTION g_clients_lock;
static client_t        *g_clients[MAX_CLIENTS];
static volatile LONG    g_client_count = 0;

struct client_s
{
    evutil_socket_t fd;           /* 已接受的 socket，交给 bufferevent 接管 */
    struct event_base  *base;     /* 本客户端专属事件循环（控制线程内创建/销毁） */
    struct bufferevent *bev;      /* 本客户端的收发封装 */

    CRITICAL_SECTION send_lock;   /* 串行化本客户端的所有发送（头+数据不被打断） */

    HANDLE ctrl_thread;           /* 控制线程 */
    HANDLE send_thread;           /* 图像发送线程（未启动为 NULL） */
    volatile LONG send_stop;      /* 停止发送线程标志 */
    uint32_t last_seq;            /* 发送线程已发送的最新帧序号 */

    volatile LONG slot;           /* 全局表中的下标，-1 表示未登记 */
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
    EnterCriticalSection(&c->send_lock);
    if (c->bev)
        bufferevent_write(c->bev, data, len);
    LeaveCriticalSection(&c->send_lock);
}

/* ---------------- 图像发送线程（每客户端一个） ----------------
 * 工作循环：
 *   等"新帧"条件变量 -> 有新帧就把最新一帧 memcpy 到私有缓冲
 *   -> 在锁外发送（16 字节头 + 图像数据）。
 * 关键设计：
 *   1) 条件变量必须配锁使用：SleepConditionVariableCS 在等待时自动释放锁、
 *      被唤醒后自动重新持锁，期间不会漏掉"新帧"信号；
 *   2) 只拷"最新一帧"而不是逐帧排队：客户端跟不上时自然跳帧，
 *      保证延迟最低；
 *   3) 网络发送在锁外进行：某个客户端发送慢不会卡住图像生成线程。
 */
static DWORD WINAPI send_image_thread(LPVOID arg)
{
    client_t *c = (client_t *)arg;
    uint8_t *frame = (uint8_t *)malloc(IMG_DATA_LEN);
    uint8_t  hdr[IMG_HEAD_LEN];
    BOOL     timed_out = FALSE;

    if (!frame)
        return 1;

    while (g_running && !c->send_stop) {
        EnterCriticalSection(&g_img_lock);

        /* 等待 img_ready 广播信号，超时 5 秒 */
        while (g_running && !c->send_stop && g_img_seq == c->last_seq) {
            if (!SleepConditionVariableCS(&g_img_cond, &g_img_lock,
                                          IMG_READY_TIMEOUT_MS)) {
                timed_out = TRUE;   /* 等待超时 */
                break;
            }
        }

        if (!g_running || c->send_stop) {
            LeaveCriticalSection(&g_img_lock);
            break;
        }

        if (timed_out && g_img_seq == c->last_seq) {
            /* 5 秒内没有新图：发一个错误头，然后继续等 */
            LeaveCriticalSection(&g_img_lock);
            timed_out = FALSE;
            build_head(hdr, CTL_IMG_DATA, ERR_IMG_TIMEOUT, 0);
            send_client(c, hdr, IMG_HEAD_LEN);
            printf("[server] 客户端 %lld 图像等待超时，已发错误头\n",
                   (long long)c->fd);
            continue;
        }
        timed_out = FALSE;

        /* 有新图：在锁内拷贝最新一帧（发送在锁外进行） */
        c->last_seq = g_img_seq;
        memcpy(frame, g_img_buf, IMG_DATA_LEN);
        LeaveCriticalSection(&g_img_lock);

        /* 先发 16 字节数据头，再发 640x480 图像 */
        build_head(hdr, CTL_IMG_DATA, ERR_OK, IMG_DATA_LEN);
        send_client(c, hdr, IMG_HEAD_LEN);
        send_client(c, frame, IMG_DATA_LEN);
    }

    free(frame);
    return 0;
}

/* ---------------- 控制命令处理 ---------------- */
/* 心跳应答：回 4 字节数据（当前毫秒时间戳的截断值），证明链路通畅。
 * 客户端空闲时每 3 秒发一次心跳，这里只做应答，不改变任何状态。 */
static void handle_heartbeat(client_t *c)
{
    uint8_t buf[IMG_HEAD_LEN + HEARTBEAT_DATA_LEN];
    int32_t val = (int32_t)(GetTickCount64() & 0x7FFFFFFF);

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
    InterlockedExchange(&c->send_stop, 0);
    c->send_thread = CreateThread(NULL, 0, send_image_thread, c, 0, NULL);
    printf("[server] 客户端 %lld 开始传输图像\n", (long long)c->fd);
}

/* 停止图像：置 send_stop=1，并广播唤醒可能正阻塞在"等新帧"上的发送线程
 * （其他客户端会跟着空醒一次，但检查条件后仍会继续等待，无害），
 * 然后等发送线程真正退出并回收句柄。 */
static void handle_stop_image(client_t *c)
{
    if (!c->send_thread) {
        printf("[server] 客户端 %lld 当前没有图像传输\n", (long long)c->fd);
        return;
    }
    InterlockedExchange(&c->send_stop, 1);
    /* 唤醒可能正在等待 img_ready 的发送线程（其他客户端会空醒一次，无害） */
    EnterCriticalSection(&g_img_lock);
    WakeAllConditionVariable(&g_img_cond);
    LeaveCriticalSection(&g_img_lock);

    WaitForSingleObject(c->send_thread, INFINITE);
    CloseHandle(c->send_thread);
    c->send_thread = NULL;
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
 * evthread_use_windows_threads()，libevent 内部结构在线程间是安全的。
 * event_base_dispatch() 一直运行到回调里调用 event_base_loopexit() 为止，
 * 返回后即开始清理本客户端的资源。 */
static DWORD WINAPI ctrl_thread_main(LPVOID arg)
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
    InterlockedExchange(&c->send_stop, 1);
    EnterCriticalSection(&g_img_lock);
    WakeAllConditionVariable(&g_img_cond);
    LeaveCriticalSection(&g_img_lock);

    if (c->send_thread) {
        WaitForSingleObject(c->send_thread, INFINITE);
        CloseHandle(c->send_thread);
        c->send_thread = NULL;
    }

    EnterCriticalSection(&g_clients_lock);
    if (c->slot >= 0) {
        g_clients[c->slot] = NULL;
        c->slot = -1;
    }
    LeaveCriticalSection(&g_clients_lock);
    InterlockedDecrement(&g_client_count);
    printf("[server] 客户端 %lld 已清理，当前 %ld/%d\n",
           (long long)c->fd, (long)g_client_count, MAX_CLIENTS);

    if (c->bev)
        bufferevent_free(c->bev);
    if (c->base)
        event_base_free(c->base);
    DeleteCriticalSection(&c->send_lock);
    CloseHandle(c->ctrl_thread);
    free(c);
    return 0;

fail:
    evutil_closesocket(c->fd);
    if (c->bev)
        bufferevent_free(c->bev);
    if (c->base)
        event_base_free(c->base);
    DeleteCriticalSection(&c->send_lock);
    EnterCriticalSection(&g_clients_lock);
    if (c->slot >= 0) {
        g_clients[c->slot] = NULL;
        c->slot = -1;
    }
    LeaveCriticalSection(&g_clients_lock);
    InterlockedDecrement(&g_client_count);
    free(c);
    return 1;
}

/* ---------------- 图像生成线程 ---------------- */
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

    _snprintf(text, sizeof(text), "FRAME %05d", seq_no);

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

/* 图像生成线程（全局唯一）。
 * 节奏控制：用 CreateWaitableTimer 高精度定时器固定 20ms/帧，
 * 比 Sleep() 更稳定，这是 50fps 输出能保持平稳的关键。
 * 生成策略：静态背景（水平渐变 + 顶部阶梯条）只计算一次，之后每帧
 * 先 memcpy，再叠加 3 个动态元素（上下扫动的亮带、移动方块、帧号），
 * 最后递增帧序号并广播条件变量。对 g_img_buf 的写入全程持锁。 */
static DWORD WINAPI gen_image_thread(LPVOID arg)
{
    static uint8_t bg[IMG_DATA_LEN];   /* 静态背景：水平渐变 + 顶部阶梯条 */
    uint32_t seq = 0;
    int tick = 0;

    /* 静态背景只生成一次，之后每帧 memcpy，降低生成耗时以稳定 50fps */
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

    while (g_running) {
        WaitForSingleObject(g_frame_timer, INFINITE);   /* 精确 20ms 周期 */
        tick++;

        EnterCriticalSection(&g_img_lock);
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
        const int BS = 40;
        int bx = (tick * 6) % (IMG_WIDTH - BS);
        int by = (tick * 6) % (IMG_HEIGHT - BS);
        for (int y = by; y < by + BS; y++) {
            for (int x = bx; x < bx + BS; x++)
                g_img_buf[y * IMG_WIDTH + x] = 255;
        }

        /* 3. 左下角大号帧号 */
        draw_frame_number(g_img_buf, (int)(seq + 1));

        g_img_seq = ++seq;
        LeaveCriticalSection(&g_img_lock);

        WakeAllConditionVariable(&g_img_cond);   /* 广播 img_ready */
    }
    return 0;
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
    LONG n = InterlockedIncrement(&g_client_count);
    int i;

    if (n > MAX_CLIENTS) {
        InterlockedDecrement(&g_client_count);
        printf("[server] 客户端数已达 %d，拒绝新连接\n", MAX_CLIENTS);
        evutil_closesocket(fd);
        return;
    }

    c = (client_t *)calloc(1, sizeof(client_t));
    if (!c) {
        InterlockedDecrement(&g_client_count);
        evutil_closesocket(fd);
        return;
    }
    c->fd = fd;
    c->slot = -1;
    InitializeCriticalSection(&c->send_lock);

    EnterCriticalSection(&g_clients_lock);
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i] == NULL) {
            g_clients[i] = c;
            c->slot = i;
            break;
        }
    }
    LeaveCriticalSection(&g_clients_lock);

    if (c->slot < 0) {   /* 表满（理论上不会发生） */
        InterlockedDecrement(&g_client_count);
        DeleteCriticalSection(&c->send_lock);
        evutil_closesocket(fd);
        free(c);
        return;
    }

    printf("[server] 接受客户端 %lld，当前 %ld/%d\n",
           (long long)fd, (long)n, MAX_CLIENTS);

    c->ctrl_thread = CreateThread(NULL, 0, ctrl_thread_main, c, 0, NULL);
}

static BOOL WINAPI console_ctrl(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&g_running, 0);
        if (g_base)
            event_base_loopbreak(g_base);
        return TRUE;
    }
    return FALSE;
}

int main(void)
{
    WSADATA wsa;
    struct sockaddr_in sin;
    int i;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup 失败\n");
        return 1;
    }
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, NULL, _IONBF, 0);   /* 重定向时也实时输出日志 */
    evthread_use_windows_threads();   /* 让 libevent 内部线程安全 */
    /* 时间精度：Windows 默认定时器精度约 15ms，timeBeginPeriod(1) 把系统
     * 定时器精度提到 1ms，配合高精度等待定时器保证 50fps 稳定不抖。 */
    timeBeginPeriod(1);               /* 提高 Sleep 精度，保证 20ms/帧 稳定输出 */

    InitializeCriticalSection(&g_img_lock);
    InitializeConditionVariable(&g_img_cond);
    InitializeCriticalSection(&g_clients_lock);
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

    /* 高精度周期定时器：每 IMG_GEN_INTERVAL_MS 触发一次，替代 Sleep。
     * 负数 due 表示"相对当前时间"，第三个参数是周期；自动复位（FALSE）
     * 保证每个周期只触发一次，不会累积丢失信号。 */
    {
        LARGE_INTEGER due;
        due.QuadPart = -10000LL * IMG_GEN_INTERVAL_MS;   /* 相对时间，100ns 单位 */
        g_frame_timer = CreateWaitableTimer(NULL, FALSE, NULL);   /* 自动复位，保证每周期只触发一次 */
        SetWaitableTimer(g_frame_timer, &due, IMG_GEN_INTERVAL_MS, NULL, NULL, FALSE);
    }
    g_gen_thread = CreateThread(NULL, 0, gen_image_thread, NULL, 0, NULL);
    SetConsoleCtrlHandler(console_ctrl, TRUE);

    printf("=============================================\n");
    printf(" libevent 图像服务器\n");
    printf(" 监听端口   : %d\n", SERVER_PORT);
    printf(" 最大客户端 : %d\n", MAX_CLIENTS);
    printf(" 图像       : %dx%d 8bit 灰度，%dms/帧\n",
           IMG_WIDTH, IMG_HEIGHT, IMG_GEN_INTERVAL_MS);
    printf(" 按 Ctrl+C 退出\n");
    printf("=============================================\n");

    event_base_dispatch(g_base);

    /* ---- 退出清理 ----
     * 顺序：置 g_running=0 并广播唤醒可能正在等待的发送线程 -> 等生成线程
     * 退出 -> 释放定时器/监听器/事件循环。各客户端的控制线程是独立线程，
     * 进程退出时随进程一起结束，无需在这里逐个等待。 */
    printf("\n服务器退出中...\n");
    InterlockedExchange(&g_running, 0);
    EnterCriticalSection(&g_img_lock);
    WakeAllConditionVariable(&g_img_cond);
    LeaveCriticalSection(&g_img_lock);
    if (g_gen_thread) {
        WaitForSingleObject(g_gen_thread, 1500);
        CloseHandle(g_gen_thread);
    }
    if (g_frame_timer) {
        CloseHandle(g_frame_timer);
        g_frame_timer = NULL;
    }
    if (g_listener)
        evconnlistener_free(g_listener);
    event_base_free(g_base);

    timeEndPeriod(1);
    WSACleanup();
    printf("服务器已退出\n");
    return 0;
}