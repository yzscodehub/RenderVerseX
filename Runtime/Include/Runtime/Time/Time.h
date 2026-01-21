#pragma once

/**
 * @file Time.h
 * @brief Time utilities and clock abstraction
 */

#include <chrono>
#include <cstdint>

namespace RVX
{
    /**
     * @brief High-resolution clock utilities
     * 
     * Provides frame-independent timing for game loops.
     * 
     * Usage:
     * @code
     * Time::Initialize();
     * 
     * while (running) {
     *     Time::Update();
     *     
     *     float dt = Time::DeltaTime();
     *     float fps = Time::FPS();
     *     double elapsed = Time::ElapsedTime();
     *     
     *     Update(dt);
     * }
     * @endcode
     */
    class Time
    {
    public:
        using Clock = std::chrono::high_resolution_clock;
        using TimePoint = Clock::time_point;
        using Duration = std::chrono::duration<double>;

        /// Initialize the time system
        static void Initialize();

        /// Update time (call once per frame)
        static void Update();

        /// Get delta time in seconds (time since last frame)
        static float DeltaTime() { return s_deltaTime; }

        /// Get unscaled delta time (ignores time scale)
        static float UnscaledDeltaTime() { return s_unscaledDeltaTime; }

        /// Get total elapsed time since initialization (in seconds)
        static double ElapsedTime() { return s_elapsedTime; }

        /// Get current time scale (default 1.0)
        static float TimeScale() { return s_timeScale; }

        /// Set time scale (for slow-mo, pause, etc.)
        static void SetTimeScale(float scale) { s_timeScale = scale; }

        /// Get current frame number
        static uint64_t FrameCount() { return s_frameCount; }

        /// Get frames per second (smoothed)
        static float FPS() { return s_fps; }

        /// Get raw time point (for custom timing)
        static TimePoint Now() { return Clock::now(); }

        /// Calculate duration in seconds between two time points
        static double Seconds(TimePoint start, TimePoint end)
        {
            return Duration(end - start).count();
        }

    private:
        static TimePoint s_startTime;
        static TimePoint s_lastFrameTime;
        static float s_deltaTime;
        static float s_unscaledDeltaTime;
        static double s_elapsedTime;
        static float s_timeScale;
        static uint64_t s_frameCount;
        static float s_fps;
        static float s_fpsAccumulator;
        static int s_fpsFrameCount;
    };

    /**
     * @brief Scoped timer for profiling
     * 
     * Usage:
     * @code
     * {
     *     ScopedTimer timer("UpdatePhysics");
     *     UpdatePhysics();
     * } // Prints elapsed time when going out of scope
     * @endcode
     */
    class ScopedTimer
    {
    public:
        explicit ScopedTimer(const char* name);
        ~ScopedTimer();

        /// Get elapsed time without stopping
        double Elapsed() const;

        // Non-copyable
        ScopedTimer(const ScopedTimer&) = delete;
        ScopedTimer& operator=(const ScopedTimer&) = delete;

    private:
        const char* m_name;
        Time::TimePoint m_start;
    };

} // namespace RVX
