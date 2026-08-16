#include "Audio/ECS/AudioEcsBridge.h"

#include <gtest/gtest.h>

#include <limits>
#include <vector>

namespace
{
    using namespace RVX;

    class FakeAudioGateway final : public Audio::IAudioEcsPlaybackGateway
    {
    public:
        struct Entry
        {
            uint32 generation = 1;
            bool allocated = false;
            bool alive = false;
        };

        [[nodiscard]] Audio::AudioEcsPlaybackHandle Play(
            const Audio::AudioEcsPlayRequest& request) override
        {
            ++playCalls;
            lastPlayRequest = request;
            playRequests.push_back(request);
            if (!allowPlay)
            {
                return Audio::AudioEcsPlaybackHandle::Invalid();
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
            entries[index].alive = !returnValidButAbsentOnNextPlay;
            returnValidButAbsentOnNextPlay = false;
            return Audio::AudioEcsPlaybackHandle::Create(index, entries[index].generation);
        }

        [[nodiscard]] bool Update(Audio::AudioEcsPlaybackHandle handle,
                                  const Audio::AudioEcsPlaybackUpdate& update) override
        {
            ++updateCalls;
            lastUpdate = update;
            return allowUpdate && IsPlaybackAlive(handle);
        }

        [[nodiscard]] Audio::AudioEcsStopResult Stop(Audio::AudioEcsPlaybackHandle handle) override
        {
            ++stopCalls;
            if (!handle.IsValid() || handle.GetIndex() >= entries.size())
            {
                return Audio::AudioEcsStopResult::AlreadyAbsent;
            }

            Entry& entry = entries[handle.GetIndex()];
            if (!entry.allocated || entry.generation != handle.GetGeneration())
            {
                return Audio::AudioEcsStopResult::AlreadyAbsent;
            }
            if (failedStopAttempts != 0)
            {
                --failedStopAttempts;
                return Audio::AudioEcsStopResult::Failed;
            }

            const bool wasAlive = entry.alive;
            entry.allocated = false;
            entry.alive = false;
            ++entry.generation;
            if (entry.generation == 0)
            {
                ++entry.generation;
            }
            freeEntries.push_back(handle.GetIndex());
            return wasAlive ? Audio::AudioEcsStopResult::Stopped :
                             Audio::AudioEcsStopResult::AlreadyAbsent;
        }

        [[nodiscard]] bool IsPlaybackAlive(Audio::AudioEcsPlaybackHandle handle) const override
        {
            return handle.IsValid() && handle.GetIndex() < entries.size() &&
                   entries[handle.GetIndex()].allocated && entries[handle.GetIndex()].alive &&
                   entries[handle.GetIndex()].generation == handle.GetGeneration();
        }

        [[nodiscard]] uint32 GetLiveCount() const
        {
            uint32 count = 0;
            for (const Entry& entry : entries)
            {
                count += entry.alive ? 1u : 0u;
            }
            return count;
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

        bool allowPlay = true;
        bool allowUpdate = true;
        bool returnValidButAbsentOnNextPlay = false;
        uint32 failedStopAttempts = 0;
        uint32 playCalls = 0;
        uint32 updateCalls = 0;
        uint32 stopCalls = 0;
        Audio::AudioEcsPlayRequest lastPlayRequest;
        Audio::AudioEcsPlaybackUpdate lastUpdate;
        std::vector<Audio::AudioEcsPlayRequest> playRequests;

    private:
        std::vector<Entry> entries;
        std::vector<uint32> freeEntries;
    };

    [[nodiscard]] SceneECS::SceneEcsTickResult Tick(SceneECS::SceneEcsRuntime& runtime)
    {
        return runtime.Tick({
            .variableDeltaSeconds = 1.0 / 60.0,
        });
    }

    [[nodiscard]] ECS::EntityHandle AddAudioEntity(SceneECS::SceneEcsRuntime& runtime,
                                                    bool playOnStart = true)
    {
        const ECS::EntityHandle entity = runtime.CreateEntity();
        if (!entity.IsValid())
        {
            return ECS::EntityHandle::Invalid();
        }

        SceneECS::AudioEmitter emitter;
        emitter.audioAssetId = {.value = 100};
        emitter.playOnStart = playOnStart;
        if (!runtime.AddFragment<SceneECS::AudioEmitter>(entity, emitter) ||
            !runtime.AddFragment<SceneECS::AudioPlaybackIntent>(entity) ||
            !runtime.AddFragment<SceneECS::AudioPlaybackState>(entity))
        {
            return ECS::EntityHandle::Invalid();
        }
        return entity;
    }

    void SetIntent(SceneECS::SceneEcsRuntime& runtime,
                   ECS::EntityHandle entity,
                   SceneECS::AudioPlaybackCommand command,
                   uint64 sequence)
    {
        ASSERT_TRUE(runtime.SetFragment<SceneECS::AudioPlaybackIntent>(
            entity, {.command = command, .sequence = sequence}));
    }
} // namespace

TEST(EcsAudioValidation, PlayUpdatesResolvedPositionAndStops)
{
    SceneECS::SceneEcsRuntime runtime;
    FakeAudioGateway gateway;
    Audio::AudioEcsBridge bridge(runtime, gateway);
    ASSERT_EQ(bridge.RegisterProcessors(), Audio::AudioEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddAudioEntity(runtime);
    ASSERT_TRUE(entity.IsValid());

    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.playCalls, 1u);
    EXPECT_EQ(gateway.GetLiveCount(), 1u);
    EXPECT_EQ(bridge.GetDiagnosticsSnapshot().activePlaybackCount, 1u);

    SceneECS::LocalTransform transform;
    transform.translation = {4.0f, 5.0f, 6.0f};
    ASSERT_TRUE(runtime.SetLocalTransform(entity, transform));
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_FLOAT_EQ(gateway.lastUpdate.worldPosition.x, 4.0f);
    EXPECT_FLOAT_EQ(gateway.lastUpdate.worldPosition.y, 5.0f);
    EXPECT_FLOAT_EQ(gateway.lastUpdate.worldPosition.z, 6.0f);

    SetIntent(runtime, entity, SceneECS::AudioPlaybackCommand::Stop, 1);
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.GetLiveCount(), 0u);
    EXPECT_FALSE(bridge.FindPlayback(runtime.GetSceneRuntimeId(), entity).IsValid());
    const auto* playbackState = runtime.GetRegistry().TryGet<SceneECS::AudioPlaybackState>(entity);
    ASSERT_NE(playbackState, nullptr);
    EXPECT_EQ(playbackState->lastConsumedCommandSequence, 1u);
    EXPECT_EQ(playbackState->status, SceneECS::AudioBindingStatus::Stopped);
}

TEST(EcsAudioValidation, ConfigurationRevisionRestartsAndPublishesOnlyTheAppliedRevision)
{
    SceneECS::SceneEcsRuntime runtime;
    FakeAudioGateway gateway;
    Audio::AudioEcsBridge bridge(runtime, gateway);
    ASSERT_EQ(bridge.RegisterProcessors(), Audio::AudioEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddAudioEntity(runtime);
    ASSERT_TRUE(Tick(runtime).succeeded);
    ASSERT_EQ(gateway.playCalls, 1u);
    ASSERT_EQ(gateway.playRequests.size(), 1u);
    const auto* initialState = runtime.GetRegistry().TryGet<SceneECS::AudioPlaybackState>(entity);
    ASSERT_NE(initialState, nullptr);
    EXPECT_EQ(initialState->lastAppliedConfigurationRevision, 1u);

    SceneECS::AudioEmitter updated = gateway.playRequests.front().emitter;
    updated.configurationRevision = 2;
    updated.minDistance = 3.0f;
    updated.maxDistance = 40.0f;
    updated.rolloffFactor = 0.25f;
    updated.attenuation = SceneECS::AudioAttenuationMode::Linear;
    updated.coneInnerAngleDegrees = 30.0f;
    updated.coneOuterAngleDegrees = 70.0f;
    updated.coneOuterGain = 0.4f;
    ASSERT_TRUE(runtime.SetFragment(entity, updated));
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.playCalls, 2u);
    EXPECT_EQ(gateway.stopCalls, 1u);
    ASSERT_EQ(gateway.playRequests.size(), 2u);
    const Audio::AudioEcsPlayRequest& restarted = gateway.playRequests.back();
    EXPECT_EQ(restarted.emitter.configurationRevision, 2u);
    EXPECT_FLOAT_EQ(restarted.emitter.minDistance, 3.0f);
    EXPECT_FLOAT_EQ(restarted.emitter.maxDistance, 40.0f);
    EXPECT_FLOAT_EQ(restarted.emitter.rolloffFactor, 0.25f);
    EXPECT_EQ(restarted.emitter.attenuation, SceneECS::AudioAttenuationMode::Linear);
    EXPECT_FLOAT_EQ(restarted.emitter.coneInnerAngleDegrees, 30.0f);
    EXPECT_FLOAT_EQ(restarted.emitter.coneOuterAngleDegrees, 70.0f);
    EXPECT_FLOAT_EQ(restarted.emitter.coneOuterGain, 0.4f);
    const auto* restartedState = runtime.GetRegistry().TryGet<SceneECS::AudioPlaybackState>(entity);
    ASSERT_NE(restartedState, nullptr);
    EXPECT_EQ(restartedState->lastAppliedConfigurationRevision, 2u);

    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.playCalls, 2u);
    EXPECT_EQ(gateway.stopCalls, 1u);
}

