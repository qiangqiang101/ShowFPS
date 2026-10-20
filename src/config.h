#pragma once

#include <windows.h>
#include <string>

enum class OverlayCorner {
    TopLeft = 0,
    TopRight = 1,
    BottomRight = 2,
    BottomLeft = 3
};

enum class TextColorPreset {
    White = 0,
    Green = 1,
    Yellow = 2,
    Cyan = 3,
    Red = 4,
    Dynamic = 5
};

struct ShowFpsConfig {
    bool enabled = true;
    bool showGpuUsage = true;       // e.g. "60 GPU:84%"
    bool showFrameTime = false;     // e.g. "60 GPU:84%  16.6 ms"
    bool showBackground = true;    // Option to disable background color
    OverlayCorner position = OverlayCorner::TopRight;
    bool detailed = false;
    int updateIntervalMs = 250;
    int toggleHotkey = VK_F11;
    
    // Positioning & Layout (closer to border)
    int marginX = 8;
    int marginY = 6;

    // Font & Styling
    int fontSize = 16;              // Configurable font size
    int bgAlpha = 220;              // 0 to 255
    COLORREF pillColor = RGB(28, 138, 115); // Signature Teal Green
    
    // Text Color options
    TextColorPreset textColorPreset = TextColorPreset::White; // 0=White, 1=Green, 2=Yellow, 3=Cyan, 4=Red, 5=Dynamic
    bool dynamicColor = false;
};

extern ShowFpsConfig g_Config;

void LoadConfig(const std::wstring& modDir);
void SaveConfig(const std::wstring& modDir);
