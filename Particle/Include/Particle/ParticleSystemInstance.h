#pragma once

/**
 * @file ParticleSystemInstance.h
 * @brief Runtime instance of a particle system
 */

#include "Particle/ParticleSystem.h"
#include "Particle/ParticleTypes.h"
#include "Core/Math/AABB.h"
#include <memory>
#include <functional>
#include <cmath>

namespace RVX::Particle
{
    // Forward declarations
    class IParticleSimulator;
    class ParticleEventHandler;

    /**
     * @brief Playback state
     */
    enum class PlaybackState : uint8
    {
        Stopped,
        Playing,
        Paused
    };

    /**
     * @brief Runtime instance of a particle system
     * 
     * ParticleSystemInstance manages the runtime state of a particle system:
     * - Particle data and simulation
     * - Playback control
     * - Transform and visibility
     * - Event handling
     */
    class ParticleSystemInstance
    {
    public:
        // =====================================================================
        // Construction
        // =====================================================================

        ParticleSystemInstance();
        explicit ParticleSystemInstance(ParticleSystem::Ptr system);
        ~ParticleSystemInstance();

        // Non-copyable
        ParticleSystemInstance(const ParticleSystemInstance&) = delete;
        ParticleSystemInstance& operator=(const ParticleSystemInstance&) = delete;

        // Movable
        ParticleSystemInstance(ParticleSystemInstance&&) noexcept;
        ParticleSystemInstance& operator=(ParticleSystemInstance&&) noexcept;

        // =====================================================================
        // System
        // =====================================================================

        /// Set the particle system to use
        void SetSystem(ParticleSystem::Ptr system);

        /// Get the particle system
        ParticleSystem::Ptr GetSystem() const { return m_system; }

        /// Check if a system is assigned
        bool HasSystem() const { return m_system != nullptr; }

        // =====================================================================
        // Playback Control
        // =====================================================================

        /// Start playing
        void Play();

        /// Stop and reset
        void Stop();

        /// Pause playback
        void Pause();

        /// Resume from pause
        void Resume();

        /// Clear all particles immediately
        void Clear();

        /// Restart from beginning
        void Restart();

        /// Get current playback state
        PlaybackState GetPlaybackState() const { return m_playbackState; }

        /// Check if currently playing
        bool IsPlaying() const { return m_playbackState == PlaybackState::Playing; }

        /// Check if paused
        bool IsPaused() const { return m_playbackState == PlaybackState::Paused; }

        /// Check if stopped
        bool IsStopped() const { return m_playbackState == PlaybackState::Stopped; }

        /// Check if system has finished (non-looping only)
        bool IsFinished() const;

        // =====================================================================
        // Simulation
        // =====================================================================

        /// Update the simulation
        void Simulate(float deltaTime);

        /// Prewarm the system (simulate ahead)
        void Prewarm(float duration);

        /// Get simulation time (seconds since start)
        float GetSimulationTime() const { return m_simulationTime; }

        /// Get normalized time (0-1 for non-looping, wraps for looping)
        float GetNormalizedTime() const;

        // =====================================================================
        // Particle Data
        // =====================================================================

        /// Get number of alive particles
        uint32 GetAliveCount() const { return m_aliveCount; }

        /// Get maximum particle count
        uint32 GetMaxParticles() const;

        /// Check if particle limit reached
        bool IsAtCapacity() const;

        /// Get particle simulator
        IParticleSimulator* GetSimulator() const { return m_simulator.get(); }

        // =====================================================================
        // Transform
        // =====================================================================

        /// Set world transform
        void SetTransform(const Mat4& transform);

        /// Get world transform
        const Mat4& GetTransform() const { return m_worldTransform; }

        /// Set position
        void SetPosition(const Vec3& position);

        /// Get position
        Vec3 GetPosition() const;

        /// Set rotation (Euler angles in degrees)
        void SetRotation(const Vec3& rotation);

        /// Set scale
        void SetScale(const Vec3& scale);

