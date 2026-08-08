#include "Core/Log.h"
#include "Scene/SceneComponent.h"
#include "Scene/SceneRuntime.h"

#include <gtest/gtest.h>

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

    class TransformActor final : public RVX::Actor
    {
    public:
        using Actor::Actor;
        const char* GetClassName() const override { return "TransformActor"; }
    };

    RVX::SceneComponent* AddRoot(RVX::Actor& actor)
    {
        auto* root = actor.AddComponent<RVX::SceneComponent>();
        if (root)
            actor.SetRootComponent(root);
        return root;
    }
} // namespace

TEST(SceneTransformHierarchyValidation, KeepLocalPropagatesAndRejectsCycles)
{
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());
    auto* parentActor = scene.SpawnActor<TransformActor>({.name = "Parent"});
    auto* childActor = scene.SpawnActor<TransformActor>({.name = "Child"});
    ASSERT_NE(parentActor, nullptr);
    ASSERT_NE(childActor, nullptr);
    auto* parent = AddRoot(*parentActor);
    auto* child = AddRoot(*childActor);
    ASSERT_NE(parent, nullptr);
    ASSERT_NE(child, nullptr);

    parent->SetRelativeLocation({10.0f, 0.0f, 0.0f});
    child->SetRelativeLocation({3.0f, 0.0f, 0.0f});
    ASSERT_TRUE(child->AttachToComponent(
        parent, RVX::AttachmentTransformRule::KeepLocal));
    scene.GetTransformStore().Resolve();

    EXPECT_NEAR(child->GetWorldLocation().x, 13.0f, 0.0001f);
    EXPECT_NEAR(child->GetRelativeLocation().x, 3.0f, 0.0001f);
    EXPECT_EQ(scene.GetTransformStore().GetParent(
                  child->GetComponentHandle()),
              parent->GetComponentHandle());
    EXPECT_FALSE(parent->AttachToComponent(child));

    const RVX::uint64 before = child->GetWorldTransformRevision();
    scene.GetTransformStore().BeginFrame();
    parent->SetRelativeLocation({20.0f, 0.0f, 0.0f});
    scene.GetTransformStore().Resolve();
    EXPECT_NEAR(child->GetWorldLocation().x, 23.0f, 0.0001f);
    EXPECT_NEAR(child->GetPreviousWorldTransform()[3].x, 13.0f, 0.0001f);
    EXPECT_GT(child->GetWorldTransformRevision(), before);
}

TEST(SceneTransformHierarchyValidation, KeepWorldSurvivesReparentAndDetach)
{
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());
    auto* firstActor = scene.SpawnActor<TransformActor>({.name = "First"});
    auto* secondActor = scene.SpawnActor<TransformActor>({.name = "Second"});
    auto* childActor = scene.SpawnActor<TransformActor>({.name = "Child"});
    ASSERT_NE(firstActor, nullptr);
    ASSERT_NE(secondActor, nullptr);
    ASSERT_NE(childActor, nullptr);
    auto* first = AddRoot(*firstActor);
    auto* second = AddRoot(*secondActor);
    auto* child = AddRoot(*childActor);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(child, nullptr);

    first->SetRelativeLocation({10.0f, 0.0f, 0.0f});
    second->SetRelativeLocation({100.0f, 0.0f, 0.0f});
    child->SetRelativeLocation({23.0f, 0.0f, 0.0f});
    scene.GetTransformStore().Resolve();

    ASSERT_TRUE(child->AttachToComponent(
        first, RVX::AttachmentTransformRule::KeepWorld));
    EXPECT_NEAR(child->GetWorldLocation().x, 23.0f, 0.0001f);
    EXPECT_NEAR(child->GetRelativeLocation().x, 13.0f, 0.0001f);

    ASSERT_TRUE(child->AttachToComponent(
        second, RVX::AttachmentTransformRule::KeepWorld));
    EXPECT_NEAR(child->GetWorldLocation().x, 23.0f, 0.0001f);
    EXPECT_NEAR(child->GetRelativeLocation().x, -77.0f, 0.0001f);

    child->DetachFromComponent(RVX::AttachmentTransformRule::KeepWorld);
    EXPECT_NEAR(child->GetWorldLocation().x, 23.0f, 0.0001f);
    EXPECT_NEAR(child->GetRelativeLocation().x, 23.0f, 0.0001f);
    EXPECT_FALSE(scene.GetTransformStore()
                     .GetParent(child->GetComponentHandle())
                     .IsValid());
}
