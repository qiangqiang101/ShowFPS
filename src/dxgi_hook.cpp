#include "dxgi_hook.h"
#include "fps_counter.h"
#include "overlay.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cwchar>
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

// Vtable-slot hooks: another overlay (RTSS, Steam, ...) can patch over the
// inline Present hook after the first frame, but calls through the swap
// chain's vtable still reach these. One entry per distinct swap chain vtable.
struct VtableHook {
    void** vtbl = nullptr;
    PresentFn origPresent = nullptr;
    Present1Fn origPresent1 = nullptr;
    FH2Hook* hookPresent = nullptr;
    FH2Hook* hookPresent1 = nullptr;
};
constexpr int kMaxVtables = 8;
VtableHook g_Vtables[kMaxVtables];
std::atomic<int> g_VtableCount{0};
std::mutex g_VtableMutex;

// Vtable of a swap chain seen in Present or at creation, for the worker thread
// to hook (hooking pauses the game's threads, so it isn't done from Present)
std::atomic<void**> g_PendingVtbl{nullptr};
std::atomic<bool> g_PendingHasPresent1{false};

std::atomic<uint64_t> g_PresentCalls{0};
std::atomic<uint64_t> g_Present1Calls{0};
std::atomic<uint64_t> g_VmtCalls{0};

// Present calls nest when several hooks are in the chain (vtable -> inline ->
// other overlays); only the outermost one counts as a frame.
thread_local int t_presentDepth = 0;

struct PresentScope {
    bool outermost;
    PresentScope() : outermost(t_presentDepth++ == 0) {}
    ~PresentScope() { --t_presentDepth; }
};

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

const VtableHook* FindVtableHook(void** vtbl) {
    const int n = g_VtableCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        if (g_Vtables[i].vtbl == vtbl) return &g_Vtables[i];
    }
    return nullptr;
}

void NoteSwapChain(IUnknown* sc, bool knownPresent1) {
    if (!sc) return;
    void** vtbl = *reinterpret_cast<void***>(sc);
    if (!vtbl || FindVtableHook(vtbl) || g_PendingVtbl.load(std::memory_order_relaxed) == vtbl) return;

    bool hasPresent1 = knownPresent1;
    if (!hasPresent1) {
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1))) && sc1) {
            hasPresent1 = (*reinterpret_cast<void***>(sc1) == vtbl);
            sc1->Release();
        }
    }
    g_PendingHasPresent1.store(hasPresent1, std::memory_order_relaxed);
    g_PendingVtbl.store(vtbl, std::memory_order_release);
}

// Detours for inline hooks
HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags) {
    PresentScope scope;
    if (scope.outermost) {
        uint64_t c = ++g_PresentCalls;
        LogFrameMilestone("Present", c, Flags);
        OnFramePresented(This, Flags);
        NoteSwapChain(This, false);
    }
    return g_OrigPresent ? g_OrigPresent(This, SyncInterval, Flags) : S_OK;
}

HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain1* This, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    PresentScope scope;
    if (scope.outermost) {
        uint64_t c = ++g_Present1Calls;
        LogFrameMilestone("Present1", c, PresentFlags);
        OnFramePresented(This, PresentFlags);
        NoteSwapChain(This, true);
    }
    return g_OrigPresent1 ? g_OrigPresent1(This, SyncInterval, PresentFlags, pPresentParameters) : S_OK;
}

// Detours for vtable-slot hooks
HRESULT STDMETHODCALLTYPE DetourVmtPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags) {
    PresentScope scope;
    if (scope.outermost) {
        uint64_t c = ++g_VmtCalls;
        LogFrameMilestone("VmtPresent", c, Flags);
        OnFramePresented(This, Flags);
    }
    const VtableHook* h = FindVtableHook(*reinterpret_cast<void***>(This));
    if (!h) h = &g_Vtables[0];
    return h->origPresent ? h->origPresent(This, SyncInterval, Flags) : S_OK;
}

HRESULT STDMETHODCALLTYPE DetourVmtPresent1(IDXGISwapChain1* This, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    PresentScope scope;
    if (scope.outermost) {
        uint64_t c = ++g_VmtCalls;
        LogFrameMilestone("VmtPresent1", c, PresentFlags);
        OnFramePresented(This, PresentFlags);
    }
    const VtableHook* h = FindVtableHook(*reinterpret_cast<void***>(This));
    if (!h) h = &g_Vtables[0];
    return h->origPresent1 ? h->origPresent1(This, SyncInterval, PresentFlags, pPresentParameters) : S_OK;
}

