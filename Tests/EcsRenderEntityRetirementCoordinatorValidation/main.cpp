#include "EcsRenderEntityRetirementCoordinator.h"

#include "Scene/ECS/RenderFragments.h"
#include "Scene/ECS/RetirementFragments.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <vector>

namespace
{
    using namespace RVX;
    using namespace RVX::ResourceSceneAdapters;
    using namespace RVX::SceneECS;

    class FakeProofGateway final : public IEcsSceneAssetRetirementProofGateway
    {
    public:
        enum class BeginMode : uint8
        {
            Accept = 0,
            Reject,
        };

        explicit FakeProofGateway(const SceneEcsRuntime& runtime)
            : m_runtime(runtime)
        {
        }

        [[nodiscard]] EcsSceneAssetRetirementBeginReceipt BeginRetirement(
            const EcsSceneAssetRetirementRequest& request) override
        {
            ++m_beginCallCount;
            m_requests.push_back(request);
            if (m_beginMode == BeginMode::Reject)
            {
                return {.code = EcsSceneAssetRetirementBeginCode::Rejected,
                        .request = request,
                        .diagnostic = "Injected admission rejection."};
            }

            const EcsSceneAssetRetirementToken token{
                .value = m_nextTokenValue++,
                .generation = 1,
            };
            m_entries.push_back({
                .token = token,
                .request = request,
                .state = m_initialProofState,
            });
            return {.code = EcsSceneAssetRetirementBeginCode::Accepted,
                    .token = token,
                    .request = request};
        }

        [[nodiscard]] EcsSceneAssetRetirementProof QueryRetirementProof(
            EcsSceneAssetRetirementToken token) const override
        {
            const Entry* entry = Find(token);
            if (entry == nullptr)
            {
                return {.state = EcsSceneAssetRetirementProofState::Failed,
                        .diagnostic = "Stale fake proof token."};
            }
            return {.state = entry->state, .request = entry->request};
        }

        [[nodiscard]] bool AcknowledgeRetirementProof(
            EcsSceneAssetRetirementToken token) override
        {
            Entry* entry = Find(token);
            if (entry == nullptr ||
                (entry->state != EcsSceneAssetRetirementProofState::Presented &&
                 entry->state != EcsSceneAssetRetirementProofState::NeverPublished))
            {
                return false;
            }

            ++m_proofAcknowledgementCallCount;
            const EntityLifecycleState* lifecycle =
                m_runtime.get().GetRegistry().TryGet<EntityLifecycleState>(
                    entry->request.members.front());
            m_sceneAcknowledgedBeforeProofAcknowledgement =
                lifecycle != nullptr &&
                (lifecycle->acknowledgedCleanupDomains &
                 ToCleanupDomainMask(CleanupDomain::Render)) != 0;
            if (m_rejectNextProofAcknowledgement)
            {
                m_rejectNextProofAcknowledgement = false;
                return false;
            }

            entry->acknowledged = true;
            return true;
        }

        void SetBeginMode(BeginMode mode) noexcept { m_beginMode = mode; }
        void SetInitialProofState(EcsSceneAssetRetirementProofState state) noexcept
        {
            m_initialProofState = state;
        }
        void SetOnlyProofState(EcsSceneAssetRetirementProofState state)
        {
            ASSERT_EQ(m_entries.size(), 1u);
            m_entries.front().state = state;
        }
        void RejectNextProofAcknowledgement() noexcept
        {
            m_rejectNextProofAcknowledgement = true;
        }

        [[nodiscard]] uint32 GetBeginCallCount() const noexcept { return m_beginCallCount; }
        [[nodiscard]] uint32 GetProofAcknowledgementCallCount() const noexcept
        {
            return m_proofAcknowledgementCallCount;
        }
        [[nodiscard]] bool WasSceneAcknowledgedBeforeProofAcknowledgement() const noexcept
        {
            return m_sceneAcknowledgedBeforeProofAcknowledgement;
        }
        [[nodiscard]] const std::vector<EcsSceneAssetRetirementRequest>& GetRequests() const noexcept
        {
            return m_requests;
        }

