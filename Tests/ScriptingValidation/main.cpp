#include "Core/Event/EventBus.h"
#include "Core/Log.h"
#include "Scene/ComponentEvents.h"
#include "Scene/SceneEntity.h"
#include "Scripting/Bindings/CoreBindings.h"
#include "Scripting/Bindings/InputBindings.h"
#include "Scripting/Bindings/MathBindings.h"
#include "Scripting/Bindings/SceneBindings.h"
#include "Scripting/LuaState.h"
#include "Scripting/ScriptComponent.h"
#include "Scripting/ScriptEngine.h"

#include <gtest/gtest.h>

namespace RVX::Tests
{
    namespace
    {
        class LoggingEnvironment final : public ::testing::Environment
        {
        public:
            void SetUp() override
            {
                if (!Log::GetCoreLogger())
                {
                    Log::Config config;
                    config.enableRotation = false;
                    config.logFileName = "ScriptingValidation.log";
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
    } // namespace

    TEST(ScriptingValidation, SceneEntityPublishesComponentAttachedEvent)
    {
        Component* capturedComponent = nullptr;
        SceneEntity* capturedEntity = nullptr;

        auto subscription = EventBus::Get().SubscribeScoped<ComponentAttachedEvent>(
            [&](const ComponentAttachedEvent& event)
            {
                capturedEntity = event.entity;
                capturedComponent = event.component;
            });

        SceneEntity entity("ScriptedEntity");
        auto* component = entity.AddComponent<ScriptComponent>();

        ASSERT_NE(component, nullptr);
        EXPECT_EQ(capturedEntity, &entity);
        EXPECT_EQ(capturedComponent, component);
    }

    TEST(ScriptingValidation, ScriptingSubsystemDefaultConstructionIsStable)
    {
        ScriptingSubsystem scripting;
        EXPECT_TRUE(scripting.GetComponents().empty());
    }

    TEST(ScriptingValidation, LuaStateDefaultConstructionIsStable)
    {
        LuaState lua;
        SUCCEED();
    }

    TEST(ScriptingValidation, LuaStateInitializationIsStable)
    {
        LuaState lua;

        ASSERT_TRUE(lua.Initialize());
        EXPECT_TRUE(lua.IsInitialized());

        lua.Shutdown();
        EXPECT_FALSE(lua.IsInitialized());
    }

    TEST(ScriptingValidation, CoreBindingsRegistrationIsStable)
    {
        LuaState lua;
        ASSERT_TRUE(lua.Initialize());

        Bindings::RegisterCoreBindings(lua);
        EXPECT_TRUE(lua.GetState()["RVX"]["Log"].valid());
        EXPECT_TRUE(lua.GetState()["RVX"]["Time"].valid());
    }

    TEST(ScriptingValidation, MathBindingsRegistrationIsStable)
    {
        LuaState lua;
        ASSERT_TRUE(lua.Initialize());

        Bindings::RegisterMathBindings(lua);
        EXPECT_TRUE(lua.GetState()["RVX"]["Math"].valid());
        EXPECT_TRUE(lua.GetState()["Vec3"].valid());
    }

    TEST(ScriptingValidation, SceneBindingsRegistrationIsStable)
    {
        LuaState lua;
        ASSERT_TRUE(lua.Initialize());

        Bindings::RegisterSceneBindings(lua);
        EXPECT_TRUE(lua.GetState()["RVX"]["SceneEntity"].valid());
        EXPECT_TRUE(lua.GetState()["RVX"]["Component"].valid());
    }

    TEST(ScriptingValidation, InputBindingsRegistrationIsStable)
    {
        LuaState lua;
        ASSERT_TRUE(lua.Initialize());

        Bindings::RegisterInputBindings(lua);
        EXPECT_TRUE(lua.GetState()["RVX"]["Input"].valid());
        EXPECT_TRUE(lua.GetState()["RVX"]["Key"].valid());
    }

    TEST(ScriptingValidation, RegisterComponentTracksScriptComponentWithoutLuaInitialization)
    {
        ScriptingSubsystem scripting;
        SceneEntity entity("ScriptedEntity");
        auto* component = entity.AddComponent<ScriptComponent>();

        ASSERT_NE(component, nullptr);
        scripting.RegisterComponent(component);

        ASSERT_EQ(scripting.GetComponents().size(), 1u);
        EXPECT_EQ(scripting.GetComponents()[0], component);
    }

    TEST(ScriptingValidation, InitializedSubsystemRegistersAttachedScriptComponents)
    {
        ScriptingSubsystemConfig config;
        config.enableHotReload = false;

        ScriptingSubsystem scripting;
        scripting.Configure(config);
        scripting.Initialize();

        ASSERT_TRUE(scripting.GetLuaState().IsInitialized());

        SceneEntity entity("ScriptedEntity");
        auto* component = entity.AddComponent<ScriptComponent>();

        ASSERT_NE(component, nullptr);
        ASSERT_EQ(scripting.GetComponents().size(), 1u);
        EXPECT_EQ(scripting.GetComponents()[0], component);

        EXPECT_TRUE(entity.RemoveComponent<ScriptComponent>());
        EXPECT_TRUE(scripting.GetComponents().empty());

        scripting.Deinitialize();
    }

} // namespace RVX::Tests