TEST(EcsAudioValidation, NoneCommandWithRetainedSequenceIsANoop)
{
    SceneECS::SceneEcsRuntime runtime;
    FakeAudioGateway gateway;
    Audio::AudioEcsBridge bridge(runtime, gateway);
    ASSERT_EQ(bridge.RegisterProcessors(), Audio::AudioEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddAudioEntity(runtime);
    ASSERT_TRUE(Tick(runtime).succeeded);
    ASSERT_EQ(gateway.playCalls, 1u);

    SetIntent(runtime, entity, SceneECS::AudioPlaybackCommand::None, 44);
    ASSERT_TRUE(Tick(runtime).succeeded);
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.playCalls, 1u);
    EXPECT_EQ(gateway.GetLiveCount(), 1u);
    const auto* state = runtime.GetRegistry().TryGet<SceneECS::AudioPlaybackState>(entity);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->status, SceneECS::AudioBindingStatus::Active);
    EXPECT_EQ(state->lastConsumedCommandSequence, 0u);
    EXPECT_EQ(bridge.GetDiagnosticsSnapshot().rejectedCommandCount, 0u);
}

TEST(EcsAudioValidation, ValidButAbsentPlayHandleRetainsAndRetriesReleaseEvidence)
{
    SceneECS::SceneEcsRuntime runtime;
    FakeAudioGateway gateway;
    gateway.returnValidButAbsentOnNextPlay = true;
    gateway.failedStopAttempts = 1;
    Audio::AudioEcsBridge bridge(runtime, gateway);
    ASSERT_EQ(bridge.RegisterProcessors(), Audio::AudioEcsBridgeRegistrationResult::Registered);
    ASSERT_TRUE(AddAudioEntity(runtime).IsValid());

    EXPECT_FALSE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.GetLiveCount(), 0u);
    EXPECT_EQ(gateway.GetAllocatedCount(), 1u);
    const auto failedDiagnostics = bridge.GetDiagnosticsSnapshot();
    EXPECT_EQ(failedDiagnostics.playbackReleaseFailureCount, 1u);
    EXPECT_EQ(failedDiagnostics.outstandingPlaybackCount, 1u);

    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.GetAllocatedCount(), 0u);
    const auto retriedDiagnostics = bridge.GetDiagnosticsSnapshot();
    EXPECT_EQ(retriedDiagnostics.outstandingPlaybackCount, 0u);
}

