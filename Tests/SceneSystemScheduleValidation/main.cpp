#include "Core/Log.h"
#include "Scene/SceneRuntime.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

namespace
{
    class LogEnvironment final : public ::testing::Environment
    {
    public:
        void SetUp() override { RVX::Log::Initialize(); }
        void TearDown() override { RVX::Log::Shutdown(); }
    };

    [[maybe_unused]] ::testing::Environment* const g_logEnvironment =
        ::testing::AddGlobalTestEnvironment(new LogEnvironment());

    class ScheduledActor final : public RVX::Actor
    {
    public:
        using Actor::Actor;
    };
}

TEST(SceneSystemScheduleValidation, ExecutesFixedPhasesInDeterministicOrder)
{
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());
    std::vector<RVX::SceneUpdatePhase> observed;

    const std::vector<RVX::SceneUpdatePhase> phases = {
        RVX::SceneUpdatePhase::BeginFrame,
        RVX::SceneUpdatePhase::Gameplay,
        RVX::SceneUpdatePhase::AnimationPrePhysics,
        RVX::SceneUpdatePhase::FixedPhysics,
        RVX::SceneUpdatePhase::TransformResolve,
        RVX::SceneUpdatePhase::BoundsSpatial,
        RVX::SceneUpdatePhase::FeatureSystems,
        RVX::SceneUpdatePhase::RenderExtraction,
        RVX::SceneUpdatePhase::EndFrame};

    for (RVX::SceneUpdatePhase phase : phases)
    {
        ASSERT_TRUE(scene.RegisterSystem(
                              "PhaseProbe",
                              phase,
                              [&observed, phase](float) { observed.push_back(phase); })
                        .IsValid());
    }
    scene.Tick(1.0f / 60.0f);
    EXPECT_EQ(observed, phases);
}

TEST(SceneSystemScheduleValidation, OrdersSystemsWithinPhaseByOrderThenRegistration)
{
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());
    std::vector<int> observed;
    scene.RegisterSystem("LateA", RVX::SceneUpdatePhase::Gameplay,
                         [&observed](float) { observed.push_back(2); }, 10);
    scene.RegisterSystem("First", RVX::SceneUpdatePhase::Gameplay,
                         [&observed](float) { observed.push_back(1); }, -10);
    scene.RegisterSystem("LateB", RVX::SceneUpdatePhase::Gameplay,
                         [&observed](float) { observed.push_back(3); }, 10);
    scene.Tick(0.0f);
    EXPECT_EQ(observed, (std::vector<int>{1, 2, 3}));
}

TEST(SceneSystemScheduleValidation, AppliesCrossThreadMutationsAtBeginFrame)
{
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());
    RVX::Actor::Handle spawnedHandle = RVX::Actor::InvalidHandle;

    std::thread producer([&scene, &spawnedHandle]() {
        scene.EnqueueMutation([&spawnedHandle](RVX::Scene& updateScene) {
            auto* actor = updateScene.SpawnActor<ScheduledActor>({});
            if (actor)
                spawnedHandle = actor->GetHandle();
        });
    });
    producer.join();

    EXPECT_FALSE(spawnedHandle.IsValid());
    scene.Tick(0.0f);
    EXPECT_TRUE(spawnedHandle.IsValid());
    EXPECT_NE(scene.ResolveActor(spawnedHandle), nullptr);
}

TEST(SceneSystemScheduleValidation, PendingDestroyIsHiddenAndReleasedAtEndFrame)
{
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());
    auto* actor = scene.SpawnActor<ScheduledActor>({});
    ASSERT_NE(actor, nullptr);
    const RVX::Actor::Handle staleHandle = actor->GetHandle();
    bool hiddenImmediately = false;

    scene.RegisterSystem(
        "DestroyProbe",
        RVX::SceneUpdatePhase::FeatureSystems,
        [&scene, actor, staleHandle, &hiddenImmediately](float) {
            EXPECT_TRUE(scene.DestroyActor(actor));
            hiddenImmediately = scene.ResolveActor(staleHandle) == nullptr;
        });
    scene.Tick(0.0f);

    EXPECT_TRUE(hiddenImmediately);
    EXPECT_EQ(scene.ResolveActor(staleHandle), nullptr);
    auto* replacement = scene.SpawnActor<ScheduledActor>({});
    ASSERT_NE(replacement, nullptr);
    EXPECT_EQ(replacement->GetHandle().GetIndex(), staleHandle.GetIndex());
    EXPECT_NE(replacement->GetHandle().GetGeneration(), staleHandle.GetGeneration());
}
