#include "dxgi_hook.h"
#include "fps_counter.h"
#include "overlay.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <vector>
#include <mutex>
#include <atomic>

namespace {

const FH2ModApi* g_api = nullptr;
FH2Mod* g_mod = nullptr;

typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain* This, UINT SyncInterval, UINT Flags);
typedef HRESULT (STDMETHODCALLTYPE *Present1Fn)(IDXGISwapChain1* This, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);
typedef HRESULT (STDMETHODCALLTYPE *CreateSwapChainFn)(IDXGIFactory* This, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain);
typedef HRESULT (STDMETHODCALLTYPE *CreateSwapChainForHwndFn)(IDXGIFactory2* This, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain);
typedef HRESULT (STDMETHODCALLTYPE *CreateSwapChainForCompositionFn)(IDXGIFactory2* This, IUnknown* pDevice, const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain);

PresentFn g_OrigPresent = nullptr;
Present1Fn g_OrigPresent1 = nullptr;
CreateSwapChainFn g_OrigCreateSwapChain = nullptr;
CreateSwapChainForHwndFn g_OrigCreateSwapChainForHwnd = nullptr;
CreateSwapChainForCompositionFn g_OrigCreateSwapChainForComposition = nullptr;

FH2Hook* g_HookPresent = nullptr;
FH2Hook* g_HookPresent1 = nullptr;
FH2Hook* g_HookCreateSwapChain = nullptr;
FH2Hook* g_HookCreateSwapChainForHwnd = nullptr;
FH2Hook* g_HookCreateSwapChainForComposition = nullptr;

// VMT Hook storage
void* g_ClonedVtable[64] = {nullptr};
PresentFn g_VmtOrigPresent = nullptr;
Present1Fn g_VmtOrigPresent1 = nullptr;
std::mutex g_VmtMutex;

std::atomic<uint64_t> g_PresentCalls{0};
std::atomic<uint64_t> g_Present1Calls{0};

void OnFramePresented(IUnknown* sc, UINT flags) {
    if ((flags & DXGI_PRESENT_TEST) == 0) {
        g_FpsCounter.OnFrame();
    }

    if (sc) {
        IDXGISwapChain* swapChain = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain), reinterpret_cast<void**>(&swapChain))) && swapChain) {
            DXGI_SWAP_CHAIN_DESC desc{};
            if (SUCCEEDED(swapChain->GetDesc(&desc)) && desc.OutputWindow) {
                g_Overlay.SetGameWindow(desc.OutputWindow);
            }
            swapChain->Release();
        }
    }
}

void LogFrameMilestone(const char* source, uint64_t count, UINT flags) {
    if (!g_api || !g_mod) return;
    if (count == 1 || count == 10 || count == 60 || count == 300 || (count % 1000 == 0)) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "%s call #%llu (Flags=0x%X)", source, count, flags);
        g_api->log(g_mod, msg);
    }
}

// Detours for inline hooks
HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags) {
    thread_local bool s_inPresent = false;
    if (!s_inPresent) {
        s_inPresent = true;
        uint64_t c = ++g_PresentCalls;
        LogFrameMilestone("Present", c, Flags);
        OnFramePresented(This, Flags);
        s_inPresent = false;
    }
    return g_OrigPresent ? g_OrigPresent(This, SyncInterval, Flags) : S_OK;
}

HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain1* This, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    thread_local bool s_inPresent = false;
    if (!s_inPresent) {
        s_inPresent = true;
        uint64_t c = ++g_Present1Calls;
        LogFrameMilestone("Present1", c, PresentFlags);
        OnFramePresented(This, PresentFlags);
        s_inPresent = false;
    }
    return g_OrigPresent1 ? g_OrigPresent1(This, SyncInterval, PresentFlags, pPresentParameters) : S_OK;
}