TEST(EcsAudioValidation, PrepareForShutdownRetainsFailedStopEvidenceUntilRetried)
{
    SceneECS::SceneEcsRuntime runtime;
    FakeAudioGateway gateway;
    Audio::AudioEcsBridge bridge(runtime, gateway);
    ASSERT_EQ(bridge.RegisterProcessors(), Audio::AudioEcsBridgeRegistrationResult::Registered);
    ASSERT_TRUE(AddAudioEntity(runtime).IsValid());
    ASSERT_TRUE(Tick(runtime).succeeded);
    ASSERT_EQ(gateway.GetLiveCount(), 1u);

    gateway.failedStopAttempts = 1;
    EXPECT_FALSE(bridge.PrepareForShutdown());
    const auto failedDiagnostics = bridge.GetDiagnosticsSnapshot();
    EXPECT_EQ(failedDiagnostics.outstandingPlaybackCount, 1u);
    EXPECT_EQ(failedDiagnostics.shutdownPreparationCount, 1u);
    EXPECT_EQ(failedDiagnostics.shutdownStopFailureCount, 1u);
    EXPECT_EQ(gateway.GetLiveCount(), 1u);

    EXPECT_TRUE(bridge.PrepareForShutdown());
    const auto drainedDiagnostics = bridge.GetDiagnosticsSnapshot();
    EXPECT_EQ(drainedDiagnostics.outstandingPlaybackCount, 0u);
    EXPECT_EQ(drainedDiagnostics.shutdownPreparationCount, 2u);
    EXPECT_EQ(drainedDiagnostics.shutdownStopFailureCount, 1u);
    EXPECT_EQ(gateway.GetLiveCount(), 0u);
}

