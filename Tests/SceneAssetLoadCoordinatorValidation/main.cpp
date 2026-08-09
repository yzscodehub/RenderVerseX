#include "Core/Log.h"
#include "Resource/PreparedResourceBundle.h"
#include "Resource/ResourceSubsystem.h"
#include "Resource/Types/ModelResource.h"
#include "ResourceSceneAdapters/ResourceSceneAdapters.h"
#include "ResourceSceneAdapters/SceneAssetLoadCoordinator.h"
#include "Scene/SceneRuntime.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

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

    class BlockingModelLoader final : public RVX::Resource::IResourceLoader
    {
    public:
        RVX::ResourceType GetResourceType() const override
        {
            return RVX::ResourceType::Model;
        }

        std::vector<std::string> GetSupportedExtensions() const override
        {
            return {".gltf"};
        }

        RVX::Resource::IResource* Load(const std::string&) override
        {
            return nullptr;
        }

        bool SupportsPreparedLoading() const override { return true; }

        bool Prepare(
            const RVX::Resource::ResourceLoadPreparationContext& context,
            RVX::Resource::PreparedResourceBundle& outBundle,
            RVX::Resource::ResourceLoadError& outError) override
        {
            ++prepareCount;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_entered = true;
            }
            m_enteredCondition.notify_all();

            std::unique_lock<std::mutex> lock(m_mutex);
            m_releaseCondition.wait(
                lock,
                [this, &context]
                {
                    return m_release || context.IsCancellationRequested();
                });
            if (context.IsCancellationRequested())
            {
                outError = {
                    RVX::Resource::ResourceLoadErrorCode::Cancelled,
                    "Coordinator validation load was cancelled"};
                return false;
            }
            lock.unlock();

            auto* model = new RVX::Resource::ModelResource();
            model->SetId(context.rootResourceId);
            model->SetPath(context.requestedPath);
            model->SetName("CoordinatorModel");
            model->SetRootNode(std::make_shared<RVX::Node>("Root"));
            return outBundle.SetRoot(
                RVX::Resource::ResourceHandle<RVX::Resource::IResource>(model));
        }

        bool WaitUntilEntered()
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_enteredCondition.wait_for(
                lock,
                std::chrono::seconds(5),
                [this] { return m_entered; });
        }

        void Release()
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_release = true;
            }
            m_releaseCondition.notify_all();
        }

        std::atomic<RVX::uint32> prepareCount{0};

    private:
        std::mutex m_mutex;
        std::condition_variable m_enteredCondition;
        std::condition_variable m_releaseCondition;
        bool m_entered = false;
        bool m_release = false;
    };
} // namespace

TEST(SceneAssetLoadCoordinatorValidation,
     CoalescesCpuLoadButCreatesIndependentSceneInstances)
{
    RVX::Resource::ResourceManagerConfig config;
    config.asyncThreadCount = 1;
    config.runtimePolicy.mode = RVX::ResourceRuntimeMode::Editor;
    config.runtimePolicy.allowSourceAssetReads = true;

    RVX::Resource::ResourceSubsystem resources;
    resources.Initialize(config);
    auto loader = std::make_unique<BlockingModelLoader>();
    BlockingModelLoader* loaderView = loader.get();
    resources.RegisterLoader(RVX::ResourceType::Model, std::move(loader));

    RVX::ResourceSceneAdapters::RegisterDefaults();
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());
    RVX::SceneAssetLoadCoordinator coordinator(scene, resources);

    std::string error;
    RVX::SceneModelLoadDesc desc;
    desc.path = "coordinator-shared.gltf";
    const RVX::SceneAssetLoadHandle first =
        coordinator.RequestModel(desc, error);
    ASSERT_TRUE(first.IsValid()) << error;
    const RVX::SceneAssetLoadHandle second =
        coordinator.RequestModel(desc, error);
    ASSERT_TRUE(second.IsValid()) << error;
    EXPECT_EQ(coordinator.GetResourceRequestId(first),
              coordinator.GetResourceRequestId(second));
    ASSERT_TRUE(loaderView->WaitUntilEntered());
    EXPECT_EQ(scene.GetActorCount(), 0U);
    EXPECT_EQ(loaderView->prepareCount.load(), 1U);

    loaderView->Release();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        resources.Tick(0.0f);
        static_cast<void>(coordinator.Update());
        const RVX::SceneAssetStatus* firstStatus =
            coordinator.GetStatus(first);
        const RVX::SceneAssetStatus* secondStatus =
            coordinator.GetStatus(second);
        if (firstStatus && secondStatus && firstStatus->IsFullyResident() &&
            secondStatus->IsFullyResident())
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ASSERT_NE(coordinator.GetStatus(first), nullptr);
    ASSERT_NE(coordinator.GetStatus(second), nullptr);
    EXPECT_TRUE(coordinator.GetStatus(first)->IsFullyResident());
    EXPECT_TRUE(coordinator.GetStatus(second)->IsFullyResident());
    ASSERT_NE(coordinator.GetModelInstance(first), nullptr);
    ASSERT_NE(coordinator.GetModelInstance(second), nullptr);
    EXPECT_NE(coordinator.GetModelInstance(first)->rootActor,
              coordinator.GetModelInstance(second)->rootActor);
    EXPECT_EQ(scene.GetActorCount(), 2U);

    EXPECT_TRUE(coordinator.Cancel(first));
    EXPECT_EQ(scene.GetActorCount(), 1U);
    EXPECT_TRUE(coordinator.GetStatus(second)->IsFullyResident());

    const RVX::SceneAssetLoadHandle stale = second;
    coordinator.CancelAll();
    EXPECT_EQ(scene.GetActorCount(), 0U);
    EXPECT_FALSE(coordinator.IsValid(stale));

    const RVX::SceneAssetLoadHandle replacement =
        coordinator.RequestModel(desc, error);
    ASSERT_TRUE(replacement.IsValid()) << error;
    EXPECT_NE(replacement, stale);
    resources.Tick(0.0f);
    static_cast<void>(coordinator.Update());
    EXPECT_TRUE(coordinator.GetStatus(replacement)->IsFullyResident());

    coordinator.CancelAll();
    scene.Shutdown();
    resources.Deinitialize();
}
