#include <gtest/gtest.h>

#include "Core/App/AppMode.h"
#include "Core/Camera/Camera.h"
#include "Core/Log.h"
#include "Core/Subsystem/EngineSubsystem.h"
#include "Core/Subsystem/SubsystemCollection.h"

#include <stdexcept>
#include <string>
#include <vector>

using namespace RVX;

namespace
{
    class ScopedCoreLog
    {
    public:
        ScopedCoreLog()
        {
            if (!Log::GetCoreLogger())
            {
                Log::Initialize();
                m_owned = true;
            }
        }

        ~ScopedCoreLog()
        {
            if (m_owned)
            {
                Log::Shutdown();
            }
        }

    private:
        bool m_owned = false;
    };

    class RecordingLifecycleSubsystem final : public EngineSubsystem
    {
    public:
        explicit RecordingLifecycleSubsystem(std::vector<std::string>* events)
            : m_events(events)
        {
        }

        const char* GetName() const override { return "RecordingLifecycleSubsystem"; }

        void Initialize() override
        {
            if (m_events)
            {
                m_events->push_back("recording:init");
            }
        }

        void Deinitialize() override
        {
            if (m_events)
            {
                m_events->push_back("recording:shutdown");
            }
        }

    private:
        std::vector<std::string>* m_events = nullptr;
    };

    class FailingLifecycleSubsystem final : public EngineSubsystem
    {
    public:
        explicit FailingLifecycleSubsystem(std::vector<std::string>* events)
            : m_events(events)
        {
        }

        const char* GetName() const override { return "FailingLifecycleSubsystem"; }

        RVX_SUBSYSTEM_DEPENDENCIES(RecordingLifecycleSubsystem)

        void Initialize() override
        {
            if (m_events)
            {
                m_events->push_back("failing:init");
            }
            throw std::runtime_error("intentional lifecycle failure");
        }

    private:
        std::vector<std::string>* m_events = nullptr;
    };

    class StagedDependencySubsystem final : public EngineSubsystem
    {
    public:
        explicit StagedDependencySubsystem(std::vector<std::string>* events)
            : m_events(events)
        {
        }

        const char* GetName() const override { return "StagedDependencySubsystem"; }

        void Initialize() override
        {
            m_events->push_back("dependency:init");
        }

        void Deinitialize() override
        {
            m_events->push_back("dependency:shutdown");
        }

        void Tick(float deltaTime) override
        {
            (void)deltaTime;
            m_events->push_back("dependency:tick");
        }

        bool ShouldTick() const override { return true; }

    private:
        std::vector<std::string>* m_events = nullptr;
    };

    class StagedTargetSubsystem final : public EngineSubsystem
    {
    public:
        explicit StagedTargetSubsystem(std::vector<std::string>* events)
            : m_events(events)
        {
        }

        const char* GetName() const override { return "StagedTargetSubsystem"; }

        RVX_SUBSYSTEM_DEPENDENCIES(StagedDependencySubsystem)

        void Initialize() override
        {
            m_events->push_back("target:init");
        }

        void Deinitialize() override
        {
            m_events->push_back("target:shutdown");
        }

        void Tick(float deltaTime) override
        {
            (void)deltaTime;
            m_events->push_back("target:tick");
        }

        bool ShouldTick() const override { return true; }

    private:
        std::vector<std::string>* m_events = nullptr;
    };

    class ThrowingShutdownSubsystem final : public EngineSubsystem
    {
    public:
        explicit ThrowingShutdownSubsystem(std::vector<std::string>* events)
            : m_events(events)
        {
        }

        const char* GetName() const override { return "ThrowingShutdownSubsystem"; }

        void Deinitialize() override
        {
            m_events->push_back("throwing:shutdown");
            throw std::runtime_error("intentional shutdown failure");
        }

    private:
        std::vector<std::string>* m_events = nullptr;
    };

    template<uint32 Identifier>
    class CompositionRecordingSubsystem final : public EngineSubsystem
    {
    public:
        CompositionRecordingSubsystem(
            const char* name,
            const char* eventPrefix,
            std::vector<std::string>* events)
            : m_name(name)
            , m_eventPrefix(eventPrefix)
            , m_events(events)
        {
        }

