#include "fps_counter.h"
#include "config.h"

FpsCounter g_FpsCounter;

FpsCounter::FpsCounter() {
    InitializeCriticalSection(&m_cs);
    Init();
}

void FpsCounter::Init() {
    QueryPerformanceFrequency(&m_frequency);
    QueryPerformanceCounter(&m_lastFrameTime);
    m_lastIntervalTime = m_lastFrameTime;
    m_intervalFrameCount = 0;
}

void FpsCounter::OnFrame() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    m_totalFrames.fetch_add(1, std::memory_order_relaxed);

    EnterCriticalSection(&m_cs);
    m_intervalFrameCount++;

    if (m_frequency.QuadPart > 0) {
        double elapsedSec = static_cast<double>(now.QuadPart - m_lastIntervalTime.QuadPart) / static_cast<double>(m_frequency.QuadPart);
        double intervalSec = static_cast<double>(g_Config.updateIntervalMs) / 1000.0;

        if (elapsedSec >= intervalSec && elapsedSec > 0.0) {
            float calculatedFps = static_cast<float>(static_cast<double>(m_intervalFrameCount) / elapsedSec);
            float calculatedFrameTimeMs = static_cast<float>((elapsedSec / static_cast<double>(m_intervalFrameCount)) * 1000.0);

            m_displayFps.store(calculatedFps, std::memory_order_relaxed);
            m_displayFrameTimeMs.store(calculatedFrameTimeMs, std::memory_order_relaxed);

            m_lastIntervalTime = now;
            m_intervalFrameCount = 0;
        }
    }

    m_lastFrameTime = now;
    LeaveCriticalSection(&m_cs);
}