// Hooks Present (and Present1) in the slots of `vtbl`. Only vtables inside a
// loaded module: a heap copy made by another overlay could be freed later.
void HookVtable(const FH2ModApi* api, FH2Mod* mod, void** vtbl, bool hasPresent1) {
    if (!vtbl) return;

    std::lock_guard<std::mutex> lock(g_VtableMutex);
    if (FindVtableHook(vtbl)) return;
    const int n = g_VtableCount.load(std::memory_order_relaxed);
    if (n >= kMaxVtables) return;

    char msg[256];
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(vtbl), &owner) || !owner) {
        std::snprintf(msg, sizeof(msg), "swap chain vtable %p isn't in a module, not hooking it", static_cast<void*>(vtbl));
        api->log(mod, msg);
        return;
    }
    wchar_t ownerPath[MAX_PATH] = L"?";
    GetModuleFileNameW(owner, ownerPath, MAX_PATH);
    const wchar_t* ownerName = wcsrchr(ownerPath, L'\\');
    ownerName = ownerName ? ownerName + 1 : ownerPath;

    VtableHook& h = g_Vtables[n];
    h.vtbl = vtbl;
    h.hookPresent = api->hook_pointer(mod, "IDXGISwapChain vtable: Present", &vtbl[8],
                                      reinterpret_cast<void*>(&DetourVmtPresent),
                                      reinterpret_cast<void**>(&h.origPresent));
    if (hasPresent1) {
        h.hookPresent1 = api->hook_pointer(mod, "IDXGISwapChain1 vtable: Present1", &vtbl[22],
                                           reinterpret_cast<void*>(&DetourVmtPresent1),
                                           reinterpret_cast<void**>(&h.origPresent1));
    }
    // Publish before enabling, so the detours can find their originals
    g_VtableCount.store(n + 1, std::memory_order_release);

    const bool presentOn = h.hookPresent && api->enable_hooks(mod, h.hookPresent) == 1;
    const bool present1On = h.hookPresent1 && api->enable_hooks(mod, h.hookPresent1) == 1;

    std::snprintf(msg, sizeof(msg), "swap chain vtable %p (%ls): Present slot %s, Present1 slot %s",
                  static_cast<void*>(vtbl), ownerName,
                  presentOn ? "hooked" : "not hooked",
                  !hasPresent1 ? "n/a" : (present1On ? "hooked" : "not hooked"));
    api->log(mod, msg);
}

void HookPendingVtable(const FH2ModApi* api, FH2Mod* mod) {
    void** vtbl = g_PendingVtbl.load(std::memory_order_acquire);
    if (!vtbl || FindVtableHook(vtbl)) return;
    HookVtable(api, mod, vtbl, g_PendingHasPresent1.load(std::memory_order_relaxed));
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
        if (hwnd && IsWindow(hwnd)) {
            g_Overlay.SetGameWindow(hwnd);
        }
        NoteSwapChain(*ppSwapChain, false);
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
        NoteSwapChain(*ppSwapChain, true);
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
        NoteSwapChain(*ppSwapChain, true);
    }
    return hr;
}

// Makes an inline hook and switches it on straight away, so there's little
// time for another overlay to patch the same code in between. A refusal is
// logged by the platform and doesn't stop the other hooks.
FH2Hook* HookAndEnable(const FH2ModApi* api, FH2Mod* mod, const char* name, void* target, void* detour, void** original) {
    if (!target) return nullptr;
    char msg[128];
    std::snprintf(msg, sizeof(msg), "Hooking %s at %p", name, target);
    api->log(mod, msg);
    FH2Hook* hook = api->hook_function(mod, name, target, detour, original);
    if (hook && api->enable_hooks(mod, hook) != 1) {
        std::snprintf(msg, sizeof(msg), "%s: hook made but couldn't be switched on", name);
        api->log(mod, msg);
    }
    return hook;
}

} // namespace

