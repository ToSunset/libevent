#pragma once
/* 实时预览窗口：Windows 用纯 Win32 GDI（24 位 DIB + BitBlt），
 * Linux 无图形界面（create 恒返回 false，仅存图模式）。 */

#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#endif

namespace cam {

class PreviewWindow {
public:
    PreviewWindow() = default;
    ~PreviewWindow();

    PreviewWindow(const PreviewWindow&) = delete;
    PreviewWindow& operator=(const PreviewWindow&) = delete;

    /* 创建窗口 + DIB，成功返回 true（Linux 恒 false） */
    bool create(int width, int height);
    void destroy();

    /* 显示一帧灰度图（w*h），内部转 24 位 RGB */
    void showFrame(const uint8_t* gray, int w, int h);

    /* 泵窗口消息；返回 false 表示收到 WM_QUIT（应退出主循环） */
    bool pumpMessages();

    void setVisible(bool v);
    bool visible() const { return visible_; }
    bool hasWindow() const;

    /* 每秒刷新标题：帧号 + fps */
    void setTitle(int w, int h, uint32_t frame, uint32_t fps);

private:
#ifdef _WIN32
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    HWND     wnd_     = nullptr;
    HDC      memDc_   = nullptr;
    HBITMAP  dib_     = nullptr;
    HGDIOBJ  oldBmp_  = nullptr;
    uint8_t* rgbBuf_  = nullptr;
#endif
    int      width_   = 640;
    int      height_  = 480;
    bool     visible_ = false;
};

}  /* namespace cam */
