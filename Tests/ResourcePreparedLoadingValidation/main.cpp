#include "Resource/ResourceManager.h"
#include "Core/Log.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace RVX;
using namespace RVX::Resource;

namespace
{
    class LogEnvironment final : public ::testing::Environment
    {
    public:
        void SetUp() override
        {
            Log::Initialize();
        }

        void TearDown() override
        {
            Log::Shutdown();
        }
    };

    [[maybe_unused]] const auto* g_logEnvironment =
        ::testing::AddGlobalTestEnvironment(new LogEnvironment);

    class PreparedTestResource : public IResource
    {
    public:
        ResourceType GetType() const override { return ResourceType::Texture; }
        const char* GetTypeName() const override { return "PreparedTestResource"; }
    };

    class ThrowingPreparedResource final : public PreparedTestResource
    {
    public:
        std::vector<ResourceId> GetRequiredDependencies() const override
        {
            throw std::runtime_error("Intentional owner-thread publication failure.");
        }
    };

    class TestImportProfileState final : public ResourceLoadPreparationState
    {
    public:
        explicit TestImportProfileState(uint64 value)
            : hash(value)
        {
        }

        uint64 hash = 0;
    };

    class PreparedTestLoader final : public IResourceLoader
    {
    public:
        ResourceType GetResourceType() const override { return ResourceType::Texture; }
        std::vector<std::string> GetSupportedExtensions() const override { return {".png"}; }
        IResource* Load(const std::string&) override
        {
            syncLoadCount.fetch_add(1, std::memory_order_relaxed);
            return new PreparedTestResource();
        }

        bool CapturePreparationState(
            uint64 requestedImportOptionsHash,
            ResourceLoadPreparationStateRef& outState,
            uint64& outCanonicalImportOptionsHash,
            ResourceLoadError& outError) const override
        {
            if (!captureMutableProfile)
            {
                outState.reset();
                outCanonicalImportOptionsHash = requestedImportOptionsHash;
                outError = {};
                return true;
            }

            const uint64 activeHash = activeImportProfileHash.load(std::memory_order_acquire);
            if (requestedImportOptionsHash != 0 && requestedImportOptionsHash != activeHash)
            {
                outError = {ResourceLoadErrorCode::InvalidRequest,
                            "Requested test import profile does not match the active profile."};
                return false;
            }
            outState = std::make_shared<TestImportProfileState>(activeHash);
            outCanonicalImportOptionsHash = activeHash;
            if (switchProfileAfterCapture.exchange(false, std::memory_order_acq_rel))
            {
                activeImportProfileHash.store(
                    profileAfterCapture.load(std::memory_order_acquire),
                    std::memory_order_release);
            }
            outError = {};
            return true;
        }

        bool Prepare(const ResourceLoadPreparationContext& context,
                     PreparedResourceBundle& outBundle,
                     ResourceLoadError& outError) override
        {
            prepareCount.fetch_add(1, std::memory_order_relaxed);
            if (const auto profile =
                    std::dynamic_pointer_cast<const TestImportProfileState>(context.loaderState))
            {
                preparedProfileHash.store(profile->hash, std::memory_order_release);
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                workerThreads.push_back(std::this_thread::get_id());
                ++m_entered;
            }
            m_enteredCondition.notify_all();

            if (blockPreparation)
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                while (!m_release && !context.IsCancellationRequested())
                {
                    m_releaseCondition.wait_for(lock, std::chrono::milliseconds(2));
                }
                if (context.IsCancellationRequested())
                {
                    outError = {ResourceLoadErrorCode::Cancelled,
                                "Preparation observed cooperative cancellation."};
                    return false;
                }
            }
            if (failPreparation)
            {
                outError = {ResourceLoadErrorCode::LoaderFailure, "Intentional prepare failure."};
                return false;
            }