TEST(EcsAudioValidation, StaleEntityAndGenerationHandleCannotControlReplacementPlayback)
{
    SceneECS::SceneEcsRuntime runtime;
    FakeAudioGateway gateway;
    Audio::AudioEcsBridge bridge(runtime, gateway);
    ASSERT_EQ(bridge.RegisterProcessors(), Audio::AudioEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle originalEntity = AddAudioEntity(runtime);
    ASSERT_TRUE(Tick(runtime).succeeded);
    const Audio::AudioEcsPlaybackHandle originalPlayback =
        bridge.FindPlayback(runtime.GetSceneRuntimeId(), originalEntity);
    ASSERT_TRUE(originalPlayback.IsValid());

    ASSERT_EQ(runtime.RequestDestroy(
                  originalEntity, SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Audio)),
              SceneECS::DestroyRequestResult::Accepted);
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(originalEntity));
    EXPECT_FALSE(bridge.FindPlayback(runtime.GetSceneRuntimeId(), originalEntity).IsValid());
    EXPECT_EQ(gateway.GetLiveCount(), 0u);

    const ECS::EntityHandle replacementEntity = AddAudioEntity(runtime);
    ASSERT_TRUE(replacementEntity.IsValid());
    EXPECT_EQ(replacementEntity.GetIndex(), originalEntity.GetIndex());
    EXPECT_NE(replacementEntity.GetGeneration(), originalEntity.GetGeneration());
    ASSERT_TRUE(Tick(runtime).succeeded);
    const Audio::AudioEcsPlaybackHandle replacementPlayback =
        bridge.FindPlayback(runtime.GetSceneRuntimeId(), replacementEntity);
    ASSERT_TRUE(replacementPlayback.IsValid());
    EXPECT_NE(replacementPlayback, originalPlayback);
    EXPECT_EQ(gateway.Stop(originalPlayback), Audio::AudioEcsStopResult::AlreadyAbsent);
    EXPECT_EQ(gateway.GetLiveCount(), 1u);
}

