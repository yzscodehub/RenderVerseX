#include "Scenes/PhysicsSandboxSample.h"

#include "Samples/SampleCLI.h"
#include "Samples/SampleSceneLifetimeScope.h"
#include "Scene/ECS/PhysicsFragments.h"
#include "Scene/ECS/RenderFragments.h"
#include "Scene/ECS/SceneEcsRuntime.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include <gtest/gtest.h>

namespace RVX
{
    class PhysicsSandboxSampleValidationAccess final
    {
    public:
        static SampleReport CompleteCreateActionAgainstRetainedSceneRevision(
            PhysicsSandboxSample& sample)
        {
            sample.m_lastRuntime.sceneFrameSequence = 156u;
            sample.m_lastObservedPresentationSequence = 42u;
            auto& action = sample.GetAction(
                PhysicsSandboxSample::ScenarioAction::CreateBodies);
            action.name = PhysicsSandboxSample::GetActionName(
                PhysicsSandboxSample::ScenarioAction::CreateBodies);
            action.prerequisitesSatisfied = true;
            sample.ArmActionForPresentation(
                PhysicsSandboxSample::ScenarioAction::CreateBodies);

            SampleRenderDiagnostics diagnostics;
            diagnostics.renderSceneValuesAvailable = true;
            diagnostics.renderSceneAppliedRevision = 7u;
            diagnostics.lastPresentedFrameSequence =
                DiagnosticValue<uint64>::Available(43u);
            SampleAssessmentChannel assessment;
            sample.CompleteAction(
                PhysicsSandboxSample::ScenarioAction::CreateBodies,
                diagnostics,
                assessment);

            SampleReport report;
            SampleFeatureReporter reporter(report);
            sample.AppendReport(reporter);
            return report;
        }
    };
} // namespace RVX

namespace
{
    using namespace RVX;

    [[nodiscard]] SceneECS::RuntimeEntityDesc MakePhysicsEntityDesc(
        const Vec3& position,
        const Vec3& halfExtents)
    {
        SceneECS::RuntimeEntityDesc desc;
        desc.localTransform.translation = position;
        desc.localTransform.scale = halfExtents * 2.0f;
        desc.bounds.extents = halfExtents;
        return desc;
    }

    [[nodiscard]] SceneECS::Collider MakeCollider(const Vec3& halfExtents)
    {
        return {
            .shape = SceneECS::ColliderShapeType::Box,
            .boxHalfExtents = halfExtents,
            .friction = 0.75f,
            .restitution = 0.0f,
        };
    }

    [[nodiscard]] SceneECS::RigidBody MakeDynamicRigidBody()
    {
        return {
            .motionType = SceneECS::RigidBodyMotionType::Dynamic,
            .mass = 1.0f,
            .linearDamping = 0.0f,
            .angularDamping = 0.0f,
            .gravityScale = 1.0f,
            .allowSleep = false,
        };
    }

    [[nodiscard]] std::optional<uint64> FindMetric(
        const SampleReport& report,
        std::string_view name)
    {
        const auto found = std::find_if(
            report.scenario.metrics.begin(), report.scenario.metrics.end(),
            [name](const SampleReportScenarioMetric& metric)
            {
                return metric.name == name;
            });
        return found == report.scenario.metrics.end()
                   ? std::nullopt
                   : std::optional<uint64>(found->value);
    }
} // namespace

TEST(PhysicsSandboxSampleValidation,
     DeclaresPureEcsBuiltInFixedStepRequirements)
{
    PhysicsSandboxSample sample;
    const SampleWorldRequirements requirements = sample.GetWorldRequirements();

    EXPECT_EQ(requirements.world.physics.backend,
              Physics::PhysicsBackendType::BuiltIn);
    EXPECT_FLOAT_EQ(requirements.world.physics.fixedTimeStep, 1.0f / 60.0f);
    EXPECT_EQ(requirements.world.physics.maxSubSteps, 4u);
}

TEST(PhysicsSandboxSampleValidation,
     UsesItsFixedCatalogAssetAndPureEcsAssessmentContract)
{
    PhysicsSandboxSample sample;
    const SampleInfo& info = sample.GetInfo();
    const SampleAssessmentContract contract = sample.GetAssessmentContract();

    EXPECT_EQ(info.id, "physics-sandbox");
    EXPECT_EQ(info.defaultAssetId, "interior-rendering-p0a");
    EXPECT_EQ(info.assetPolicy, SampleAssetPolicy::Fixed);

    ASSERT_TRUE(contract.IsValid());
    EXPECT_EQ(contract.revision, "2");
    ASSERT_EQ(contract.actions.size(), 8u);
    EXPECT_EQ(contract.actions.front().code.GetValue(),
              "PHYSICS.SANDBOX.ACTION.CREATE_BODIES");
    EXPECT_EQ(contract.actions.back().code.GetValue(),
              "PHYSICS.SANDBOX.ACTION.REJECT_STALE_HANDLE");
    ASSERT_EQ(contract.invariants.size(), 7u);
    EXPECT_EQ(contract.invariants.front().code.GetValue(),
              "PHYSICS.SANDBOX.BUILTIN_AUTHORITY");
    ASSERT_EQ(contract.metrics.size(), 3u);
    EXPECT_EQ(contract.metrics[0].code.GetValue(),
              "PHYSICS.SANDBOX.FIXED_STEPS");
    EXPECT_EQ(contract.metrics[1].code.GetValue(),
              "PHYSICS.SANDBOX.BODY_CENSUS");
    EXPECT_EQ(contract.metrics[2].code.GetValue(),
              "PHYSICS.SANDBOX.CONTACT_EVIDENCE");
    ASSERT_EQ(contract.capabilities.size(), 2u);
    EXPECT_EQ(contract.capabilities[0].code.GetValue(),
              "PHYSICS.CAPABILITY.BUILTIN");
    EXPECT_EQ(contract.capabilities[1].code.GetValue(),
              "PHYSICS.CAPABILITY.JOLT");
    EXPECT_FALSE(contract.capabilities[1].gating);
}

