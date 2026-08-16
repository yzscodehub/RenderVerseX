#include "Scripting/ECS/ScriptEcsBridge.h"

#include "Core/Log.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

namespace
{
    using namespace RVX;

    class LoggingEnvironment final : public ::testing::Environment
    {
    public:
        void SetUp() override
        {
            if (!Log::GetCoreLogger())
            {
                Log::Config config;
                config.enableRotation = false;
                config.logFileName = "EcsScriptingValidation.log";
                Log::Initialize(config);
            }
        }

        void TearDown() override
        {
            if (Log::GetCoreLogger())
            {
                Log::Shutdown();
            }
        }
    };

    [[maybe_unused]] ::testing::Environment* s_loggingEnvironment =
        ::testing::AddGlobalTestEnvironment(new LoggingEnvironment());

    class FakeScriptGateway final : public Scripting::IScriptEcsExecutionGateway
    {
    public:
        struct Entry
        {
            uint32 generation = 1;
            bool allocated = false;
            bool alive = false;
            bool started = false;
        };

        [[nodiscard]] Scripting::ScriptEcsInstanceHandle Create(
            const Scripting::ScriptEcsProgramRequest& request) override
        {
            ++createCalls;
            requests.push_back(request);
            if (!allowCreate || !request.scriptAssetId.IsValid() || request.configurationRevision == 0)
            {
                return Scripting::ScriptEcsInstanceHandle::Invalid();
            }
            uint32 index = RVX_INVALID_INDEX;
            if (freeEntries.empty())
            {
                index = static_cast<uint32>(entries.size());
                entries.push_back({});
            }
            else
            {
                index = freeEntries.back();
                freeEntries.pop_back();
            }
            entries[index].allocated = true;
            entries[index].alive = true;
            entries[index].started = false;
            return Scripting::ScriptEcsInstanceHandle::Create(index, entries[index].generation);
        }

        [[nodiscard]] bool Start(Scripting::ScriptEcsInstanceHandle instance,
                                 ECS::SceneRuntimeId sceneRuntimeId,
                                 ECS::EntityHandle entity) override
        {
            ++startCalls;
            Entry* entry = Find(instance);
            if (!allowStart || entry == nullptr || !sceneRuntimeId.IsValid() || !entity.IsValid())
            {
                return false;
            }
            entry->started = true;
            return true;
        }

        [[nodiscard]] bool Update(Scripting::ScriptEcsInstanceHandle instance, float) override
        {
            ++updateCalls;
            return allowUpdate && Find(instance) != nullptr && Find(instance)->started;
        }

        [[nodiscard]] Scripting::ScriptEcsStopResult Stop(
            Scripting::ScriptEcsInstanceHandle instance) override
        {
            ++stopCalls;
            Entry* entry = Find(instance);
            if (entry == nullptr)
            {
                return Scripting::ScriptEcsStopResult::AlreadyAbsent;
            }
            if (failedStopAttempts != 0)
            {
                --failedStopAttempts;
                return Scripting::ScriptEcsStopResult::Failed;
            }
            entry->allocated = false;
            entry->alive = false;
            entry->started = false;
            ++entry->generation;
            if (entry->generation == 0)
            {
                ++entry->generation;
            }
            freeEntries.push_back(instance.GetIndex());
            return Scripting::ScriptEcsStopResult::Stopped;
        }

        [[nodiscard]] bool Reload(Scripting::ScriptEcsInstanceHandle instance) override
        {
            ++reloadCalls;
            return allowReload && Find(instance) != nullptr;
        }

        [[nodiscard]] bool IsInstanceAlive(Scripting::ScriptEcsInstanceHandle instance) const override
        {
            return Find(instance) != nullptr;
        }

        [[nodiscard]] bool PrepareForShutdown() override
        {
            ++shutdownCalls;
            return allowShutdown && GetAllocatedCount() == 0;
        }

        [[nodiscard]] uint32 GetAllocatedCount() const
        {
            uint32 count = 0;
            for (const Entry& entry : entries)
            {
                count += entry.allocated ? 1u : 0u;
            }
            return count;
        }

        bool allowCreate = true;
        bool allowStart = true;
        bool allowUpdate = true;
        bool allowReload = true;
        bool allowShutdown = true;
        uint32 failedStopAttempts = 0;
        uint32 createCalls = 0;
        uint32 startCalls = 0;
        uint32 updateCalls = 0;
        uint32 stopCalls = 0;
        uint32 reloadCalls = 0;
        uint32 shutdownCalls = 0;
        std::vector<Scripting::ScriptEcsProgramRequest> requests;

