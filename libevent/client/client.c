/* client.c —— libevent 图像客户端（PC 端）
 *
 * 【架构讲解】
 *   客户端是"单线程"程序：所有网络收发、定时器、键盘动作都在同一个
 *   event_base 事件循环里完成，天然线程安全。唯一的额外线程是"键盘线程"，
 *   因为 getchar() 是阻塞调用，不能放进 libevent 回调；它通过
 *   g_key_action + event_active() 把按键跨线程交给主循环处理。
 *
 *   连接与重连：
 *     - 主循环持有一个 bufferevent_socket 与服务器通信；
 *     - 空闲时每 3 秒发一次心跳（ST_IDLE -> ST_WAIT_HB -> 应答后回 ST_IDLE）；
 *     - 按 s 进入收图状态（ST_IMAGE），解析"16 字节头 + 图像数据"的帧格式；
 *     - 断线 / 读超时 / 出错统一走 teardown_connection() ->
 *       schedule_reconnect()，2 秒后自动重连，可长时间无人值守运行。
 *
 *   收图状态机：
 *     ST_DISCONNECTED -> ST_CONNECTING -> ST_IDLE --心跳--> ST_WAIT_HB
 *                                          |--按 s--> ST_IMAGE
 *     任何状态下断线都会回到 ST_DISCONNECTED，再走重连。
 *
 *   界面：
 *     用纯 Win32 GDI 开一个 640x480 预览窗口；libevent 线程里挂一个 10ms
 *     定时器泵窗口消息（PeekMessage），无需单独开 UI 线程。
 *
 * 功能：
 *   1. 连接服务器（默认 127.0.0.1:9995）
 *   2. 空闲时每 3 秒请求一次心跳，等待 4 字节应答（读超时 3 秒）
 *   3. 按 s 开始收图：持续接收 16 字节头 + 图像数据（读超时 3 秒）
 *      按 q 停止收图
 *   4. 断线 / 出错 / 超时后自动重连（间隔 2 秒）
 *
 * 用法：client [服务器IP] [端口] [-s]
 *       -s 表示连接成功后自动开始收图（配合脚本/测试用）
 */

#ifdef _WIN32
#define _WIN32_WINNT 0x0600
#define WINVER       0x0600
#include <winsock2.h>
#include <windows.h>
#else
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>

#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/util.h>
#include <event2/thread.h>

#include "protocol.h"

/* ---------------- 跨平台小工具 ---------------- */
/* 原子交换：Windows 用 Interlocked，Linux(gcc) 用 __atomic。
 * 用于键盘线程与主循环之间的按键传递、保存开关、退出标志。 */
static int32_t atomic_exchange(volatile int32_t *p, int32_t v)
{
#ifdef _WIN32
    return (int32_t)InterlockedExchange((volatile LONG *)p, (LONG)v);
#else
    return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
#endif
}

/* 单调时钟毫秒：用于 fps 统计，跨平台。 */
static uint32_t now_ms(void)
{
#ifdef _WIN32
    return (uint32_t)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
#endif
}

/* BMP 文件头/信息头：布局与 Windows 的 BITMAPFILEHEADER/BITMAPINFOHEADER
 * 完全一致，用 stdint 类型自描述，避免依赖 windows.h。 */
#define BMP_COMP_RGB 0
#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
} bmp_fileheader_t;
typedef struct {
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
} bmp_infoheader_t;
#pragma pack(pop)

/* ---------------- 状态 ---------------- */
/* 连接状态机：描述客户端当前"所处阶段"，所有回调都先看状态再行动，
 * 保证任何时刻只会走一条正确的逻辑分支。 */
typedef enum {
    ST_DISCONNECTED,   /* 未连接（等待重连） */
    ST_CONNECTING,     /* connect 已发出，等 BEV_EVENT_CONNECTED */
    ST_IDLE,           /* 已连接且空闲：只发心跳 */
    ST_WAIT_HB,        /* 已发心跳请求，等 4 字节应答 */
    ST_IMAGE,          /* 收图中 */
} state_t;

/* 主循环全局对象：
 *   g_base      唯一事件循环，所有 socket/定时器都挂在这里；
 *   g_bev       与服务器的连接封装（失败为 NULL）；
 *   g_state     当前状态机的状态；
 *   g_running   进程退出标志；g_reconnect_pending 防止重连定时器重复堆积；
 *   g_key_action 键盘线程写入、主循环消费的"待办按键"。 */
static struct event_base  *g_base;
static struct bufferevent *g_bev;
static state_t  g_state = ST_DISCONNECTED;
static volatile int32_t g_running = 1;
static volatile int32_t g_reconnect_pending = 0;
static volatile int32_t g_key_action = 0;