        const char* GetName() const override { return m_name; }

        void Initialize() override
        {
            if (m_events)
            {
                m_events->push_back(std::string(m_eventPrefix) + ":init");
            }
        }

        void Deinitialize() override
        {
            if (m_events)
            {
                m_events->push_back(std::string(m_eventPrefix) + ":shutdown");
            }
        }

    private:
        const char* m_name = "";
        const char* m_eventPrefix = "";
        std::vector<std::string>* m_events = nullptr;
    };

    using CompositionDependentSubsystem = CompositionRecordingSubsystem<0>;
    using CompositionPrerequisiteSubsystem = CompositionRecordingSubsystem<1>;
    using CompositionAdditionalPrerequisiteSubsystem =
        CompositionRecordingSubsystem<2>;
    using CompositionMissingDependentSubsystem =
        CompositionRecordingSubsystem<3>;
    using CompositionMissingPrerequisiteSubsystem =
        CompositionRecordingSubsystem<4>;
    using DeterministicFirstSubsystem = CompositionRecordingSubsystem<5>;
    using DeterministicSecondSubsystem = CompositionRecordingSubsystem<6>;
    using DeterministicThirdSubsystem = CompositionRecordingSubsystem<7>;

    class CompositionIntrinsicDependentSubsystem final
        : public EngineSubsystem
    {
    public:
        explicit CompositionIntrinsicDependentSubsystem(
            std::vector<std::string>* events)
            : m_events(events)
        {
        }

        const char* GetName() const override
        {
            return "CompositionIntrinsicDependentSubsystem";
        }

        RVX_SUBSYSTEM_DEPENDENCIES(CompositionPrerequisiteSubsystem)

        void Initialize() override
        {
            m_events->push_back("intrinsic-dependent:init");
        }

        void Deinitialize() override
        {
            m_events->push_back("intrinsic-dependent:shutdown");
        }

    private:
        std::vector<std::string>* m_events = nullptr;
    };

    TEST(AppModeBoundaryValidation, RuntimeModeUsesCookedRuntimeContracts)
    {
        constexpr AppModeTraits traits = GetAppModeTraits(AppMode::Runtime);

        EXPECT_EQ(traits.mode, AppMode::Runtime);
        EXPECT_EQ(traits.name, "Runtime");
        EXPECT_TRUE(traits.runtimeExecutionEnabled);
        EXPECT_FALSE(traits.editorShellVisible);
        EXPECT_FALSE(traits.authoringToolsEnabled);
        EXPECT_FALSE(traits.previewWorld);
        EXPECT_FALSE(traits.sourceAssetAccessAllowed);
        EXPECT_TRUE(traits.cookedRuntimeArtifactsRequired);
        EXPECT_TRUE(IsRuntimeExecutionMode(AppMode::Runtime));
        EXPECT_FALSE(IsEditorShellMode(AppMode::Runtime));
        EXPECT_FALSE(AllowsSourceAssetAccess(AppMode::Runtime));
        EXPECT_FALSE(AllowsHotReload(AppMode::Runtime));
    }

    TEST(AppModeBoundaryValidation, EditorAndPreviewModesPermitAuthoringShells)
    {
        constexpr AppModeTraits editor = GetAppModeTraits(AppMode::Editor);
        constexpr AppModeTraits preview = GetAppModeTraits(AppMode::Preview);

        EXPECT_TRUE(editor.editorShellVisible);
        EXPECT_TRUE(editor.authoringToolsEnabled);
        EXPECT_FALSE(editor.runtimeExecutionEnabled);
        EXPECT_TRUE(editor.sourceAssetAccessAllowed);
        EXPECT_FALSE(editor.cookedRuntimeArtifactsRequired);
        EXPECT_TRUE(editor.hotReloadAllowed);
        EXPECT_TRUE(AllowsHotReload(AppMode::Editor));

        EXPECT_TRUE(preview.editorShellVisible);
        EXPECT_TRUE(preview.authoringToolsEnabled);
        EXPECT_FALSE(preview.runtimeExecutionEnabled);
        EXPECT_TRUE(preview.previewWorld);
        EXPECT_TRUE(preview.sourceAssetAccessAllowed);
        EXPECT_FALSE(preview.cookedRuntimeArtifactsRequired);
        EXPECT_TRUE(preview.hotReloadAllowed);
        EXPECT_TRUE(AllowsHotReload(AppMode::Preview));
    }