bool InstallDxgiHooks(const FH2ModApi* api, FH2Mod* mod) {
    if (!api || !mod || api->api_version < 3) {
        return false;
    }
    g_api = api;
    g_mod = mod;

    // 1. Create a dummy device and swap chain first: overlays that hook DXGI
    //    lazily do it when a device is made, and we'd rather hook after them
    //    than have them patch over our hooks.
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
    bool vtableHasPresent1 = false;
    IDXGISwapChain1* dummySwapChain1 = nullptr;
    if (SUCCEEDED(dummySwapChain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&dummySwapChain1)))) {
        void** vtable1 = *reinterpret_cast<void***>(dummySwapChain1);
        present1Addr = vtable1[22]; // IDXGISwapChain1::Present1 is index 22
        vtableHasPresent1 = (vtable1 == vtable);
        dummySwapChain1->Release();
    }

    // Clean up dummy COM objects and window (the vtable lives on in dxgi.dll)
    dummySwapChain->Release();
    if (dummyContext) dummyContext->Release();
    if (dummyDevice) dummyDevice->Release();
    DestroyWindow(dummyHwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    // 3. Hook Factory CreateSwapChain methods to intercept swapchains when created
    IDXGIFactory2* factory2 = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory2))) && factory2) {
        void** factoryVtbl = *reinterpret_cast<void***>(factory2);
        g_HookCreateSwapChain = HookAndEnable(api, mod, "IDXGIFactory::CreateSwapChain",
            factoryVtbl[10], reinterpret_cast<void*>(&DetourCreateSwapChain),
            reinterpret_cast<void**>(&g_OrigCreateSwapChain));
        g_HookCreateSwapChainForHwnd = HookAndEnable(api, mod, "IDXGIFactory2::CreateSwapChainForHwnd",
            factoryVtbl[15], reinterpret_cast<void*>(&DetourCreateSwapChainForHwnd),
            reinterpret_cast<void**>(&g_OrigCreateSwapChainForHwnd));
        g_HookCreateSwapChainForComposition = HookAndEnable(api, mod, "IDXGIFactory2::CreateSwapChainForComposition",
            factoryVtbl[24], reinterpret_cast<void*>(&DetourCreateSwapChainForComposition),
            reinterpret_cast<void**>(&g_OrigCreateSwapChainForComposition));
        factory2->Release();
    }

    // 4. Inline hooks on Present / Present1 (catch every swap chain class)
    g_HookPresent = HookAndEnable(api, mod, "IDXGISwapChain::Present",
        presentAddr, reinterpret_cast<void*>(&DetourPresent),
        reinterpret_cast<void**>(&g_OrigPresent));
    g_HookPresent1 = HookAndEnable(api, mod, "IDXGISwapChain1::Present1",
        present1Addr, reinterpret_cast<void*>(&DetourPresent1),
        reinterpret_cast<void**>(&g_OrigPresent1));

    // 5. Vtable-slot hooks on dxgi's swap chain class, in case the inline ones
    //    get patched over by another overlay
    HookVtable(api, mod, vtable, vtableHasPresent1);

    if (!g_HookPresent && !g_HookPresent1 && g_VtableCount.load() == 0) {
        api->log(mod, "no Present hook could be made");
        return false;
    }

    api->log(mod, "DXGI Present hooks installed");
    return true;
}

void DxgiHookService(const FH2ModApi* api, FH2Mod* mod) {
    if (!api || !mod || api->api_version < 3) return;
    HookPendingVtable(api, mod);
}

void DxgiHookDiagnose(const FH2ModApi* api, FH2Mod* mod, bool retry) {
    if (!api || !mod) return;
    char msg[512];
    std::snprintf(msg, sizeof(msg),
        "[bugtrap] calls: Present=%llu Present1=%llu vtable=%llu; vtables hooked=%d pending=%p; "
        "hooks: Present=%p Present1=%p CreateSC=%p CreateSCHwnd=%p CreateSCComp=%p",
        static_cast<unsigned long long>(g_PresentCalls.load()),
        static_cast<unsigned long long>(g_Present1Calls.load()),
        static_cast<unsigned long long>(g_VmtCalls.load()),
        g_VtableCount.load(), static_cast<void*>(g_PendingVtbl.load()),
        static_cast<void*>(g_HookPresent), static_cast<void*>(g_HookPresent1),
        static_cast<void*>(g_HookCreateSwapChain), static_cast<void*>(g_HookCreateSwapChainForHwnd),
        static_cast<void*>(g_HookCreateSwapChainForComposition));
    api->log(mod, msg);

    if (retry && api->api_version >= 3) {
        HookPendingVtable(api, mod);
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