// Detours for VMT hooks (intercepts direct calls on the swap chain object)
HRESULT STDMETHODCALLTYPE DetourVmtPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags) {
    thread_local bool s_inPresent = false;
    if (!s_inPresent) {
        s_inPresent = true;
        uint64_t c = ++g_PresentCalls;
        LogFrameMilestone("VmtPresent", c, Flags);
        OnFramePresented(This, Flags);
        s_inPresent = false;
    }
    return g_VmtOrigPresent ? g_VmtOrigPresent(This, SyncInterval, Flags) : S_OK;
}

HRESULT STDMETHODCALLTYPE DetourVmtPresent1(IDXGISwapChain1* This, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    thread_local bool s_inPresent = false;
    if (!s_inPresent) {
        s_inPresent = true;
        uint64_t c = ++g_Present1Calls;
        LogFrameMilestone("VmtPresent1", c, PresentFlags);
        OnFramePresented(This, PresentFlags);
        s_inPresent = false;
    }
    return g_VmtOrigPresent1 ? g_VmtOrigPresent1(This, SyncInterval, PresentFlags, pPresentParameters) : S_OK;
}

void HookSwapChainInstance(IUnknown* sc, HWND hwnd) {
    if (!sc) return;

    if (hwnd && IsWindow(hwnd)) {
        g_Overlay.SetGameWindow(hwnd);
    }

    std::lock_guard<std::mutex> lock(g_VmtMutex);
    void** origVtbl = *reinterpret_cast<void***>(sc);
    if (!origVtbl || origVtbl == g_ClonedVtable) {
        return; // Already hooked
    }

    // Clone the vtable (copy up to 64 pointers)
    std::memcpy(g_ClonedVtable, origVtbl, sizeof(void*) * 64);
    g_VmtOrigPresent = reinterpret_cast<PresentFn>(origVtbl[8]);
    g_VmtOrigPresent1 = reinterpret_cast<Present1Fn>(origVtbl[22]);

    g_ClonedVtable[8] = reinterpret_cast<void*>(&DetourVmtPresent);
    g_ClonedVtable[22] = reinterpret_cast<void*>(&DetourVmtPresent1);

    // Replace the vtable on the instance
    *reinterpret_cast<void***>(sc) = g_ClonedVtable;

    if (g_api && g_mod) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "Hooked swapchain instance %p via VMT clone", static_cast<void*>(sc));
        g_api->log(g_mod, msg);
    }
}

// Detours for IDXGIFactory swap chain creation
HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(
    IDXGIFactory* This,
    IUnknown* pDevice,
    DXGI_SWAP_CHAIN_DESC* pDesc,
    IDXGISwapChain** ppSwapChain
) {
    HRESULT hr = g_OrigCreateSwapChain ? g_OrigCreateSwapChain(This, pDevice, pDesc, ppSwapChain) : E_FAIL;
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        HWND hwnd = pDesc ? pDesc->OutputWindow : nullptr;
        HookSwapChainInstance(*ppSwapChain, hwnd);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(
    IDXGIFactory2* This,
    IUnknown* pDevice,
    HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain
) {
    if (hWnd && IsWindow(hWnd)) {
        g_Overlay.SetGameWindow(hWnd);
    }

    HRESULT hr = g_OrigCreateSwapChainForHwnd ?
        g_OrigCreateSwapChainForHwnd(This, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain) : E_FAIL;

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        HookSwapChainInstance(*ppSwapChain, hWnd);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForComposition(
    IDXGIFactory2* This,
    IUnknown* pDevice,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain
) {
    HRESULT hr = g_OrigCreateSwapChainForComposition ?
        g_OrigCreateSwapChainForComposition(This, pDevice, pDesc, pRestrictToOutput, ppSwapChain) : E_FAIL;

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        HookSwapChainInstance(*ppSwapChain, nullptr);
    }
    return hr;
}

} // namespace