    TEST(AppModeBoundaryValidation, PlayInEditorKeepsRuntimeWorldInsideEditorShell)
    {
        constexpr AppModeTraits traits = GetAppModeTraits(AppMode::PlayInEditor);

        EXPECT_EQ(ToString(AppMode::PlayInEditor), "PlayInEditor");
        EXPECT_TRUE(traits.editorShellVisible);
        EXPECT_FALSE(traits.authoringToolsEnabled);
        EXPECT_TRUE(traits.runtimeExecutionEnabled);
        EXPECT_FALSE(traits.previewWorld);
        EXPECT_FALSE(traits.sourceAssetAccessAllowed);
        EXPECT_TRUE(traits.cookedRuntimeArtifactsRequired);
        EXPECT_FALSE(traits.hotReloadAllowed);
        EXPECT_TRUE(IsEditorShellMode(AppMode::PlayInEditor));
        EXPECT_TRUE(IsRuntimeExecutionMode(AppMode::PlayInEditor));
        EXPECT_FALSE(AllowsHotReload(AppMode::PlayInEditor));
    }

    TEST(AppModeBoundaryValidation, CookAndTestModesPermitSourceInputsWithoutEditorShellHotReload)
    {
        constexpr AppModeTraits cook = GetAppModeTraits(AppMode::Cook);
        constexpr AppModeTraits test = GetAppModeTraits(AppMode::Test);

        EXPECT_EQ(ToString(AppMode::Cook), "Cook");
        EXPECT_FALSE(cook.editorShellVisible);
        EXPECT_FALSE(cook.authoringToolsEnabled);
        EXPECT_FALSE(cook.runtimeExecutionEnabled);
        EXPECT_FALSE(cook.previewWorld);
        EXPECT_TRUE(cook.sourceAssetAccessAllowed);
        EXPECT_FALSE(cook.cookedRuntimeArtifactsRequired);
        EXPECT_FALSE(cook.hotReloadAllowed);
        EXPECT_FALSE(IsEditorShellMode(AppMode::Cook));
        EXPECT_FALSE(IsRuntimeExecutionMode(AppMode::Cook));
        EXPECT_TRUE(AllowsSourceAssetAccess(AppMode::Cook));
        EXPECT_FALSE(AllowsHotReload(AppMode::Cook));

        EXPECT_EQ(ToString(AppMode::Test), "Test");
        EXPECT_FALSE(test.editorShellVisible);
        EXPECT_FALSE(test.authoringToolsEnabled);
        EXPECT_FALSE(test.runtimeExecutionEnabled);
        EXPECT_FALSE(test.previewWorld);
        EXPECT_TRUE(test.sourceAssetAccessAllowed);
        EXPECT_FALSE(test.cookedRuntimeArtifactsRequired);
        EXPECT_FALSE(test.hotReloadAllowed);
        EXPECT_FALSE(IsEditorShellMode(AppMode::Test));
        EXPECT_FALSE(IsRuntimeExecutionMode(AppMode::Test));
        EXPECT_TRUE(AllowsSourceAssetAccess(AppMode::Test));
        EXPECT_FALSE(AllowsHotReload(AppMode::Test));
    }

    TEST(AppModeBoundaryValidation, SubsystemInitializationFailureUnwindsInitializedDependencies)
    {
        ScopedCoreLog log;
        std::vector<std::string> events;
        SubsystemCollection<EngineSubsystem> collection;

        auto* recording = collection.AddSubsystem<RecordingLifecycleSubsystem>(&events);
        auto* failing = collection.AddSubsystem<FailingLifecycleSubsystem>(&events);

        EXPECT_FALSE(collection.InitializeAll());
        EXPECT_FALSE(collection.IsInitialized());
        EXPECT_FALSE(recording->IsInitialized());
        EXPECT_FALSE(failing->IsInitialized());
        EXPECT_EQ((std::vector<std::string>{
                      "recording:init",
                      "failing:init",
                      "recording:shutdown",
                  }),
                  events);
    }