    private:
        [[nodiscard]] Entry* Find(Scripting::ScriptEcsInstanceHandle instance)
        {
            return const_cast<Entry*>(static_cast<const FakeScriptGateway*>(this)->Find(instance));
        }

        [[nodiscard]] const Entry* Find(Scripting::ScriptEcsInstanceHandle instance) const
        {
            if (!instance.IsValid() || instance.GetIndex() >= entries.size())
            {
                return nullptr;
            }
            const Entry& entry = entries[instance.GetIndex()];
            return entry.allocated && entry.alive && entry.generation == instance.GetGeneration() ?
                       &entry :
                       nullptr;
        }

        std::vector<Entry> entries;
        std::vector<uint32> freeEntries;
    };

    class ScopedTempDirectory
    {
    public:
        ScopedTempDirectory()
            : path(std::filesystem::temp_directory_path() /
                   ("rvx_ecs_script_" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
        {
            std::error_code error;
            available = std::filesystem::create_directories(path, error) && !error;
        }

        ~ScopedTempDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

        std::filesystem::path path;
        bool available = false;
    };

    [[nodiscard]] SceneECS::SceneEcsTickResult Tick(SceneECS::SceneEcsRuntime& runtime)
    {
        return runtime.Tick({.variableDeltaSeconds = 1.0 / 60.0});
    }

    [[nodiscard]] ECS::EntityHandle AddScriptEntity(SceneECS::SceneEcsRuntime& runtime,
                                                     bool startOnLoad = true)
    {
        const ECS::EntityHandle entity = runtime.CreateEntity();
        if (!entity.IsValid())
        {
            return ECS::EntityHandle::Invalid();
        }
        if (!runtime.AddFragment<SceneECS::ScriptBehaviour>(
                entity, {.scriptAssetId = {.value = 700}, .configurationRevision = 1,
                         .startOnLoad = startOnLoad}) ||
            !runtime.AddFragment<SceneECS::ScriptExecutionIntent>(entity) ||
            !runtime.AddFragment<SceneECS::ScriptExecutionState>(entity))
        {
            return ECS::EntityHandle::Invalid();
        }
        return entity;
    }

    void SetIntent(SceneECS::SceneEcsRuntime& runtime,
                   ECS::EntityHandle entity,
                   SceneECS::ScriptExecutionCommand command,
                   uint64 sequence)
    {
        ASSERT_TRUE(runtime.SetFragment<SceneECS::ScriptExecutionIntent>(
            entity, {.command = command, .sequence = sequence}));
    }
} // namespace

TEST(EcsScriptingValidation, LifecycleUsesOnlyHandlesAndValueFragments)
{
    SceneECS::SceneEcsRuntime runtime;
    FakeScriptGateway gateway;
    Scripting::ScriptEcsBridge bridge(runtime, gateway);
    ASSERT_EQ(bridge.RegisterProcessors(), Scripting::ScriptEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddScriptEntity(runtime);

    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.createCalls, 1u);
    EXPECT_EQ(gateway.startCalls, 1u);
    EXPECT_EQ(gateway.updateCalls, 0u);
    const Scripting::ScriptEcsInstanceHandle instance =
        bridge.FindInstance(runtime.GetSceneRuntimeId(), entity);
    ASSERT_TRUE(instance.IsValid());

    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.updateCalls, 1u);
    const auto* state = runtime.GetRegistry().TryGet<SceneECS::ScriptExecutionState>(entity);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->status, SceneECS::ScriptBindingStatus::Running);
    EXPECT_EQ(state->lastAppliedConfigurationRevision, 1u);
}