            IResource* resource = failPublicationTransaction
                ? static_cast<IResource*>(new ThrowingPreparedResource())
                : static_cast<IResource*>(new PreparedTestResource());
            resource->SetId(rootRebindCollision
                                ? GenerateResourceId(context.resourceIdentityPath + "#worker-root")
                                : context.rootResourceId);
            resource->SetPath(context.requestedPath);
            resource->SetName(std::filesystem::path(context.requestedPath).stem().string());
            if (throwOnLoadedObserver)
            {
                resource->SetOnLoaded(
                    [](IResource*)
                    {
                        throw std::runtime_error("Intentional on-loaded observer failure.");
                    });
            }
            if (failPublicationTransaction || rootRebindCollision)
            {
                auto* dependency = new PreparedTestResource();
                dependency->SetId(rootRebindCollision
                                      ? context.rootResourceId
                                      : GenerateResourceId(context.resourceIdentityPath + "#dependency"));
                dependency->SetPath(context.resourceIdentityPath + "#dependency");
                dependency->SetName("PreparedDependency");
                if (!outBundle.AddDependency(ResourceHandle<IResource>(dependency)))
                {
                    outError = {ResourceLoadErrorCode::LoaderFailure,
                                "Unable to stage test dependency."};
                    return false;
                }
            }
            return outBundle.SetRoot(ResourceHandle<IResource>(resource));
        }

        bool SupportsPreparedLoading() const override { return true; }

        bool WaitUntilEntered(uint32 count)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_enteredCondition.wait_for(lock,
                                               std::chrono::seconds(5),
                                               [this, count] { return m_entered >= count; });
        }

        void Release()
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_release = true;
            }
            m_releaseCondition.notify_all();
        }

        std::atomic<uint32> prepareCount{0};
        std::atomic<uint32> syncLoadCount{0};
        bool blockPreparation = false;
        bool failPreparation = false;
        bool failPublicationTransaction = false;
        bool rootRebindCollision = false;
        bool throwOnLoadedObserver = false;
        bool captureMutableProfile = false;
        mutable std::atomic<uint64> activeImportProfileHash{0};
        std::atomic<uint64> preparedProfileHash{0};
        mutable std::atomic<bool> switchProfileAfterCapture{false};
        mutable std::atomic<uint64> profileAfterCapture{0};
        std::vector<std::thread::id> workerThreads;

    private:
        std::mutex m_mutex;
        std::condition_variable m_enteredCondition;
        std::condition_variable m_releaseCondition;
        uint32 m_entered = 0;
        bool m_release = false;
    };

    class ManagerGuard final
    {
    public:
        explicit ManagerGuard(uint32 workerCount)
        {
            ResourceManagerConfig config;
            config.asyncThreadCount = static_cast<int>(workerCount);
            config.runtimePolicy.mode = ResourceRuntimeMode::Editor;
            config.runtimePolicy.allowSourceAssetReads = true;
            ResourceManager::Get().Initialize(config);
        }

        ~ManagerGuard()
        {
            ResourceManager::Get().Shutdown();
            JobSystem::Get().Shutdown();
        }
    };
} // namespace

TEST(ResourcePreparedLoadingValidation, CoalescesIdenticalKeysAndPublishesOnlyFromOwnerPump)
{
    ManagerGuard guard(0);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    ResourceManager::Get().RegisterLoader(ResourceType::Texture, std::move(loader));

    auto first = ResourceManager::Get().RequestAsync<PreparedTestResource>("coalesced.png");
    auto second = ResourceManager::Get().RequestAsync<PreparedTestResource>("coalesced.png");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.GetRequestId(), second.GetRequestId());
    EXPECT_EQ(loaderPtr->prepareCount.load(std::memory_order_relaxed), 1u);
    EXPECT_FALSE(first.TryGet());

    ResourceManager::Get().ProcessCompletedLoads();
    EXPECT_TRUE(first.TryGet());
    EXPECT_TRUE(second.TryGet());
    EXPECT_TRUE(ResourceManager::Get().IsLoaded("coalesced.png"));
}