    private:
        struct Entry
        {
            EcsSceneAssetRetirementToken token;
            EcsSceneAssetRetirementRequest request;
            EcsSceneAssetRetirementProofState state =
                EcsSceneAssetRetirementProofState::Pending;
            bool acknowledged = false;
        };

        [[nodiscard]] Entry* Find(EcsSceneAssetRetirementToken token) noexcept
        {
            const auto found = std::find_if(m_entries.begin(), m_entries.end(),
                                            [token](const Entry& entry)
                                            {
                                                return entry.token == token;
                                            });
            return found != m_entries.end() ? &*found : nullptr;
        }

        [[nodiscard]] const Entry* Find(EcsSceneAssetRetirementToken token) const noexcept
        {
            return const_cast<FakeProofGateway*>(this)->Find(token);
        }

        std::reference_wrapper<const SceneEcsRuntime> m_runtime;
        std::vector<Entry> m_entries;
        std::vector<EcsSceneAssetRetirementRequest> m_requests;
        BeginMode m_beginMode = BeginMode::Accept;
        EcsSceneAssetRetirementProofState m_initialProofState =
            EcsSceneAssetRetirementProofState::Pending;
        uint64 m_nextTokenValue = 1;
        uint32 m_beginCallCount = 0;
        uint32 m_proofAcknowledgementCallCount = 0;
        bool m_rejectNextProofAcknowledgement = false;
        bool m_sceneAcknowledgedBeforeProofAcknowledgement = false;
    };

    [[nodiscard]] ECS::EntityHandle CreateDirectRenderable(SceneEcsRuntime& runtime)
    {
        const ECS::EntityHandle entity = runtime.CreateEntity();
        EXPECT_TRUE(entity.IsValid());
        EXPECT_TRUE(runtime.AddFragment<SceneECS::Mesh>(entity, {}));
        return entity;
    }

    void RequestRenderRetirement(SceneEcsRuntime& runtime, ECS::EntityHandle entity)
    {
        ASSERT_EQ(runtime.RequestDestroy(
                      entity,
                      ToCleanupDomainMask(CleanupDomain::Render)),
                  DestroyRequestResult::Accepted);
    }

    void TickSuccessfully(SceneEcsRuntime& runtime)
    {
        EXPECT_TRUE(runtime.Tick().succeeded);
    }
} // namespace

TEST(EcsRenderEntityRetirementCoordinatorValidation,
     PendingProofBecomesPresentedThenSceneAcknowledgesBeforeTokenRelease)
{
    SceneEcsRuntime runtime;
    FakeProofGateway gateway(runtime);
    EcsRenderEntityRetirementCoordinator coordinator(runtime, gateway);
    const ECS::EntityHandle entity = CreateDirectRenderable(runtime);
    RequestRenderRetirement(runtime, entity);

    TickSuccessfully(runtime);
    ASSERT_TRUE(coordinator.HasOutstandingRetirements());
    ASSERT_EQ(gateway.GetBeginCallCount(), 1u);
    ASSERT_EQ(gateway.GetRequests().size(), 1u);
    EXPECT_EQ(gateway.GetRequests().front().sceneRuntimeId, runtime.GetSceneRuntimeId());
    EXPECT_EQ(gateway.GetRequests().front().rootEntity, entity);
    ASSERT_EQ(gateway.GetRequests().front().members.size(), 1u);
    EXPECT_EQ(gateway.GetRequests().front().members.front(), entity);

    gateway.SetOnlyProofState(EcsSceneAssetRetirementProofState::Presented);
    TickSuccessfully(runtime);
    EXPECT_TRUE(coordinator.HasOutstandingRetirements());
    TickSuccessfully(runtime);

    EXPECT_FALSE(coordinator.HasOutstandingRetirements());
    EXPECT_TRUE(gateway.WasSceneAcknowledgedBeforeProofAcknowledgement());
    EXPECT_EQ(gateway.GetProofAcknowledgementCallCount(), 1u);
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(entity));
}

