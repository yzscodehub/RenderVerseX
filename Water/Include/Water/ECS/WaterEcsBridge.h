#pragma once

/** @file WaterEcsBridge.h  @brief Pure-ECS water feature gateway boundary. */

#include "Core/Handle.h"
#include "RenderContracts/WaterRenderSnapshot.h"
#include "Scene/ECS/Fragments.h"
#include "Scene/ECS/SceneEcsRuntime.h"

#include <memory>
#include <vector>

namespace RVX::Water
{
    enum class WaterEcsCommand : uint8 { None = 0, Activate, Deactivate, Refresh };
    enum class WaterEcsBindingStatus : uint8
    {
        Unbound = 0, Active, Inactive, PendingDestroy, InvalidConfiguration,
        RuntimeRejected, SynchronizationFailed,
    };

    /** @brief POD authoring data; resource objects and WaterSurface stay gateway-owned. */
    struct WaterEcsConfig
    {
        AssetId surfaceAssetId{};
        AssetId materialAssetId{};
        uint64 configurationRevision = 1;
        Vec2 size{1.0f, 1.0f};
        float depth = 1.0f;
        uint32 resolution = 1;
        WaterRenderSnapshotSurfaceType surfaceType = WaterRenderSnapshotSurfaceType::Ocean;
        WaterRenderSnapshotSimulationType simulationType = WaterRenderSnapshotSimulationType::Gerstner;
        bool autoActivate = true;
        bool reflectionEnabled = true;
        bool refractionEnabled = true;
        bool causticsEnabled = true;
        bool underwaterEffectsEnabled = true;
        bool foamEnabled = true;
    };
    struct WaterEcsIntent { WaterEcsCommand command = WaterEcsCommand::None; uint64 sequence = 0; };
    struct WaterEcsState
    {
        WaterEcsBindingStatus status = WaterEcsBindingStatus::Unbound;
        uint64 lastConsumedCommandSequence = 0;
        uint64 lastAppliedConfigurationRevision = 0;
        uint64 lastPublishedPayloadRevision = 0;
        uint64 synchronizationRevision = 0;
    };
    static_assert(ECS::Fragment<WaterEcsConfig>);
    static_assert(ECS::Fragment<WaterEcsIntent>);
    static_assert(ECS::Fragment<WaterEcsState>);

    struct WaterEcsRuntimeHandleTag {};
    using WaterEcsRuntimeHandle = Handle<WaterEcsRuntimeHandleTag>;
    struct WaterEcsRuntimeRequest
    {
        WaterEcsConfig config;
        Mat4 worldTransform{1.0f};
        WaterEcsCommand command = WaterEcsCommand::None;
        float64 deltaSeconds = 0.0;
    };
    struct WaterEcsRuntimeSnapshot { WaterRenderSnapshotItem item; uint64 payloadRevision = 0; };
    enum class WaterEcsReleaseResult : uint8 { Released = 0, AlreadyAbsent, Failed };

    /** @brief Value/handle seam; implementations keep water simulation and GPU state out of ECS. */
    class IWaterEcsGateway
    {
    public:
        virtual ~IWaterEcsGateway() = default;
        [[nodiscard]] virtual WaterEcsRuntimeHandle Create(const WaterEcsRuntimeRequest& request) = 0;
        [[nodiscard]] virtual bool Update(WaterEcsRuntimeHandle handle,
                                          const WaterEcsRuntimeRequest& request) = 0;
        [[nodiscard]] virtual bool CaptureSnapshot(WaterEcsRuntimeHandle handle,
                                                   WaterEcsRuntimeSnapshot& outSnapshot) const = 0;
        [[nodiscard]] virtual WaterEcsReleaseResult Release(WaterEcsRuntimeHandle handle) = 0;
        [[nodiscard]] virtual bool IsAlive(WaterEcsRuntimeHandle handle) const = 0;
    };

    enum class WaterEcsBridgeRegistrationResult : uint8
    { Registered = 0, AlreadyRegistered, InvalidRuntime, GatewayUnavailable, ProcessorRegistrationFailed };
    struct WaterEcsBindingDiagnostic
    {
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
        WaterEcsRuntimeHandle runtime = WaterEcsRuntimeHandle::Invalid();
        WaterEcsBindingStatus status = WaterEcsBindingStatus::Unbound;
        uint64 configurationRevision = 0;
        bool awaitingCleanupAcknowledgement = false;
    };
    struct WaterEcsBridgeDiagnosticsSnapshot
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        WaterEcsBridgeRegistrationResult registration = WaterEcsBridgeRegistrationResult::InvalidRuntime;
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
        std::vector<WaterEcsBindingDiagnostic> bindings;
    };

    class WaterEcsBridge
    {
    public:
        WaterEcsBridge(SceneECS::SceneEcsRuntime& runtime, IWaterEcsGateway& gateway);
        ~WaterEcsBridge();
        WaterEcsBridge(const WaterEcsBridge&) = delete;
        WaterEcsBridge& operator=(const WaterEcsBridge&) = delete;
        [[nodiscard]] WaterEcsBridgeRegistrationResult RegisterProcessors();
        [[nodiscard]] bool IsRegistered() const;
        [[nodiscard]] ECS::SceneRuntimeId GetSceneRuntimeId() const;
        /** @brief True only after side-table release and EndFrameCleanup snapshot removal. */
        [[nodiscard]] bool PrepareForShutdown();
        [[nodiscard]] WaterEcsRuntimeHandle FindRuntime(ECS::SceneRuntimeId sceneRuntimeId,
                                                         ECS::EntityHandle entity) const;
        [[nodiscard]] WaterEcsBridgeDiagnosticsSnapshot GetDiagnosticsSnapshot() const;
    private:
        struct State;
        std::shared_ptr<State> m_state;
    };
} // namespace RVX::Water