TEST(EcsScriptingValidation, CommandsAdvanceOnlyAfterEffectsSucceedAndNeverRepeat)
{
    SceneECS::SceneEcsRuntime runtime;
    FakeScriptGateway gateway;
    Scripting::ScriptEcsBridge bridge(runtime, gateway);
    ASSERT_EQ(bridge.RegisterProcessors(), Scripting::ScriptEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddScriptEntity(runtime, false);
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.createCalls, 0u);

    SetIntent(runtime, entity, SceneECS::ScriptExecutionCommand::Start, 7);
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.createCalls, 1u);
    EXPECT_EQ(gateway.startCalls, 1u);
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.startCalls, 1u);

    gateway.allowReload = false;
    SetIntent(runtime, entity, SceneECS::ScriptExecutionCommand::Reload, 8);
    ASSERT_TRUE(Tick(runtime).succeeded);
    auto* failedReloadState = runtime.GetRegistry().TryGet<SceneECS::ScriptExecutionState>(entity);
    ASSERT_NE(failedReloadState, nullptr);
    EXPECT_EQ(failedReloadState->lastConsumedCommandSequence, 7u);
    gateway.allowReload = true;
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.reloadCalls, 2u);
    EXPECT_EQ(failedReloadState->lastConsumedCommandSequence, 8u);

    gateway.failedStopAttempts = 1;
    SetIntent(runtime, entity, SceneECS::ScriptExecutionCommand::Stop, 9);
    EXPECT_FALSE(Tick(runtime).succeeded);
    EXPECT_EQ(failedReloadState->lastConsumedCommandSequence, 8u);
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(failedReloadState->lastConsumedCommandSequence, 9u);
    EXPECT_EQ(gateway.GetAllocatedCount(), 0u);
}

TEST(EcsScriptingValidation, StaleGenerationAndDurableCleanupCannotReleaseReplacement)
{
    SceneECS::SceneEcsRuntime runtime;
    FakeScriptGateway gateway;
    Scripting::ScriptEcsBridge bridge(runtime, gateway);
    ASSERT_EQ(bridge.RegisterProcessors(), Scripting::ScriptEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle originalEntity = AddScriptEntity(runtime);
    ASSERT_TRUE(Tick(runtime).succeeded);
    const Scripting::ScriptEcsInstanceHandle original =
        bridge.FindInstance(runtime.GetSceneRuntimeId(), originalEntity);
    ASSERT_TRUE(original.IsValid());

    ASSERT_EQ(runtime.RequestDestroy(
                  originalEntity, SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::None)),
              SceneECS::DestroyRequestResult::Accepted);
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(originalEntity));
    EXPECT_EQ(gateway.Stop(original), Scripting::ScriptEcsStopResult::AlreadyAbsent);

    const ECS::EntityHandle replacementEntity = AddScriptEntity(runtime);
    ASSERT_TRUE(Tick(runtime).succeeded);
    const Scripting::ScriptEcsInstanceHandle replacement =
        bridge.FindInstance(runtime.GetSceneRuntimeId(), replacementEntity);
    ASSERT_TRUE(replacement.IsValid());
    EXPECT_NE(replacement, original);
    EXPECT_EQ(gateway.GetAllocatedCount(), 1u);
}

TEST(EcsScriptingValidation, StructuralAndCleanupCursorLossReconcileFromDurableState)
{
    SceneECS::SceneEcsRuntime runtime(1, 1);
    FakeScriptGateway gateway;
    Scripting::ScriptEcsBridge bridge(runtime, gateway);
    ASSERT_EQ(bridge.RegisterProcessors(), Scripting::ScriptEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle first = AddScriptEntity(runtime);
    const ECS::EntityHandle second = AddScriptEntity(runtime);
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.GetAllocatedCount(), 2u);
    EXPECT_GE(bridge.GetDiagnosticsSnapshot().structuralContinuityLossCount, 1u);

    ASSERT_EQ(runtime.RequestDestroy(
                  first, SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::None)),
              SceneECS::DestroyRequestResult::Accepted);
    ASSERT_EQ(runtime.RequestDestroy(
                  second, SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::None)),
              SceneECS::DestroyRequestResult::Accepted);
    ASSERT_EQ(runtime.PublishPendingDestroyCleanup(), 2u);
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(first));
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(second));
    EXPECT_EQ(gateway.GetAllocatedCount(), 0u);
    EXPECT_GE(bridge.GetDiagnosticsSnapshot().cleanupContinuityLossCount, 1u);
}

