#include "config.h"

#include <cstdio>

ShowFpsConfig g_Config;

static std::wstring GetIniPath(const std::wstring& modDir) {
    if (modDir.size() >= 4 && modDir.rfind(L".ini") == modDir.size() - 4) {
        return modDir;
    }
    return modDir + L"\\ShowFPS.ini";
}

void LoadConfig(const std::wstring& modDir) {
    std::wstring ini = GetIniPath(modDir);

    g_Config.enabled = GetPrivateProfileIntW(L"ShowFPS", L"Enabled", 1, ini.c_str()) != 0;
    g_Config.showGpuUsage = GetPrivateProfileIntW(L"ShowFPS", L"ShowGpuUsage", 1, ini.c_str()) != 0;
    g_Config.showFrameTime = GetPrivateProfileIntW(L"ShowFPS", L"ShowFrameTime", 0, ini.c_str()) != 0;
    g_Config.showBackground = GetPrivateProfileIntW(L"ShowFPS", L"ShowBackground", 1, ini.c_str()) != 0;
    
    int pos = GetPrivateProfileIntW(L"ShowFPS", L"Position", 1, ini.c_str());
    if (pos >= 0 && pos <= 3) {
        g_Config.position = static_cast<OverlayCorner>(pos);
    } else {
        g_Config.position = OverlayCorner::TopRight;
    }

    g_Config.detailed = GetPrivateProfileIntW(L"ShowFPS", L"Detailed", 0, ini.c_str()) != 0;
    g_Config.updateIntervalMs = GetPrivateProfileIntW(L"ShowFPS", L"UpdateIntervalMs", 250, ini.c_str());
    if (g_Config.updateIntervalMs < 50) g_Config.updateIntervalMs = 50;
    if (g_Config.updateIntervalMs > 2000) g_Config.updateIntervalMs = 2000;

    g_Config.toggleHotkey = GetPrivateProfileIntW(L"ShowFPS", L"ToggleHotkey", VK_F11, ini.c_str());
    
    g_Config.fontSize = GetPrivateProfileIntW(L"ShowFPS", L"FontSize", 16, ini.c_str());
    if (g_Config.fontSize < 10) g_Config.fontSize = 10;
    if (g_Config.fontSize > 48) g_Config.fontSize = 48;

    g_Config.marginX = GetPrivateProfileIntW(L"ShowFPS", L"MarginX", 8, ini.c_str());
    g_Config.marginY = GetPrivateProfileIntW(L"ShowFPS", L"MarginY", 6, ini.c_str());

    g_Config.bgAlpha = GetPrivateProfileIntW(L"ShowFPS", L"BgAlpha", 220, ini.c_str());
    if (g_Config.bgAlpha < 0) g_Config.bgAlpha = 0;
    if (g_Config.bgAlpha > 255) g_Config.bgAlpha = 255;

    int colorPreset = GetPrivateProfileIntW(L"ShowFPS", L"TextColorPreset", 0, ini.c_str());
    if (colorPreset >= 0 && colorPreset <= 5) {
        g_Config.textColorPreset = static_cast<TextColorPreset>(colorPreset);
    } else {
        g_Config.textColorPreset = TextColorPreset::White;
    }

    g_Config.dynamicColor = GetPrivateProfileIntW(L"ShowFPS", L"DynamicColor", 0, ini.c_str()) != 0;
}

void SaveConfig(const std::wstring& modDir) {
    std::wstring ini = GetIniPath(modDir);

    wchar_t buf[32];
    
    std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%d", g_Config.enabled ? 1 : 0);
    WritePrivateProfileStringW(L"ShowFPS", L"Enabled", buf, ini.c_str());

    std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%d", g_Config.showGpuUsage ? 1 : 0);
    WritePrivateProfileStringW(L"ShowFPS", L"ShowGpuUsage", buf, ini.c_str());

    std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%d", g_Config.showFrameTime ? 1 : 0);
    WritePrivateProfileStringW(L"ShowFPS", L"ShowFrameTime", buf, ini.c_str());

    std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%d", g_Config.showBackground ? 1 : 0);
    WritePrivateProfileStringW(L"ShowFPS", L"ShowBackground", buf, ini.c_str());

    std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%d", static_cast<int>(g_Config.position));
    WritePrivateProfileStringW(L"ShowFPS", L"Position", buf, ini.c_str());

    std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%d", g_Config.detailed ? 1 : 0);
    WritePrivateProfileStringW(L"ShowFPS", L"Detailed", buf, ini.c_str());

    std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%d", g_Config.fontSize);
    WritePrivateProfileStringW(L"ShowFPS", L"FontSize", buf, ini.c_str());

    std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%d", g_Config.marginX);
    WritePrivateProfileStringW(L"ShowFPS", L"MarginX", buf, ini.c_str());

    std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%d", g_Config.marginY);
    WritePrivateProfileStringW(L"ShowFPS", L"MarginY", buf, ini.c_str());

    std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%d", static_cast<int>(g_Config.textColorPreset));
    WritePrivateProfileStringW(L"ShowFPS", L"TextColorPreset", buf, ini.c_str());

    std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%d", g_Config.dynamicColor ? 1 : 0);
    WritePrivateProfileStringW(L"ShowFPS", L"DynamicColor", buf, ini.c_str());

    std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%d", g_Config.updateIntervalMs);
    WritePrivateProfileStringW(L"ShowFPS", L"UpdateIntervalMs", buf, ini.c_str());

    std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%d", g_Config.toggleHotkey);
    WritePrivateProfileStringW(L"ShowFPS", L"ToggleHotkey", buf, ini.c_str());

    std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%d", g_Config.bgAlpha);
    WritePrivateProfileStringW(L"ShowFPS", L"BgAlpha", buf, ini.c_str());
}