bool InstallDxgiHooks(const FH2ModApi* api, FH2Mod* mod) {
    if (!api || !mod || api->api_version < 3) {
        return false;
    }
    g_api = api;
    g_mod = mod;

    // 1. Hook Factory CreateSwapChain methods to intercept swapchains when created
    IDXGIFactory2* factory2 = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory2))) && factory2) {
        void** factoryVtbl = *reinterpret_cast<void***>(factory2);
        void* createScAddr = factoryVtbl[10];           // IDXGIFactory::CreateSwapChain (index 10)
        void* createScForHwndAddr = factoryVtbl[15];    // IDXGIFactory2::CreateSwapChainForHwnd (index 15)
        void* createScForCompAddr = factoryVtbl[24];    // IDXGIFactory2::CreateSwapChainForComposition (index 24)

        if (createScAddr) {
            char logMsg[128];
            std::snprintf(logMsg, sizeof(logMsg), "Hooking CreateSwapChain at %p", createScAddr);
            api->log(mod, logMsg);
            g_HookCreateSwapChain = api->hook_function(
                mod, "IDXGIFactory::CreateSwapChain",
                createScAddr, reinterpret_cast<void*>(&DetourCreateSwapChain),
                reinterpret_cast<void**>(&g_OrigCreateSwapChain)
            );
        }

        if (createScForHwndAddr) {
            char logMsg[128];
            std::snprintf(logMsg, sizeof(logMsg), "Hooking CreateSwapChainForHwnd at %p", createScForHwndAddr);
            api->log(mod, logMsg);
            g_HookCreateSwapChainForHwnd = api->hook_function(
                mod, "IDXGIFactory2::CreateSwapChainForHwnd",
                createScForHwndAddr, reinterpret_cast<void*>(&DetourCreateSwapChainForHwnd),
                reinterpret_cast<void**>(&g_OrigCreateSwapChainForHwnd)
            );
        }

        if (createScForCompAddr) {
            g_HookCreateSwapChainForComposition = api->hook_function(
                mod, "IDXGIFactory2::CreateSwapChainForComposition",
                createScForCompAddr, reinterpret_cast<void*>(&DetourCreateSwapChainForComposition),
                reinterpret_cast<void**>(&g_OrigCreateSwapChainForComposition)
            );
        }

        factory2->Release();
    }

    // 2. Create a hidden dummy window to instantiate a dummy swapchain and hook Present / Present1
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

    // 3. Extract vtable addresses
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

    // 4. Hook through platform MinHook
    if (presentAddr) {
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
    }

    if (present1Addr) {
        char logMsg[128];
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

    api->log(mod, "DXGI Present and Factory hooks enabled successfully");
    return true;
}

void DxgiHookDiagnose(const FH2ModApi* api, FH2Mod* mod, bool retry) {
    if (!api || !mod) return;
    char msg[256];
    std::snprintf(msg, sizeof(msg),
        "[bugtrap] hooks: Present=%p Present1=%p CreateSC=%p CreateSCHwnd=%p CreateSCComp=%p; "
        "orig: Present=%p Present1=%p; vmt: Present=%p Present1=%p; calls: Present=%llu Present1=%llu",
        static_cast<void*>(g_HookPresent), static_cast<void*>(g_HookPresent1),
        static_cast<void*>(g_HookCreateSwapChain), static_cast<void*>(g_HookCreateSwapChainForHwnd),
        static_cast<void*>(g_HookCreateSwapChainForComposition),
        reinterpret_cast<void*>(g_OrigPresent), reinterpret_cast<void*>(g_OrigPresent1),
        reinterpret_cast<void*>(g_VmtOrigPresent), reinterpret_cast<void*>(g_VmtOrigPresent1),
        static_cast<unsigned long long>(g_PresentCalls.load()),
        static_cast<unsigned long long>(g_Present1Calls.load()));
    api->log(mod, msg);

    if (retry && api->api_version >= 3) {
        int res = api->enable_hooks(mod, nullptr);
        std::snprintf(msg, sizeof(msg), "[bugtrap] re-enabled hooks, enable_hooks returned %d", res);
        api->log(mod, msg);
    }
}

void UninstallDxgiHooks(const FH2ModApi* api, FH2Mod* mod) {
    if (api && mod && api->api_version >= 3) {
        api->disable_hooks(mod, nullptr);
    }
}