TEST(EcsScriptingValidation, FailedCleanupAndShutdownRetainEvidenceUntilRetry)
{
    SceneECS::SceneEcsRuntime runtime;
    FakeScriptGateway gateway;
    Scripting::ScriptEcsBridge bridge(runtime, gateway);
    ASSERT_EQ(bridge.RegisterProcessors(), Scripting::ScriptEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddScriptEntity(runtime);
    ASSERT_TRUE(Tick(runtime).succeeded);

    gateway.failedStopAttempts = 1;
    ASSERT_EQ(runtime.RequestDestroy(
                  entity, SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::None)),
              SceneECS::DestroyRequestResult::Accepted);
    EXPECT_FALSE(Tick(runtime).succeeded);
    EXPECT_TRUE(runtime.GetRegistry().IsAlive(entity));
    const auto* pendingLifecycle =
        runtime.GetRegistry().TryGet<SceneECS::EntityLifecycleState>(entity);
    ASSERT_NE(pendingLifecycle, nullptr);
    EXPECT_EQ(pendingLifecycle->phase, SceneECS::EntityLifecyclePhase::PendingDestroy);
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(entity));

    ASSERT_TRUE(AddScriptEntity(runtime).IsValid());
    ASSERT_TRUE(Tick(runtime).succeeded);
    gateway.failedStopAttempts = 1;
    EXPECT_FALSE(bridge.PrepareForShutdown());
    EXPECT_EQ(gateway.GetAllocatedCount(), 1u);
    EXPECT_TRUE(bridge.PrepareForShutdown());
    EXPECT_EQ(gateway.GetAllocatedCount(), 0u);
}

TEST(EcsScriptingValidation, ProcessorRegistrationIsAtomicAndDoesNotInstallLegacyFallback)
{
    SceneECS::SceneEcsRuntime runtime;
    FakeScriptGateway firstGateway;
    FakeScriptGateway secondGateway;
    Scripting::ScriptEcsBridge first(runtime, firstGateway);
    Scripting::ScriptEcsBridge second(runtime, secondGateway);
    ASSERT_EQ(first.RegisterProcessors(), Scripting::ScriptEcsBridgeRegistrationResult::Registered);
    EXPECT_EQ(second.RegisterProcessors(),
              Scripting::ScriptEcsBridgeRegistrationResult::ProcessorRegistrationFailed);
    ASSERT_TRUE(AddScriptEntity(runtime).IsValid());
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(firstGateway.createCalls, 1u);
    EXPECT_EQ(secondGateway.createCalls, 0u);
}

TEST(EcsScriptingValidation, ProductionGatewayRequiresReturnedTableAndRetainsLiveInstanceOnReloadFailure)
{
    SCOPED_TRACE("create temporary script directory");
    ScopedTempDirectory directory;
    if (!directory.available)
    {
        GTEST_SKIP() << "Temporary scripting test directory is unavailable.";
    }
    const std::filesystem::path scriptPath = directory.path / "pure_ecs.lua";
    {
        std::ofstream script(scriptPath);
        ASSERT_TRUE(script.is_open());
        script << "return { OnStart = function(self) self.started = true end, "
                  "OnUpdate = function(self, dt) self.last_dt = dt end, "
                  "OnDestroy = function(self) self.destroyed = true end }";
    }

    ScriptingSubsystem subsystem;
    ScriptingSubsystemConfig config;
    config.scriptsDirectory = directory.path;
    config.enableHotReload = false;
    subsystem.Configure(config);
    SCOPED_TRACE("initialize scripting subsystem");
    subsystem.Initialize();
    ASSERT_TRUE(subsystem.GetLuaState().IsInitialized());

    uint64 sourceRevision = 1;
    Scripting::ScriptEcsEngineGateway gateway(
        subsystem,
        [&sourceRevision](AssetId assetId) -> std::optional<Scripting::ScriptEcsSource>
        {
            if (assetId.value != 900)
            {
                return std::nullopt;
            }
            return Scripting::ScriptEcsSource{
                .relativePath = "pure_ecs.lua",
                .sourceRevision = sourceRevision,
            };
        });
    SCOPED_TRACE("create Lua-backed ECS instance");
    const Scripting::ScriptEcsInstanceHandle instance = gateway.Create({
        .scriptAssetId = {.value = 900},
        .configurationRevision = 1,
    });
    ASSERT_TRUE(instance.IsValid());
    SCOPED_TRACE("start and update Lua-backed ECS instance");
    ASSERT_TRUE(gateway.Start(instance, ECS::SceneRuntimeId(1), ECS::EntityHandle::Create(2, 1)));
    ASSERT_TRUE(gateway.Update(instance, 0.25f));

    {
        std::ofstream script(scriptPath, std::ios::trunc);
        ASSERT_TRUE(script.is_open());
        script << "return 42";
    }
    ++sourceRevision;
    SCOPED_TRACE("reject non-table reload while retaining old instance");
    EXPECT_FALSE(gateway.Reload(instance));
    EXPECT_TRUE(gateway.IsInstanceAlive(instance));
    EXPECT_EQ(gateway.Stop(instance), Scripting::ScriptEcsStopResult::Stopped);
    EXPECT_TRUE(gateway.PrepareForShutdown());
    subsystem.Deinitialize();
}