TEST(PhysicsSandboxSampleValidation,
     ReportsOnlyPureEcsScenarioEvidenceBeforeSetup)
{
    PhysicsSandboxSample sample;
    SampleReport report;
    SampleFeatureReporter reporter(report);
    sample.AppendReport(reporter);

    EXPECT_EQ(report.schemaVersion, RVX_SAMPLE_REPORT_SCHEMA_VERSION);
    EXPECT_EQ(report.scenario.contractRevision, 2u);
    EXPECT_EQ(report.scenario.phase, "pending");
    EXPECT_EQ(report.scenario.actions.size(), 8u);
    EXPECT_EQ(report.scenario.invariants.size(), 7u);
    EXPECT_GE(report.scenario.metrics.size(), 20u);
    EXPECT_EQ(FindMetric(report, "sceneRuntimeId"), 0u);
    EXPECT_EQ(FindMetric(report, "physicsFixedStepSequence"), 0u);
    EXPECT_EQ(FindMetric(report, "bodyCount"), 0u);
    EXPECT_EQ(FindMetric(report, "colliderCount"), 0u);
    EXPECT_EQ(FindMetric(report, "pendingBridgeCleanup"), 0u);
    EXPECT_EQ(FindMetric(report, "bridgeStructuralContinuityLoss"), 0u);
    EXPECT_EQ(FindMetric(report, "bridgeCleanupContinuityLoss"), 0u);

    const SampleReadiness readiness = sample.GetReadiness({});
    EXPECT_EQ(readiness.state, SampleReadinessState::Pending);
    EXPECT_FALSE(readiness.reason.empty());
}

TEST(PhysicsSandboxSampleValidation,
     FrameOnlyActionReceiptUsesRetainedRenderSceneRevisionNotSceneFrame)
{
    PhysicsSandboxSample sample;
    const SampleReport report =
        PhysicsSandboxSampleValidationAccess::
            CompleteCreateActionAgainstRetainedSceneRevision(sample);

    ASSERT_EQ(report.scenario.actions.size(), 8u);
    const auto found = std::find_if(
        report.scenario.actions.begin(),
        report.scenario.actions.end(),
        [](const SampleReportScenarioAction& action)
        {
            return action.name == "CreateBodies";
        });
    ASSERT_NE(found, report.scenario.actions.end());
    EXPECT_TRUE(found->passed);
    EXPECT_EQ(found->targetSceneRevision, 7u);
    EXPECT_EQ(found->appliedSceneRevision, 7u);
    EXPECT_EQ(found->completedPresentationSequence, 43u);
}

