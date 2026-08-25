# CLASSES.md —— 类架构说明（C++11，全部 .h/.cpp）

本项目的所有业务实体均为 class，每个类一个 .h（声明）+ .cpp（实现），
头文件统一 `#pragma once`；资源全部由构造/析构（RAII）管理，
动态内存使用 std::unique_ptr / std::vector / std::thread，无裸 new/delete。

## 一、类总览

| 类名 | 文件 | 一句话职责 |
|------|------|-----------|
| Server | server/server.h/.cpp | 组合根：组装所有子系统，主循环监听 9995 |
| ImageSource | server/image_source.h/.cpp | 图像生成线程 + 生产者/消费者模型 |
| ClientSession | server/client_session.h/.cpp | 一个 TCP 连接 = 一个会话对象 |
| ClientManager | server/client_manager.h/.cpp | 会话管理表（最多 10 个） |
| WebServer | server/web_server.h/.cpp | mongoose HTTP 浏览器界面（8080） |
| PngEncoder | server/png_encoder.h/.cpp | 自包含极简 PNG 编码器 |
| Client | client/client.h/.cpp | 客户端状态机 + 事件循环 |
| PreviewWindow | client/preview_window.h/.cpp | Win32 GDI 预览窗口（Linux 空壳） |
| Logger | server/common/logger.h（client/common 同步）/.cpp | 分级日志（DBG/INF/WRN/ERR） |
| FrameHeader | server/common/protocol.h（client/common 同步） | 16 字节协议头（class 封装） |

## 二、类图（依赖关系）

```
                       ┌─────────────┐
                       │   Server    │  组合根：main.cpp 只创建它
                       └──────┬──────┘
              ┌───────────────┼───────────────┐
              ▼               ▼               ▼
      ┌────────────┐  ┌──────────────┐  ┌─────────────┐
      │ImageSource │  │ClientManager │  │  WebServer  │
      │ 生成线程    │  │ 会话表(≤10)  │  │ HTTP 8080   │
      └─────┬──────┘  └──────┬───────┘  └──┬────┬─────┘
            │                │             │    │
            │                ▼             │    ▼
            │        ┌──────────────┐      │  ┌────────────┐
            │        │ClientSession │      │  │PngEncoder  │
            │        │ 控制线程+发送  │      │  │ PNG 编码   │
            │        └──────┬───────┘      │  └────────────┘
            └───────────────┴──────────────┘

  客户端：        ┌──────────┐     ┌───────────────┐
                  │  Client  │────▶│ PreviewWindow │
                  │ 状态机    │     │ Win32 GDI     │
                  └──────────┘     └───────────────┘

  公共：  Logger（两端共用）   FrameHeader（协议头，两端共用）
```

## 三、逐类说明

### 1. class Server（server/server.h）
- 职责：组合根。持有 ImageSource、ClientManager、WebServer；
  主线程跑监听事件循环（9995 端口），Ctrl+C 统一走 stop()。
- public：`run()` 启动并运行；`stop()` 置停止标志并打断各事件循环。
- private：`onAccept()` 接受连接（static 回调，arg 传 this）；
  `onSigint()` 信号处理。
- 依赖：ImageSource、ClientManager、WebServer。

### 2. class ImageSource（server/image_source.h）
- 职责：唯一写图线程，libevent 定时器按间隔（默认 20ms）生成 640x480
  8bit 灰度测试图，广播"新帧"信号；暂停 = 全局冻结，恢复后从原位置继续。
- public：`start/stop`、`pause/resume`、`isRunning`、`setIntervalMs`、
  `copyLatestFrame`、`seq`、`waitForNewFrame`、`wakeAll`、`breakLoop`。
- private：`threadMain/onFrameTimer/genOneFrame/drawFrameNumber`、
  `lock_/cond_/frame_/seq_`、`intervalMs_/running_/stopped_`、`thread_`。

