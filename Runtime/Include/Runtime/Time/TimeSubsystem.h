#pragma once

/**
 * @file TimeSubsystem.h
 * @brief Time management subsystem
 */

#include "Core/Subsystem/EngineSubsystem.h"
#include "Runtime/Time/Time.h"

namespace RVX
{
    /**
     * @brief Time subsystem - manages frame timing and time scale
     * 
     * This subsystem wraps the static Time class to provide subsystem-based
     * access to time utilities.
     * 
     * Usage:
     * @code
     * auto* timeSys = engine.GetSubsystem<TimeSubsystem>();
     * 
     * float dt = timeSys->GetDeltaTime();
     * float fps = timeSys->GetFPS();
     * 
     * // Slow motion
     * timeSys->SetTimeScale(0.5f);
     * @endcode
     */
    class TimeSubsystem : public EngineSubsystem
    {
    public:
        const char* GetName() const override { return "TimeSubsystem"; }
        bool ShouldTick() const override { return true; }

        void Initialize() override;
        void Deinitialize() override;
        void Tick(float deltaTime) override;

        // =====================================================================
        // Time Access
        // =====================================================================

        /// Get delta time in seconds (scaled by time scale)
        float GetDeltaTime() const { return Time::DeltaTime(); }

        /// Get unscaled delta time (ignores time scale)
        float GetUnscaledDeltaTime() const { return Time::UnscaledDeltaTime(); }

        /// Get total elapsed time since engine start
        double GetElapsedTime() const { return Time::ElapsedTime(); }

        /// Get current time scale
        float GetTimeScale() const { return Time::TimeScale(); }

        /// Set time scale (1.0 = normal, 0.5 = half speed, 2.0 = double speed)
        void SetTimeScale(float scale) { Time::SetTimeScale(scale); }

        /// Get current frame number
        uint64_t GetFrameCount() const { return Time::FrameCount(); }

        /// Get frames per second (smoothed average)
        float GetFPS() const { return Time::FPS(); }

        /// Pause time (sets time scale to 0)
        void Pause() { m_pausedTimeScale = Time::TimeScale(); Time::SetTimeScale(0.0f); }

        /// Resume time (restores previous time scale)
        void Resume() { Time::SetTimeScale(m_pausedTimeScale); }

        /// Check if time is paused
        bool IsPaused() const { return Time::TimeScale() == 0.0f; }

    private:
        float m_pausedTimeScale = 1.0f;
    };

} // namespace RVX