/* 服务器地址 */
static char g_server_ip[64] = "127.0.0.1";
static int  g_server_port = SERVER_PORT;
static int  g_auto_start_image = 0;

/* ---------------- 收发状态机 ----------------
 * 所有数据都按"先 16 字节头、再 nDataLen 字节载荷"两阶段解析：
 *   g_want_header=1 时收集头部（可能分多次到达，g_hdr_got 记录进度）；
 *   头部收齐后校验 flag，再按 nDataLen 进入收载荷阶段；
 *   载荷收齐后调 on_payload() 处理，然后复位继续下一帧。
 * 这种"攒到长度再处理"的方式是流式协议的标准做法，与 TCP 粘包/半包
 * 问题天然兼容。 */
static int     g_want_header = 1;
static uint8_t g_hdr[IMG_HEAD_LEN];
static int     g_hdr_got = 0;
static uint8_t *g_payload = NULL;
static size_t  g_payload_cap = 0;
static size_t  g_payload_len = 0;
static size_t  g_payload_got = 0;

/* 统计 */
static uint32_t g_frame_count = 0;
static uint32_t g_hb_count = 0;
static uint32_t g_reconnect_count = 0;
static int      g_save_every = 30;   /* 开启保存后，每隔多少帧存一张 BMP */
static volatile int32_t g_saving = 0; /* 保存开关：1=保存中 0=不保存（纯演示） */
static char     g_save_dir[64] = "";  /* 本 client 专属保存目录（按 b 首次开启保存时创建，空串=当前目录） */

/* ---------------- 实时预览窗口（Win32 GDI） ----------------
 * GDI 资源全部在主循环线程创建/使用（窗口消息也由 10ms 定时器泵），
 * 因此不存在跨线程操作窗口的问题。g_rgb_buf 是 DIB 的像素内存，
 * 直接往里面填 24 位 RGB 再 BitBlt 到窗口，就是最简单高效的预览方式。 */
static struct event *g_ui_timer = NULL;    /* 窗口消息泵 + fps 统计 */
#ifdef _WIN32
static HWND     g_wnd = NULL;
static HDC      g_mem_dc = NULL;
static HBITMAP  g_dib = NULL;
static HGDIOBJ  g_old_bmp = NULL;
static uint8_t *g_rgb_buf = NULL;          /* 24 位 RGB 缓冲（DIB 段内存） */
#endif
static uint32_t g_second_frames = 0;
static uint32_t g_fps = 0;
static uint32_t g_fps_start = 0;

/* 定时器 */
static struct event *g_hb_timer = NULL;        /* 心跳周期（空闲时 3 秒） */
static struct event *g_reconnect_timer = NULL; /* 断线重连延时 */
static struct event *g_key_event = NULL;       /* 键盘线程 -> 主循环 */

/* 前向声明 */
static void do_connect(void);
static void read_cb(struct bufferevent *bev, void *arg);
static void event_cb(struct bufferevent *bev, short events, void *arg);
static void init_save_dir(void);

/* ---------------- 小工具 ---------------- */
/* 设置读超时（0=禁用）。超时只在"等对方应答/等图像"时开启，
 * 超时后 libevent 会触发 BEV_EVENT_TIMEOUT，走重连逻辑。 */
static void set_read_timeout(long ms)
{
    struct timeval tv;
    if (!g_bev)
        return;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    bufferevent_set_timeouts(g_bev, &tv, NULL);  /* 0 表示禁用 */
}

/* 复位帧解析游标：准备开始解析新的一帧（头部进度、载荷进度全部清零）。 */
static void recv_reset(void)
{
    g_want_header = 1;
    g_hdr_got = 0;
    g_payload_got = 0;
    g_payload_len = 0;
}

/* 进入"收载荷"阶段：校验长度 -> 按需扩容 -> 记录目标长度。
 * 16MB 上限是防御性检查：防止异常头部给出离谱长度导致内存暴涨。 */
static int expect_payload(size_t len)
{
    if (len > 16 * 1024 * 1024) {   /* 上限保护，防异常长度 */
        printf("[client] 数据长度过大 (%zu)，放弃当前连接\n", len);
        return -1;
    }
    if (len > g_payload_cap) {
        uint8_t *np = (uint8_t *)realloc(g_payload, len);
        if (!np)
            return -1;
        g_payload = np;
        g_payload_cap = len;
    }
    g_payload_len = len;
    g_payload_got = 0;
    g_want_header = 0;
    return 0;
}

/* 发送 16 字节请求头：心跳请求、请求图像、停止图像都走这里。
 * 客户端发往服务器的请求一律不带载荷，len 恒为 0。 */