### 3. class ClientSession（server/client_session.h）
- 职责：一个 TCP 连接 = 一个会话。独立控制线程（event_base + bufferevent）
  解析协议命令（心跳/请求图像/停止图像/暂停恢复图像源），
  可选的发送线程等"新帧"信号后发送"16 字节头 + 图像"；断开后自行清理。
- public：`start/startSending/stopSending/send`、`fd/slot/setSlot`。
- private：`ctrlThreadMain/sendThreadMain/onRead/onEvent/handleHeartbeat/
  sendError`、`bev_/base_/sendLock_/sendStop_/lastSeq_/sendBuf_`。
- 依赖：ImageSource、ClientManager。

### 4. class ClientManager（server/client_manager.h）
- 职责：固定容量会话表（kMaxClients=10），std::unique_ptr 持有会话；
  表满拒绝新连接；会话关闭回调里摘表并销毁对象。
- public：`add(fd)` 新建会话；`onClosed(s)` 摘表；`count()`。
- private：`slots_`（unique_ptr 数组）、`count_`、`lock_`。
- 依赖：ClientSession、ImageSource。

### 5. class WebServer（server/web_server.h）
- 职责：独立线程轮询 mg_mgr，提供浏览器界面：`/`、`/frame`、
  `/api/stats|start|stop|heartbeat|fps|display`。
- public：`start/stop/requestStop`。
- private：`threadMain/onEvent/handleHttp/serveIndex/serveFrame/serveStats/
  handleFps/handleDisplay/loadIndexHtml`、`frozen_/heartbeat_/dispW_/dispH_`。
- 依赖：ImageSource、ClientManager、PngEncoder。

### 6. class PngEncoder（server/png_encoder.h）
- 职责：8bit 灰度 → PNG（zlib stored 块，无第三方依赖）。
- public：`encodeGray(gray, w, h)` 返回 std::vector<uint8_t>。
- private：`crcTab_`、`crc32/adler32/putU32/writeChunk`。

### 7. class Client（client/client.h）
- 职责：单线程 libevent 事件循环 + 连接状态机
  （Disconnected → Connecting → Idle → WaitHb/Image）；
  心跳探活、断线重连、按键控制、BMP 保存。
- public：`run()`。
- private：`doConnect/teardownConnection/scheduleReconnect/requestImage/
  stopImage/onHeader/onPayload/readCb/eventCb/keyControlCb/...`、
  `bev_/state_/payload_/frameCount_`、定时器与统计成员。
- 依赖：PreviewWindow、Logger。

### 8. class PreviewWindow（client/preview_window.h）
- 职责：Win32 GDI 预览窗口（24 位 DIB + BitBlt）；Linux 为空壳。
- public：`create/destroy/showFrame/pumpMessages/setVisible/setTitle/hasWindow`。
- private：`wnd_/memDc_/dib_/rgbBuf_`、`width_/height_`。

### 9. class Logger（server/common/logger.h（client/common 同步））
- 职责：分级日志（kDebug/kInfo/kWarn/kError/kOff）+ 时间戳 + 线程安全。
- public：`instance()`、`setLevel`、`debug/info/warn/error`。
- private：`log()`、`level_`、`ts_`、`mu_`。

### 10. class FrameHeader（server/common/protocol.h（client/common 同步））
- 职责：16 字节协议头（flag"CAM0" + ctl + err + len），class 封装。
- public：`flag/ctl/err/len`、构造、`kSize`、`isValid`、`toBytes`、`fromBytes`。

## 四、设计原则
1. 每个类独立 .h/.cpp，`#pragma once` 防重包含。
2. 构造初始化列表 + 析构释放（RAII），libevent 回调必须 static，用 arg 传 this。
3. 动态内存只有 std::unique_ptr / std::vector / std::thread，无裸 new/delete。
4. 组合优于继承：类之间是"拥有/使用"关系（见类图），无多继承。
5. 线程模型：Server 主循环监听；ImageSource 生成线程；WebServer 独立线程；
   每客户端一个控制线程（detach）+ 可选发送线程；Client 单线程 + 键盘线程。
