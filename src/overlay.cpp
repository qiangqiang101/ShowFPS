#include "overlay.h"
#include "config.h"
#include "fps_counter.h"
#include "gpu_monitor.h"

#include <cmath>
#include <vector>
#include <dwmapi.h>

OverlayWindow g_Overlay;

namespace {
const wchar_t OVERLAY_CLASS_NAME[] = L"ShowFPS_OverlayClass";
const UINT_PTR TIMER_ID = 1001;
const UINT TIMER_INTERVAL_MS = 33; // ~30 FPS overlay refresh

struct WindowSearchContext {
    DWORD targetPid;
    HWND bestHwnd;
    int maxArea;
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<WindowSearchContext*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->targetPid && IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
        RECT r;
        if (GetClientRect(hwnd, &r)) {
            int area = (r.right - r.left) * (r.bottom - r.top);
            if (area > ctx->maxArea) {
                ctx->maxArea = area;
                ctx->bestHwnd = hwnd;
            }
        }
    }
    return TRUE;
}

COLORREF GetActiveTextColor(float fps) {
    if (g_Config.dynamicColor || g_Config.textColorPreset == TextColorPreset::Dynamic) {
        if (fps >= 50.0f) return RGB(74, 222, 128); // Green
        if (fps >= 30.0f) return RGB(250, 204, 21); // Yellow/Amber
        return RGB(248, 113, 113);                  // Red
    }
    switch (g_Config.textColorPreset) {
    case TextColorPreset::Green:   return RGB(74, 222, 128);
    case TextColorPreset::Yellow:  return RGB(250, 204, 21);
    case TextColorPreset::Cyan:    return RGB(56, 189, 248);
    case TextColorPreset::Red:     return RGB(248, 113, 113);
    case TextColorPreset::White:
    default:                       return RGB(255, 255, 255);
    }
}

} // namespace

OverlayWindow::OverlayWindow() = default;

OverlayWindow::~OverlayWindow() {
    Stop();
}

bool OverlayWindow::Start(HINSTANCE hInstance) {
    if (m_running.load()) return true;

    m_hInstance = hInstance;
    m_running.store(true);

    m_thread = CreateThread(nullptr, 0, ThreadProc, this, 0, &m_threadId);
    return m_thread != nullptr;
}

