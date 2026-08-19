#pragma once

/** @file TerrainEcsBridge.h  @brief Pure-ECS terrain feature gateway boundary. */

#include "Core/Handle.h"
#include "RenderContracts/TerrainRenderSnapshot.h"
#include "Scene/ECS/Fragments.h"
#include "Scene/ECS/SceneEcsRuntime.h"

#include <memory>
#include <vector>

namespace RVX::Terrain
{
    enum class TerrainEcsCommand : uint8 { None = 0, Activate, Deactivate, Rebuild };
    enum class TerrainEcsBindingStatus : uint8 { Unbound = 0, Active, Inactive, PendingDestroy, InvalidConfiguration, RuntimeRejected, SynchronizationFailed };
    /** @brief POD terrain authoring. Heightmap/material runtime objects remain gateway-private. */
    struct TerrainEcsConfig
    {
        AssetId heightmapAssetId{};
        AssetId materialAssetId{};
        uint64 configurationRevision = 1;
        Vec3 size{1.0f, 1.0f, 1.0f};
        float lodBias = 0.0f;
        uint32 patchSize = 1;
        uint32 maxLODLevels = 1;
        bool autoActivate = true;
        bool castsShadow = true;
        bool receivesShadow = true;
        bool collisionEnabled = false;
    };
    struct TerrainEcsIntent { TerrainEcsCommand command = TerrainEcsCommand::None; uint64 sequence = 0; };
    struct TerrainEcsState
    {
        TerrainEcsBindingStatus status = TerrainEcsBindingStatus::Unbound;
        uint64 lastConsumedCommandSequence = 0;
        uint64 lastAppliedConfigurationRevision = 0;
        uint64 lastPublishedPayloadRevision = 0;
        uint64 synchronizationRevision = 0;
    };
    static_assert(ECS::Fragment<TerrainEcsConfig>);
    static_assert(ECS::Fragment<TerrainEcsIntent>);
    static_assert(ECS::Fragment<TerrainEcsState>);
    struct TerrainEcsRuntimeHandleTag {};
    using TerrainEcsRuntimeHandle = Handle<TerrainEcsRuntimeHandleTag>;
    struct TerrainEcsRuntimeRequest { TerrainEcsConfig config; Mat4 worldTransform{1.0f}; TerrainEcsCommand command = TerrainEcsCommand::None; float64 deltaSeconds = 0.0; };
    struct TerrainEcsRuntimeSnapshot { TerrainRenderSnapshotItem item; uint64 payloadRevision = 0; };
    enum class TerrainEcsReleaseResult : uint8 { Released = 0, AlreadyAbsent, Failed };
    /** @brief Value-only gateway seam with a subsystem-owned generational terrain side table. */
    class ITerrainEcsGateway
    {
    public:
        virtual ~ITerrainEcsGateway() = default;
        [[nodiscard]] virtual TerrainEcsRuntimeHandle Create(const TerrainEcsRuntimeRequest& request) = 0;
        [[nodiscard]] virtual bool Update(TerrainEcsRuntimeHandle handle, const TerrainEcsRuntimeRequest& request) = 0;
        [[nodiscard]] virtual bool CaptureSnapshot(TerrainEcsRuntimeHandle handle, TerrainEcsRuntimeSnapshot& outSnapshot) const = 0;
        [[nodiscard]] virtual TerrainEcsReleaseResult Release(TerrainEcsRuntimeHandle handle) = 0;
        [[nodiscard]] virtual bool IsAlive(TerrainEcsRuntimeHandle handle) const = 0;
    };
    enum class TerrainEcsBridgeRegistrationResult : uint8 { Registered = 0, AlreadyRegistered, InvalidRuntime, GatewayUnavailable, ProcessorRegistrationFailed };
    struct TerrainEcsBindingDiagnostic { ECS::EntityHandle entity = ECS::EntityHandle::Invalid(); TerrainEcsRuntimeHandle runtime = TerrainEcsRuntimeHandle::Invalid(); TerrainEcsBindingStatus status = TerrainEcsBindingStatus::Unbound; uint64 configurationRevision = 0; bool awaitingCleanupAcknowledgement = false; };
    struct TerrainEcsBridgeDiagnosticsSnapshot
    {
        ECS::SceneRuntimeId sceneRuntimeId; TerrainEcsBridgeRegistrationResult registration = TerrainEcsBridgeRegistrationResult::InvalidRuntime;
        uint32 bindingCount = 0, outstandingRuntimeCount = 0, publishedSnapshotCount = 0, pendingCleanupCount = 0;
        uint64 structuralContinuityLossCount = 0, cleanupContinuityLossCount = 0, authoritativeReconcileCount = 0;
        uint64 createdRuntimeCount = 0, releasedRuntimeCount = 0, rejectedConfigurationCount = 0, rejectedCommandCount = 0, synchronizationFailureCount = 0;
        uint64 shutdownPreparationCount = 0, shutdownReleaseFailureCount = 0;
        std::vector<TerrainEcsBindingDiagnostic> bindings;
    };
    class TerrainEcsBridge
    {
    public:
        TerrainEcsBridge(SceneECS::SceneEcsRuntime& runtime, ITerrainEcsGateway& gateway);
        ~TerrainEcsBridge();
        TerrainEcsBridge(const TerrainEcsBridge&) = delete;
        TerrainEcsBridge& operator=(const TerrainEcsBridge&) = delete;
        [[nodiscard]] TerrainEcsBridgeRegistrationResult RegisterProcessors();
        [[nodiscard]] bool IsRegistered() const;
        [[nodiscard]] ECS::SceneRuntimeId GetSceneRuntimeId() const;
        /** @brief True only after side tables and terrain frozen values have both drained. */
        [[nodiscard]] bool PrepareForShutdown();
        [[nodiscard]] TerrainEcsRuntimeHandle FindRuntime(ECS::SceneRuntimeId sceneRuntimeId, ECS::EntityHandle entity) const;
        [[nodiscard]] TerrainEcsBridgeDiagnosticsSnapshot GetDiagnosticsSnapshot() const;
    private: struct State; std::shared_ptr<State> m_state;
    };
} // namespace RVX::Terrain