TEST(EcsAudioValidation, DisableStopsAndReactivationReplaysPlayOnStart)
{
    SceneECS::SceneEcsRuntime runtime;
    FakeAudioGateway gateway;
    Audio::AudioEcsBridge bridge(runtime, gateway);
    ASSERT_EQ(bridge.RegisterProcessors(), Audio::AudioEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddAudioEntity(runtime);
    ASSERT_TRUE(Tick(runtime).succeeded);
    ASSERT_EQ(gateway.playCalls, 1u);

    ASSERT_TRUE(runtime.SetActive(entity, false));
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.GetLiveCount(), 0u);
    const auto* inactiveState = runtime.GetRegistry().TryGet<SceneECS::AudioPlaybackState>(entity);
    ASSERT_NE(inactiveState, nullptr);
    EXPECT_EQ(inactiveState->status, SceneECS::AudioBindingStatus::Inactive);

    ASSERT_TRUE(runtime.SetActive(entity, true));
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.playCalls, 2u);
    EXPECT_EQ(gateway.GetLiveCount(), 1u);
}

TEST(EcsAudioValidation, CommandsAreConsumedExactlyOnceAfterTheirEffectsSucceed)
{
    SceneECS::SceneEcsRuntime runtime;
    FakeAudioGateway gateway;
    Audio::AudioEcsBridge bridge(runtime, gateway);
    ASSERT_EQ(bridge.RegisterProcessors(), Audio::AudioEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddAudioEntity(runtime, false);
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.playCalls, 0u);

    SetIntent(runtime, entity, SceneECS::AudioPlaybackCommand::Play, 12);
    ASSERT_TRUE(Tick(runtime).succeeded);
    ASSERT_EQ(gateway.playCalls, 1u);
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.playCalls, 1u);

    SetIntent(runtime, entity, SceneECS::AudioPlaybackCommand::Stop, 12);
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.GetLiveCount(), 1u);

    SetIntent(runtime, entity, SceneECS::AudioPlaybackCommand::Stop, 13);
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.GetLiveCount(), 0u);
    const uint32 stopCalls = gateway.stopCalls;
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.stopCalls, stopCalls);
    const auto* playbackState = runtime.GetRegistry().TryGet<SceneECS::AudioPlaybackState>(entity);
    ASSERT_NE(playbackState, nullptr);
    EXPECT_EQ(playbackState->lastConsumedCommandSequence, 13u);
}

TEST(EcsAudioValidation, InvalidEnumsAndNonFiniteConfigurationFailClosed)
{
    SceneECS::SceneEcsRuntime runtime;
    FakeAudioGateway gateway;
    Audio::AudioEcsBridge bridge(runtime, gateway);
    ASSERT_EQ(bridge.RegisterProcessors(), Audio::AudioEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddAudioEntity(runtime);
    SceneECS::AudioEmitter invalidEmitter;
    invalidEmitter.audioAssetId = {.value = 100};
    invalidEmitter.playOnStart = true;
    invalidEmitter.volume = std::numeric_limits<float>::quiet_NaN();
    ASSERT_TRUE(runtime.SetFragment(entity, invalidEmitter));
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.playCalls, 0u);
    const auto* invalidState = runtime.GetRegistry().TryGet<SceneECS::AudioPlaybackState>(entity);
    ASSERT_NE(invalidState, nullptr);
    EXPECT_EQ(invalidState->status, SceneECS::AudioBindingStatus::InvalidConfiguration);

    invalidEmitter.volume = 1.0f;
    ASSERT_TRUE(runtime.SetFragment(entity, invalidEmitter));
    SetIntent(runtime, entity, static_cast<SceneECS::AudioPlaybackCommand>(99), 1);
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_EQ(gateway.playCalls, 0u);
    const auto* commandState = runtime.GetRegistry().TryGet<SceneECS::AudioPlaybackState>(entity);
    ASSERT_NE(commandState, nullptr);
    EXPECT_EQ(commandState->status, SceneECS::AudioBindingStatus::InvalidConfiguration);
}