static void send_head(int32_t ctl, int32_t len)
{
    uint8_t buf[IMG_HEAD_LEN];
    ImgHead_t *h = (ImgHead_t *)buf;
    if (!g_bev)
        return;
    h->flag[0] = 'C'; h->flag[1] = 'A'; h->flag[2] = 'M'; h->flag[3] = '0';
    h->ctl_code = ctl;
    h->err_code = ERR_OK;
    h->nDataLen = len;
    bufferevent_write(g_bev, buf, IMG_HEAD_LEN);
}

/* 把一帧灰度图存成 8 位 BMP 文件。
 * BMP 结构：文件头 + 信息头 + 256 色调色板 + 像素数据；
 * 8bpp 时每行正好 640 字节，无需行对齐；BMP 像素自下而上存储，
 * 所以从最后一行开始逐行写出。 */
static void save_frame(const uint8_t *data, size_t len, uint32_t idx)
{
    char name[64];
    FILE *fp;
    bmp_fileheader_t bf;
    bmp_infoheader_t bi;
    const int row = IMG_WIDTH;   /* 8bpp，640 字节，无需行填充 */

    if (len < (size_t)(IMG_WIDTH * IMG_HEIGHT))
        return;
    if (g_save_dir[0])
        snprintf(name, sizeof(name), "%s/frame_%04u.bmp", g_save_dir, idx);
    else
        snprintf(name, sizeof(name), "frame_%04u.bmp", idx);
    fp = fopen(name, "wb");
    if (!fp)
        return;

    memset(&bf, 0, sizeof(bf));
    memset(&bi, 0, sizeof(bi));
    bf.bfType = 0x4D42;                              /* "BM" */
    bf.bfOffBits = (uint32_t)(sizeof(bf) + sizeof(bi) + 256 * 4);
    bf.bfSize = bf.bfOffBits + (uint32_t)(row * IMG_HEIGHT);

    bi.biSize = (uint32_t)sizeof(bi);
    bi.biWidth = IMG_WIDTH;
    bi.biHeight = IMG_HEIGHT;
    bi.biPlanes = 1;
    bi.biBitCount = 8;
    bi.biCompression = BMP_COMP_RGB;
    bi.biSizeImage = (uint32_t)(row * IMG_HEIGHT);
    bi.biClrUsed = 256;

    fwrite(&bf, 1, sizeof(bf), fp);
    fwrite(&bi, 1, sizeof(bi), fp);
    for (int i = 0; i < 256; i++) {
        uint8_t pal[4] = { (uint8_t)i, (uint8_t)i, (uint8_t)i, 0 };
        fwrite(pal, 1, sizeof(pal), fp);
    }
    /* BMP 行自下而上存储 */
    for (int y = IMG_HEIGHT - 1; y >= 0; y--)
        fwrite(data + (size_t)y * IMG_WIDTH, 1, IMG_WIDTH, fp);

    fclose(fp);
    printf("[client] 已保存 %s\n", name);
}

