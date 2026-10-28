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

        // Gap threshold scales with the update interval, otherwise an interval near
        // the 2s cap would always look like a gap and the displayed FPS would stay 0.
        const double gapSec = (intervalSec * 2.0 > 2.0) ? intervalSec * 2.0 : 2.0;
        if (elapsedSec > gapSec) {
            // Gap, loading screen or pause: reset window so average isn't dragged down
            m_lastIntervalTime = now;
            m_intervalFrameCount = 1;
        } else if (elapsedSec >= intervalSec && elapsedSec > 0.0) {
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

double FpsCounter::SecondsSinceLastFrame() const {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (m_frequency.QuadPart <= 0) return 0.0;
    return static_cast<double>(now.QuadPart - m_lastFrameTime.QuadPart) / static_cast<double>(m_frequency.QuadPart);
}

float FpsCounter::GetFps() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (m_frequency.QuadPart > 0 && m_lastFrameTime.QuadPart > 0) {
        double elapsedSinceLastFrame = static_cast<double>(now.QuadPart - m_lastFrameTime.QuadPart) / static_cast<double>(m_frequency.QuadPart);
        // If more than 1.5 seconds have elapsed since the last rendered frame, framerate has halted
        if (elapsedSinceLastFrame > 1.5) {
            return 0.0f;
        }
    }
    return m_displayFps.load(std::memory_order_relaxed);
}