TEST(EcsAudioValidation, StructuralJournalLossRebuildsAllEligiblePlaybacks)
{
    SceneECS::SceneEcsRuntime runtime(8192, 1);
    FakeAudioGateway gateway;
    Audio::AudioEcsBridge bridge(runtime, gateway);
    ASSERT_EQ(bridge.RegisterProcessors(), Audio::AudioEcsBridgeRegistrationResult::Registered);
    ASSERT_TRUE(AddAudioEntity(runtime).IsValid());
    ASSERT_TRUE(AddAudioEntity(runtime).IsValid());

    ASSERT_TRUE(Tick(runtime).succeeded);
    const auto diagnostics = bridge.GetDiagnosticsSnapshot();
    EXPECT_GE(diagnostics.structuralContinuityLossCount, 1u);
    EXPECT_GE(diagnostics.authoritativeReconcileCount, 1u);
    EXPECT_EQ(diagnostics.activePlaybackCount, 2u);
    EXPECT_EQ(gateway.GetLiveCount(), 2u);
}

TEST(EcsAudioValidation, CleanupJournalLossRebuildsDurableAudioCleanup)
{
    SceneECS::SceneEcsRuntime runtime(1, 8192);
    FakeAudioGateway gateway;
    Audio::AudioEcsBridge bridge(runtime, gateway);
    ASSERT_EQ(bridge.RegisterProcessors(), Audio::AudioEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle first = AddAudioEntity(runtime);
    const ECS::EntityHandle second = AddAudioEntity(runtime);
    ASSERT_TRUE(Tick(runtime).succeeded);
    ASSERT_EQ(gateway.GetLiveCount(), 2u);

    ASSERT_EQ(runtime.RequestDestroy(
                  first, SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Audio)),
              SceneECS::DestroyRequestResult::Accepted);
    ASSERT_EQ(runtime.RequestDestroy(
                  second, SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Audio)),
              SceneECS::DestroyRequestResult::Accepted);
    ASSERT_EQ(runtime.PublishPendingDestroyCleanup(), 2u);
    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(first));
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(second));
    EXPECT_EQ(gateway.GetLiveCount(), 0u);
    EXPECT_GE(bridge.GetDiagnosticsSnapshot().cleanupContinuityLossCount, 1u);
}

TEST(EcsAudioValidation, FailedCleanupStopRetriesWithoutAcknowledgingAGhost)
{
    SceneECS::SceneEcsRuntime runtime;
    FakeAudioGateway gateway;
    Audio::AudioEcsBridge bridge(runtime, gateway);
    ASSERT_EQ(bridge.RegisterProcessors(), Audio::AudioEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddAudioEntity(runtime);
    ASSERT_TRUE(Tick(runtime).succeeded);
    ASSERT_EQ(gateway.GetLiveCount(), 1u);

    gateway.failedStopAttempts = 1;
    ASSERT_EQ(runtime.RequestDestroy(
                  entity, SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Audio)),
              SceneECS::DestroyRequestResult::Accepted);
    EXPECT_FALSE(Tick(runtime).succeeded);
    EXPECT_TRUE(runtime.GetRegistry().IsAlive(entity));
    EXPECT_EQ(gateway.GetLiveCount(), 1u);

    ASSERT_TRUE(Tick(runtime).succeeded);
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(entity));
    EXPECT_EQ(gateway.GetLiveCount(), 0u);
}

TEST(EcsAudioValidation, BridgeDestructionStopsAllOwnedGatewayPlaybacks)
{
    SceneECS::SceneEcsRuntime runtime;
    FakeAudioGateway gateway;
    const ECS::EntityHandle entity = AddAudioEntity(runtime);
    ASSERT_TRUE(entity.IsValid());
    {
        Audio::AudioEcsBridge bridge(runtime, gateway);
        ASSERT_EQ(bridge.RegisterProcessors(), Audio::AudioEcsBridgeRegistrationResult::Registered);
        ASSERT_TRUE(Tick(runtime).succeeded);
        ASSERT_EQ(gateway.GetLiveCount(), 1u);
    }
    EXPECT_EQ(gateway.GetLiveCount(), 0u);
    EXPECT_TRUE(Tick(runtime).succeeded);
}
