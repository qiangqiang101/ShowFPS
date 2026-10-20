#include "dxgi_hook.h"
#include "fps_counter.h"
#include "overlay.h"

#include <d3d11.h>
#include <cstdio>

namespace {

typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain* This, UINT SyncInterval, UINT Flags);
typedef HRESULT (STDMETHODCALLTYPE *Present1Fn)(IDXGISwapChain1* This, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);

PresentFn g_OrigPresent = nullptr;
Present1Fn g_OrigPresent1 = nullptr;

FH2Hook* g_HookPresent = nullptr;
FH2Hook* g_HookPresent1 = nullptr;

HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags) {
    if (This) {
        g_FpsCounter.OnFrame();

        DXGI_SWAP_CHAIN_DESC desc{};
        if (SUCCEEDED(This->GetDesc(&desc)) && desc.OutputWindow) {
            g_Overlay.SetGameWindow(desc.OutputWindow);
        }
    }
    return g_OrigPresent ? g_OrigPresent(This, SyncInterval, Flags) : S_OK;
}

HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain1* This, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    if (This) {
        g_FpsCounter.OnFrame();

        DXGI_SWAP_CHAIN_DESC desc{};
        if (SUCCEEDED(This->GetDesc(&desc)) && desc.OutputWindow) {
            g_Overlay.SetGameWindow(desc.OutputWindow);
        }
    }
    return g_OrigPresent1 ? g_OrigPresent1(This, SyncInterval, PresentFlags, pPresentParameters) : S_OK;
}

} // namespace

bool InstallDxgiHooks(const FH2ModApi* api, FH2Mod* mod) {
    if (!api || !mod || api->api_version < 3) {
        return false;
    }

    // 1. Create a hidden dummy window to instantiate a dummy swapchain
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ShowFPS_DummyDxgiClass";
    RegisterClassExW(&wc);

    HWND dummyHwnd = CreateWindowExW(
        0, wc.lpszClassName, L"",
        WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    if (!dummyHwnd) {
        api->log(mod, "Failed to create dummy window for DXGI hook");
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.Width = 100;
    scd.BufferDesc.Height = 100;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = dummyHwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;

    ID3D11Device* dummyDevice = nullptr;
    ID3D11DeviceContext* dummyContext = nullptr;
    IDXGISwapChain* dummySwapChain = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &scd,
        &dummySwapChain,
        &dummyDevice,
        &featureLevel,
        &dummyContext
    );

    if (FAILED(hr)) {
        // Fallback to WARP software rasterizer if hardware is unavailable
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &scd,
            &dummySwapChain,
            &dummyDevice,
            &featureLevel,
            &dummyContext
        );
    }

    if (FAILED(hr) || !dummySwapChain) {
        api->log(mod, "Failed to create dummy D3D11 device and swap chain");
        DestroyWindow(dummyHwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    // 2. Extract vtable addresses
    void** vtable = *reinterpret_cast<void***>(dummySwapChain);
    void* presentAddr = vtable[8]; // IDXGISwapChain::Present is index 8

    void* present1Addr = nullptr;
    IDXGISwapChain1* dummySwapChain1 = nullptr;
    if (SUCCEEDED(dummySwapChain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&dummySwapChain1)))) {
        void** vtable1 = *reinterpret_cast<void***>(dummySwapChain1);
        present1Addr = vtable1[22]; // IDXGISwapChain1::Present1 is index 22
        dummySwapChain1->Release();
    }

    // Clean up dummy COM objects and window
    dummySwapChain->Release();
    if (dummyContext) dummyContext->Release();
    if (dummyDevice) dummyDevice->Release();
    DestroyWindow(dummyHwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    // 3. Hook through platform MinHook
    char logMsg[128];
    std::snprintf(logMsg, sizeof(logMsg), "Hooking IDXGISwapChain::Present at %p", presentAddr);
    api->log(mod, logMsg);

    g_HookPresent = api->hook_function(
        mod,
        "IDXGISwapChain::Present",
        presentAddr,
        reinterpret_cast<void*>(&DetourPresent),
        reinterpret_cast<void**>(&g_OrigPresent)
    );

    if (!g_HookPresent) {
        api->log(mod, "Failed to hook IDXGISwapChain::Present");
        return false;
    }

    if (present1Addr) {
        std::snprintf(logMsg, sizeof(logMsg), "Hooking IDXGISwapChain1::Present1 at %p", present1Addr);
        api->log(mod, logMsg);

        g_HookPresent1 = api->hook_function(
            mod,
            "IDXGISwapChain1::Present1",
            present1Addr,
            reinterpret_cast<void*>(&DetourPresent1),
            reinterpret_cast<void**>(&g_OrigPresent1)
        );
    }

    int hookRes = api->enable_hooks(mod, nullptr);
    if (!hookRes) {
        api->log(mod, "enable_hooks returned 0");
        return false;
    }

    api->log(mod, "DXGI Present hooks enabled successfully");
    return true;
}

void UninstallDxgiHooks(const FH2ModApi* api, FH2Mod* mod) {
    if (api && mod && api->api_version >= 3) {
        api->disable_hooks(mod, nullptr);
    }
}
