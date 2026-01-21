/**
 * @file Time.cpp
 * @brief Time utilities implementation
 */

#include "Runtime/Time/Time.h"
#include "Core/Log.h"

namespace RVX
{

// Static member initialization
Time::TimePoint Time::s_startTime;
Time::TimePoint Time::s_lastFrameTime;
float Time::s_deltaTime = 0.016f;
float Time::s_unscaledDeltaTime = 0.016f;
double Time::s_elapsedTime = 0.0;
float Time::s_timeScale = 1.0f;
uint64_t Time::s_frameCount = 0;
float Time::s_fps = 0.0f;
float Time::s_fpsAccumulator = 0.0f;
int Time::s_fpsFrameCount = 0;

void Time::Initialize()
{
    s_startTime = Clock::now();
    s_lastFrameTime = s_startTime;
    s_deltaTime = 0.016f;  // Assume 60 FPS initially
    s_unscaledDeltaTime = 0.016f;
    s_elapsedTime = 0.0;
    s_timeScale = 1.0f;
    s_frameCount = 0;
    s_fps = 60.0f;
    s_fpsAccumulator = 0.0f;
    s_fpsFrameCount = 0;

    RVX_CORE_DEBUG("Time system initialized");
}

void Time::Update()
{
    TimePoint currentTime = Clock::now();
    
    // Calculate delta time
    s_unscaledDeltaTime = static_cast<float>(Seconds(s_lastFrameTime, currentTime));
    s_deltaTime = s_unscaledDeltaTime * s_timeScale;
    
    // Clamp delta time to prevent huge jumps (e.g., after breakpoint)
    constexpr float maxDeltaTime = 0.25f;  // 4 FPS minimum
    if (s_unscaledDeltaTime > maxDeltaTime)
    {
        s_unscaledDeltaTime = maxDeltaTime;
        s_deltaTime = maxDeltaTime * s_timeScale;
    }

    // Update elapsed time
    s_elapsedTime = Seconds(s_startTime, currentTime);
    s_lastFrameTime = currentTime;
    s_frameCount++;

    // Calculate FPS (smoothed over ~1 second)
    s_fpsAccumulator += s_unscaledDeltaTime;
    s_fpsFrameCount++;

    if (s_fpsAccumulator >= 1.0f)
    {
        s_fps = static_cast<float>(s_fpsFrameCount) / s_fpsAccumulator;
        s_fpsAccumulator = 0.0f;
        s_fpsFrameCount = 0;
    }
}

// ScopedTimer implementation
ScopedTimer::ScopedTimer(const char* name)
    : m_name(name)
    , m_start(Time::Now())
{
}

ScopedTimer::~ScopedTimer()
{
    double elapsed = Elapsed();
    RVX_CORE_DEBUG("[Timer] {}: {:.3f} ms", m_name, elapsed * 1000.0);
}

double ScopedTimer::Elapsed() const
{
    return Time::Seconds(m_start, Time::Now());
}

} // namespace RVX