        // =====================================================================
        // Bounds
        // =====================================================================

        /// Get world-space bounding box
        AABB GetWorldBounds() const;

        /// Get local-space bounding box
        AABB GetLocalBounds() const;

        /// Update bounds from current particles
        void UpdateBounds();

        // =====================================================================
        // Overrides
        // =====================================================================

        /// Override emission rate multiplier
        void SetEmissionRateMultiplier(float multiplier) { m_emissionRateMultiplier = multiplier; }
        float GetEmissionRateMultiplier() const { return m_emissionRateMultiplier; }

        /// Override start color
        void SetStartColorOverride(const Vec4& color) { m_startColorOverride = color; m_hasStartColorOverride = true; }
        void ClearStartColorOverride() { m_hasStartColorOverride = false; }

        /// Override start size
        void SetStartSizeOverride(float size) { m_startSizeOverride = size; m_hasStartSizeOverride = true; }
        void ClearStartSizeOverride() { m_hasStartSizeOverride = false; }

        /// Override simulation speed
        void SetSimulationSpeedMultiplier(float multiplier) { m_simulationSpeedMultiplier = multiplier; }
        float GetSimulationSpeedMultiplier() const { return m_simulationSpeedMultiplier; }

        // =====================================================================
        // Events
        // =====================================================================

        /// Get event handler
        ParticleEventHandler* GetEventHandler() const { return m_eventHandler.get(); }

        // =====================================================================
        // LOD
        // =====================================================================

        /// Get current LOD level (0 = highest quality)
        uint32 GetCurrentLODLevel() const { return m_currentLODLevel; }

        /// Force a specific LOD level (-1 = automatic)
        void SetForcedLODLevel(int32 level) { m_forcedLODLevel = level; }

        /// Update LOD based on distance
        void UpdateLOD(float distanceToCamera);

        // =====================================================================
        // Visibility
        // =====================================================================

        /// Set visibility
        void SetVisible(bool visible) { m_visible = visible; }
        bool IsVisible() const { return m_visible; }

        /// Set whether to simulate when not visible
        void SetSimulateWhenHidden(bool simulate) { m_simulateWhenHidden = simulate; }
        bool GetSimulateWhenHidden() const { return m_simulateWhenHidden; }

        // =====================================================================
        // ID
        // =====================================================================

        /// Get unique instance ID
        uint64 GetInstanceId() const { return m_instanceId; }

    private:
        void Initialize();
        void UpdateEmission(float deltaTime);
        void ApplyOverrides();

        ParticleSystem::Ptr m_system;
        std::unique_ptr<IParticleSimulator> m_simulator;
        std::unique_ptr<ParticleEventHandler> m_eventHandler;

        // State
        PlaybackState m_playbackState = PlaybackState::Stopped;
        float m_simulationTime = 0.0f;
        float m_emissionAccumulator = 0.0f;
        uint32 m_aliveCount = 0;

        // Transform
        Mat4 m_worldTransform = Mat4Identity();
        Vec3 m_position{0.0f, 0.0f, 0.0f};
        Vec3 m_rotation{0.0f, 0.0f, 0.0f};
        Vec3 m_scale{1.0f, 1.0f, 1.0f};

        // Bounds
        AABB m_localBounds;
        bool m_boundsDirty = true;

        // Overrides
        float m_emissionRateMultiplier = 1.0f;
        float m_simulationSpeedMultiplier = 1.0f;
        Vec4 m_startColorOverride{1.0f, 1.0f, 1.0f, 1.0f};
        float m_startSizeOverride = 1.0f;
        bool m_hasStartColorOverride = false;
        bool m_hasStartSizeOverride = false;

        // LOD
        uint32 m_currentLODLevel = 0;
        int32 m_forcedLODLevel = -1;

        // Visibility
        bool m_visible = true;
        bool m_simulateWhenHidden = false;

        // ID
        uint64 m_instanceId = 0;
        static uint64 s_nextInstanceId;
    };

} // namespace RVX::Particle