TEST(EcsRenderEntityRetirementCoordinatorValidation,
     NeverPublishedIsExplicitlyAcknowledgedWithoutFabricatingPresentation)
{
    SceneEcsRuntime runtime;
    FakeProofGateway gateway(runtime);
    gateway.SetInitialProofState(EcsSceneAssetRetirementProofState::NeverPublished);
    EcsRenderEntityRetirementCoordinator coordinator(runtime, gateway);
    const ECS::EntityHandle entity = CreateDirectRenderable(runtime);
    RequestRenderRetirement(runtime, entity);

    TickSuccessfully(runtime);
    TickSuccessfully(runtime);
    TickSuccessfully(runtime);

    const EcsRenderEntityRetirementDiagnostics diagnostics = coordinator.GetDiagnostics();
    EXPECT_EQ(diagnostics.neverPublishedCount, 1u);
    EXPECT_EQ(gateway.GetProofAcknowledgementCallCount(), 1u);
    EXPECT_TRUE(gateway.WasSceneAcknowledgedBeforeProofAcknowledgement());
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(entity));
}

TEST(EcsRenderEntityRetirementCoordinatorValidation,
     SpecializedMarkerExcludesAssetOwnedInstanceMembers)
{
    SceneEcsRuntime runtime;
    FakeProofGateway gateway(runtime);
    EcsRenderEntityRetirementCoordinator coordinator(runtime, gateway);
    const ECS::EntityHandle entity = CreateDirectRenderable(runtime);
    ASSERT_TRUE(runtime.AddFragment<ECS::Tag<SpecializedRenderRetirement>>(entity));
    RequestRenderRetirement(runtime, entity);

    TickSuccessfully(runtime);

    EXPECT_EQ(gateway.GetBeginCallCount(), 0u);
    EXPECT_FALSE(coordinator.HasOutstandingRetirements());
    EXPECT_GE(coordinator.GetDiagnostics().specializedExclusionCount, 1u);
    const EntityLifecycleState* lifecycle =
        runtime.GetRegistry().TryGet<EntityLifecycleState>(entity);
    ASSERT_NE(lifecycle, nullptr);
    EXPECT_EQ(lifecycle->phase, EntityLifecyclePhase::CleanupRequired);
    EXPECT_TRUE(runtime.AcknowledgeCleanup(entity, ToCleanupDomainMask(CleanupDomain::Render)));
    TickSuccessfully(runtime);
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(entity));
}

TEST(EcsRenderEntityRetirementCoordinatorValidation,
     CleanupJournalLossRebuildsFromLiveLifecycleAuthority)
{
    SceneEcsRuntime runtime(1);
    FakeProofGateway gateway(runtime);
    EcsRenderEntityRetirementCoordinator coordinator(runtime, gateway);

    const ECS::EntityHandle expired = runtime.CreateEntity();
    ASSERT_EQ(runtime.RequestDestroy(
                  expired,
                  ToCleanupDomainMask(CleanupDomain::None)),
              DestroyRequestResult::Accepted);
    ASSERT_EQ(runtime.PublishPendingDestroyCleanup(), 1u);
    ASSERT_EQ(runtime.AdvanceRetirements(), 1u);
    ASSERT_EQ(runtime.RecycleRecyclableEntities(), 1u);

    const ECS::EntityHandle entity = CreateDirectRenderable(runtime);
    RequestRenderRetirement(runtime, entity);
    ASSERT_EQ(runtime.PublishPendingDestroyCleanup(), 1u);

    TickSuccessfully(runtime);

    const EcsRenderEntityRetirementDiagnostics diagnostics = coordinator.GetDiagnostics();
    EXPECT_EQ(diagnostics.cleanupContinuityLossCount, 1u);
    EXPECT_EQ(diagnostics.authoritativeRebuildCount, 1u);
    EXPECT_EQ(gateway.GetBeginCallCount(), 1u);
    ASSERT_EQ(gateway.GetRequests().size(), 1u);
    EXPECT_EQ(gateway.GetRequests().front().members.front(), entity);
}

