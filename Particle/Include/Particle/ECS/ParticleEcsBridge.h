#pragma once

/**
 * @file ParticleEcsBridge.h
 * @brief Pure-ECS particle feature bridge with value-only gateway seams.
 */

#include "Core/Handle.h"
#include "RenderContracts/ParticleRenderSnapshot.h"
#include "Scene/ECS/Fragments.h"
#include "Scene/ECS/SceneEcsRuntime.h"

#include <memory>
#include <vector>

namespace RVX::Particle
{
    /** @brief Explicit particle lifecycle command carried by a monotonic sequence. */
    enum class ParticleEcsCommand : uint8
    {
        None = 0,
        Play,
        Stop,
        Pause,
        Restart,
        Clear,
    };

    /** @brief Observable value-only status of one particle ECS binding. */
    enum class ParticleEcsBindingStatus : uint8
    {
        Unbound = 0,
        Active,
        Stopped,
        Paused,
        PendingDestroy,
        InvalidConfiguration,
        RuntimeRejected,
        SynchronizationFailed,
    };

    /** @brief Data-only authored particle configuration retained in the Scene registry. */
    struct ParticleEcsConfig
    {
        AssetId systemAssetId{};
        uint64 configurationRevision = 1;
        float emissionRateScale = 1.0f;
        float simulationSpeed = 1.0f;
        uint32 maxParticleCount = 1;
        bool autoPlay = true;
        bool visible = true;
        bool simulateWhenHidden = false;
    };

    /** @brief One value-only particle command; sequence is consumed only after gateway success. */
    struct ParticleEcsIntent
    {
        ParticleEcsCommand command = ParticleEcsCommand::None;
        uint64 sequence = 0;
    };

    /** @brief Runtime-visible state without a ParticleSystemInstance or backend pointer. */
    struct ParticleEcsState
    {
        ParticleEcsBindingStatus status = ParticleEcsBindingStatus::Unbound;
        uint64 lastConsumedCommandSequence = 0;
        uint64 lastAppliedConfigurationRevision = 0;
        uint64 lastPublishedPayloadRevision = 0;
        uint64 synchronizationRevision = 0;
    };

    static_assert(ECS::Fragment<ParticleEcsConfig>);
    static_assert(ECS::Fragment<ParticleEcsIntent>);
    static_assert(ECS::Fragment<ParticleEcsState>);

    /** @brief Generation-safe identity owned solely by the particle gateway side table. */
    struct ParticleEcsRuntimeHandleTag {};
    using ParticleEcsRuntimeHandle = Handle<ParticleEcsRuntimeHandleTag>;

    /** @brief Immutable value input passed from the bridge to a particle subsystem gateway. */
    struct ParticleEcsRuntimeRequest
    {
        ParticleEcsConfig config;
        Mat4 worldTransform{1.0f};
        ParticleEcsCommand command = ParticleEcsCommand::None;
        float64 deltaSeconds = 0.0;
    };

    /** @brief Complete value result captured from a gateway-owned particle runtime instance. */
    struct ParticleEcsRuntimeSnapshot
    {
        ParticleRenderSnapshotItem item;
        uint64 payloadRevision = 0;
    };

    enum class ParticleEcsReleaseResult : uint8
    {
        Released = 0,
        AlreadyAbsent,
        Failed,
    };

    /**
     * @brief Subsystem-owned gateway boundary for particle runtime instances.
     *
     * ECS passes only copied configuration, transforms, commands, and handles.
     * The implementation owns all ParticleSubsystem, ParticleSystem,
     * ParticleSystemInstance, GPU, and resource references in its own
     * generation-safe side table.
     */
    class IParticleEcsGateway
    {
    public:
        virtual ~IParticleEcsGateway() = default;

