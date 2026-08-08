#pragma once

/**
 * @file ParticleComponent.h
 * @brief Scene component for particle systems
 */

#include "Core/Math/AABB.h"
#include "Particle/ParticleSystem.h"
#include "Particle/ParticleSystemInstance.h"
#include "RenderContracts/FeatureRenderSnapshot.h"
#include "Scene/Component.h"
#include <string>

namespace RVX::Particle
{
    class ParticleSubsystem;

    enum class ParticleInstanceOwnership
    {
        None = 0,
        SubsystemOwned,
        LegacyFallback
    };

    /**
     * @brief Scene component that attaches a particle system to an entity
     */
    class ParticleComponent : public Component, public RVX::IRenderFeatureSnapshotProvider
    {
    public:
        ParticleComponent() = default;
        ~ParticleComponent() override;

        // =====================================================================
        // Component Interface
        // =====================================================================

        const char* GetTypeName() const override { return "ParticleComponent"; }
        bool ProvidesBounds() const override { return true; }
        AABB GetLocalBounds() const override;

        void OnAttach() override;
        void OnDetach() override;
        void Tick(float deltaTime) override;
        [[nodiscard]] SceneUpdatePhase GetSceneUpdatePhase() const override
        {
            return SceneUpdatePhase::FeatureSystems;
        }

        // =====================================================================
        // Particle System
        // =====================================================================

        /// Set the particle system to use
        void SetParticleSystem(ParticleSystem::Ptr system);

        /// Get the particle system
        ParticleSystem::Ptr GetParticleSystem() const { return m_particleSystem; }

        /// Set by path (loads asynchronously)
        void SetParticleSystemPath(const std::string& path);

        /// Get the runtime instance
        ParticleSystemInstance* GetInstance() const { return m_instance; }

        /// Get how the current runtime instance is owned.
        ParticleInstanceOwnership GetInstanceOwnership() const { return m_instanceOwnership; }

        /// Whether the current instance uses the compatibility fallback path.
        bool IsUsingLegacyFallback() const { return m_instanceOwnership == ParticleInstanceOwnership::LegacyFallback; }

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

        /// Clear all particles
        void Clear();

        /// Restart from beginning
        void Restart();

        /// Check if playing
        bool IsPlaying() const;

        /// Check if paused
        bool IsPaused() const;

        /// Check if stopped
        bool IsStopped() const;

        /// Set auto-play on attach
        void SetAutoPlay(bool autoPlay) { m_autoPlay = autoPlay; }
        bool GetAutoPlay() const { return m_autoPlay; }

        // =====================================================================
        // Overrides
        // =====================================================================

        /// Override emission rate (multiplier)
        void SetEmissionRate(float rate);
        float GetEmissionRate() const { return m_emissionRateOverride; }

        /// Override start color
        void SetStartColor(const Vec4& color);
        const Vec4& GetStartColor() const { return m_startColorOverride; }
        void ClearStartColorOverride();

        /// Override start size
        void SetStartSize(float size);
        float GetStartSize() const { return m_startSizeOverride; }
        void ClearStartSizeOverride();

        /// Override simulation speed
        void SetSimulationSpeed(float speed);
        float GetSimulationSpeed() const { return m_simulationSpeedOverride; }

        // =====================================================================
        // Visibility
        // =====================================================================

        /// Set visibility
        void SetVisible(bool visible);
        bool IsVisible() const { return m_visible; }

        /// Set simulate when hidden
        void SetSimulateWhenHidden(bool simulate);
        bool GetSimulateWhenHidden() const { return m_simulateWhenHidden; }

        /// Append this component's particle data into a scene-level feature snapshot.
        bool AppendRenderFeatureSnapshot(RVX::RenderFeatureSnapshot& outSnapshot) const override;

    private:
        void CreateInstance();
        void DestroyInstance();
        void UpdateTransform();

        ParticleSystem::Ptr m_particleSystem;
        ParticleSystemInstance* m_instance = nullptr;
        ParticleSubsystem* m_instanceSubsystem = nullptr;
        ParticleInstanceOwnership m_instanceOwnership = ParticleInstanceOwnership::None;
        std::string m_particleSystemPath;

        // Settings
        bool m_autoPlay = true;
        bool m_visible = true;
        bool m_simulateWhenHidden = false;

        // Overrides
        float m_emissionRateOverride = 1.0f;
        Vec4 m_startColorOverride{1.0f, 1.0f, 1.0f, 1.0f};
        float m_startSizeOverride = 1.0f;
        float m_simulationSpeedOverride = 1.0f;
        bool m_hasStartColorOverride = false;
        bool m_hasStartSizeOverride = false;
    };

} // namespace RVX::Particle