void OverlayWindow::Stop() {
    if (!m_running.load()) return;
    m_running.store(false);

    if (m_overlayHwnd && IsWindow(m_overlayHwnd)) {
        PostMessageW(m_overlayHwnd, WM_CLOSE, 0, 0);
    }

    if (m_thread) {
        WaitForSingleObject(m_thread, 2000);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
}

void OverlayWindow::SetGameWindow(HWND hwnd) {
    if (hwnd && IsWindow(hwnd)) {
        m_gameHwnd = hwnd;
    }
}

void OverlayWindow::RequestRedraw() {
    if (m_overlayHwnd && IsWindow(m_overlayHwnd)) {
        PostMessageW(m_overlayHwnd, WM_TIMER, TIMER_ID, 0);
    }
}

DWORD WINAPI OverlayWindow::ThreadProc(LPVOID param) {
    auto* self = static_cast<OverlayWindow*>(param);
    self->RunMessageLoop();
    return 0;
}

LRESULT CALLBACK OverlayWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_TIMER:
        if (self && wParam == TIMER_ID) {
            self->UpdatePositionAndRender();
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

HWND OverlayWindow::FindGameWindowFallback() {
    WindowSearchContext ctx{};
    ctx.targetPid = GetCurrentProcessId();
    ctx.bestHwnd = nullptr;
    ctx.maxArea = 0;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.bestHwnd;
}

void OverlayWindow::RunMessageLoop() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = m_hInstance ? m_hInstance : GetModuleHandleW(nullptr);
    wc.lpszClassName = OVERLAY_CLASS_NAME;
    RegisterClassExW(&wc);

    m_overlayHwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        OVERLAY_CLASS_NAME,
        L"ShowFPS_Overlay",
        WS_POPUP,
        0, 0, 1, 1,
        nullptr, nullptr, wc.hInstance, this
    );

    if (!m_overlayHwnd) {
        m_running.store(false);
        return;
    }

    SetTimer(m_overlayHwnd, TIMER_ID, TIMER_INTERVAL_MS, nullptr);

    MSG msg;
    while (m_running.load() && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (m_overlayHwnd && IsWindow(m_overlayHwnd)) {
        KillTimer(m_overlayHwnd, TIMER_ID);
        DestroyWindow(m_overlayHwnd);
        m_overlayHwnd = nullptr;
    }

    UnregisterClassW(OVERLAY_CLASS_NAME, wc.hInstance);
}

void OverlayWindow::UpdatePositionAndRender() {
    // 1. Hotkey check
    if (g_Config.toggleHotkey > 0) {
        bool isDown = (GetAsyncKeyState(g_Config.toggleHotkey) & 0x8000) != 0;
        if (isDown && !m_wasHotkeyDown) {
            g_Config.enabled = !g_Config.enabled;
        }
        m_wasHotkeyDown = isDown;
    }

    // 2. Check enabled state
    if (!g_Config.enabled) {
        if (IsWindowVisible(m_overlayHwnd)) {
            ShowWindow(m_overlayHwnd, SW_HIDE);
        }
        return;
    }

    // 3. Find/Validate game window
    if (!m_gameHwnd || !IsWindow(m_gameHwnd)) {
        m_gameHwnd = FindGameWindowFallback();
    }

    if (!m_gameHwnd || !IsWindow(m_gameHwnd) || IsIconic(m_gameHwnd)) {
        if (IsWindowVisible(m_overlayHwnd)) {
            ShowWindow(m_overlayHwnd, SW_HIDE);
        }
        return;
    }

    HWND fg = GetForegroundWindow();
    DWORD fgPid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fgPid);
    if (fgPid != GetCurrentProcessId()) {
        if (IsWindowVisible(m_overlayHwnd)) {
            ShowWindow(m_overlayHwnd, SW_HIDE);
        }
        return;
    }

    // 4. Format text & determine colors
    float fps = g_FpsCounter.GetFps();
    float ft = g_FpsCounter.GetFrameTimeMs();
    int gpu = g_GpuMonitor.GetGpuUsage();

    // Emergency fallback if DXGI hooks receive no calls after window is active
    if (g_FpsCounter.GetTotalFrames() == 0 && m_gameHwnd && IsWindow(m_gameHwnd)) {
        static DWORD s_firstActiveTime = 0;
        if (s_firstActiveTime == 0) s_firstActiveTime = GetTickCount();
        if (GetTickCount() - s_firstActiveTime > 3000) {
            DWM_TIMING_INFO ti{};
            ti.cbSize = sizeof(ti);
            if (SUCCEEDED(DwmGetCompositionTimingInfo(m_gameHwnd, &ti)) && ti.rateRefresh.uiDenominator > 0) {
                float refreshFps = static_cast<float>(ti.rateRefresh.uiNumerator) / static_cast<float>(ti.rateRefresh.uiDenominator);
                if (refreshFps > 0.0f) {
                    fps = refreshFps;
                    ft = (1000.0f / refreshFps);
                }
            }
        }
    }

    wchar_t textBuf[128];
    if (g_Config.showGpuUsage && g_Config.showFrameTime) {
        if (g_Config.detailed) {
            std::swprintf(textBuf, sizeof(textBuf) / sizeof(wchar_t), L"%.1f GPU: %d%% %.1f ms", fps, gpu, ft);
        } else {
            std::swprintf(textBuf, sizeof(textBuf) / sizeof(wchar_t), L"%.0f GPU: %d%% %.1f ms", fps, gpu, ft);
        }
    } else if (g_Config.showGpuUsage) {
        if (g_Config.detailed) {
            std::swprintf(textBuf, sizeof(textBuf) / sizeof(wchar_t), L"%.1f GPU: %d%%", fps, gpu);
        } else {
            std::swprintf(textBuf, sizeof(textBuf) / sizeof(wchar_t), L"%.0f GPU: %d%%", fps, gpu);
        }
    } else if (g_Config.showFrameTime) {
        if (g_Config.detailed) {
            std::swprintf(textBuf, sizeof(textBuf) / sizeof(wchar_t), L"%.1f FPS %.1f ms", fps, ft);
        } else {
            std::swprintf(textBuf, sizeof(textBuf) / sizeof(wchar_t), L"%.0f FPS %.1f ms", fps, ft);
        }
    } else {
        if (g_Config.detailed) {
            std::swprintf(textBuf, sizeof(textBuf) / sizeof(wchar_t), L"%.1f FPS", fps);
        } else {
            std::swprintf(textBuf, sizeof(textBuf) / sizeof(wchar_t), L"%.0f FPS", fps);
        }
    }

    // Pill background color (dynamic or teal)
    COLORREF pillBgColor;
    if (fps >= 50.0f) {
        pillBgColor = RGB(30, 142, 118); // Teal Green
    } else if (fps >= 30.0f) {
        pillBgColor = RGB(202, 138, 4);   // Amber
    } else {
        pillBgColor = RGB(220, 38, 38);   // Red
    }

    COLORREF textColor = GetActiveTextColor(fps);

    // 5. Measure text size
    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    
    HFONT hFont = CreateFontW(
        -g_Config.fontSize, 0, 0, 0,
        FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI"
    );
    HGDIOBJ oldFont = SelectObject(memDC, hFont);

    RECT textRect = {0, 0, 0, 0};
    DrawTextW(memDC, textBuf, -1, &textRect, DT_CALCRECT | DT_SINGLELINE);

    int textW = textRect.right - textRect.left;
    int textH = textRect.bottom - textRect.top;

    // Tight padding closer to the border
    int padX = g_Config.showBackground ? 9 : 3;
    int padY = g_Config.showBackground ? 3 : 1;
    int badgeW = textW + (padX * 2);
    int badgeH = textH + (padY * 2);

    SelectObject(memDC, oldFont);
    DeleteObject(hFont);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    // 6. Calculate position relative to game client area (using configured tight margins)
    RECT clientRect{};
    GetClientRect(m_gameHwnd, &clientRect);
    POINT ptClientTopLeft = {clientRect.left, clientRect.top};
    ClientToScreen(m_gameHwnd, &ptClientTopLeft);

    int clientW = clientRect.right - clientRect.left;
    int clientH = clientRect.bottom - clientRect.top;

    int posX = 0;
    int posY = 0;
    const int marginX = g_Config.marginX;
    const int marginY = g_Config.marginY;

    switch (g_Config.position) {
    case OverlayCorner::TopLeft:
        posX = ptClientTopLeft.x + marginX;
        posY = ptClientTopLeft.y + marginY;
        break;
    case OverlayCorner::TopRight:
    default:
        posX = ptClientTopLeft.x + clientW - badgeW - marginX;
        posY = ptClientTopLeft.y + marginY;
        break;
    case OverlayCorner::BottomRight:
        posX = ptClientTopLeft.x + clientW - badgeW - marginX;
        posY = ptClientTopLeft.y + clientH - badgeH - marginY;
        break;
    case OverlayCorner::BottomLeft:
        posX = ptClientTopLeft.x + marginX;
        posY = ptClientTopLeft.y + clientH - badgeH - marginY;
        break;
    }

    // 7. Render badge into 32-bit DIB and call UpdateLayeredWindow
    RenderBadge(badgeW, badgeH, textBuf, textColor, pillBgColor);

    SetWindowPos(
        m_overlayHwnd,
        HWND_TOPMOST,
        posX, posY, badgeW, badgeH,
        SWP_NOACTIVATE | SWP_SHOWWINDOW
    );
}

