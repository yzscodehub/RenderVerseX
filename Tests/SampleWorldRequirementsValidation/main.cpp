#include "Samples/Sample.h"
#include "World/World.h"

#include <gtest/gtest.h>

namespace RVX
{
    namespace
    {
        class DefaultRequirementsSample final : public ISample
        {
        public:
            const SampleInfo& GetInfo() const noexcept override { return m_info; }
            bool Setup(SampleContext&, std::string&) override { return true; }
            void Update(SampleContext&, float) override {}
            void OnInput(SampleContext&) override {}
            void AppendReport(SampleFeatureReporter&) const override {}
            void Shutdown(SampleContext&) override {}

        private:
            SampleInfo m_info{};
        };
    } // namespace

    TEST(SampleWorldRequirementsValidation, DefaultSampleRequestsOnePureEcsWorld)
    {
        DefaultRequirementsSample sample;
        const SampleWorldRequirements requirements = sample.GetWorldRequirements();

        EXPECT_EQ(requirements.world.name, "World");
        EXPECT_GT(requirements.world.physics.fixedTimeStep, 0.0f);
    }

    TEST(SampleWorldRequirementsValidation, WorldPreservesValueConfiguration)
    {
        WorldConfig config;
        config.name = "ConfiguredSampleWorld";
        config.physics.gravity = Vec3(1.0f, -2.0f, 3.0f);
        config.physics.fixedTimeStep = 1.0f / 120.0f;

        World world(config);

        EXPECT_EQ(world.GetConfig().name, config.name);
        EXPECT_EQ(world.GetConfig().physics.gravity, config.physics.gravity);
        EXPECT_FLOAT_EQ(world.GetConfig().physics.fixedTimeStep,
                        config.physics.fixedTimeStep);
        EXPECT_TRUE(world.GetSceneEcsRuntime().GetSceneRuntimeId().IsValid());
        EXPECT_EQ(world.GetCameraService().GetSceneRuntimeId(),
                  world.GetSceneEcsRuntime().GetSceneRuntimeId());
    }

    TEST(SampleWorldRequirementsValidation, WorldsHaveDistinctSceneRuntimeIdentity)
    {
        World first(WorldConfig{.name = "First"});
        World second(WorldConfig{.name = "Second"});

        EXPECT_NE(first.GetSceneEcsRuntime().GetSceneRuntimeId(),
                  second.GetSceneEcsRuntime().GetSceneRuntimeId());
    }

    TEST(SampleWorldRequirementsValidation, CameraIdentityIsSceneQualified)
    {
        World first(WorldConfig{.name = "First"});
        World second(WorldConfig{.name = "Second"});

        const WorldECS::WorldEcsCameraRef camera =
            first.GetCameraService().CreateMainCamera();
        ASSERT_TRUE(camera.IsValid());
        EXPECT_EQ(camera.sceneRuntimeId,
                  first.GetSceneEcsRuntime().GetSceneRuntimeId());
        EXPECT_FALSE(second.GetCameraService().Activate(camera));
    }
} // namespace RVX
