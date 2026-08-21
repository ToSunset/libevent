#include "preview_window.h"

#include <cstdio>

#ifdef _WIN32

namespace cam {

/* 窗口过程：ESC / 关闭按钮只隐藏窗口（关闭监控），不退出程序 */
LRESULT CALLBACK PreviewWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<PreviewWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (self && self->memDc_)
            BitBlt(dc, 0, 0, self->width_, self->height_,
                   self->memDc_, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            ShowWindow(hwnd, SW_HIDE);
            self->visible_ = false;
            return 0;
        }
        break;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        self->visible_ = false;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool PreviewWindow::create(int width, int height)
{
    width_  = width;
    height_ = height;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ImgPreviewWnd";
    RegisterClassExW(&wc);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = width_;
    bi.bmiHeader.biHeight      = -height_;   /* 负数 = 自上而下 */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    wnd_ = CreateWindowExW(0, L"ImgPreviewWnd", L"图像预览",
                           WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           width_ + 16, height_ + 40,
                           nullptr, nullptr, wc.hInstance, nullptr);
    if (!wnd_) return false;
    SetWindowLongPtrW(wnd_, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(this));

    memDc_ = CreateCompatibleDC(nullptr);
    dib_   = CreateDIBSection(memDc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib_ || !bits) {
        DestroyWindow(wnd_);
        wnd_ = nullptr;
        return false;
    }
    oldBmp_ = SelectObject(memDc_, dib_);
    rgbBuf_ = static_cast<uint8_t*>(bits);

    ShowWindow(wnd_, SW_SHOW);
    ShowWindow(wnd_, SW_SHOW);   /* 第二次调用强制显示 */
    visible_ = true;
    return true;
}

void PreviewWindow::destroy()
{
#ifdef _WIN32
    if (wnd_) DestroyWindow(wnd_);
    wnd_ = nullptr;
    if (dib_) {
        if (memDc_) SelectObject(memDc_, oldBmp_);
        DeleteObject(dib_);
        dib_ = nullptr;
    }
    if (memDc_) {
        DeleteDC(memDc_);
        memDc_ = nullptr;
    }
    rgbBuf_ = nullptr;
    visible_ = false;
#endif
}

PreviewWindow::~PreviewWindow()
{
    destroy();
}

void PreviewWindow::showFrame(const uint8_t* gray, int w, int h)
{
    if (!rgbBuf_ || gray == nullptr || w != width_ || h != height_) return;
    uint8_t* p = rgbBuf_;
    const size_t n = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < n; i++) {
        const uint8_t v = gray[i];
        *p++ = v;
        *p++ = v;
        *p++ = v;
    }
    /* 监控关闭（窗口隐藏）时只更新内存像素、跳过重绘；重新打开时立即显示最新帧 */
    if (wnd_ && IsWindowVisible(wnd_)) {
        InvalidateRect(wnd_, nullptr, FALSE);
        UpdateWindow(wnd_);
    }
}

bool PreviewWindow::pumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

void PreviewWindow::setVisible(bool v)
{
    if (!wnd_) return;
    ShowWindow(wnd_, v ? SW_SHOW : SW_HIDE);
    if (v) {
        InvalidateRect(wnd_, nullptr, FALSE);
        UpdateWindow(wnd_);
    }
    visible_ = v;
}

bool PreviewWindow::hasWindow() const
{
#ifdef _WIN32
    return wnd_ != nullptr;
#else
    return false;
#endif
}

void PreviewWindow::setTitle(int w, int h, uint32_t frame, uint32_t fps)
{
    if (!wnd_) return;
    wchar_t title[128];
    swprintf_s(title, 128, L"图像预览 %dx%d - 帧号 %u - %u fps",
               w, h, frame, fps);
    SetWindowTextW(wnd_, title);
}

}  /* namespace cam */

#else  /* Linux 空壳：无图形界面 */

namespace cam {

bool PreviewWindow::create(int, int) { return false; }
void PreviewWindow::destroy() {}
void PreviewWindow::showFrame(const uint8_t*, int, int) {}
bool PreviewWindow::pumpMessages() { return true; }
void PreviewWindow::setVisible(bool) {}
bool PreviewWindow::hasWindow() const { return false; }
void PreviewWindow::setTitle(int, int, uint32_t, uint32_t) {}
PreviewWindow::~PreviewWindow() { destroy(); }

}  /* namespace cam */

#endif