    TEST(AppModeBoundaryValidation, StagedInitializationHooksObserveDependencyAndTargetState)
    {
        ScopedCoreLog log;
        std::vector<std::string> events;
        SubsystemCollection<EngineSubsystem> collection;

        auto* dependency = collection.AddSubsystem<StagedDependencySubsystem>(&events);
        auto* target = collection.AddSubsystem<StagedTargetSubsystem>(&events);
        bool beforeSawRequiredState = false;
        bool afterSawInitializedTarget = false;

        EXPECT_TRUE(collection.InitializeAll(
            [&](EngineSubsystem& subsystem) {
                if (&subsystem == target)
                {
                    beforeSawRequiredState = dependency->IsInitialized() &&
                                             !target->IsInitialized();
                    events.push_back("target:before");
                }
            },
            [&](EngineSubsystem& subsystem) {
                if (&subsystem == target)
                {
                    afterSawInitializedTarget = target->IsInitialized();
                    events.push_back("target:after");
                }
            }));

        EXPECT_TRUE(beforeSawRequiredState);
        EXPECT_TRUE(afterSawInitializedTarget);
        EXPECT_EQ((std::vector<std::string>{
                      "dependency:init",
                      "target:before",
                      "target:init",
                      "target:after",
                  }),
                  events);
    }

    TEST(AppModeBoundaryValidation, BeforeInitializeHookFailureUnwindsOnlyInitializedDependencies)
    {
        ScopedCoreLog log;
        std::vector<std::string> events;
        SubsystemCollection<EngineSubsystem> collection;

        auto* dependency = collection.AddSubsystem<StagedDependencySubsystem>(&events);
        auto* target = collection.AddSubsystem<StagedTargetSubsystem>(&events);

        EXPECT_FALSE(collection.InitializeAll(
            [&](EngineSubsystem& subsystem) {
                if (&subsystem == target)
                {
                    events.push_back("target:before-throws");
                    throw std::runtime_error("intentional before-hook failure");
                }
            }));

        EXPECT_FALSE(collection.IsInitialized());
        EXPECT_FALSE(dependency->IsInitialized());
        EXPECT_FALSE(target->IsInitialized());
        EXPECT_EQ((std::vector<std::string>{
                      "dependency:init",
                      "target:before-throws",
                      "dependency:shutdown",
                  }),
                  events);
    }

    TEST(AppModeBoundaryValidation, AfterInitializeHookFailureUnwindsTargetAndDependencies)
    {
        ScopedCoreLog log;
        std::vector<std::string> events;
        SubsystemCollection<EngineSubsystem> collection;

        auto* dependency = collection.AddSubsystem<StagedDependencySubsystem>(&events);
        auto* target = collection.AddSubsystem<StagedTargetSubsystem>(&events);

        EXPECT_FALSE(collection.InitializeAll(
            {},
            [&](EngineSubsystem& subsystem) {
                if (&subsystem == target)
                {
                    events.push_back("target:after-throws");
                    throw std::runtime_error("intentional after-hook failure");
                }
            }));

        EXPECT_FALSE(collection.IsInitialized());
        EXPECT_FALSE(dependency->IsInitialized());
        EXPECT_FALSE(target->IsInitialized());
        EXPECT_EQ((std::vector<std::string>{
                      "dependency:init",
                      "target:init",
                      "target:after-throws",
                      "target:shutdown",
                      "dependency:shutdown",
                  }),
                  events);
    }