/* ---------------- 实时预览窗口（仅 Windows：Win32 GDI） ---------------- */
#ifdef _WIN32
/* 窗口过程：WM_PAINT 时把 DIB 内容 BitBlt 到窗口；ESC 或关闭按钮
 * 都会销毁窗口，WM_DESTROY 里 PostQuitMessage 使消息泵退出。 */
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (g_mem_dc)
            BitBlt(dc, 0, 0, IMG_WIDTH, IMG_HEIGHT, g_mem_dc, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* 泵窗口消息：在 libevent 线程里由 10ms UI 定时器调用。
 * PeekMessage 非阻塞地取走消息，取到 WM_QUIT 就停主循环；
 * 这样窗口消息和网络事件在同一个线程处理，无需单独开消息线程。 */
static void pump_window_messages(void)
{
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            atomic_exchange(&g_running, 0);
            event_base_loopbreak(g_base);
            return;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

/* 创建预览窗口 + 24 位 DIB（640x480），返回 0 表示成功。
 * DIB 是"内存位图"：CreateDIBSection 返回的 bits 指针可以直接读写像素，
 * 再通过兼容 DC 和 BitBlt 显示到窗口，全程无 GDI+，开销很小。 */
static int ui_window_create(void)
{
    WNDCLASSEX wc;
    BITMAPINFO bi;
    void *bits = NULL;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"ImgPreviewWnd";
    RegisterClassExW(&wc);

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = IMG_WIDTH;
    bi.bmiHeader.biHeight = -IMG_HEIGHT;   /* 负数 = 自上而下 */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    g_wnd = CreateWindowExW(0, L"ImgPreviewWnd", L"图像预览 (640x480)",
                           WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           IMG_WIDTH + 16, IMG_HEIGHT + 40,
                           NULL, NULL, wc.hInstance, NULL);
    if (!g_wnd)
        return -1;

    g_mem_dc = CreateCompatibleDC(NULL);
    g_dib = CreateDIBSection(g_mem_dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!g_dib || !bits) {
        DestroyWindow(g_wnd);
        g_wnd = NULL;
        return -1;
    }
    g_old_bmp = SelectObject(g_mem_dc, g_dib);
    g_rgb_buf = (uint8_t *)bits;

    ShowWindow(g_wnd, SW_SHOW);
    ShowWindow(g_wnd, SW_SHOW);   /* 第二次调用强制显示（首次会采用启动时的隐藏标志） */
    return 0;
}

/* 把一帧灰度图显示到预览窗口：灰度 8 位 -> RGB 24 位逐点展开，
 * 然后使窗口区域无效并立即重绘。 */
static void preview_show(const uint8_t *gray)
{
    uint8_t *p;
    if (!g_rgb_buf)
        return;
    p = g_rgb_buf;
    for (int i = 0; i < IMG_WIDTH * IMG_HEIGHT; i++) {
        uint8_t v = gray[i];
        *p++ = v;
        *p++ = v;
        *p++ = v;
    }
    if (g_wnd) {
        InvalidateRect(g_wnd, NULL, FALSE);
        UpdateWindow(g_wnd);
    }
}

/* 每 10ms 泵一次窗口消息；每秒刷新一次标题（帧号 + fps）。
 * 用窗口标题显示统计信息，避免在画面上叠字影响看图。 */
static void ui_timer_cb(evutil_socket_t fd, short what, void *arg)
{
    uint32_t now = now_ms();

    pump_window_messages();
    if (!g_wnd)
        return;

    if (now - g_fps_start >= 1000) {
        wchar_t title[128];
        g_fps = g_second_frames;
        g_second_frames = 0;
        g_fps_start = now;
        swprintf_s(title, 128, L"图像预览 %dx%d - 帧号 %u - %u fps",
                   IMG_WIDTH, IMG_HEIGHT, g_frame_count, g_fps);
        SetWindowTextW(g_wnd, title);
    }
}

#else
/* Linux 空壳：无图形界面，ui_window_create 返回 -1 即走“仅存图”分支。 */
static int ui_window_create(void) { return -1; }
static void preview_show(const uint8_t *gray) { (void)gray; }
static void ui_timer_cb(evutil_socket_t fd, short what, void *arg)
{
    (void)fd; (void)what; (void)arg;
    (void)g_fps; (void)g_fps_start;   /* 避免 Linux 下 unused 警告 */
}
#endif

/* ---------------- 定时器/动作 ---------------- */
/* 挂起心跳定时器：间隔 CLIENT_HEARTBEAT_INTERVAL_MS（空闲时才有效）。 */
static void arm_hb_timer(void)
{
    struct timeval tv;
    if (!g_hb_timer)
        return;
    tv.tv_sec = CLIENT_HEARTBEAT_INTERVAL_MS / 1000;
    tv.tv_usec = (CLIENT_HEARTBEAT_INTERVAL_MS % 1000) * 1000;
    event_add(g_hb_timer, &tv);
}

/* 摘除心跳定时器：进入收图状态或断开连接时停用心跳。 */
static void disarm_hb_timer(void)
{
    if (g_hb_timer)
        event_del(g_hb_timer);
}

static void schedule_reconnect(void);

/* 心跳定时器回调：仅 ST_IDLE 可用。
 * 发心跳请求 -> 切到 ST_WAIT_HB -> 开启读超时；
 * 服务器应答到达后（on_payload）回到 ST_IDLE 并重新挂起 3 秒定时器。
 * 这构成"周期性探活"闭环：链路断了会在 3 秒内被发现并触发重连。 */
static void hb_timer_cb(evutil_socket_t fd, short what, void *arg)
{
    if (g_state != ST_IDLE)
        return;
    printf("[client] 发送心跳请求\n");
    send_head(CTL_HEARTBEAT_REQ, 0);
    disarm_hb_timer();
    g_state = ST_WAIT_HB;
    recv_reset();
    set_read_timeout(CLIENT_READ_TIMEOUT_MS);
}

/* 请求图像：只能从 ST_IDLE 进入。
 * 发请求头 -> 切到 ST_IMAGE -> 开启读超时；之后服务器持续推
 * CTL_IMG_DATA 帧，每帧由 read_cb 的"头->载荷"状态机解析。 */
static void request_image(void)
{
    if (g_state != ST_IDLE) {
        printf("[client] 当前状态 %d，需空闲状态才能开始收图\n", (int)g_state);
        return;
    }
    printf("[client] 发送请求图像...\n");
    send_head(CTL_REQ_IMAGE, 0);
    disarm_hb_timer();
    g_state = ST_IMAGE;
    recv_reset();
    set_read_timeout(CLIENT_READ_TIMEOUT_MS);
}

/* 停止图像：只能从 ST_IMAGE 退出。
 * 发停止头 -> 回 ST_IDLE -> 关读超时 -> 重启心跳，
 * 并清空输入缓冲里残留的半帧数据，避免残留字节被误当成协议头解析
 * （这正是之前"按 q 后非法头标志刷屏、误重连"问题的根源）。 */
static void stop_image(void)
{
    if (g_state != ST_IMAGE) {
        printf("[client] 当前不在收图状态\n");
        return;
    }
    atomic_exchange(&g_saving, 0);   /* 演示停止，保存同步关闭 */
    printf("[client] 发送停止图像...\n");
    send_head(CTL_STOP_IMAGE, 0);
    g_state = ST_IDLE;
    recv_reset();
    set_read_timeout(0);
    /* 丢弃缓冲里残留的半帧图像数据，避免被当成包头解析 */
    if (g_bev)
        evbuffer_drain(bufferevent_get_input(g_bev), -1);
    arm_hb_timer();
}

/* 销毁连接：释放 bufferevent（BEV_OPT_CLOSE_ON_FREE 会顺带关 socket）、
 * 停心跳定时器、回到 ST_DISCONNECTED。重连前必须先走这里清理干净。 */
static void teardown_connection(void)
{
    if (g_bev) {
        bufferevent_free(g_bev);
        g_bev = NULL;
    }
    disarm_hb_timer();
    g_state = ST_DISCONNECTED;
    recv_reset();
}

/* 安排一次重连：2 秒后执行 reconnect_timer_cb。
 * g_reconnect_pending 做去重，防止断线事件重复触发导致连开多个定时器。 */
static void schedule_reconnect(void)
{
    struct timeval tv;
    if (!g_running || g_reconnect_pending)
        return;
    g_reconnect_pending = 1;
    tv.tv_sec = CLIENT_RECONNECT_DELAY_MS / 1000;
    tv.tv_usec = (CLIENT_RECONNECT_DELAY_MS % 1000) * 1000;
    event_add(g_reconnect_timer, &tv);
}

/* 重连定时器到点：清去重标志，直接发起新连接。 */
static void reconnect_timer_cb(evutil_socket_t fd, short what, void *arg)
{
    g_reconnect_pending = 0;
    g_reconnect_count++;
    printf("[client] 第 %u 次重连 %s:%d ...\n",
           g_reconnect_count, g_server_ip, g_server_port);
    do_connect();
}

/* 发起连接：先清理旧连接，再创建 bufferevent_socket（fd=-1 表示由
 * libevent 内部创建 socket），解析 IP 后异步 connect。
 * 连接结果由 event_cb 的 BEV_EVENT_CONNECTED 回调通知，这里不阻塞。 */
static void do_connect(void)
{
    struct sockaddr_in sin;

    teardown_connection();   /* 确保旧连接已清理 */

    g_bev = bufferevent_socket_new(g_base, -1, BEV_OPT_CLOSE_ON_FREE);
    if (!g_bev) {
        schedule_reconnect();
        return;
    }
    bufferevent_setcb(g_bev, read_cb, NULL, event_cb, NULL);
    bufferevent_enable(g_bev, EV_READ | EV_WRITE);

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((unsigned short)g_server_port);
    if (evutil_inet_pton(AF_INET, g_server_ip, &sin.sin_addr) != 1) {
        printf("[client] IP 地址无效：%s\n", g_server_ip);
        teardown_connection();
        schedule_reconnect();
        return;
    }

    g_state = ST_CONNECTING;
    if (bufferevent_socket_connect(g_bev, (struct sockaddr *)&sin,
                                   sizeof(sin)) < 0) {
        printf("[client] 连接失败，稍后重试\n");
        teardown_connection();
        schedule_reconnect();
        return;
    }
}

/* ---------------- 回调 ---------------- */
/* 一帧完整载荷收齐后调用（read_cb 在载荷进度满时触发）：
 *   ST_WAIT_HB -> 心跳应答，回 ST_IDLE、重启心跳定时器；
 *   ST_IMAGE   -> 计帧、刷新预览窗口（演示），保存仅在 g_saving 开启时进行，
 *                 然后复位状态机继续收下一帧。 */
static void on_payload(void)
{
    if (g_state == ST_WAIT_HB) {
        int32_t val;
        memcpy(&val, g_payload, HEARTBEAT_DATA_LEN);
        g_hb_count++;
        printf("[client] 心跳应答 #%u：%d\n", g_hb_count, (int)val);
        g_state = ST_IDLE;
        recv_reset();
        set_read_timeout(0);
        arm_hb_timer();
        return;
    }

    if (g_state == ST_IMAGE) {
        if (g_payload_len != IMG_DATA_LEN) {
            printf("[client] 图像长度异常：%zu (期望 %d)\n",
                   g_payload_len, IMG_DATA_LEN);
        } else {
            g_frame_count++;
            g_second_frames++;
            /* 图像演示：始终显示，不保存 */
            preview_show(g_payload);
            /* 保存是独立开关：按 b 开启、按 e 关闭，演示时默认不落盘 */
            if (g_saving && g_frame_count % g_save_every == 0)
                save_frame(g_payload, g_payload_len, g_frame_count);
            printf("[client] 收到图像 #%u：%zu 字节\n",
                   g_frame_count, g_payload_len);
        }
        recv_reset();   /* 继续收下一帧 */
        return;
    }
}

/* 16 字节头收齐后调用：按当前状态校验控制码/错误码/长度。
 * 校验通过 -> 进入收载荷阶段；不通过 -> 标记断开并安排重连
 * （goto bad 是唯一的失败出口）。 */
static void on_header(const ImgHead_t *h)
{
    if (g_state == ST_WAIT_HB) {
        if (h->ctl_code != CTL_HEARTBEAT_RESP) {
            printf("[client] 心跳阶段收到意外控制码 %d，重连\n", h->ctl_code);
            goto bad;
        }
        if (expect_payload(HEARTBEAT_DATA_LEN) < 0)
            goto bad;
        return;
    }

    if (g_state == ST_IMAGE) {
        if (h->ctl_code != CTL_IMG_DATA) {
            printf("[client] 收图阶段收到意外控制码 %d，重连\n", h->ctl_code);
            goto bad;
        }
        if (h->err_code != ERR_OK) {
            printf("[client] 服务器错误码 %d（如图像超时），继续等下一帧\n",
                   h->err_code);
            recv_reset();
            return;
        }
        if (h->nDataLen == 0) {
            recv_reset();
            return;
        }
        if (expect_payload((size_t)h->nDataLen) < 0)
            goto bad;
        return;
    }

    printf("[client] 状态 %d 下收到意外数据，重连\n", (int)g_state);
bad:
    g_state = ST_DISCONNECTED;
    schedule_reconnect();
}

/* 读回调：收到任何数据都会进来，是客户端解析的核心状态机。
 *   1) ST_IDLE 下不应有数据：直接丢弃输入缓冲（残留帧数据），不解析不重连；
 *   2) 否则循环处理：先收 16 字节头（可能分多次到达），头齐后校验 flag
 *      并调 on_header() 分发；再按 nDataLen 收载荷，收齐调 on_payload()。
 * 循环写在回调里是为了应付"一次到达多个包"的情况（TCP 粘包）。 */
static void read_cb(struct bufferevent *bev, void *arg)
{
    struct evbuffer *in = bufferevent_get_input(bev);

    /* 空闲状态不应收到任何数据：残留的帧数据直接丢弃，不解析、不重连 */
    if (g_state == ST_IDLE) {
        evbuffer_drain(in, -1);
        return;
    }

    while (evbuffer_get_length(in) > 0) {
        if (g_want_header) {
            int need = IMG_HEAD_LEN - g_hdr_got;
            int n = (int)bufferevent_read(bev, g_hdr + g_hdr_got, (size_t)need);
            if (n <= 0)
                break;
            g_hdr_got += n;
            if (g_hdr_got < IMG_HEAD_LEN)
                break;
            g_hdr_got = 0;
            {
                ImgHead_t *h = (ImgHead_t *)g_hdr;
                if (h->flag[0] != 'C' || h->flag[1] != 'A' ||
                    h->flag[2] != 'M' || h->flag[3] != '0') {
                    printf("[client] 非法头标志，重连\n");
                    schedule_reconnect();
                    return;
                }
                on_header(h);
                if (!g_running || g_state == ST_DISCONNECTED)
                    return;
            }
        } else {
            int need = (int)(g_payload_len - g_payload_got);
            int n = (int)bufferevent_read(bev, g_payload + g_payload_got,
                                          (size_t)need);
            if (n <= 0)
                break;
            g_payload_got += (size_t)n;
            if (g_payload_got >= g_payload_len) {
                on_payload();
                if (!g_running || g_state == ST_DISCONNECTED)
                    return;
            } else {
                break;
            }
        }
    }
}

/* bufferevent 事件回调：
 *   BEV_EVENT_CONNECTED -> 连接成功：进 ST_IDLE、启动心跳定时器；
 *                          若带 -s 参数则立即请求图像；
 *   EOF/ERROR/TIMEOUT   -> 连接中断：清理连接并安排 2 秒后重连。 */
static void event_cb(struct bufferevent *bev, short events, void *arg)
{
    if (events & BEV_EVENT_CONNECTED) {
        printf("[client] 连接成功 (%s:%d)\n", g_server_ip, g_server_port);
        g_state = ST_IDLE;
        recv_reset();
        set_read_timeout(0);
        arm_hb_timer();
        if (g_auto_start_image)
            request_image();
        return;
    }

    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
        const char *why;
        if (events & BEV_EVENT_TIMEOUT)
            why = "读超时";
        else if (events & BEV_EVENT_EOF)
            why = "服务器断开";
        else
            why = "网络错误";
        printf("[client] 连接中断：%s，准备重连...\n", why);
        teardown_connection();
        schedule_reconnect();
    }
}

/* ---------------- 键盘控制 ---------------- */
/* 键盘动作回调：由主循环执行，因此对状态机/连接的修改都是线程安全的。
 * 动作来自 g_key_action（键盘线程写入），这里取出后分发到具体功能。 */
static void key_control_cb(evutil_socket_t fd, short what, void *arg)
{
    int act = (int)atomic_exchange(&g_key_action, 0);

    switch (act) {
    case 's': case 'S':
        request_image();
        break;
    case 'q': case 'Q':
        stop_image();
        break;
    case 'h': case 'H':
        if (g_state == ST_IDLE)
            hb_timer_cb(fd, what, arg);
        break;
    case 'b': case 'B':
        if (g_state != ST_IMAGE) {
            printf("[client] 请先按 s 开始图像演示\n");
            break;
        }
        if (!g_save_dir[0])
            init_save_dir();   /* 首次开启保存时才创建目录，纯演示不产生目录/文件 */
        atomic_exchange(&g_saving, 1);
        printf("[client] 开始保存图片（每 %d 帧存一张 BMP）\n", g_save_every);
        break;
    case 'e': case 'E':
        atomic_exchange(&g_saving, 0);
        printf("[client] 停止保存图片（演示继续）\n");
        break;
    case 'x': case 'X':
        printf("[client] 退出\n");
        atomic_exchange(&g_running, 0);
        event_base_loopbreak(g_base);
        break;
    default:
        break;
    }
}

/* 键盘线程：getchar() 是阻塞调用，放进 libevent 回调会卡死事件循环，
 * 所以单独开一个线程读控制台。每按一键：
 *   写 g_key_action -> event_active() 主动唤醒主循环中的 g_key_event。
 * event_active 是跨线程安全的，即使主循环正阻塞在别的事件上也会被唤醒。 */
#ifdef _WIN32
static DWORD WINAPI stdin_thread(LPVOID arg)
#else
static void *stdin_thread(void *arg)
#endif
{
    int ch;
    while (g_running && (ch = getchar()) != EOF) {
        if (ch == '\r' || ch == '\n')
            continue;
        atomic_exchange(&g_key_action, ch);
        event_active(g_key_event, EV_READ, 0);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* 创建本 client 专属的图片保存目录：frames_时间_PID。
 * 时间精确到秒 + 进程 PID 一起保证唯一性：同时开多个客户端时，
 * 每个进程各存各的目录，图片不会混到一起。 */
static void init_save_dir(void)
{
    char dir[64];
#ifdef _WIN32
    SYSTEMTIME st;

    GetLocalTime(&st);
    snprintf(dir, sizeof(dir), "frames_%04u%02u%02u_%02u%02u%02u_%u",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond,
             (unsigned)GetCurrentProcessId());

    if (CreateDirectoryA(dir, NULL) ||
        GetLastError() == ERROR_ALREADY_EXISTS) {
        snprintf(g_save_dir, sizeof(g_save_dir), "%s", dir);
        printf("保存目录 : %s/\n", g_save_dir);
    } else {
        printf("保存目录 : 创建失败（错误码 %lu），退回当前目录\n",
               (unsigned long)GetLastError());
    }
#else
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    snprintf(dir, sizeof(dir), "frames_%04d%02d%02d_%02d%02d%02d_%d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)getpid());
    if (mkdir(dir, 0755) == 0 || errno == EEXIST) {
        snprintf(g_save_dir, sizeof(g_save_dir), "%s", dir);
        printf("保存目录 : %s/\n", g_save_dir);
    } else {
        printf("保存目录 : 创建失败（错误码 %d），退回当前目录\n", errno);
    }
#endif
}

/* ---------------- 主流程 ---------------- */
int main(int argc, char **argv)
{
#ifdef _WIN32
    WSADATA wsa;
#endif
    int i;

    /* 参数：client [服务器IP] [端口] [-s]
     * argv[1] 不是 "-" 开头则当作服务器 IP，argv[2] 当作端口。 */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0)
            g_auto_start_image = 1;
        else if (i == 1 && argv[i][0] != '-')
            snprintf(g_server_ip, sizeof(g_server_ip), "%s", argv[i]);
        else if (i == 2 && argv[i][0] != '-')
            g_server_port = atoi(argv[i]);
    }

#ifdef _WIN32
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup 失败\n");
        return 1;
    }
    SetConsoleOutputCP(CP_UTF8);
    evthread_use_windows_threads();   /* libevent 内部锁使用 Windows 线程原语 */
#else
    evthread_use_pthreads();          /* libevent 内部锁使用 pthread */
#endif
    setvbuf(stdout, NULL, _IONBF, 0);   /* 重定向时也实时输出日志 */

    g_base = event_base_new();
    if (!g_base) {
        printf("event_base_new 失败\n");
        return 1;
    }

    /* 创建事件：fd=-1 表示"非 socket 定时器/手动事件"。
     * hb_timer 是周期事件（EV_PERSIST），每次到点自动重新挂起；
     * reconnect_timer / key_event 是一次性事件，由代码主动 event_add。 */
    g_hb_timer = event_new(g_base, -1, EV_PERSIST, hb_timer_cb, NULL);
    g_reconnect_timer = event_new(g_base, -1, 0, reconnect_timer_cb, NULL);
    g_key_event = event_new(g_base, -1, 0, key_control_cb, NULL);
    g_ui_timer = event_new(g_base, -1, EV_PERSIST, ui_timer_cb, NULL);

#ifdef _WIN32
    CreateThread(NULL, 0, stdin_thread, NULL, 0, NULL);
#else
    {
        pthread_t tid;
        if (pthread_create(&tid, NULL, stdin_thread, NULL) == 0)
            pthread_detach(tid);
    }
#endif

    /* 实时预览窗口：Windows 有 GDI 窗口；Linux 无图形界面（仅存图）。
     * ui_window_create 在 Linux 恒返回 -1，走 else 分支，主流程不变。 */
    if (ui_window_create() == 0) {
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000;
        event_add(g_ui_timer, &tv);
        g_fps_start = now_ms();
        printf("预览窗口 : 已打开，实时显示收到的图像（窗口内按 ESC 退出）\n");
    } else {
        printf("预览窗口 : 未打开，当前仅存图模式（Linux 下为正常状态）\n");
    }

    printf("=============================================\n");
    printf(" libevent 图像客户端\n");
    printf(" 服务器 : %s:%d\n", g_server_ip, g_server_port);
    printf(" 心跳   : 每 %dms 一次（空闲时）\n", CLIENT_HEARTBEAT_INTERVAL_MS);
    printf(" 读超时 : %dms\n", CLIENT_READ_TIMEOUT_MS);
    printf(" 按键   : s=开始图像演示  q=停止演示\n");
    printf("          b=开始保存图片  e=停止保存\n");
    printf("          h=手动心跳      x=退出\n");
    printf("=============================================\n");

    /* 发起首次连接 -> 进入事件循环。
     * 保存目录不在这里创建：按 b 开始保存时才懒创建，纯演示不产生目录/文件。
     * event_base_dispatch 会一直运行，直到 g_running=0 被 loopbreak。 */
    do_connect();
    event_base_dispatch(g_base);

    /* ---- 退出清理：按创建顺序的逆序释放资源 ---- */
    printf("[client] 程序结束\n");
    teardown_connection();
    event_free(g_hb_timer);
    event_free(g_reconnect_timer);
    event_free(g_key_event);
    if (g_ui_timer)
        event_free(g_ui_timer);
#ifdef _WIN32
    if (g_wnd)
        DestroyWindow(g_wnd);
    if (g_dib) {
        SelectObject(g_mem_dc, g_old_bmp);
        DeleteObject(g_dib);
    }
    if (g_mem_dc)
        DeleteDC(g_mem_dc);
#endif
    event_base_free(g_base);
    if (g_payload)
        free(g_payload);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}