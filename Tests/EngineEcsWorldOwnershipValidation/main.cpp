#include "Core/Log.h"
#include "Engine/Engine.h"
#include "Resource/ResourceSubsystem.h"
#include "World/World.h"

#include <gtest/gtest.h>

#include <type_traits>

namespace
{
using namespace RVX;

template<typename T>
concept HasIndependentRenderShutdown = requires(T& value)
{
    value.ShutdownRenderRuntime();
};

template<typename T>
concept HasLegacyRetirementService = requires(T& value)
{
    value.GetSceneAssetRetirementService();
};

static_assert(!HasIndependentRenderShutdown<Engine>);
static_assert(!HasLegacyRetirementService<Engine>);
static_assert(!std::is_default_constructible_v<Engine>);
static_assert(!std::is_copy_constructible_v<EngineRuntimeDependencies>);
static_assert(std::is_nothrow_move_constructible_v<EngineRuntimeDependencies>);

class LogEnvironment final : public ::testing::Environment
{
public:
    void SetUp() override { Log::Initialize(); }
    void TearDown() override { Log::Shutdown(); }
};

[[maybe_unused]] ::testing::Environment* const g_logEnvironment =
    ::testing::AddGlobalTestEnvironment(new LogEnvironment());

TEST(EngineEcsWorldOwnershipValidation,
     RejectsAResourceSubsystemWithoutRuntimeDependencies)
{
    Engine engine(EngineRuntimeDependencies{});
    EngineConfig config;
    config.enableJobSystem = false;
    engine.SetConfig(config);
    ASSERT_NE(engine.AddSubsystem<Resource::ResourceSubsystem>(), nullptr);

    EXPECT_FALSE(engine.Initialize());
    EXPECT_FALSE(engine.IsInitialized());
}

TEST(EngineEcsWorldOwnershipValidation,
     RefusesToPublishAWorldWithoutTheSingleGlobalEcsRenderComposition)
{
    Engine engine(EngineRuntimeDependencies{});
    EngineConfig config;
    config.enableJobSystem = false;
    engine.SetConfig(config);
    ASSERT_TRUE(engine.Initialize());

    WorldConfig worldConfig;
    worldConfig.name = "NoGlobalRenderOwner";
    EXPECT_EQ(engine.CreateWorld(worldConfig), nullptr);
    EXPECT_EQ(engine.GetWorld(worldConfig.name), nullptr);
    EXPECT_EQ(engine.GetWorldEcsRuntimeServices(nullptr), nullptr);

    const uint64 frameBeforeSimulationTick = engine.GetFrameNumber();
    engine.TickWithoutRender(0.0f);
    // TickWithoutRender is simulation-oriented but remains an Engine-owned
    // ECS publication/proof path whenever a global render owner is present.
    // This no-render fixture still verifies that it advances the normal owner
    // frame path instead of providing a legacy direct-render escape hatch.
    EXPECT_EQ(engine.GetFrameNumber(), frameBeforeSimulationTick + 1u);
    engine.Shutdown();
    EXPECT_TRUE(engine.GetLastShutdownDiagnostics().clean);
}
} // namespace