    TEST(AppModeBoundaryValidation, TargetedShutdownIsIdempotentAndSkipsTicksAndLaterShutdown)
    {
        ScopedCoreLog log;
        std::vector<std::string> events;
        SubsystemCollection<EngineSubsystem> collection;

        auto* dependency = collection.AddSubsystem<StagedDependencySubsystem>(&events);
        auto* target = collection.AddSubsystem<StagedTargetSubsystem>(&events);
        ASSERT_TRUE(collection.InitializeAll());
        events.clear();

        EXPECT_TRUE(collection.DeinitializeSubsystem<StagedTargetSubsystem>());
        EXPECT_FALSE(target->IsInitialized());
        EXPECT_TRUE(dependency->IsInitialized());
        EXPECT_TRUE(collection.IsInitialized());
        EXPECT_TRUE(collection.DeinitializeSubsystem<StagedTargetSubsystem>());

        collection.TickAll(1.0f / 60.0f);
        collection.TickPhase(TickPhase::Update, 1.0f / 60.0f);

        collection.DeinitializeAll();
        EXPECT_FALSE(dependency->IsInitialized());
        EXPECT_FALSE(collection.IsInitialized());
        EXPECT_FALSE(collection.DeinitializeSubsystem<FailingLifecycleSubsystem>());

        EXPECT_EQ((std::vector<std::string>{
                      "target:shutdown",
                      "dependency:tick",
                      "dependency:tick",
                      "dependency:shutdown",
                  }),
                  events);
    }

    TEST(AppModeBoundaryValidation, TargetedShutdownRecomputesStateAfterFinalLiveSubsystemStops)
    {
        ScopedCoreLog log;
        std::vector<std::string> events;
        SubsystemCollection<EngineSubsystem> collection;

        auto* dependency = collection.AddSubsystem<StagedDependencySubsystem>(&events);
        auto* target = collection.AddSubsystem<StagedTargetSubsystem>(&events);
        ASSERT_TRUE(collection.InitializeAll());
        events.clear();

        EXPECT_TRUE(collection.DeinitializeSubsystem<StagedTargetSubsystem>());
        EXPECT_FALSE(target->IsInitialized());
        EXPECT_TRUE(dependency->IsInitialized());
        EXPECT_TRUE(collection.IsInitialized());

        EXPECT_TRUE(collection.DeinitializeSubsystem<StagedDependencySubsystem>());
        EXPECT_FALSE(dependency->IsInitialized());
        EXPECT_FALSE(collection.IsInitialized());
        EXPECT_EQ((std::vector<std::string>{
                      "target:shutdown",
                      "dependency:shutdown",
                  }),
                  events);
    }

    TEST(AppModeBoundaryValidation, TargetedShutdownFailureClearsLifecycleState)
    {
        ScopedCoreLog log;
        std::vector<std::string> events;
        SubsystemCollection<EngineSubsystem> collection;

        auto* throwing = collection.AddSubsystem<ThrowingShutdownSubsystem>(&events);
        ASSERT_TRUE(collection.InitializeAll());

        EXPECT_FALSE(collection.DeinitializeSubsystem<ThrowingShutdownSubsystem>());
        EXPECT_FALSE(throwing->IsInitialized());
        EXPECT_FALSE(collection.IsInitialized());
        EXPECT_TRUE(collection.DeinitializeSubsystem<ThrowingShutdownSubsystem>());
        collection.DeinitializeAll();

        EXPECT_EQ((std::vector<std::string>{"throwing:shutdown"}), events);
    }

    TEST(
        AppModeBoundaryValidation,
        CompositionDependenciesOverrideRegistrationOrderAndReverseShutdown)
    {
        ScopedCoreLog log;
        std::vector<std::string> events;
        SubsystemCollection<EngineSubsystem> collection;

        collection.AddSubsystem<CompositionDependentSubsystem>(
            "CompositionDependentSubsystem",
            "dependent",
            &events);
        collection.AddSubsystem<CompositionPrerequisiteSubsystem>(
            "CompositionPrerequisiteSubsystem",
            "prerequisite",
            &events);

        const auto result = collection.AddInitializationDependency<
            CompositionDependentSubsystem,
            CompositionPrerequisiteSubsystem>();
        ASSERT_EQ(
            result.code,
            SubsystemDependencyRegistrationCode::Added);
        ASSERT_TRUE(collection.InitializeAll());
        collection.DeinitializeAll();

        EXPECT_EQ((std::vector<std::string>{
                      "prerequisite:init",
                      "dependent:init",
                      "dependent:shutdown",
                      "prerequisite:shutdown",
                  }),
                  events);
    }