void OverlayWindow::RenderBadge(int width, int height, const std::wstring& text, COLORREF textColor, COLORREF bgColor) {
    if (width <= 0 || height <= 0) return;

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    uint32_t* pixels = nullptr;
    HBITMAP hBitmap = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
    if (!hBitmap || !pixels) {
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return;
    }

    HGDIOBJ oldBitmap = SelectObject(memDC, hBitmap);

    // Initialize all pixels to transparent
    memset(pixels, 0, width * height * sizeof(uint32_t));

    // 1. Fill background with rounded pill if enabled
    if (g_Config.showBackground) {
        int radius = 6;
        uint8_t bgA = static_cast<uint8_t>(g_Config.bgAlpha);
        uint8_t bgR = GetRValue(bgColor);
        uint8_t bgG = GetGValue(bgColor);
        uint8_t bgB = GetBValue(bgColor);

        uint8_t bgR_pm = (bgR * bgA) / 255;
        uint8_t bgG_pm = (bgG * bgA) / 255;
        uint8_t bgB_pm = (bgB * bgA) / 255;
        uint32_t bgPixel = (static_cast<uint32_t>(bgA) << 24) |
                           (static_cast<uint32_t>(bgR_pm) << 16) |
                           (static_cast<uint32_t>(bgG_pm) << 8) |
                           static_cast<uint32_t>(bgB_pm);

        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                bool inCorner = false;
                int dx = 0;
                int dy = 0;

                if (x < radius && y < radius) {
                    dx = radius - x; dy = radius - y; inCorner = true;
                } else if (x >= width - radius && y < radius) {
                    dx = x - (width - radius - 1); dy = radius - y; inCorner = true;
                } else if (x < radius && y >= height - radius) {
                    dx = radius - x; dy = y - (height - radius - 1); inCorner = true;
                } else if (x >= width - radius && y >= height - radius) {
                    dx = x - (width - radius - 1); dy = y - (height - radius - 1); inCorner = true;
                }

                if (inCorner) {
                    int distSq = dx * dx + dy * dy;
                    int radSq = radius * radius;
                    if (distSq <= radSq) {
                        pixels[y * width + x] = bgPixel;
                    }
                } else {
                    pixels[y * width + x] = bgPixel;
                }
            }
        }
    }

    // 2. Render text onto a separate monochrome mask for clean anti-aliasing
    HDC maskDC = CreateCompatibleDC(screenDC);
    HBITMAP maskBmp = CreateCompatibleBitmap(screenDC, width, height);
    HGDIOBJ oldMaskBmp = SelectObject(maskDC, maskBmp);

    RECT fullRect = {0, 0, width, height};
    FillRect(maskDC, &fullRect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    HFONT hFont = CreateFontW(
        -g_Config.fontSize, 0, 0, 0,
        FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI"
    );
    HGDIOBJ oldMaskFont = SelectObject(maskDC, hFont);

    SetBkMode(maskDC, TRANSPARENT);
    SetTextColor(maskDC, RGB(255, 255, 255));
    DrawTextW(maskDC, text.c_str(), -1, &fullRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    BITMAPINFO maskBmi{};
    maskBmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    maskBmi.bmiHeader.biWidth = width;
    maskBmi.bmiHeader.biHeight = -height;
    maskBmi.bmiHeader.biPlanes = 1;
    maskBmi.bmiHeader.biBitCount = 32;
    maskBmi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint32_t> maskPixels(width * height);
    GetDIBits(maskDC, maskBmp, 0, height, maskPixels.data(), &maskBmi, DIB_RGB_COLORS);

    uint8_t tR = GetRValue(textColor);
    uint8_t tG = GetGValue(textColor);
    uint8_t tB = GetBValue(textColor);

    // Apply dark shadow / outline
    if (!g_Config.showBackground) {
        // When background is disabled, render a robust 8-way dark outline around text for maximum readability
        const int offsets[8][2] = {
            {-1, -1}, {0, -1}, {1, -1},
            {-1,  0},         {1,  0},
            {-1,  1}, {0,  1}, {1,  1}
        };
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                uint32_t maskVal = maskPixels[y * width + x] & 0xFF;
                if (maskVal > 15) {
                    for (const auto& off : offsets) {
                        int ox = x + off[0];
                        int oy = y + off[1];
                        if (ox >= 0 && ox < width && oy >= 0 && oy < height) {
                            int idx = oy * width + ox;
                            uint8_t curA = (pixels[idx] >> 24) & 0xFF;
                            uint8_t outlineA = static_cast<uint8_t>((maskVal * 200) / 255);
                            if (outlineA > curA) {
                                pixels[idx] = (static_cast<uint32_t>(outlineA) << 24); // Black outline with alpha
                            }
                        }
                    }
                }
            }
        }
    } else {
        // Standard subtle drop shadow (+1, +1) when background is on
        for (int y = 0; y < height - 1; ++y) {
            for (int x = 0; x < width - 1; ++x) {
                uint32_t maskVal = maskPixels[y * width + x] & 0xFF;
                if (maskVal > 15) {
                    int sy = y + 1;
                    int sx = x + 1;
                    int idx = sy * width + sx;
                    uint8_t shadowA = static_cast<uint8_t>((maskVal * 120) / 255);
                    uint32_t cur = pixels[idx];
                    uint8_t curA = (cur >> 24) & 0xFF;
                    if (curA > 0) {
                        uint8_t curR = (cur >> 16) & 0xFF;
                        uint8_t curG = (cur >> 8) & 0xFF;
                        uint8_t curB = cur & 0xFF;
                        uint8_t newR = (curR * (255 - shadowA)) / 255;
                        uint8_t newG = (curG * (255 - shadowA)) / 255;
                        uint8_t newB = (curB * (255 - shadowA)) / 255;
                        pixels[idx] = (curA << 24) | (newR << 16) | (newG << 8) | newB;
                    }
                }
            }
        }
    }

    // Blend main text
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint32_t maskVal = maskPixels[y * width + x] & 0xFF;
            if (maskVal > 0) {
                int idx = y * width + x;
                uint8_t textA = static_cast<uint8_t>(maskVal);
                uint8_t textR_pm = (tR * textA) / 255;
                uint8_t textG_pm = (tG * textA) / 255;
                uint8_t textB_pm = (tB * textA) / 255;

                uint32_t cur = pixels[idx];
                uint8_t curA = (cur >> 24) & 0xFF;
                uint8_t curR = (cur >> 16) & 0xFF;
                uint8_t curG = (cur >> 8) & 0xFF;
                uint8_t curB = cur & 0xFF;

                uint8_t invA = 255 - textA;
                uint8_t outR = textR_pm + ((curR * invA) / 255);
                uint8_t outG = textG_pm + ((curG * invA) / 255);
                uint8_t outB = textB_pm + ((curB * invA) / 255);
                uint8_t outA = textA + ((curA * invA) / 255);

                pixels[idx] = (outA << 24) | (outR << 16) | (outG << 8) | outB;
            }
        }
    }

    // Clean up mask
    SelectObject(maskDC, oldMaskFont);
    DeleteObject(hFont);
    SelectObject(maskDC, oldMaskBmp);
    DeleteObject(maskBmp);
    DeleteDC(maskDC);

    // 3. Update layered window surface
    POINT ptSrc = {0, 0};
    SIZE size = {width, height};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(
        m_overlayHwnd,
        screenDC,
        nullptr,
        &size,
        memDC,
        &ptSrc,
        0,
        &blend,
        ULW_ALPHA
    );

    SelectObject(memDC, oldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}
