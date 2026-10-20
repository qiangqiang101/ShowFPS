#pragma once

#include <windows.h>
#include <atomic>
#include <string>

class OverlayWindow {
public:
    OverlayWindow();
    ~OverlayWindow();

    bool Start(HINSTANCE hInstance);
    void Stop();

    void SetGameWindow(HWND hwnd);
    HWND GetGameWindow() const { return m_gameHwnd; }

    void RequestRedraw();

private:
    static DWORD WINAPI ThreadProc(LPVOID param);
    void RunMessageLoop();
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void UpdatePositionAndRender();
    void RenderBadge(int width, int height, const std::wstring& text, COLORREF textColor, COLORREF bgColor);

    HWND FindGameWindowFallback();

    HINSTANCE m_hInstance = nullptr;
    HWND m_overlayHwnd = nullptr;
    HWND m_gameHwnd = nullptr;
    HANDLE m_thread = nullptr;
    DWORD m_threadId = 0;
    std::atomic<bool> m_running{false};
    bool m_wasHotkeyDown = false;
};

extern OverlayWindow g_Overlay;