    TEST(
        AppModeBoundaryValidation,
        CompositionDependencyRegistrationIsTypedIdempotentAndFailClosed)
    {
        ScopedCoreLog log;
        std::vector<std::string> events;

        SubsystemCollection<EngineSubsystem> missingPrerequisite;
        missingPrerequisite.AddSubsystem<CompositionDependentSubsystem>(
            "CompositionDependentSubsystem",
            "dependent",
            &events);
        const auto missingPrerequisiteResult =
            missingPrerequisite.AddInitializationDependency<
                CompositionDependentSubsystem,
                CompositionMissingPrerequisiteSubsystem>();
        EXPECT_EQ(
            missingPrerequisiteResult.code,
            SubsystemDependencyRegistrationCode::MissingPrerequisite);

        SubsystemCollection<EngineSubsystem> missingDependent;
        missingDependent.AddSubsystem<CompositionPrerequisiteSubsystem>(
            "CompositionPrerequisiteSubsystem",
            "prerequisite",
            &events);
        const auto missingDependentResult =
            missingDependent.AddInitializationDependency<
                CompositionMissingDependentSubsystem,
                CompositionPrerequisiteSubsystem>();
        EXPECT_EQ(
            missingDependentResult.code,
            SubsystemDependencyRegistrationCode::MissingDependent);

        SubsystemCollection<EngineSubsystem> collection;
        collection.AddSubsystem<CompositionDependentSubsystem>(
            "CompositionDependentSubsystem",
            "dependent",
            &events);
        collection.AddSubsystem<CompositionPrerequisiteSubsystem>(
            "CompositionPrerequisiteSubsystem",
            "prerequisite",
            &events);
        collection.AddSubsystem<CompositionAdditionalPrerequisiteSubsystem>(
            "CompositionAdditionalPrerequisiteSubsystem",
            "additional",
            &events);

        const auto selfDependency =
            collection.AddInitializationDependency<
                CompositionDependentSubsystem,
                CompositionDependentSubsystem>();
        EXPECT_EQ(
            selfDependency.code,
            SubsystemDependencyRegistrationCode::SelfDependency);

        const auto added = collection.AddInitializationDependency<
            CompositionDependentSubsystem,
            CompositionPrerequisiteSubsystem>();
        EXPECT_EQ(
            added.code,
            SubsystemDependencyRegistrationCode::Added);
        EXPECT_TRUE(added.IsAccepted());

        const auto duplicate = collection.AddInitializationDependency<
            CompositionDependentSubsystem,
            CompositionPrerequisiteSubsystem>();
        EXPECT_EQ(
            duplicate.code,
            SubsystemDependencyRegistrationCode::AlreadyRegistered);
        EXPECT_TRUE(duplicate.IsAccepted());

        ASSERT_TRUE(collection.InitializeAll());
        const auto activeMutation =
            collection.AddInitializationDependency<
                CompositionDependentSubsystem,
                CompositionAdditionalPrerequisiteSubsystem>();
        EXPECT_EQ(
            activeMutation.code,
            SubsystemDependencyRegistrationCode::LifecycleActive);

        collection.DeinitializeAll();
        collection.Clear();
        events.clear();
        collection.AddSubsystem<CompositionDependentSubsystem>(
            "CompositionDependentSubsystem",
            "dependent",
            &events);
        collection.AddSubsystem<CompositionPrerequisiteSubsystem>(
            "CompositionPrerequisiteSubsystem",
            "prerequisite",
            &events);
        const auto afterClear = collection.AddInitializationDependency<
            CompositionDependentSubsystem,
            CompositionPrerequisiteSubsystem>();
        EXPECT_EQ(
            afterClear.code,
            SubsystemDependencyRegistrationCode::Added);
    }