TEST(ResourcePreparedLoadingValidation, ConcurrentCallersCoalesceAndCallbacksReturnToOwnerPump)
{
    ManagerGuard guard(2);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    loaderPtr->blockPreparation = true;
    ResourceManager::Get().RegisterLoader(ResourceType::Texture, std::move(loader));

    auto ownerHandle = ResourceManager::Get().RequestAsync<PreparedTestResource>("shared.png");
    ASSERT_TRUE(ownerHandle);
    ASSERT_TRUE(loaderPtr->WaitUntilEntered(1));

    ResourceLoadHandle<PreparedTestResource> workerHandle;
    std::atomic<bool> callbackCalled{false};
    std::thread::id callbackThread;
    std::thread requester(
        [&]
        {
            workerHandle =
                ResourceManager::Get().RequestAsync<PreparedTestResource>("./shared.png");
            ResourceManager::Get().LoadAsync<PreparedTestResource>(
                "shared.png",
                [&](ResourceHandle<PreparedTestResource> resource)
                {
                    EXPECT_TRUE(resource);
                    callbackThread = std::this_thread::get_id();
                    callbackCalled.store(true, std::memory_order_release);
                });
        });
    requester.join();

    ASSERT_TRUE(workerHandle);
    EXPECT_EQ(workerHandle.GetRequestId(), ownerHandle.GetRequestId());
    EXPECT_EQ(loaderPtr->prepareCount.load(std::memory_order_relaxed), 1u);
    EXPECT_FALSE(callbackCalled.load(std::memory_order_acquire));

    loaderPtr->Release();
    for (uint32 attempt = 0;
         attempt < 100 && (!ownerHandle.TryGet() ||
                           !callbackCalled.load(std::memory_order_acquire));
         ++attempt)
    {
        ResourceManager::Get().ProcessCompletedLoads();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    EXPECT_TRUE(ownerHandle.TryGet());
    EXPECT_TRUE(workerHandle.TryGet());
    EXPECT_TRUE(callbackCalled.load(std::memory_order_acquire));
    EXPECT_EQ(callbackThread, std::this_thread::get_id());
}

TEST(ResourcePreparedLoadingValidation, DistinctAssetsCanPrepareConcurrentlyWithoutOwnerPublication)
{
    ManagerGuard guard(2);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    loaderPtr->blockPreparation = true;
    ResourceManager::Get().RegisterLoader(ResourceType::Texture, std::move(loader));

    auto first = ResourceManager::Get().RequestAsync<PreparedTestResource>("first.png");
    auto second = ResourceManager::Get().RequestAsync<PreparedTestResource>("second.png");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(loaderPtr->WaitUntilEntered(2));
    EXPECT_FALSE(first.TryGet());
    EXPECT_FALSE(second.TryGet());

    loaderPtr->Release();
    for (uint32 attempt = 0; attempt < 100 && (!first.TryGet() || !second.TryGet()); ++attempt)
    {
        ResourceManager::Get().ProcessCompletedLoads();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(first.TryGet());
    EXPECT_TRUE(second.TryGet());
    EXPECT_EQ(loaderPtr->prepareCount.load(std::memory_order_relaxed), 2u);
}

TEST(ResourcePreparedLoadingValidation, CancellationAndPreparedFailureDoNotPublishPartialCacheState)
{
    ManagerGuard guard(0);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    ResourceManager::Get().RegisterLoader(ResourceType::Texture, std::move(loader));

    auto cancelled = ResourceManager::Get().RequestAsync<PreparedTestResource>("cancelled.png");
    ASSERT_TRUE(cancelled);
    EXPECT_TRUE(cancelled.Cancel());
    ResourceManager::Get().ProcessCompletedLoads();
    EXPECT_FALSE(ResourceManager::Get().IsLoaded("cancelled.png"));

    loaderPtr->failPreparation = true;
    auto failed = ResourceManager::Get().RequestAsync<PreparedTestResource>("failed.png");
    ASSERT_TRUE(failed);
    ResourceManager::Get().ProcessCompletedLoads();
    const ResourceLoadSnapshot failedSnapshot = failed.GetSnapshot();
    EXPECT_EQ(failedSnapshot.state, ResourceLoadState::Failed);
    EXPECT_FALSE(ResourceManager::Get().IsLoaded("failed.png"));
}

TEST(ResourcePreparedLoadingValidation, AssetKeyOptionsSplitOperationsAndCanonicalAliasesShareDefaultIdentity)
{
    ManagerGuard guard(0);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    ResourceManager::Get().RegisterLoader(ResourceType::Texture, std::move(loader));

    ResourceLoadOptions firstOptions;
    firstOptions.importOptionsHash = 101;
    ResourceLoadOptions secondOptions;
    secondOptions.importOptionsHash = 202;
    auto first = ResourceManager::Get().RequestAsync<PreparedTestResource>("options.png", firstOptions);
    auto second = ResourceManager::Get().RequestAsync<PreparedTestResource>("options.png", secondOptions);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_NE(first.GetRequestId(), second.GetRequestId());
    ResourceManager::Get().ProcessCompletedLoads();
    ASSERT_TRUE(first.TryGet());
    ASSERT_TRUE(second.TryGet());
    EXPECT_NE(first.TryGet().Get(), second.TryGet().Get());
    EXPECT_EQ(loaderPtr->prepareCount.load(std::memory_order_relaxed), 2u);
    EXPECT_TRUE(ResourceManager::Get().IsLoaded(first.GetSnapshot().assetKey));
    EXPECT_TRUE(ResourceManager::Get().IsLoaded(second.GetSnapshot().assetKey));
    ASSERT_EQ(ResourceManager::Get().GetRegistry()->GetCount(), 2u);
    ASSERT_TRUE(ResourceManager::Get().GetRegistry()->FindByPath("options.png"));
    EXPECT_EQ(ResourceManager::Get().GetRegistry()->FindByPath("options.png")->id,
              second.TryGet().GetId());

    ResourceManager::Get().Unload(second.GetSnapshot().assetKey);
    EXPECT_TRUE(ResourceManager::Get().IsLoaded(first.GetSnapshot().assetKey));
    EXPECT_FALSE(ResourceManager::Get().IsLoaded(second.GetSnapshot().assetKey));
    ASSERT_TRUE(ResourceManager::Get().GetRegistry()->FindByPath("options.png"));
    EXPECT_EQ(ResourceManager::Get().GetRegistry()->FindByPath("options.png")->id,
              first.TryGet().GetId());

    auto async = ResourceManager::Get().RequestAsync<PreparedTestResource>("./aliases.png");
    ASSERT_TRUE(async);
    ResourceManager::Get().ProcessCompletedLoads();
    const ResourceHandle<PreparedTestResource> asyncResource = async.TryGet();
    ASSERT_TRUE(asyncResource);
    IResource* const synchronousAlias = ResourceManager::Get().LoadResource("aliases.png");
    EXPECT_EQ(synchronousAlias, asyncResource.Get());
    EXPECT_EQ(loaderPtr->syncLoadCount.load(std::memory_order_relaxed), 0u);
    EXPECT_TRUE(ResourceManager::Get().IsLoaded("aliases.png"));
    EXPECT_TRUE(ResourceManager::Get().IsLoaded("./aliases.png"));
}

TEST(ResourcePreparedLoadingValidation, PreparedCacheCommitIsLoadedAndBudgetCannotEvictPinnedBatch)
{
    ResourceCache directCache;
    ResourceHandle<PreparedTestResource> direct(new PreparedTestResource());
    direct->SetId(GenerateResourceId("prepared-cache-visibility"));
    direct->SetPath("prepared-cache-visibility");
    direct->SetName("PreparedCacheVisibility");
    ASSERT_FALSE(direct->IsLoaded());
    ASSERT_TRUE(directCache.StorePreparedBatch({direct.Get()}));
    EXPECT_TRUE(direct->IsLoaded());
    EXPECT_TRUE(directCache.ContainsLoaded(direct.GetId()));

    ManagerGuard guard(0);
    ResourceManager::Get().SetCacheLimit(1);
    auto loader = std::make_unique<PreparedTestLoader>();
    ResourceManager::Get().RegisterLoader(ResourceType::Texture, std::move(loader));

    auto request = ResourceManager::Get().RequestAsync<PreparedTestResource>("pinned-budget.png");
    ASSERT_TRUE(request);
    ResourceManager::Get().ProcessCompletedLoads();
    ASSERT_TRUE(request.TryGet());
    EXPECT_TRUE(request.TryGet()->IsLoaded());
    EXPECT_TRUE(ResourceManager::Get().IsLoaded(request.GetSnapshot().assetKey));
}

TEST(ResourcePreparedLoadingValidation, LoaderProfileSnapshotAndAssetKeyCannotDriftBeforeWorkerRuns)
{
    ManagerGuard guard(1);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    loaderPtr->captureMutableProfile = true;
    loaderPtr->blockPreparation = true;
    loaderPtr->activeImportProfileHash.store(101, std::memory_order_release);
    ResourceManager::Get().RegisterLoader(ResourceType::Texture, std::move(loader));

    auto first = ResourceManager::Get().RequestAsync<PreparedTestResource>("profile.png");
    ASSERT_TRUE(first);
    EXPECT_EQ(first.GetSnapshot().assetKey.importOptionsHash, 101u);
    loaderPtr->activeImportProfileHash.store(202, std::memory_order_release);
    ASSERT_TRUE(loaderPtr->WaitUntilEntered(1));
    loaderPtr->Release();
    for (uint32 attempt = 0; attempt < 100 && !first.TryGet(); ++attempt)
    {
        ResourceManager::Get().ProcessCompletedLoads();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(first.TryGet());
    EXPECT_EQ(loaderPtr->preparedProfileHash.load(std::memory_order_acquire), 101u);

    ResourceLoadOptions staleOptions;
    staleOptions.importOptionsHash = 101;
    auto stale = ResourceManager::Get().RequestAsync<PreparedTestResource>("other-profile.png",
                                                                           staleOptions);
    EXPECT_FALSE(stale);

    auto second = ResourceManager::Get().RequestAsync<PreparedTestResource>("profile.png");
    ASSERT_TRUE(second);
    EXPECT_NE(second.GetRequestId(), first.GetRequestId());
    EXPECT_EQ(second.GetSnapshot().assetKey.importOptionsHash, 202u);
}

TEST(ResourcePreparedLoadingValidation, ResourceIdReloadFailsClosedWhenImportProfileCannotBeReproduced)
{
    ManagerGuard guard(0);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    loaderPtr->captureMutableProfile = true;
    loaderPtr->activeImportProfileHash.store(101, std::memory_order_release);
    ResourceManager::Get().RegisterLoader(ResourceType::Texture, std::move(loader));

    ResourceId variantId = InvalidResourceId;
    {
        auto request =
            ResourceManager::Get().RequestAsync<PreparedTestResource>("reload-profile.png");
        ASSERT_TRUE(request);
        ResourceManager::Get().ProcessCompletedLoads();
        const ResourceHandle<PreparedTestResource> resource = request.TryGet();
        ASSERT_TRUE(resource);
        variantId = resource.GetId();
    }

    ResourceManager::Get().UnloadUnused();
    ASSERT_FALSE(ResourceManager::Get().IsLoaded(variantId));
    ASSERT_TRUE(ResourceManager::Get().GetRegistry()->FindById(variantId));

    loaderPtr->activeImportProfileHash.store(202, std::memory_order_release);
    EXPECT_EQ(ResourceManager::Get().LoadResource(variantId), nullptr);
    EXPECT_EQ(ResourceManager::Get().GetLastLoadDiagnostic().failure,
              ResourceLoadFailureCode::AssetIdentityMismatch);
    EXPECT_EQ(loaderPtr->syncLoadCount.load(std::memory_order_relaxed), 0u);

    loaderPtr->activeImportProfileHash.store(101, std::memory_order_release);
    loaderPtr->profileAfterCapture.store(202, std::memory_order_release);
    loaderPtr->switchProfileAfterCapture.store(true, std::memory_order_release);
    IResource* const reloaded = ResourceManager::Get().LoadResource(variantId);
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(reloaded->GetId(), variantId);
    EXPECT_EQ(loaderPtr->preparedProfileHash.load(std::memory_order_acquire), 101u);
    EXPECT_EQ(loaderPtr->activeImportProfileHash.load(std::memory_order_acquire), 202u);
    EXPECT_EQ(loaderPtr->syncLoadCount.load(std::memory_order_relaxed), 0u);
}

TEST(ResourcePreparedLoadingValidation, ResourceIdReloadRejectsUnpersistedPlatformAndSchemaVariants)
{
    ManagerGuard guard(0);
    auto loader = std::make_unique<PreparedTestLoader>();
    ResourceManager::Get().RegisterLoader(ResourceType::Texture, std::move(loader));

    ResourceId variantId = InvalidResourceId;
    {
        ResourceLoadOptions options;
        options.platformProfileHash = 77;
        options.loaderSchemaVersion = 2;
        auto request = ResourceManager::Get().RequestAsync<PreparedTestResource>(
            "platform-profile.png",
            options);
        ASSERT_TRUE(request);
        ResourceManager::Get().ProcessCompletedLoads();
        const ResourceHandle<PreparedTestResource> resource = request.TryGet();
        ASSERT_TRUE(resource);
        variantId = resource.GetId();
    }

    ResourceManager::Get().UnloadUnused();
    ASSERT_FALSE(ResourceManager::Get().IsLoaded(variantId));
    EXPECT_EQ(ResourceManager::Get().LoadResource(variantId), nullptr);
    EXPECT_EQ(ResourceManager::Get().GetLastLoadDiagnostic().failure,
              ResourceLoadFailureCode::AssetIdentityMismatch);
}

TEST(ResourcePreparedLoadingValidation, SynchronousPathLoadConsumesTheCapturedImmutableProfile)
{
    ManagerGuard guard(0);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    loaderPtr->captureMutableProfile = true;
    loaderPtr->activeImportProfileHash.store(101, std::memory_order_release);
    loaderPtr->profileAfterCapture.store(202, std::memory_order_release);
    loaderPtr->switchProfileAfterCapture.store(true, std::memory_order_release);
    ResourceManager::Get().RegisterLoader(ResourceType::Texture, std::move(loader));

    IResource* const loaded = ResourceManager::Get().LoadResource(
        "synchronous-profile.png",
        ResourceType::Texture);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaderPtr->preparedProfileHash.load(std::memory_order_acquire), 101u);
    EXPECT_EQ(loaderPtr->activeImportProfileHash.load(std::memory_order_acquire), 202u);
    EXPECT_EQ(loaderPtr->syncLoadCount.load(std::memory_order_relaxed), 0u);
}

TEST(ResourcePreparedLoadingValidation, OwnerPublishFailureRollsBackDependenciesAndShutdownCancelsWorkers)
{
    {
        ManagerGuard guard(0);
        auto loader = std::make_unique<PreparedTestLoader>();
        PreparedTestLoader* loaderPtr = loader.get();
        loaderPtr->failPublicationTransaction = true;
        ResourceManager::Get().RegisterLoader(ResourceType::Texture, std::move(loader));

        std::atomic<uint32> lifecycleEvents{0};
        ResourceManager::Get().SetLifecycleEventCallback(
            [&lifecycleEvents](const ResourceLifecycleEvent&)
            {
                lifecycleEvents.fetch_add(1, std::memory_order_relaxed);
            });

        auto failed = ResourceManager::Get().RequestAsync<PreparedTestResource>("transaction.png");
        ASSERT_TRUE(failed);
        ResourceManager::Get().ProcessCompletedLoads();
        EXPECT_EQ(failed.GetSnapshot().state, ResourceLoadState::Failed);
        EXPECT_EQ(ResourceManager::Get().GetRegistry()->GetCount(), 0u);
        EXPECT_EQ(ResourceManager::Get().GetCache().GetStats().totalResources, 0u);
        EXPECT_EQ(lifecycleEvents.load(std::memory_order_relaxed), 0u);
    }

    ManagerGuard guard(2);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    loaderPtr->blockPreparation = true;
    ResourceManager::Get().RegisterLoader(ResourceType::Texture, std::move(loader));

    std::atomic<uint32> lifecycleEvents{0};
    ResourceManager::Get().SetLifecycleEventCallback(
        [&lifecycleEvents](const ResourceLifecycleEvent&)
        {
            lifecycleEvents.fetch_add(1, std::memory_order_relaxed);
        });
    auto pending = ResourceManager::Get().RequestAsync<PreparedTestResource>("shutdown.png");
    ASSERT_TRUE(pending);
    ASSERT_TRUE(loaderPtr->WaitUntilEntered(1));
    ResourceManager::Get().Shutdown();
    EXPECT_EQ(pending.GetSnapshot().state, ResourceLoadState::Cancelled);
    EXPECT_EQ(lifecycleEvents.load(std::memory_order_relaxed), 0u);
}

TEST(ResourcePreparedLoadingValidation, CanonicalRootCollisionFailsAndObserverFailureCannotUndoCommit)
{
    ManagerGuard guard(0);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    ResourceManager::Get().RegisterLoader(ResourceType::Texture, std::move(loader));

    loaderPtr->rootRebindCollision = true;
    auto collision = ResourceManager::Get().RequestAsync<PreparedTestResource>("collision.png");
    ASSERT_TRUE(collision);
    ResourceManager::Get().ProcessCompletedLoads();
    EXPECT_EQ(collision.GetSnapshot().state, ResourceLoadState::Failed);
    EXPECT_EQ(ResourceManager::Get().GetRegistry()->GetCount(), 0u);
    EXPECT_EQ(ResourceManager::Get().GetCache().GetStats().totalResources, 0u);

    loaderPtr->rootRebindCollision = false;
    loaderPtr->throwOnLoadedObserver = true;
    std::atomic<uint32> readyEvents{0};
    ResourceManager::Get().SetLifecycleEventCallback(
        [&readyEvents](const ResourceLifecycleEvent& event)
        {
            if (event.type == ResourceLifecycleEventType::Ready)
            {
                readyEvents.fetch_add(1, std::memory_order_relaxed);
            }
        });
    auto committed = ResourceManager::Get().RequestAsync<PreparedTestResource>("observer.png");
    ASSERT_TRUE(committed);
    ResourceManager::Get().ProcessCompletedLoads();
    EXPECT_EQ(committed.GetSnapshot().state, ResourceLoadState::Ready);
    ASSERT_TRUE(committed.TryGet());
    EXPECT_TRUE(committed.TryGet()->IsLoaded());
    EXPECT_TRUE(ResourceManager::Get().IsLoaded("observer.png"));
    EXPECT_EQ(ResourceManager::Get().GetRegistry()->GetCount(), 1u);
    EXPECT_EQ(readyEvents.load(std::memory_order_relaxed), 1u);
}

TEST(ResourcePreparedLoadingValidation, ShutdownCannotDispatchOwnerCallbacksFromWorkerThread)
{
    ManagerGuard guard(0);
    auto loader = std::make_unique<PreparedTestLoader>();
    ResourceManager::Get().RegisterLoader(ResourceType::Texture, std::move(loader));

    std::atomic<bool> callbackCalled{false};
    std::thread::id callbackThread;
    ResourceManager::Get().LoadAsync<PreparedTestResource>(
        "shutdown-affinity.png",
        [&](ResourceHandle<PreparedTestResource> resource)
        {
            EXPECT_TRUE(resource);
            callbackThread = std::this_thread::get_id();
            callbackCalled.store(true, std::memory_order_release);
        });

    std::thread shutdownThread([] { ResourceManager::Get().Shutdown(); });
    shutdownThread.join();
    EXPECT_TRUE(ResourceManager::Get().IsInitialized());
    EXPECT_FALSE(callbackCalled.load(std::memory_order_acquire));

    ResourceManager::Get().ProcessCompletedLoads();
    EXPECT_TRUE(callbackCalled.load(std::memory_order_acquire));
    EXPECT_EQ(callbackThread, std::this_thread::get_id());
}
