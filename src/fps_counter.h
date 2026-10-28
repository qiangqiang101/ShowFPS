#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>

class FpsCounter {
public:
    FpsCounter();

    void Init();
    void OnFrame();

    float GetFps();
    float GetFrameTimeMs() const { return m_displayFrameTimeMs.load(std::memory_order_relaxed); }
    double SecondsSinceLastFrame() const;
    uint64_t GetTotalFrames() const { return m_totalFrames.load(std::memory_order_relaxed); }

private:
    LARGE_INTEGER m_frequency{};
    LARGE_INTEGER m_lastFrameTime{};
    LARGE_INTEGER m_lastIntervalTime{};
    
    uint32_t m_intervalFrameCount = 0;
    std::atomic<uint64_t> m_totalFrames{0};
    
    std::atomic<float> m_displayFps{0.0f};
    std::atomic<float> m_displayFrameTimeMs{0.0f};

    CRITICAL_SECTION m_cs;
};

extern FpsCounter g_FpsCounter;