TEST(EcsRenderEntityRetirementCoordinatorValidation,
     AdmissionFailureIsRetainedAndRetriedDurably)
{
    SceneEcsRuntime runtime;
    FakeProofGateway gateway(runtime);
    gateway.SetBeginMode(FakeProofGateway::BeginMode::Reject);
    EcsRenderEntityRetirementCoordinator coordinator(runtime, gateway);
    const ECS::EntityHandle entity = CreateDirectRenderable(runtime);
    RequestRenderRetirement(runtime, entity);

    TickSuccessfully(runtime);
    EXPECT_TRUE(coordinator.HasOutstandingRetirements());
    EXPECT_EQ(gateway.GetBeginCallCount(), 1u);
    EXPECT_EQ(coordinator.GetDiagnostics().retirementAdmissionFailureCount, 1u);

    gateway.SetBeginMode(FakeProofGateway::BeginMode::Accept);
    TickSuccessfully(runtime);
    EXPECT_EQ(gateway.GetBeginCallCount(), 2u);
    gateway.SetOnlyProofState(EcsSceneAssetRetirementProofState::Presented);
    TickSuccessfully(runtime);
    TickSuccessfully(runtime);

    EXPECT_FALSE(coordinator.HasOutstandingRetirements());
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(entity));
}

TEST(EcsRenderEntityRetirementCoordinatorValidation,
     FailedProofQueryIsRetainedAndRetriedWithoutDroppingTheToken)
{
    SceneEcsRuntime runtime;
    FakeProofGateway gateway(runtime);
    EcsRenderEntityRetirementCoordinator coordinator(runtime, gateway);
    const ECS::EntityHandle entity = CreateDirectRenderable(runtime);
    RequestRenderRetirement(runtime, entity);

    TickSuccessfully(runtime);
    gateway.SetOnlyProofState(EcsSceneAssetRetirementProofState::Failed);
    TickSuccessfully(runtime);
    EXPECT_TRUE(coordinator.HasOutstandingRetirements());
    EXPECT_EQ(coordinator.GetDiagnostics().retirementQueryFailureCount, 1u);

    gateway.SetOnlyProofState(EcsSceneAssetRetirementProofState::Presented);
    TickSuccessfully(runtime);
    TickSuccessfully(runtime);
    EXPECT_FALSE(coordinator.HasOutstandingRetirements());
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(entity));
}

TEST(EcsRenderEntityRetirementCoordinatorValidation,
     TokenAcknowledgementFailureRetainsProofAfterSceneAcknowledgement)
{
    SceneEcsRuntime runtime;
    FakeProofGateway gateway(runtime);
    EcsRenderEntityRetirementCoordinator coordinator(runtime, gateway);
    const ECS::EntityHandle entity = CreateDirectRenderable(runtime);
    RequestRenderRetirement(runtime, entity);

    TickSuccessfully(runtime);
    gateway.SetOnlyProofState(EcsSceneAssetRetirementProofState::Presented);
    TickSuccessfully(runtime);
    gateway.RejectNextProofAcknowledgement();
    TickSuccessfully(runtime);

    EXPECT_TRUE(coordinator.HasOutstandingRetirements());
    EXPECT_TRUE(gateway.WasSceneAcknowledgedBeforeProofAcknowledgement());
    EXPECT_EQ(gateway.GetProofAcknowledgementCallCount(), 1u);
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(entity));

    TickSuccessfully(runtime);
    EXPECT_FALSE(coordinator.HasOutstandingRetirements());
    EXPECT_EQ(gateway.GetProofAcknowledgementCallCount(), 2u);
}