TEST(PhysicsSandboxSampleValidation,
     AtomicPhysicsValueBatchCarriesExactSceneGenerationAndFragments)
{
    static_assert(std::is_trivially_copyable_v<SceneECS::RigidBody>);
    static_assert(std::is_trivially_copyable_v<SceneECS::Collider>);
    static_assert(std::is_trivially_copyable_v<SceneECS::PhysicsBodyState>);

    SceneECS::SceneEcsRuntime runtime;
    SampleSceneLifetimeScope lifetime(runtime);
    ASSERT_TRUE(lifetime.ReserveOwnershipCapacity(1u));

    const Vec3 initialHalfExtents(0.35f);
    auto transaction = runtime.BeginSpawnTransaction();
    const SceneECS::SceneSpawnEntityId pending = transaction.Create(
        MakePhysicsEntityDesc(Vec3(3.1f, 1.8f, 0.0f), initialHalfExtents));
    ASSERT_TRUE(pending.IsValid());
    ASSERT_TRUE(transaction.Add<SceneECS::RigidBody>(
        pending, MakeDynamicRigidBody()));
    ASSERT_TRUE(transaction.Add<SceneECS::Collider>(
        pending, MakeCollider(initialHalfExtents)));
    ASSERT_TRUE(transaction.Add<SceneECS::PhysicsBodyState>(pending, {}));

    const SceneECS::SceneSpawnCommitResult result = transaction.Commit();
    ASSERT_TRUE(result.IsApplied());
    const SceneECS::SceneEntityRef entity = result.GetEntityRef(pending);
    ASSERT_TRUE(entity.IsValid());
    EXPECT_EQ(entity.sceneRuntimeId, runtime.GetSceneRuntimeId());
    EXPECT_EQ(runtime.GetEntityRef(entity.entity), entity);
    ASSERT_TRUE(lifetime.AdoptBatch(std::span(&entity, 1u)));

    const SceneECS::RigidBody* rigidBody =
        runtime.GetRegistry().TryGet<SceneECS::RigidBody>(entity.entity);
    const SceneECS::Collider* collider =
        runtime.GetRegistry().TryGet<SceneECS::Collider>(entity.entity);
    const SceneECS::PhysicsBodyState* bodyState =
        runtime.GetRegistry().TryGet<SceneECS::PhysicsBodyState>(entity.entity);
    ASSERT_NE(rigidBody, nullptr);
    ASSERT_NE(collider, nullptr);
    ASSERT_NE(bodyState, nullptr);
    EXPECT_EQ(rigidBody->motionType, SceneECS::RigidBodyMotionType::Dynamic);
    EXPECT_FLOAT_EQ(rigidBody->mass, 1.0f);
    EXPECT_FLOAT_EQ(rigidBody->gravityScale, 1.0f);
    EXPECT_FALSE(rigidBody->allowSleep);
    EXPECT_EQ(collider->shape, SceneECS::ColliderShapeType::Box);
    EXPECT_EQ(collider->boxHalfExtents, initialHalfExtents);
    EXPECT_EQ(bodyState->status, SceneECS::PhysicsBodyBindingStatus::Unbound);
    EXPECT_EQ(bodyState->lastSynchronizedFixedStep, 0u);
    EXPECT_EQ(lifetime.GetDiagnostics().aliveEntityCount, 1u);
}

TEST(PhysicsSandboxSampleValidation,
     ColliderValueRefreshAndRetirementRejectStaleGenerationMutation)
{
    SceneECS::SceneEcsRuntime runtime;
    SampleSceneLifetimeScope lifetime(runtime);
    const Vec3 initialHalfExtents(0.45f);
    const Vec3 refreshedHalfExtents(0.7f, 0.35f, 0.5f);
    const SceneECS::SceneEntityRef entity =
        lifetime.CreateAndAdoptWithFragments(
            MakePhysicsEntityDesc(Vec3(3.1f, 1.8f, 0.0f), initialHalfExtents),
            MakeDynamicRigidBody(),
            MakeCollider(initialHalfExtents),
            SceneECS::PhysicsBodyState{},
            SceneECS::Mesh{.meshAssetId = AssetId{1}, .submeshCount = 1},
            SceneECS::Visibility{});
    ASSERT_TRUE(entity.IsValid());

    SceneECS::Collider refreshed = MakeCollider(refreshedHalfExtents);
    ASSERT_TRUE(runtime.SetFragment<SceneECS::Collider>(entity.entity, refreshed));
    const SceneECS::Collider* beforeDestroy =
        runtime.GetRegistry().TryGet<SceneECS::Collider>(entity.entity);
    ASSERT_NE(beforeDestroy, nullptr);
    EXPECT_EQ(beforeDestroy->boxHalfExtents, refreshedHalfExtents);

    ASSERT_EQ(runtime.RequestDestroy(
                  entity.entity,
                  SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::None)),
              SceneECS::DestroyRequestResult::Accepted);
    EXPECT_FALSE(runtime.SetFragment<SceneECS::Collider>(
        entity.entity, MakeCollider(Vec3(0.25f))));
    lifetime.Collect();
    EXPECT_EQ(lifetime.GetDiagnostics().pendingDestroyEntityCount, 1u);

    ASSERT_EQ(runtime.PublishPendingDestroyCleanup(), 1u);
    const SceneECS::EntityLifecycleState* lifecycleState =
        runtime.GetRegistry().TryGet<SceneECS::EntityLifecycleState>(entity.entity);
    ASSERT_NE(lifecycleState, nullptr);
    const SceneECS::CleanupDomainMask inferredDomains =
        SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Physics) |
        SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Render);
    EXPECT_EQ(lifecycleState->requiredCleanupDomains, inferredDomains);
    ASSERT_TRUE(runtime.AcknowledgeCleanup(
        entity.entity, inferredDomains));
    ASSERT_EQ(runtime.AdvanceRetirements(), 1u);
    ASSERT_EQ(runtime.RecycleRecyclableEntities(), 1u);
    lifetime.Collect();

    EXPECT_FALSE(runtime.GetEntityRef(entity.entity).IsValid());
    EXPECT_FALSE(runtime.SetFragment<SceneECS::Collider>(
        entity.entity, MakeCollider(Vec3(0.25f))));
    EXPECT_EQ(lifetime.GetDiagnostics().ownedEntityCount, 0u);
    EXPECT_EQ(lifetime.GetDiagnostics().recycledEntityCount, 1u);

    const ECS::EntityHandle replacement = runtime.CreateEntity();
    ASSERT_TRUE(replacement.IsValid());
    EXPECT_EQ(replacement.GetIndex(), entity.entity.GetIndex());
    EXPECT_NE(replacement.GetGeneration(), entity.entity.GetGeneration());
}
