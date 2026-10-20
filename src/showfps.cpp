#include "fh2mod.h"
#include "config.h"
#include "fps_counter.h"
#include "gpu_monitor.h"
#include "overlay.h"
#include "dxgi_hook.h"

#include <windows.h>
#include <cstdio>
#include <string>

namespace {

HINSTANCE g_hInst = nullptr;
const FH2ModApi* g_api = nullptr;
FH2Mod* g_mod = nullptr;
std::wstring g_modDir;

static std::wstring GetModuleDir() {
    wchar_t path[MAX_PATH] = {0};
    if (g_hInst && GetModuleFileNameW(g_hInst, path, MAX_PATH)) {
        wchar_t* lastSlash = wcsrchr(path, L'\\');
        if (lastSlash) {
            *lastSlash = L'\0';
            return std::wstring(path);
        }
    }
    if (g_api && g_api->azimuth_dir) {
        return std::wstring(g_api->azimuth_dir) + L"\\fh2-mods";
    }
    return L"fh2-mods";
}

void OnSettingChange_Enabled(void* user, const char* key, int value) {
    (void)user; (void)key;
    g_Config.enabled = (value != 0);
    SaveConfig(g_modDir);
    g_Overlay.RequestRedraw();
}

void OnSettingChange_GpuUsage(void* user, const char* key, int value) {
    (void)user; (void)key;
    g_Config.showGpuUsage = (value != 0);
    SaveConfig(g_modDir);
    g_Overlay.RequestRedraw();
}

void OnSettingChange_FrameTime(void* user, const char* key, int value) {
    (void)user; (void)key;
    g_Config.showFrameTime = (value != 0);
    SaveConfig(g_modDir);
    g_Overlay.RequestRedraw();
}

void OnSettingChange_Background(void* user, const char* key, int value) {
    (void)user; (void)key;
    g_Config.showBackground = (value != 0);
    SaveConfig(g_modDir);
    g_Overlay.RequestRedraw();
}

void OnSettingChange_Position(void* user, const char* key, int value) {
    (void)user; (void)key;
    g_Config.position = (value != 0) ? OverlayCorner::TopRight : OverlayCorner::TopLeft;
    SaveConfig(g_modDir);
    g_Overlay.RequestRedraw();
}

void OnSettingChange_Detailed(void* user, const char* key, int value) {
    (void)user; (void)key;
    g_Config.detailed = (value != 0);
    SaveConfig(g_modDir);
    g_Overlay.RequestRedraw();
}

void OnSettingChange_DynamicColor(void* user, const char* key, int value) {
    (void)user; (void)key;
    g_Config.dynamicColor = (value != 0);
    SaveConfig(g_modDir);
    g_Overlay.RequestRedraw();
}

void OnSettingSlide_FontSize(void* user, const char* key, float value) {
    (void)user; (void)key;
    int sz = static_cast<int>(value + 0.5f);
    if (sz < 10) sz = 10;
    if (sz > 48) sz = 48;
    g_Config.fontSize = sz;
    SaveConfig(g_modDir);
    g_Overlay.RequestRedraw();
}

void OnSettingSlide_TextColor(void* user, const char* key, float value) {
    (void)user; (void)key;
    int p = static_cast<int>(value + 0.5f);
    if (p < 0) p = 0;
    if (p > 5) p = 5;
    g_Config.textColorPreset = static_cast<TextColorPreset>(p);
    SaveConfig(g_modDir);
    g_Overlay.RequestRedraw();
}

DWORD WINAPI WorkerThread(LPVOID) {
    // 1. Initialize FPS counter
    g_FpsCounter.Init();

    // 2. Initialize GPU utilization monitor
    g_GpuMonitor.Start();

    // 3. Start transparent overlay window
    if (!g_Overlay.Start(g_hInst)) {
        g_api->log(g_mod, "Failed to start overlay window");
    } else {
        g_api->log(g_mod, "Overlay window started successfully");
    }

    // 4. Hook DXGI SwapChain Present for exact frame timing
    if (!InstallDxgiHooks(g_api, g_mod)) {
        g_api->log(g_mod, "Failed to install DXGI hooks (fallback timer active)");
    }

    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hInst = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
    }
    return TRUE;
}

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    if (!api || api->api_version < 1) return 1;

    g_api = api;
    g_modDir = GetModuleDir();

    FH2ModInfo info{};
    info.size             = sizeof(info);
    info.name             = "ShowFPS";
    info.version          = SHOWFPS_VERSION;
    info.github_repo      = "qiangqiang101/ShowFPS";
    info.platform_version = FH2MOD_PLATFORM_VERSION;
    info.description      = "Displaying FPS counter and GPU utilization in game.";
    info.check_updates    = 0;

    g_mod = api->register_mod(&info);
    if (!g_mod) return 1;

    api->log(g_mod, "ShowFPS " SHOWFPS_VERSION " starting...");

    // Load settings from ShowFPS.ini located next to ShowFPS.dll
    LoadConfig(g_modDir);

    // Register settings in Pause > Settings > MODS if supported
    if (api->api_version >= 4) {
        FH2Setting sEnabled{};
        sEnabled.size        = sizeof(sEnabled);
        sEnabled.key         = "ShowFPS_Enabled";
        sEnabled.label       = "Show FPS Counter";
        sEnabled.description = "Displays real-time FPS and GPU usage overlay. Toggle with F11.";
        sEnabled.value       = g_Config.enabled ? 1 : 0;
        sEnabled.on_change   = OnSettingChange_Enabled;
        api->add_setting(g_mod, &sEnabled);

        FH2Setting sGpu{};
        sGpu.size        = sizeof(sGpu);
        sGpu.key         = "ShowFPS_GPU";
        sGpu.label       = "Show GPU Usage (%)";
        sGpu.description = "Displays GPU load percentage in the badge (e.g. 60 GPU:84%).";
        sGpu.value       = g_Config.showGpuUsage ? 1 : 0;
        sGpu.on_change   = OnSettingChange_GpuUsage;
        api->add_setting(g_mod, &sGpu);

        FH2Setting sFrameTime{};
        sFrameTime.size        = sizeof(sFrameTime);
        sFrameTime.key         = "ShowFPS_FrameTime";
        sFrameTime.label       = "Show Frame Time (ms)";
        sFrameTime.description = "Shows frametime in milliseconds next to FPS.";
        sFrameTime.value       = g_Config.showFrameTime ? 1 : 0;
        sFrameTime.on_change   = OnSettingChange_FrameTime;
        api->add_setting(g_mod, &sFrameTime);

        FH2Setting sBg{};
        sBg.size        = sizeof(sBg);
        sBg.key         = "ShowFPS_Background";
        sBg.label       = "Show Background Box";
        sBg.description = "Show translucent background pill (On) or text with outline only (Off).";
        sBg.value       = g_Config.showBackground ? 1 : 0;
        sBg.on_change   = OnSettingChange_Background;
        api->add_setting(g_mod, &sBg);

        FH2Setting sPos{};
        sPos.size        = sizeof(sPos);
        sPos.key         = "ShowFPS_Position";
        sPos.label       = "Corner: Top-Right";
        sPos.description = "Show FPS in top-right corner (On) or top-left corner (Off).";
        sPos.value       = (g_Config.position == OverlayCorner::TopRight) ? 1 : 0;
        sPos.on_change   = OnSettingChange_Position;
        api->add_setting(g_mod, &sPos);

        FH2Setting sDynColor{};
        sDynColor.size        = sizeof(sDynColor);
        sDynColor.key         = "ShowFPS_DynamicColor";
        sDynColor.label       = "Dynamic Color by FPS";
        sDynColor.description = "Changes text color based on framerate (Green >= 50, Yellow 30-49, Red < 30).";
        sDynColor.value       = g_Config.dynamicColor ? 1 : 0;
        sDynColor.on_change   = OnSettingChange_DynamicColor;
        api->add_setting(g_mod, &sDynColor);

        FH2Setting sDetailed{};
        sDetailed.size        = sizeof(sDetailed);
        sDetailed.key         = "ShowFPS_Detailed";
        sDetailed.label       = "Detailed Decimal FPS";
        sDetailed.description = "Shows decimal precision (e.g. 59.8 FPS) instead of rounded integers.";
        sDetailed.value       = g_Config.detailed ? 1 : 0;
        sDetailed.on_change   = OnSettingChange_Detailed;
        api->add_setting(g_mod, &sDetailed);

        // Sliders (API 5+)
        if (api->api_version >= 5) {
            FH2Setting sFontSize{};
            sFontSize.size         = sizeof(sFontSize);
            sFontSize.key          = "ShowFPS_FontSize";
            sFontSize.label        = "Font Size";
            sFontSize.description  = "Adjusts the font size of the overlay (12 to 32 pixels).";
            sFontSize.kind         = FH2_SETTING_SLIDER;
            sFontSize.min          = 12.0f;
            sFontSize.max          = 32.0f;
            sFontSize.step         = 1.0f;
            sFontSize.slider_value = static_cast<float>(g_Config.fontSize);
            sFontSize.decimals     = 0;
            sFontSize.on_slide     = OnSettingSlide_FontSize;
            api->add_setting(g_mod, &sFontSize);

            FH2Setting sColor{};
            sColor.size         = sizeof(sColor);
            sColor.key          = "ShowFPS_TextColor";
            sColor.label        = "Text Color (0-4)";
            sColor.description  = "Preset: 0=White, 1=Green, 2=Yellow, 3=Cyan, 4=Red.";
            sColor.kind         = FH2_SETTING_SLIDER;
            sColor.min          = 0.0f;
            sColor.max          = 4.0f;
            sColor.step         = 1.0f;
            sColor.slider_value = static_cast<float>(static_cast<int>(g_Config.textColorPreset));
            sColor.decimals     = 0;
            sColor.on_slide     = OnSettingSlide_TextColor;
            api->add_setting(g_mod, &sColor);
        }
    }

    // Launch background worker thread for DXGI hooking, GPU monitoring and overlay loop
    HANDLE thread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    }

    api->log(g_mod, "ShowFPS initialized");
    return 0;
}
