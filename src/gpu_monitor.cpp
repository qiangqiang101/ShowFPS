#include "gpu_monitor.h"

#include <pdh.h>
#include <pdhmsg.h>
#include <vector>

GpuMonitor g_GpuMonitor;

GpuMonitor::GpuMonitor() = default;

GpuMonitor::~GpuMonitor() {
    Stop();
}

bool GpuMonitor::Start() {
    if (m_running.load()) return true;

    // Try NVML first (fast-path for NVIDIA GPUs)
    if (InitNvml()) {
        m_useNvml = true;
    } else {
        // Fallback to universal Windows PDH counters (AMD, Intel, etc.)
        if (InitPdh()) {
            m_usePdh = true;
        }
    }

    m_running.store(true);
    m_thread = CreateThread(nullptr, 0, MonitorThreadProc, this, 0, nullptr);
    return m_thread != nullptr;
}

void GpuMonitor::Stop() {
    if (!m_running.load()) return;
    m_running.store(false);

    if (m_thread) {
        WaitForSingleObject(m_thread, 1500);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }

    if (m_useNvml) ShutdownNvml();
    if (m_usePdh) ShutdownPdh();
}

DWORD WINAPI GpuMonitor::MonitorThreadProc(LPVOID param) {
    auto* self = static_cast<GpuMonitor*>(param);
    self->RunMonitorLoop();
    return 0;
}

void GpuMonitor::RunMonitorLoop() {
    while (m_running.load()) {
        int usage = 0;
        bool sampled = false;

        if (m_useNvml) {
            sampled = SampleNvml(usage);
        }

        if (!sampled && m_usePdh) {
            sampled = SamplePdh(usage);
        }

        if (sampled) {
            if (usage < 0) usage = 0;
            if (usage > 100) usage = 100;
            m_gpuUsage.store(usage, std::memory_order_relaxed);
        }

        // Sample interval: 500 ms
        for (int i = 0; i < 10 && m_running.load(); ++i) {
            Sleep(50);
        }
    }
}

bool GpuMonitor::InitNvml() {
    m_hNvml = LoadLibraryW(L"nvml.dll");
    if (!m_hNvml) return false;

    m_nvmlInit = reinterpret_cast<nvmlInit_t>(GetProcAddress(m_hNvml, "nvmlInit_v2"));
    if (!m_nvmlInit) m_nvmlInit = reinterpret_cast<nvmlInit_t>(GetProcAddress(m_hNvml, "nvmlInit"));

    m_nvmlGetHandle = reinterpret_cast<nvmlDeviceGetHandleByIndex_t>(GetProcAddress(m_hNvml, "nvmlDeviceGetHandleByIndex_v2"));
    if (!m_nvmlGetHandle) m_nvmlGetHandle = reinterpret_cast<nvmlDeviceGetHandleByIndex_t>(GetProcAddress(m_hNvml, "nvmlDeviceGetHandleByIndex"));

    m_nvmlGetUtil = reinterpret_cast<nvmlDeviceGetUtilizationRates_t>(GetProcAddress(m_hNvml, "nvmlDeviceGetUtilizationRates"));
    m_nvmlShutdown = reinterpret_cast<nvmlShutdown_t>(GetProcAddress(m_hNvml, "nvmlShutdown"));

    if (!m_nvmlInit || !m_nvmlGetHandle || !m_nvmlGetUtil) {
        FreeLibrary(m_hNvml);
        m_hNvml = nullptr;
        return false;
    }

    if (m_nvmlInit() != 0) {
        FreeLibrary(m_hNvml);
        m_hNvml = nullptr;
        return false;
    }

    if (m_nvmlGetHandle(0, &m_nvmlDevice) != 0 || !m_nvmlDevice) {
        if (m_nvmlShutdown) m_nvmlShutdown();
        FreeLibrary(m_hNvml);
        m_hNvml = nullptr;
        return false;
    }

    return true;
}

bool GpuMonitor::SampleNvml(int& usage) {
    if (!m_nvmlGetUtil || !m_nvmlDevice) return false;

    nvmlUtilization_t util{};
    if (m_nvmlGetUtil(m_nvmlDevice, &util) == 0) {
        usage = static_cast<int>(util.gpu);
        return true;
    }
    return false;
}

void GpuMonitor::ShutdownNvml() {
    if (m_nvmlShutdown) m_nvmlShutdown();
    if (m_hNvml) {
        FreeLibrary(m_hNvml);
        m_hNvml = nullptr;
    }
    m_nvmlDevice = nullptr;
    m_useNvml = false;
}

bool GpuMonitor::InitPdh() {
    PDH_HQUERY query = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
        return false;
    }

    PDH_HCOUNTER counter = nullptr;
    PDH_STATUS status = PdhAddEnglishCounterW(query, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &counter);
    if (status != ERROR_SUCCESS) {
        status = PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter);
    }

    if (status != ERROR_SUCCESS) {
        PdhCloseQuery(query);
        return false;
    }

    PdhCollectQueryData(query);

    m_pdhQuery = query;
    m_pdhCounter = counter;
    return true;
}

bool GpuMonitor::SamplePdh(int& usage) {
    if (!m_pdhQuery || !m_pdhCounter) return false;

    auto query = static_cast<PDH_HQUERY>(m_pdhQuery);
    auto counter = static_cast<PDH_HCOUNTER>(m_pdhCounter);

    if (PdhCollectQueryData(query) != ERROR_SUCCESS) {
        return false;
    }

    DWORD bufferSize = 0;
    DWORD itemCount = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, nullptr);
    if (status != PDH_MORE_DATA || bufferSize == 0) {
        return false;
    }

    std::vector<uint8_t> buffer(bufferSize);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, items);

    if (status == ERROR_SUCCESS) {
        double total = 0.0;
        for (DWORD i = 0; i < itemCount; ++i) {
            if (items[i].FmtValue.CStatus == ERROR_SUCCESS && items[i].FmtValue.doubleValue > 0.0) {
                total += items[i].FmtValue.doubleValue;
            }
        }
        usage = static_cast<int>(total + 0.5);
        return true;
    }

    return false;
}

void GpuMonitor::ShutdownPdh() {
    if (m_pdhQuery) {
        PdhCloseQuery(static_cast<PDH_HQUERY>(m_pdhQuery));
        m_pdhQuery = nullptr;
        m_pdhCounter = nullptr;
    }
    m_usePdh = false;
}