        [[nodiscard]] virtual ParticleEcsRuntimeHandle Create(
            const ParticleEcsRuntimeRequest& request) = 0;
        [[nodiscard]] virtual bool Update(ParticleEcsRuntimeHandle handle,
                                          const ParticleEcsRuntimeRequest& request) = 0;
        [[nodiscard]] virtual bool CaptureSnapshot(
            ParticleEcsRuntimeHandle handle,
            ParticleEcsRuntimeSnapshot& outSnapshot) const = 0;
        [[nodiscard]] virtual ParticleEcsReleaseResult Release(
            ParticleEcsRuntimeHandle handle) = 0;
        [[nodiscard]] virtual bool IsAlive(ParticleEcsRuntimeHandle handle) const = 0;
    };

    enum class ParticleEcsBridgeRegistrationResult : uint8
    {
        Registered = 0,
        AlreadyRegistered,
        InvalidRuntime,
        GatewayUnavailable,
        ProcessorRegistrationFailed,
    };

    struct ParticleEcsBindingDiagnostic
    {
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
        ParticleEcsRuntimeHandle runtime = ParticleEcsRuntimeHandle::Invalid();
        ParticleEcsBindingStatus status = ParticleEcsBindingStatus::Unbound;
        uint64 configurationRevision = 0;
        bool awaitingCleanupAcknowledgement = false;
    };

    /** @brief Deterministic, value-only diagnostics for host shutdown and validation. */
    struct ParticleEcsBridgeDiagnosticsSnapshot
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        ParticleEcsBridgeRegistrationResult registration =
            ParticleEcsBridgeRegistrationResult::InvalidRuntime;
        uint32 bindingCount = 0;
        uint32 outstandingRuntimeCount = 0;
        uint32 publishedSnapshotCount = 0;
        uint32 pendingCleanupCount = 0;
        uint64 structuralContinuityLossCount = 0;
        uint64 cleanupContinuityLossCount = 0;
        uint64 authoritativeReconcileCount = 0;
        uint64 createdRuntimeCount = 0;
        uint64 releasedRuntimeCount = 0;
        uint64 rejectedConfigurationCount = 0;
        uint64 rejectedCommandCount = 0;
        uint64 synchronizationFailureCount = 0;
        uint64 shutdownPreparationCount = 0;
        uint64 shutdownReleaseFailureCount = 0;
        std::vector<ParticleEcsBindingDiagnostic> bindings;
    };

    /**
     * @brief Owns particle ECS associations for exactly one Scene runtime.
     *
     * Hosts must destroy feature entities and drive EndFrameCleanup before
     * teardown, then call PrepareForShutdown until it reports true.  That
     * final proof requires both gateway side-table handles and Scene feature
     * snapshots to be absent; it never clears either one behind lifecycle or
     * render retirement consumers.
     */
    class ParticleEcsBridge
    {
    public:
        ParticleEcsBridge(SceneECS::SceneEcsRuntime& runtime,
                          IParticleEcsGateway& gateway);
        ~ParticleEcsBridge();

        ParticleEcsBridge(const ParticleEcsBridge&) = delete;
        ParticleEcsBridge& operator=(const ParticleEcsBridge&) = delete;
        ParticleEcsBridge(ParticleEcsBridge&&) = delete;
        ParticleEcsBridge& operator=(ParticleEcsBridge&&) = delete;

        [[nodiscard]] ParticleEcsBridgeRegistrationResult RegisterProcessors();
        [[nodiscard]] bool IsRegistered() const;
        [[nodiscard]] ECS::SceneRuntimeId GetSceneRuntimeId() const;
        [[nodiscard]] bool PrepareForShutdown();
        [[nodiscard]] ParticleEcsRuntimeHandle FindRuntime(
            ECS::SceneRuntimeId sceneRuntimeId,
            ECS::EntityHandle entity) const;
        [[nodiscard]] ParticleEcsBridgeDiagnosticsSnapshot GetDiagnosticsSnapshot() const;

    private:
        struct State;
        std::shared_ptr<State> m_state;
    };
} // namespace RVX::Particle
