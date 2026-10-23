#pragma once

/**
 * @file ScriptEcsBridge.h
 * @brief Pure ECS scripting bridge and an optional Lua-backed production gateway.
 */

#include "Core/Handle.h"
#include "Scene/ECS/SceneEcsRuntime.h"
#include "Scene/ECS/ScriptFragments.h"
#include "Scripting/ScriptEngine.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace RVX::Scripting
{
    /** @brief Generation-safe identity for a VM object owned outside ECS storage. */
    struct ScriptEcsInstanceHandleTag {};
    using ScriptEcsInstanceHandle = Handle<ScriptEcsInstanceHandleTag>;

    /** @brief Value-only program request emitted by the ECS bridge. */
    struct ScriptEcsProgramRequest
    {
        AssetId scriptAssetId{};
        uint64 configurationRevision = 0;
    };

    /** @brief Exact release outcome required before an ECS cleanup acknowledgement. */
    enum class ScriptEcsStopResult : uint8
    {
        Stopped = 0,
        AlreadyAbsent,
        Failed,
    };

    /**
     * @brief Scripting service boundary containing only values and opaque handles.
     *
     * Implementations own all Lua state, source paths, ScriptHandles and script
     * tables. They must reject calls made from a non-owner thread.
     */
    class IScriptEcsExecutionGateway
    {
    public:
        virtual ~IScriptEcsExecutionGateway() = default;

        [[nodiscard]] virtual ScriptEcsInstanceHandle Create(const ScriptEcsProgramRequest& request) = 0;
        [[nodiscard]] virtual bool Start(ScriptEcsInstanceHandle instance,
                                         ECS::SceneRuntimeId sceneRuntimeId,
                                         ECS::EntityHandle entity) = 0;
        [[nodiscard]] virtual bool Update(ScriptEcsInstanceHandle instance, float deltaTime) = 0;
        [[nodiscard]] virtual ScriptEcsStopResult Stop(ScriptEcsInstanceHandle instance) = 0;
        /** @brief Reload transactionally: failure leaves the current live instance intact. */
        [[nodiscard]] virtual bool Reload(ScriptEcsInstanceHandle instance) = 0;
        [[nodiscard]] virtual bool IsInstanceAlive(ScriptEcsInstanceHandle instance) const = 0;
        /** @brief Explicit same-thread drain before the host tears down the VM. */
        [[nodiscard]] virtual bool PrepareForShutdown() = 0;
    };

    /** @brief Resolver-local source metadata; paths never enter an ECS fragment. */
    struct ScriptEcsSource
    {
        std::filesystem::path relativePath;
        uint64 sourceRevision = 0;
    };

    /** @brief Maps a stable AssetId to source data outside ECS ownership. */
    using ScriptEcsSourceResolver = std::function<std::optional<ScriptEcsSource>(AssetId)>;

    /**
     * @brief ScriptingSubsystem-backed gateway with a generational VM side table.
     *
     * The subsystem and resolver must outlive this gateway. Script chunks must
     * return a table; no global/table fallback is used. A failed reload retains
     * the old table and instance. All calls including destruction-side cleanup
     * are affinity-checked against the construction thread.
     */
    class ScriptEcsEngineGateway final : public IScriptEcsExecutionGateway
    {
    public:
        ScriptEcsEngineGateway(::RVX::ScriptingSubsystem& subsystem,
                               ScriptEcsSourceResolver sourceResolver);
        ~ScriptEcsEngineGateway() override;

        ScriptEcsEngineGateway(const ScriptEcsEngineGateway&) = delete;
        ScriptEcsEngineGateway& operator=(const ScriptEcsEngineGateway&) = delete;
        ScriptEcsEngineGateway(ScriptEcsEngineGateway&&) = delete;
        ScriptEcsEngineGateway& operator=(ScriptEcsEngineGateway&&) = delete;

        [[nodiscard]] ScriptEcsInstanceHandle Create(const ScriptEcsProgramRequest& request) override;
        [[nodiscard]] bool Start(ScriptEcsInstanceHandle instance,
                                 ECS::SceneRuntimeId sceneRuntimeId,
                                 ECS::EntityHandle entity) override;
        [[nodiscard]] bool Update(ScriptEcsInstanceHandle instance, float deltaTime) override;
        [[nodiscard]] ScriptEcsStopResult Stop(ScriptEcsInstanceHandle instance) override;
        [[nodiscard]] bool Reload(ScriptEcsInstanceHandle instance) override;
        [[nodiscard]] bool IsInstanceAlive(ScriptEcsInstanceHandle instance) const override;
        [[nodiscard]] bool PrepareForShutdown() override;

    private:
        struct Entry;
        struct Program;

        [[nodiscard]] bool IsOwnerThread() const;
        [[nodiscard]] Entry* FindEntry(ScriptEcsInstanceHandle instance);
        [[nodiscard]] const Entry* FindEntry(ScriptEcsInstanceHandle instance) const;
        [[nodiscard]] std::optional<Program> ResolveProgram(AssetId assetId, bool forceReload);
        [[nodiscard]] bool Retire(ScriptEcsInstanceHandle instance);

        ::RVX::ScriptingSubsystem* m_subsystem = nullptr;
        ScriptEcsSourceResolver m_sourceResolver;
        std::thread::id m_ownerThread;
        std::vector<Entry> m_entries;
        std::vector<uint32> m_freeEntries;
    };

    /** @brief Explicit registration result; the bridge never falls back to ScriptComponent. */
    enum class ScriptEcsBridgeRegistrationResult : uint8
    {
        Registered = 0,
        AlreadyRegistered,
        InvalidRuntime,
        GatewayUnavailable,
        ThreadAffinityViolation,
        ProcessorRegistrationFailed,
    };

    /** @brief Deterministic value diagnostic for an entity-to-instance binding. */
    struct ScriptEcsBindingDiagnostic
    {
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
        ScriptEcsInstanceHandle instance = ScriptEcsInstanceHandle::Invalid();
        SceneECS::ScriptBindingStatus status = SceneECS::ScriptBindingStatus::Unbound;
        uint64 lastConsumedCommandSequence = 0;
        bool awaitingCleanupAcknowledgement = false;
    };

    struct ScriptEcsBridgeDiagnosticsSnapshot
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        ScriptEcsBridgeRegistrationResult registration =
            ScriptEcsBridgeRegistrationResult::InvalidRuntime;
        uint32 activeInstanceCount = 0;
        uint32 outstandingInstanceCount = 0;
        uint32 pendingCleanupCount = 0;
        uint64 structuralContinuityLossCount = 0;
        uint64 cleanupContinuityLossCount = 0;
        uint64 authoritativeReconcileCount = 0;
        uint64 createCount = 0;
        uint64 startCount = 0;
        uint64 stopCount = 0;
        uint64 reloadCount = 0;
        uint64 rejectedConfigurationCount = 0;
        uint64 rejectedCommandCount = 0;
        uint64 invocationFailureCount = 0;
        uint64 releaseFailureCount = 0;
        uint64 shutdownPreparationCount = 0;
        uint64 shutdownFailureCount = 0;
        std::vector<ScriptEcsBindingDiagnostic> bindings;
    };

    /**
     * @brief Registers variable Gameplay and EndFrameCleanup processors for one Scene runtime.
     *
     * The referenced gateway must outlive the bridge. Call PrepareForShutdown()
     * on the Scene runtime owner thread, retrying on false, before destroying
     * the bridge gateway or Lua subsystem. Processor callbacks retain only weak
     * state and become inert after bridge destruction.
     */
    class ScriptEcsBridge
    {
    public:
        ScriptEcsBridge(SceneECS::SceneEcsRuntime& runtime, IScriptEcsExecutionGateway& gateway);
        ~ScriptEcsBridge();

        ScriptEcsBridge(const ScriptEcsBridge&) = delete;
        ScriptEcsBridge& operator=(const ScriptEcsBridge&) = delete;
        ScriptEcsBridge(ScriptEcsBridge&&) = delete;
        ScriptEcsBridge& operator=(ScriptEcsBridge&&) = delete;

        [[nodiscard]] ScriptEcsBridgeRegistrationResult RegisterProcessors();
        [[nodiscard]] bool IsRegistered() const;
        [[nodiscard]] ECS::SceneRuntimeId GetSceneRuntimeId() const;
        [[nodiscard]] bool PrepareForShutdown();
        [[nodiscard]] ScriptEcsInstanceHandle FindInstance(ECS::SceneRuntimeId sceneRuntimeId,
                                                            ECS::EntityHandle entity) const;
        [[nodiscard]] ScriptEcsBridgeDiagnosticsSnapshot GetDiagnosticsSnapshot() const;

    private:
        struct State;
        std::shared_ptr<State> m_state;
    };
} // namespace RVX::Scripting