    TEST(
        AppModeBoundaryValidation,
        CompositionAndIntrinsicDependenciesShareOneDeduplicatedGraph)
    {
        ScopedCoreLog log;
        std::vector<std::string> events;
        SubsystemCollection<EngineSubsystem> collection;

        collection.AddSubsystem<CompositionIntrinsicDependentSubsystem>(
            &events);
        collection.AddSubsystem<CompositionPrerequisiteSubsystem>(
            "CompositionPrerequisiteSubsystem",
            "prerequisite",
            &events);
        ASSERT_TRUE(
            (collection.AddInitializationDependency<
                 CompositionIntrinsicDependentSubsystem,
                 CompositionPrerequisiteSubsystem>()
                 .IsAccepted()));

        ASSERT_TRUE(collection.InitializeAll());
        collection.DeinitializeAll();
        EXPECT_EQ((std::vector<std::string>{
                      "prerequisite:init",
                      "intrinsic-dependent:init",
                      "intrinsic-dependent:shutdown",
                      "prerequisite:shutdown",
                  }),
                  events);
    }

    TEST(
        AppModeBoundaryValidation,
        CompositionDependencyCyclesFailBeforeInitialization)
    {
        ScopedCoreLog log;
        std::vector<std::string> events;
        SubsystemCollection<EngineSubsystem> collection;

        collection.AddSubsystem<CompositionDependentSubsystem>(
            "CompositionDependentSubsystem",
            "dependent",
            &events);
        collection.AddSubsystem<CompositionPrerequisiteSubsystem>(
            "CompositionPrerequisiteSubsystem",
            "prerequisite",
            &events);
        ASSERT_TRUE(
            (collection.AddInitializationDependency<
                 CompositionDependentSubsystem,
                 CompositionPrerequisiteSubsystem>()
                 .IsAccepted()));
        ASSERT_TRUE(
            (collection.AddInitializationDependency<
                 CompositionPrerequisiteSubsystem,
                 CompositionDependentSubsystem>()
                 .IsAccepted()));

        EXPECT_FALSE(collection.InitializeAll());
        EXPECT_TRUE(events.empty());
    }

    TEST(
        AppModeBoundaryValidation,
        UnrelatedSubsystemInitializationOrderIsDeterministic)
    {
        ScopedCoreLog log;
        const std::vector<std::string> expected{
            "first:init",
            "second:init",
            "third:init",
            "third:shutdown",
            "second:shutdown",
            "first:shutdown",
        };

        for (uint32 iteration = 0; iteration < 32; ++iteration)
        {
            std::vector<std::string> events;
            SubsystemCollection<EngineSubsystem> collection;
            collection.AddSubsystem<DeterministicFirstSubsystem>(
                "DeterministicFirstSubsystem",
                "first",
                &events);
            collection.AddSubsystem<DeterministicSecondSubsystem>(
                "DeterministicSecondSubsystem",
                "second",
                &events);
            collection.AddSubsystem<DeterministicThirdSubsystem>(
                "DeterministicThirdSubsystem",
                "third",
                &events);

            ASSERT_TRUE(collection.InitializeAll()) << "iteration " << iteration;
            collection.DeinitializeAll();
            EXPECT_EQ(events, expected) << "iteration " << iteration;
        }
    }

    TEST(AppModeBoundaryValidation, CameraContractLivesInCoreForSharedEditorRuntimeUse)
    {
        Camera camera;
        camera.SetPerspective(1.0f, 16.0f / 9.0f, 0.1f, 1000.0f);
        camera.SetPosition(Vec3(0.0f, 2.0f, 8.0f));
        camera.LookAt(Vec3(0.0f, 0.0f, 0.0f));

        EXPECT_FLOAT_EQ(camera.GetPosition().x, 0.0f);
        EXPECT_FLOAT_EQ(camera.GetPosition().y, 2.0f);
        EXPECT_FLOAT_EQ(camera.GetPosition().z, 8.0f);
        EXPECT_NE(camera.GetView()[3][2], 0.0f);
        EXPECT_NE(camera.GetProjection()[2][2], 1.0f);
        EXPECT_NE(camera.GetViewProjection()[3][2], 0.0f);
    }
} // namespace
