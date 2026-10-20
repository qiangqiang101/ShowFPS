#pragma once

#include <windows.h>
#include <atomic>

class GpuMonitor {
public:
    GpuMonitor();
    ~GpuMonitor();

    bool Start();
    void Stop();

    int GetGpuUsage() const { return m_gpuUsage.load(std::memory_order_relaxed); }

private:
    static DWORD WINAPI MonitorThreadProc(LPVOID param);
    void RunMonitorLoop();

    bool InitNvml();
    bool SampleNvml(int& usage);
    void ShutdownNvml();

    bool InitPdh();
    bool SamplePdh(int& usage);
    void ShutdownPdh();

    std::atomic<bool> m_running{false};
    std::atomic<int> m_gpuUsage{0};
    HANDLE m_thread = nullptr;

    // NVML function pointers
    HMODULE m_hNvml = nullptr;
    void* m_nvmlDevice = nullptr;
    typedef int (*nvmlInit_t)();
    typedef int (*nvmlShutdown_t)();
    typedef int (*nvmlDeviceGetHandleByIndex_t)(unsigned int index, void** device);
    typedef struct { unsigned int gpu; unsigned int memory; } nvmlUtilization_t;
    typedef int (*nvmlDeviceGetUtilizationRates_t)(void* device, nvmlUtilization_t* utilization);
    
    nvmlInit_t m_nvmlInit = nullptr;
    nvmlShutdown_t m_nvmlShutdown = nullptr;
    nvmlDeviceGetHandleByIndex_t m_nvmlGetHandle = nullptr;
    nvmlDeviceGetUtilizationRates_t m_nvmlGetUtil = nullptr;
    bool m_useNvml = false;

    // PDH handles
    void* m_pdhQuery = nullptr;
    void* m_pdhCounter = nullptr;
    bool m_usePdh = false;
};

extern GpuMonitor g_GpuMonitor;