TEST(EcsRenderEntityRetirementCoordinatorValidation,
     DeviceLossIsDiagnosedAndRetainsTheSceneCleanupGate)
{
    SceneEcsRuntime runtime;
    FakeProofGateway gateway(runtime);
    EcsRenderEntityRetirementCoordinator coordinator(runtime, gateway);
    const ECS::EntityHandle entity = CreateDirectRenderable(runtime);
    RequestRenderRetirement(runtime, entity);

    TickSuccessfully(runtime);
    gateway.SetOnlyProofState(EcsSceneAssetRetirementProofState::DeviceLost);
    TickSuccessfully(runtime);

    EXPECT_TRUE(coordinator.HasOutstandingRetirements());
    EXPECT_EQ(coordinator.GetDiagnostics().deviceLostCount, 1u);
    EXPECT_EQ(gateway.GetProofAcknowledgementCallCount(), 0u);
    EXPECT_TRUE(runtime.GetRegistry().IsAlive(entity));
}

TEST(EcsRenderEntityRetirementCoordinatorValidation,
     StaleGenerationDoesNotStartANewProofForAReusedSlot)
{
    SceneEcsRuntime runtime;
    FakeProofGateway gateway(runtime);
    EcsRenderEntityRetirementCoordinator coordinator(runtime, gateway);
    const ECS::EntityHandle stale = CreateDirectRenderable(runtime);
    RequestRenderRetirement(runtime, stale);
    ASSERT_EQ(runtime.PublishPendingDestroyCleanup(), 1u);
    ASSERT_TRUE(runtime.AcknowledgeCleanup(stale, ToCleanupDomainMask(CleanupDomain::Render)));
    ASSERT_EQ(runtime.AdvanceRetirements(), 1u);
    ASSERT_EQ(runtime.RecycleRecyclableEntities(), 1u);

    const ECS::EntityHandle replacement = runtime.CreateEntity();
    ASSERT_TRUE(replacement.IsValid());
    EXPECT_EQ(replacement.GetIndex(), stale.GetIndex());
    EXPECT_NE(replacement.GetGeneration(), stale.GetGeneration());
    TickSuccessfully(runtime);

    EXPECT_EQ(gateway.GetBeginCallCount(), 0u);
    EXPECT_GE(coordinator.GetDiagnostics().staleCleanupRecordCount, 1u);
}

TEST(EcsRenderEntityRetirementCoordinatorValidation,
     ShutdownIsFalseUntilOutstandingProofHasBeenDrained)
{
    SceneEcsRuntime runtime;
    FakeProofGateway gateway(runtime);
    EcsRenderEntityRetirementCoordinator coordinator(runtime, gateway);
    const ECS::EntityHandle entity = CreateDirectRenderable(runtime);
    RequestRenderRetirement(runtime, entity);

    TickSuccessfully(runtime);
    EXPECT_FALSE(coordinator.PrepareForShutdown());
    EXPECT_TRUE(coordinator.GetDiagnostics().shutdownRequested);

    gateway.SetOnlyProofState(EcsSceneAssetRetirementProofState::Presented);
    TickSuccessfully(runtime);
    TickSuccessfully(runtime);
    EXPECT_TRUE(coordinator.PrepareForShutdown());
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(entity));
}

TEST(EcsRenderEntityRetirementCoordinatorValidation,
     DestructorDoesNotDiscardOutstandingProofCapturedByTheRegisteredProcessor)
{
    SceneEcsRuntime runtime;
    FakeProofGateway gateway(runtime);
    const ECS::EntityHandle entity = CreateDirectRenderable(runtime);
    RequestRenderRetirement(runtime, entity);

    {
        EcsRenderEntityRetirementCoordinator coordinator(runtime, gateway);
        TickSuccessfully(runtime);
        EXPECT_TRUE(coordinator.HasOutstandingRetirements());
    }

    gateway.SetOnlyProofState(EcsSceneAssetRetirementProofState::Presented);
    TickSuccessfully(runtime);
    TickSuccessfully(runtime);

    EXPECT_EQ(gateway.GetProofAcknowledgementCallCount(), 1u);
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(entity));
}
