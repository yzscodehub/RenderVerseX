#include "Resource/ResourceManager.h"
#include "AssetImport/SourceResourceLoaders.h"
#include "AssetImport/GLTFImporter.h"
#include "Resource/Types/ModelResource.h"
#include "Core/Log.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
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

    class ThrowingSecondGetTypePreparedResource final : public PreparedTestResource
    {
    public:
        ResourceType GetType() const override
        {
            if (++m_getTypeCallCount == 2)
            {
                throw std::runtime_error(
                    "Intentional second owner-publication GetType failure.");
            }
            return ResourceType::Texture;
        }

    private:
        mutable uint32 m_getTypeCallCount = 0;
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
        bool Supports(
            const ResourceLoaderRequest& request) const noexcept override
        {
            return request.type == ResourceType::Texture &&
                request.domain == ResourceLoadDomain::Source &&
                std::filesystem::path(request.resolvedPath).extension() == ".png";
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
                : throwOnSecondGetType
                      ? static_cast<IResource*>(new ThrowingSecondGetTypePreparedResource())
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
            if (failPublicationTransaction || rootRebindCollision ||
                forcedDependencyResourceId != InvalidResourceId)
            {
                auto* dependency = new PreparedTestResource();
                dependency->SetId(forcedDependencyResourceId != InvalidResourceId
                                      ? forcedDependencyResourceId
                                      : rootRebindCollision
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
            if (!outBundle.SetRoot(ResourceHandle<IResource>(resource)))
            {
                return false;
            }
            if (!observedContentIdentity.IsEmpty() &&
                !outBundle.SetObservedContentIdentity(observedContentIdentity))
            {
                outError = {ResourceLoadErrorCode::LoaderFailure,
                            "Test loader received an invalid observed content identity."};
                return false;
            }
            return true;
        }

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
        bool throwOnSecondGetType = false;
        bool throwOnLoadedObserver = false;
        ResourceId forcedDependencyResourceId = InvalidResourceId;
        bool captureMutableProfile = false;
        mutable std::atomic<uint64> activeImportProfileHash{0};
        std::atomic<uint64> preparedProfileHash{0};
        mutable std::atomic<bool> switchProfileAfterCapture{false};
        mutable std::atomic<uint64> profileAfterCapture{0};
        std::vector<std::thread::id> workerThreads;
        ResourceContentIdentity observedContentIdentity;

    private:
        std::mutex m_mutex;
        std::condition_variable m_enteredCondition;
        std::condition_variable m_releaseCondition;
        uint32 m_entered = 0;
        bool m_release = false;
    };

    class CountingChangeMonitor final : public IResourceChangeMonitor
    {
    public:
        void Watch(ResourceId resourceId, std::string path) override
        {
            (void)resourceId;
            (void)path;
            ++watchCount;
        }

        void Unwatch(ResourceId resourceId) override
        {
            (void)resourceId;
        }

        std::vector<ResourceSourceChange> Poll() override
        {
            return {};
        }

        uint32 watchCount = 0;
    };

    class ManagerGuard final
    {
    public:
        explicit ManagerGuard(
            uint32 workerCount,
            bool useBuiltinLoaders = false,
            bool useChangeMonitor = false)
        {
            m_config.asyncThreadCount = static_cast<int>(workerCount);
            m_config.runtimePolicy.mode = ResourceRuntimeMode::Editor;
            m_config.runtimePolicy.allowSourceAssetReads = true;
            m_dependencies = useBuiltinLoaders
                ? CreateRuntimeResourceManagerDependencies()
                : ResourceManagerDependencies{};
            if (useChangeMonitor)
            {
                auto monitor = std::make_unique<CountingChangeMonitor>();
                m_changeMonitor = monitor.get();
                m_dependencies.changeMonitor = std::move(monitor);
            }
        }

        ~ManagerGuard()
        {
            m_manager.Shutdown();
            JobSystem::Get().Shutdown();
        }

        bool AddLoader(std::unique_ptr<IResourceLoader> loader)
        {
            if (m_manager.IsInitialized())
                return false;
            std::string error;
            return m_dependencies.loaderCatalog.Add(
                std::move(loader), error);
        }

        ResourceManager& GetManager()
        {
            if (!m_manager.IsInitialized())
                m_manager.Initialize(m_config, std::move(m_dependencies));
            return m_manager;
        }

        [[nodiscard]] CountingChangeMonitor* GetChangeMonitor() const
        {
            return m_changeMonitor;
        }

    private:
        ResourceManager m_manager;
        ResourceManagerConfig m_config;
        ResourceManagerDependencies m_dependencies;
        CountingChangeMonitor* m_changeMonitor = nullptr;
    };

    ResourceContentIdentity MakeContentIdentity(
        std::string digest = "f009e85dae4e41d6d1cc9ba534a5948090c986993523280fd7e513df87c40300",
        uint64 byteCount = 2045)
    {
        ResourceContentIdentity identity;
        identity.schemaVersion = RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
        identity.domain = ResourceContentIdentityDomain::Source;
        identity.scope = ResourceContentIdentityScope::SelfContainedArtifact;
        identity.algorithm = ResourceContentHashAlgorithm::SHA256;
        identity.digest = std::move(digest);
        identity.byteCount = byteCount;
        identity.fileCount = 1;
        return identity;
    }

    std::filesystem::path FindR7TriangleFixture()
    {
        auto searchFrom = [](std::filesystem::path directory)
        {
            while (!directory.empty())
            {
                const std::filesystem::path fixture = directory /
                    "Tests/Fixtures/ModelViewer/R7Triangle.gltf";
                if (std::filesystem::is_regular_file(fixture))
                {
                    return fixture;
                }

                const std::filesystem::path parent = directory.parent_path();
                if (parent == directory)
                {
                    break;
                }
                directory = parent;
            }
            return std::filesystem::path{};
        };

        if (const std::filesystem::path fromWorkingDirectory =
                searchFrom(std::filesystem::current_path());
            !fromWorkingDirectory.empty())
        {
            return fromWorkingDirectory;
        }
        return searchFrom(std::filesystem::absolute(std::filesystem::path(__FILE__)).parent_path());
    }
} // namespace

TEST(ResourcePreparedLoadingValidation, CoalescesIdenticalKeysAndPublishesOnlyFromOwnerPump)
{
    ManagerGuard guard(0);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    ASSERT_TRUE(guard.AddLoader(std::move(loader)));

    auto first = guard.GetManager().RequestAsync<PreparedTestResource>("coalesced.png");
    auto second = guard.GetManager().RequestAsync<PreparedTestResource>("coalesced.png");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.GetRequestId(), second.GetRequestId());
    EXPECT_EQ(loaderPtr->prepareCount.load(std::memory_order_relaxed), 1u);
    EXPECT_FALSE(first.TryGet());

    guard.GetManager().ProcessCompletedLoads();
    EXPECT_TRUE(first.TryGet());
    EXPECT_TRUE(second.TryGet());
    EXPECT_TRUE(guard.GetManager().IsLoaded("coalesced.png"));
}

TEST(ResourcePreparedLoadingValidation, ManagersOwnIndependentCachesCallbacksAndChangeMonitors)
{
    ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    config.runtimePolicy.mode = ResourceRuntimeMode::Editor;
    config.runtimePolicy.allowSourceAssetReads = true;
    config.enableHotReload = true;

    ResourceManagerDependencies firstDependencies;
    auto firstLoader = std::make_unique<PreparedTestLoader>();
    auto firstMonitor = std::make_unique<CountingChangeMonitor>();
    CountingChangeMonitor* firstMonitorObserver = firstMonitor.get();
    firstDependencies.changeMonitor = std::move(firstMonitor);
    std::string error;
    ASSERT_TRUE(firstDependencies.loaderCatalog.Add(
        std::move(firstLoader), error)) << error;

    ResourceManagerDependencies secondDependencies;
    auto secondLoader = std::make_unique<PreparedTestLoader>();
    auto secondMonitor = std::make_unique<CountingChangeMonitor>();
    CountingChangeMonitor* secondMonitorObserver = secondMonitor.get();
    secondDependencies.changeMonitor = std::move(secondMonitor);
    ASSERT_TRUE(secondDependencies.loaderCatalog.Add(
        std::move(secondLoader), error)) << error;

    ResourceManager first;
    ResourceManager second;
    first.Initialize(config, std::move(firstDependencies));
    second.Initialize(config, std::move(secondDependencies));
    ASSERT_TRUE(first.IsInitialized());
    ASSERT_TRUE(second.IsInitialized());

    uint32 firstReadyCount = 0;
    uint32 secondReadyCount = 0;
    first.SetLifecycleEventCallback(
        [&firstReadyCount](const ResourceLifecycleEvent& event)
        {
            if (event.type == ResourceLifecycleEventType::Ready)
                ++firstReadyCount;
        });
    second.SetLifecycleEventCallback(
        [&secondReadyCount](const ResourceLifecycleEvent& event)
        {
            if (event.type == ResourceLifecycleEventType::Ready)
                ++secondReadyCount;
        });

    ASSERT_TRUE(first.Load<PreparedTestResource>("first-manager.png"));
    ASSERT_TRUE(second.Load<PreparedTestResource>("second-manager.png"));
    first.ProcessCompletedLoads();
    second.ProcessCompletedLoads();

    EXPECT_TRUE(first.IsLoaded("first-manager.png"));
    EXPECT_FALSE(first.IsLoaded("second-manager.png"));
    EXPECT_TRUE(second.IsLoaded("second-manager.png"));
    EXPECT_FALSE(second.IsLoaded("first-manager.png"));
    EXPECT_EQ(firstReadyCount, 1u);
    EXPECT_EQ(secondReadyCount, 1u);
    EXPECT_EQ(firstMonitorObserver->watchCount, 1u);
    EXPECT_EQ(secondMonitorObserver->watchCount, 1u);

    first.Shutdown();
    EXPECT_FALSE(first.IsInitialized());
    EXPECT_TRUE(second.IsInitialized());
    EXPECT_TRUE(second.IsLoaded("second-manager.png"));

    ASSERT_TRUE(second.Load<PreparedTestResource>("second-manager-next.png"));
    second.ProcessCompletedLoads();
    EXPECT_EQ(firstReadyCount, 1u);
    EXPECT_EQ(secondReadyCount, 2u);
    EXPECT_EQ(secondMonitorObserver->watchCount, 2u);
    second.Shutdown();
}

TEST(ResourcePreparedLoadingValidation, ConcurrentCallersCoalesceAndCallbacksReturnToOwnerPump)
{
    ManagerGuard guard(2);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    loaderPtr->blockPreparation = true;
    ASSERT_TRUE(guard.AddLoader(std::move(loader)));

    auto ownerHandle = guard.GetManager().RequestAsync<PreparedTestResource>("shared.png");
    ASSERT_TRUE(ownerHandle);
    ASSERT_TRUE(loaderPtr->WaitUntilEntered(1));

    ResourceLoadHandle<PreparedTestResource> workerHandle;
    std::atomic<bool> callbackCalled{false};
    std::thread::id callbackThread;
    std::thread requester(
        [&]
        {
            workerHandle =
                guard.GetManager().RequestAsync<PreparedTestResource>("./shared.png");
            guard.GetManager().LoadAsync<PreparedTestResource>(
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
        guard.GetManager().ProcessCompletedLoads();
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
    ASSERT_TRUE(guard.AddLoader(std::move(loader)));

    auto first = guard.GetManager().RequestAsync<PreparedTestResource>("first.png");
    auto second = guard.GetManager().RequestAsync<PreparedTestResource>("second.png");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(loaderPtr->WaitUntilEntered(2));
    EXPECT_FALSE(first.TryGet());
    EXPECT_FALSE(second.TryGet());

    loaderPtr->Release();
    for (uint32 attempt = 0; attempt < 100 && (!first.TryGet() || !second.TryGet()); ++attempt)
    {
        guard.GetManager().ProcessCompletedLoads();
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
    ASSERT_TRUE(guard.AddLoader(std::move(loader)));

    auto cancelled = guard.GetManager().RequestAsync<PreparedTestResource>("cancelled.png");
    ASSERT_TRUE(cancelled);
    EXPECT_TRUE(cancelled.Cancel());
    guard.GetManager().ProcessCompletedLoads();
    EXPECT_FALSE(guard.GetManager().IsLoaded("cancelled.png"));

    loaderPtr->failPreparation = true;
    auto failed = guard.GetManager().RequestAsync<PreparedTestResource>("failed.png");
    ASSERT_TRUE(failed);
    guard.GetManager().ProcessCompletedLoads();
    const ResourceLoadSnapshot failedSnapshot = failed.GetSnapshot();
    EXPECT_EQ(failedSnapshot.state, ResourceLoadState::Failed);
    EXPECT_FALSE(guard.GetManager().IsLoaded("failed.png"));
}

TEST(ResourcePreparedLoadingValidation, AssetKeyOptionsSplitOperationsAndCanonicalAliasesShareDefaultIdentity)
{
    ManagerGuard guard(0);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    ASSERT_TRUE(guard.AddLoader(std::move(loader)));

    ResourceLoadOptions firstOptions;
    firstOptions.importOptionsHash = 101;
    ResourceLoadOptions secondOptions;
    secondOptions.importOptionsHash = 202;
    auto first = guard.GetManager().RequestAsync<PreparedTestResource>("options.png", firstOptions);
    auto second = guard.GetManager().RequestAsync<PreparedTestResource>("options.png", secondOptions);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_NE(first.GetRequestId(), second.GetRequestId());
    guard.GetManager().ProcessCompletedLoads();
    ASSERT_TRUE(first.TryGet());
    ASSERT_TRUE(second.TryGet());
    EXPECT_NE(first.TryGet().Get(), second.TryGet().Get());
    EXPECT_EQ(loaderPtr->prepareCount.load(std::memory_order_relaxed), 2u);
    EXPECT_TRUE(guard.GetManager().IsLoaded(first.GetSnapshot().assetKey));
    EXPECT_TRUE(guard.GetManager().IsLoaded(second.GetSnapshot().assetKey));
    ASSERT_EQ(guard.GetManager().GetRegistry()->GetCount(), 2u);
    ASSERT_TRUE(guard.GetManager().GetRegistry()->FindByPath("options.png"));
    EXPECT_EQ(guard.GetManager().GetRegistry()->FindByPath("options.png")->id,
              second.TryGet().GetId());

    EXPECT_EQ(guard.GetManager().Unload(second.GetSnapshot().assetKey),
              AssetResidencyReleaseResult::Unloaded);
    EXPECT_TRUE(guard.GetManager().IsLoaded(first.GetSnapshot().assetKey));
    EXPECT_FALSE(guard.GetManager().IsLoaded(second.GetSnapshot().assetKey));
    ASSERT_TRUE(guard.GetManager().GetRegistry()->FindByPath("options.png"));
    EXPECT_EQ(guard.GetManager().GetRegistry()->FindByPath("options.png")->id,
              first.TryGet().GetId());

    auto async = guard.GetManager().RequestAsync<PreparedTestResource>("./aliases.png");
    ASSERT_TRUE(async);
    guard.GetManager().ProcessCompletedLoads();
    const ResourceHandle<PreparedTestResource> asyncResource = async.TryGet();
    ASSERT_TRUE(asyncResource);
    IResource* const synchronousAlias = guard.GetManager().LoadResource("aliases.png");
    EXPECT_EQ(synchronousAlias, asyncResource.Get());
    EXPECT_EQ(loaderPtr->syncLoadCount.load(std::memory_order_relaxed), 0u);
    EXPECT_TRUE(guard.GetManager().IsLoaded("aliases.png"));
    EXPECT_TRUE(guard.GetManager().IsLoaded("./aliases.png"));
}

TEST(ResourcePreparedLoadingValidation, ContentIdentityVerificationPrecedesEveryPublication)
{
    ManagerGuard guard(0);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    const ResourceContentIdentity observed = MakeContentIdentity();
    loaderPtr->observedContentIdentity = observed;
    ASSERT_TRUE(guard.AddLoader(std::move(loader)));

    ResourceLoadOptions verifiedOptions;
    verifiedOptions.expectedContentIdentity = observed;
    auto verified = guard.GetManager().RequestAsync<PreparedTestResource>(
        "verified-identity.png", verifiedOptions);
    ASSERT_TRUE(verified);
    guard.GetManager().ProcessCompletedLoads();
    const ResourceHandle<PreparedTestResource> verifiedResource = verified.TryGet();
    ASSERT_TRUE(verifiedResource);
    const ResourceContentVerificationReceipt& verifiedReceipt =
        verifiedResource->GetContentVerificationReceipt();
    EXPECT_EQ(verifiedReceipt.status, ResourceContentVerificationStatus::Verified);
    EXPECT_TRUE(verifiedReceipt.IsVerified());
    EXPECT_EQ(verifiedReceipt.expected, observed);
    EXPECT_EQ(verifiedReceipt.observed, observed);
    EXPECT_STREQ(GetResourceContentVerificationStatusName(verifiedReceipt.status), "verified");
    const size_t publishedCount = guard.GetManager().GetRegistry()->GetCount();

    ResourceLoadOptions mismatchOptions;
    ResourceContentIdentity mismatch = observed;
    mismatch.digest[0] = 'e';
    mismatchOptions.expectedContentIdentity = mismatch;
    auto mismatched = guard.GetManager().RequestAsync<PreparedTestResource>(
        "verified-identity.png", mismatchOptions);
    ASSERT_TRUE(mismatched);
    guard.GetManager().ProcessCompletedLoads();
    EXPECT_EQ(mismatched.GetSnapshot().state, ResourceLoadState::Failed);
    EXPECT_EQ(mismatched.GetSnapshot().error.code, ResourceLoadErrorCode::ContentIdentityMismatch);
    EXPECT_FALSE(guard.GetManager().IsLoaded(mismatched.GetSnapshot().assetKey));
    EXPECT_EQ(guard.GetManager().GetRegistry()->GetCount(), publishedCount);
    EXPECT_EQ(guard.GetManager().GetCache().GetStats().totalResources, publishedCount);

    loaderPtr->observedContentIdentity = {};
    ResourceLoadOptions unavailableOptions;
    unavailableOptions.expectedContentIdentity = MakeContentIdentity(
        "a009e85dae4e41d6d1cc9ba534a5948090c986993523280fd7e513df87c40300");
    auto unavailable = guard.GetManager().RequestAsync<PreparedTestResource>(
        "unavailable-identity.png", unavailableOptions);
    ASSERT_TRUE(unavailable);
    guard.GetManager().ProcessCompletedLoads();
    EXPECT_EQ(unavailable.GetSnapshot().state, ResourceLoadState::Failed);
    EXPECT_EQ(unavailable.GetSnapshot().error.code,
              ResourceLoadErrorCode::ContentIdentityUnavailable);
    EXPECT_FALSE(guard.GetManager().IsLoaded(unavailable.GetSnapshot().assetKey));
    EXPECT_EQ(guard.GetManager().GetRegistry()->GetCount(), publishedCount);
}

TEST(ResourcePreparedLoadingValidation, UnrequestedObservedIdentityNeverClaimsVerification)
{
    ManagerGuard guard(0);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    loaderPtr->observedContentIdentity = MakeContentIdentity();
    ASSERT_TRUE(guard.AddLoader(std::move(loader)));

    auto request = guard.GetManager().RequestAsync<PreparedTestResource>("observed-identity.png");
    ASSERT_TRUE(request);
    guard.GetManager().ProcessCompletedLoads();
    const ResourceHandle<PreparedTestResource> resource = request.TryGet();
    ASSERT_TRUE(resource);
    const ResourceContentVerificationReceipt& receipt = resource->GetContentVerificationReceipt();
    EXPECT_EQ(receipt.status, ResourceContentVerificationStatus::Observed);
    EXPECT_FALSE(receipt.IsVerified());
    EXPECT_TRUE(receipt.expected.IsEmpty());
    EXPECT_TRUE(receipt.observed.IsValid());
}

TEST(ResourcePreparedLoadingValidation, ModelPreparedLoadVerifiesR7TriangleConsumedSourceBytes)
{
    const std::filesystem::path fixture = FindR7TriangleFixture();
    ASSERT_TRUE(std::filesystem::is_regular_file(fixture));

    ManagerGuard guard(0, true);
    ASSERT_TRUE(guard.AddLoader(
        std::make_unique<RVX::AssetImport::SourceModelLoader>()));
    ResourceLoadOptions options;
    options.expectedContentIdentity = MakeContentIdentity(
        "f41b11c0403cc68c8da3988a954391c8b2e4f5b46a9d800f82c9d4256b395aab",
        1939);
    auto request = guard.GetManager().RequestAsync<ModelResource>(fixture.string(), options);
    ASSERT_TRUE(request);
    guard.GetManager().ProcessCompletedLoads();
    const ResourceHandle<ModelResource> model = request.TryGet();
    ASSERT_TRUE(model) << request.GetSnapshot().error.message;
    const ResourceContentVerificationReceipt& receipt = model->GetContentVerificationReceipt();
    EXPECT_EQ(receipt.status, ResourceContentVerificationStatus::Verified);
    EXPECT_EQ(receipt.observed.digest,
              "f41b11c0403cc68c8da3988a954391c8b2e4f5b46a9d800f82c9d4256b395aab");
    EXPECT_EQ(receipt.observed.byteCount, 1939u);
    EXPECT_EQ(receipt.observed.fileCount, 1u);
    EXPECT_EQ(receipt.observed.scope,
              ResourceContentIdentityScope::SelfContainedArtifact);
}

TEST(ResourcePreparedLoadingValidation, GLTFDependencyClosureTracksActualExternalReadBytes)
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "RenderVerseXContentIdentityClosure" /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::filesystem::path gltfPath = directory / "closure.gltf";
    const std::filesystem::path payloadPath = directory / "payload.bin";
    std::filesystem::create_directories(directory);

    const auto writeGltf = [&gltfPath](const std::string& payloadUri)
    {
        std::ofstream gltf(gltfPath, std::ios::binary);
        gltf
            << "{\"asset\":{\"version\":\"2.0\"},"
               "\"buffers\":[{\"uri\":\""
            << payloadUri
            << "\",\"byteLength\":42}],"
               "\"bufferViews\":["
               "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
               "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}],"
               "\"accessors\":["
               "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
               "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
               "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
               "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    };
    const auto writePayload = [](const std::filesystem::path& path, float firstX)
    {
        const float positions[9] = {
            firstX, 0.0f, 0.0f,
            1.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f,
        };
        const uint16 indices[3] = {0, 1, 2};
        std::ofstream payload(path, std::ios::binary | std::ios::trunc);
        payload.write(reinterpret_cast<const char*>(positions), sizeof(positions));
        payload.write(reinterpret_cast<const char*>(indices), sizeof(indices));
    };

    writeGltf("payload.bin");
    writePayload(payloadPath, 0.0f);

    GLTFImporter importer;
    const GLTFImportResult first = importer.Import(gltfPath.string());
    ASSERT_TRUE(first.success) << first.errorMessage;
    ASSERT_TRUE(first.observedContentIdentity.IsValid());
    EXPECT_EQ(first.observedContentIdentity.scope,
              ResourceContentIdentityScope::DependencyClosure);
    EXPECT_EQ(first.observedContentIdentity.fileCount, 2u);

    writePayload(payloadPath, 0.25f);
    const GLTFImportResult second = importer.Import(gltfPath.string());
    ASSERT_TRUE(second.success) << second.errorMessage;
    EXPECT_EQ(second.observedContentIdentity.scope,
              ResourceContentIdentityScope::DependencyClosure);
    EXPECT_EQ(second.observedContentIdentity.fileCount, 2u);
    EXPECT_NE(second.observedContentIdentity.digest, first.observedContentIdentity.digest);

    const std::string outsideName = "outside-" + directory.filename().string() + ".bin";
    const std::filesystem::path outsidePayload = directory.parent_path() / outsideName;
    {
        writeGltf("../" + outsideName);
        writePayload(outsidePayload, 0.0f);
    }
    const GLTFImportResult escaped = importer.Import(gltfPath.string());
    std::filesystem::remove_all(directory);
    std::filesystem::remove(outsidePayload);
    EXPECT_FALSE(escaped.success);
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
    auto loader = std::make_unique<PreparedTestLoader>();
    ASSERT_TRUE(guard.AddLoader(std::move(loader)));
    guard.GetManager().SetCacheLimit(1);

    auto request = guard.GetManager().RequestAsync<PreparedTestResource>("pinned-budget.png");
    ASSERT_TRUE(request);
    guard.GetManager().ProcessCompletedLoads();
    ASSERT_TRUE(request.TryGet());
    EXPECT_TRUE(request.TryGet()->IsLoaded());
    EXPECT_TRUE(guard.GetManager().IsLoaded(request.GetSnapshot().assetKey));
}

TEST(ResourcePreparedLoadingValidation, OwnerPublicationExceptionRollsBackAndRetryUsesNewRequest)
{
    ManagerGuard guard(0);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    loaderPtr->throwOnSecondGetType = true;
    ASSERT_TRUE(guard.AddLoader(std::move(loader)));

    auto failed = guard.GetManager().RequestAsync<PreparedTestResource>(
        "owner-publication-throw.png");
    ASSERT_TRUE(failed);
    const ResourceLoadRequestId failedRequestId = failed.GetRequestId();
    EXPECT_NO_THROW(guard.GetManager().ProcessCompletedLoads());
    EXPECT_EQ(failed.GetSnapshot().state, ResourceLoadState::Failed);
    EXPECT_EQ(failed.GetSnapshot().error.code, ResourceLoadErrorCode::PublishFailure);
    EXPECT_EQ(guard.GetManager().GetRegistry()->GetCount(), 0u);
    EXPECT_EQ(guard.GetManager().GetCache().GetStats().totalResources, 0u);

    loaderPtr->throwOnSecondGetType = false;
    auto retry = guard.GetManager().RequestAsync<PreparedTestResource>(
        "owner-publication-throw.png");
    ASSERT_TRUE(retry);
    EXPECT_NE(retry.GetRequestId(), failedRequestId);
    EXPECT_NO_THROW(guard.GetManager().ProcessCompletedLoads());
    EXPECT_EQ(retry.GetSnapshot().state, ResourceLoadState::Ready);
    ASSERT_TRUE(retry.TryGet());
    EXPECT_TRUE(guard.GetManager().IsLoaded(retry.GetSnapshot().assetKey));
}

TEST(ResourcePreparedLoadingValidation, ReservedRootIdentityCannotBeOverwrittenByAnEvictedDependency)
{
    ManagerGuard guard(0);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    const ResourceContentIdentity expectedIdentity = MakeContentIdentity();
    loaderPtr->observedContentIdentity = expectedIdentity;
    ASSERT_TRUE(guard.AddLoader(std::move(loader)));

    ResourceLoadOptions verifiedOptions;
    verifiedOptions.expectedContentIdentity = expectedIdentity;
    auto verified = guard.GetManager().RequestAsync<PreparedTestResource>(
        "reserved-root-a.png", verifiedOptions);
    ASSERT_TRUE(verified);
    guard.GetManager().ProcessCompletedLoads();
    const ResourceHandle<PreparedTestResource> original = verified.TryGet();
    ASSERT_TRUE(original);
    const ResourceId reservedRootId = original.GetId();
    ASSERT_TRUE(original->GetContentVerificationReceipt().IsVerified());
    ASSERT_TRUE(guard.GetManager().GetCache().Remove(reservedRootId));
    EXPECT_FALSE(guard.GetManager().GetCache().Contains(reservedRootId));
    const auto originalMetadata = guard.GetManager().GetRegistry()->FindById(reservedRootId);
    ASSERT_TRUE(originalMetadata);

    loaderPtr->forcedDependencyResourceId = reservedRootId;
    auto conflicting = guard.GetManager().RequestAsync<PreparedTestResource>(
        "reserved-root-b.png");
    ASSERT_TRUE(conflicting);
    EXPECT_NO_THROW(guard.GetManager().ProcessCompletedLoads());
    EXPECT_EQ(conflicting.GetSnapshot().state, ResourceLoadState::Failed);
    EXPECT_EQ(conflicting.GetSnapshot().error.code, ResourceLoadErrorCode::PublishFailure);
    EXPECT_FALSE(guard.GetManager().GetCache().Contains(reservedRootId));
    const auto metadataAfterConflict = guard.GetManager().GetRegistry()->FindById(reservedRootId);
    ASSERT_TRUE(metadataAfterConflict);
    EXPECT_EQ(metadataAfterConflict->path, originalMetadata->path);
    EXPECT_EQ(metadataAfterConflict->type, originalMetadata->type);

    ResourceHandle<PreparedTestResource> unverifiedCacheEntry(new PreparedTestResource());
    unverifiedCacheEntry->SetId(reservedRootId);
    unverifiedCacheEntry->SetPath("reserved-root-a.png");
    unverifiedCacheEntry->SetName("UnverifiedReservedRoot");
    ASSERT_TRUE(guard.GetManager().GetCache().StorePreparedBatch(
        {unverifiedCacheEntry.Get()}));
    auto rejectedCacheHit = guard.GetManager().RequestAsync<PreparedTestResource>(
        "reserved-root-a.png", verifiedOptions);
    EXPECT_FALSE(rejectedCacheHit);
    EXPECT_EQ(guard.GetManager().GetCache().Get(reservedRootId),
              unverifiedCacheEntry.Get());
    ASSERT_TRUE(guard.GetManager().GetCache().Remove(reservedRootId));

    loaderPtr->forcedDependencyResourceId = InvalidResourceId;
    auto retry = guard.GetManager().RequestAsync<PreparedTestResource>(
        "reserved-root-a.png", verifiedOptions);
    ASSERT_TRUE(retry);
    EXPECT_NE(retry.GetRequestId(), verified.GetRequestId());
    EXPECT_NO_THROW(guard.GetManager().ProcessCompletedLoads());
    const ResourceHandle<PreparedTestResource> reverified = retry.TryGet();
    ASSERT_TRUE(reverified);
    EXPECT_NE(reverified.Get(), original.Get());
    const ResourceContentVerificationReceipt& receipt =
        reverified->GetContentVerificationReceipt();
    EXPECT_TRUE(receipt.IsVerified());
    EXPECT_EQ(receipt.expected, expectedIdentity);
    EXPECT_EQ(receipt.observed, expectedIdentity);
}

TEST(ResourcePreparedLoadingValidation, VerifiedIdentityRootsDoNotRegisterForHotReload)
{
    ManagerGuard guard(0, false, true);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    const ResourceContentIdentity expectedIdentity = MakeContentIdentity();
    loaderPtr->observedContentIdentity = expectedIdentity;
    ASSERT_TRUE(guard.AddLoader(std::move(loader)));
    guard.GetManager().EnableHotReload(true);
    ASSERT_TRUE(guard.GetManager().IsHotReloadEnabled());
    ASSERT_NE(guard.GetChangeMonitor(), nullptr);

    std::atomic<uint32> reloadedEvents{0};
    guard.GetManager().SetLifecycleEventCallback(
        [&reloadedEvents](const ResourceLifecycleEvent& event)
        {
            if (event.type == ResourceLifecycleEventType::Reloaded)
            {
                reloadedEvents.fetch_add(1, std::memory_order_relaxed);
            }
        });

    ResourceLoadOptions options;
    options.expectedContentIdentity = expectedIdentity;
    auto request = guard.GetManager().RequestAsync<PreparedTestResource>(
        "hot-reload-pinned.png", options);
    ASSERT_TRUE(request);
    guard.GetManager().ProcessCompletedLoads();
    const ResourceHandle<PreparedTestResource> pinned = request.TryGet();
    ASSERT_TRUE(pinned);
    IResource* const original = pinned.Get();
    EXPECT_EQ(guard.GetChangeMonitor()->watchCount, 0u);
    EXPECT_EQ(guard.GetManager().GetHotReloadDiagnostic().registeredResourceCount,
              0u);
    guard.GetManager().ProcessCompletedLoads();
    EXPECT_EQ(reloadedEvents.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(request.TryGet().Get(), original);
    EXPECT_TRUE(pinned->IsLoaded());
    const ResourceContentVerificationReceipt& receipt =
        pinned->GetContentVerificationReceipt();
    EXPECT_TRUE(receipt.IsVerified());
    EXPECT_EQ(receipt.expected, expectedIdentity);
    EXPECT_EQ(receipt.observed, expectedIdentity);
}

TEST(ResourcePreparedLoadingValidation, LoaderProfileSnapshotAndAssetKeyCannotDriftBeforeWorkerRuns)
{
    ManagerGuard guard(1);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    loaderPtr->captureMutableProfile = true;
    loaderPtr->blockPreparation = true;
    loaderPtr->activeImportProfileHash.store(101, std::memory_order_release);
    ASSERT_TRUE(guard.AddLoader(std::move(loader)));

    auto first = guard.GetManager().RequestAsync<PreparedTestResource>("profile.png");
    ASSERT_TRUE(first);
    EXPECT_EQ(first.GetSnapshot().assetKey.importOptionsHash, 101u);
    loaderPtr->activeImportProfileHash.store(202, std::memory_order_release);
    ASSERT_TRUE(loaderPtr->WaitUntilEntered(1));
    loaderPtr->Release();
    for (uint32 attempt = 0; attempt < 100 && !first.TryGet(); ++attempt)
    {
        guard.GetManager().ProcessCompletedLoads();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(first.TryGet());
    EXPECT_EQ(loaderPtr->preparedProfileHash.load(std::memory_order_acquire), 101u);

    ResourceLoadOptions staleOptions;
    staleOptions.importOptionsHash = 101;
    auto stale = guard.GetManager().RequestAsync<PreparedTestResource>("other-profile.png",
                                                                           staleOptions);
    EXPECT_FALSE(stale);

    auto second = guard.GetManager().RequestAsync<PreparedTestResource>("profile.png");
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
    ASSERT_TRUE(guard.AddLoader(std::move(loader)));

    ResourceId variantId = InvalidResourceId;
    {
        auto request =
            guard.GetManager().RequestAsync<PreparedTestResource>("reload-profile.png");
        ASSERT_TRUE(request);
        guard.GetManager().ProcessCompletedLoads();
        const ResourceHandle<PreparedTestResource> resource = request.TryGet();
        ASSERT_TRUE(resource);
        variantId = resource.GetId();
    }

    guard.GetManager().UnloadUnused();
    ASSERT_FALSE(guard.GetManager().IsLoaded(variantId));
    ASSERT_TRUE(guard.GetManager().GetRegistry()->FindById(variantId));

    loaderPtr->activeImportProfileHash.store(202, std::memory_order_release);
    EXPECT_EQ(guard.GetManager().LoadResource(variantId), nullptr);
    EXPECT_EQ(guard.GetManager().GetLastLoadDiagnostic().failure,
              ResourceLoadFailureCode::AssetIdentityMismatch);
    EXPECT_EQ(loaderPtr->syncLoadCount.load(std::memory_order_relaxed), 0u);

    loaderPtr->activeImportProfileHash.store(101, std::memory_order_release);
    loaderPtr->profileAfterCapture.store(202, std::memory_order_release);
    loaderPtr->switchProfileAfterCapture.store(true, std::memory_order_release);
    IResource* const reloaded = guard.GetManager().LoadResource(variantId);
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
    ASSERT_TRUE(guard.AddLoader(std::move(loader)));

    ResourceId variantId = InvalidResourceId;
    {
        ResourceLoadOptions options;
        options.platformProfileHash = 77;
        options.loaderSchemaVersion = 2;
        auto request = guard.GetManager().RequestAsync<PreparedTestResource>(
            "platform-profile.png",
            options);
        ASSERT_TRUE(request);
        guard.GetManager().ProcessCompletedLoads();
        const ResourceHandle<PreparedTestResource> resource = request.TryGet();
        ASSERT_TRUE(resource);
        variantId = resource.GetId();
    }

    guard.GetManager().UnloadUnused();
    ASSERT_FALSE(guard.GetManager().IsLoaded(variantId));
    EXPECT_EQ(guard.GetManager().LoadResource(variantId), nullptr);
    EXPECT_EQ(guard.GetManager().GetLastLoadDiagnostic().failure,
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
    ASSERT_TRUE(guard.AddLoader(std::move(loader)));

    IResource* const loaded = guard.GetManager().LoadResource(
        "synchronous-profile.png",
        ResourceType::Texture);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaderPtr->preparedProfileHash.load(std::memory_order_acquire), 101u);
    EXPECT_EQ(loaderPtr->activeImportProfileHash.load(std::memory_order_acquire), 202u);
    EXPECT_EQ(loaderPtr->syncLoadCount.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(loaded->GetContentVerificationReceipt().status,
              ResourceContentVerificationStatus::NotRequested);
}

TEST(ResourcePreparedLoadingValidation, OwnerPublishFailureRollsBackDependenciesAndShutdownCancelsWorkers)
{
    {
        ManagerGuard guard(0);
        auto loader = std::make_unique<PreparedTestLoader>();
        PreparedTestLoader* loaderPtr = loader.get();
        loaderPtr->failPublicationTransaction = true;
        ASSERT_TRUE(guard.AddLoader(std::move(loader)));

        std::atomic<uint32> lifecycleEvents{0};
        guard.GetManager().SetLifecycleEventCallback(
            [&lifecycleEvents](const ResourceLifecycleEvent&)
            {
                lifecycleEvents.fetch_add(1, std::memory_order_relaxed);
            });

        auto failed = guard.GetManager().RequestAsync<PreparedTestResource>("transaction.png");
        ASSERT_TRUE(failed);
        guard.GetManager().ProcessCompletedLoads();
        EXPECT_EQ(failed.GetSnapshot().state, ResourceLoadState::Failed);
        EXPECT_EQ(guard.GetManager().GetRegistry()->GetCount(), 0u);
        EXPECT_EQ(guard.GetManager().GetCache().GetStats().totalResources, 0u);
        EXPECT_EQ(lifecycleEvents.load(std::memory_order_relaxed), 0u);
    }

    ManagerGuard guard(2);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    loaderPtr->blockPreparation = true;
    ASSERT_TRUE(guard.AddLoader(std::move(loader)));

    std::atomic<uint32> lifecycleEvents{0};
    guard.GetManager().SetLifecycleEventCallback(
        [&lifecycleEvents](const ResourceLifecycleEvent&)
        {
            lifecycleEvents.fetch_add(1, std::memory_order_relaxed);
        });
    auto pending = guard.GetManager().RequestAsync<PreparedTestResource>("shutdown.png");
    ASSERT_TRUE(pending);
    ASSERT_TRUE(loaderPtr->WaitUntilEntered(1));
    guard.GetManager().Shutdown();
    EXPECT_EQ(pending.GetSnapshot().state, ResourceLoadState::Cancelled);
    EXPECT_EQ(lifecycleEvents.load(std::memory_order_relaxed), 0u);
}

TEST(ResourcePreparedLoadingValidation, CanonicalRootCollisionFailsAndObserverFailureCannotUndoCommit)
{
    ManagerGuard guard(0);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    ASSERT_TRUE(guard.AddLoader(std::move(loader)));

    loaderPtr->rootRebindCollision = true;
    auto collision = guard.GetManager().RequestAsync<PreparedTestResource>("collision.png");
    ASSERT_TRUE(collision);
    guard.GetManager().ProcessCompletedLoads();
    EXPECT_EQ(collision.GetSnapshot().state, ResourceLoadState::Failed);
    EXPECT_EQ(guard.GetManager().GetRegistry()->GetCount(), 0u);
    EXPECT_EQ(guard.GetManager().GetCache().GetStats().totalResources, 0u);

    loaderPtr->rootRebindCollision = false;
    loaderPtr->throwOnLoadedObserver = true;
    std::atomic<uint32> readyEvents{0};
    guard.GetManager().SetLifecycleEventCallback(
        [&readyEvents](const ResourceLifecycleEvent& event)
        {
            if (event.type == ResourceLifecycleEventType::Ready)
            {
                readyEvents.fetch_add(1, std::memory_order_relaxed);
            }
        });
    auto committed = guard.GetManager().RequestAsync<PreparedTestResource>("observer.png");
    ASSERT_TRUE(committed);
    guard.GetManager().ProcessCompletedLoads();
    EXPECT_EQ(committed.GetSnapshot().state, ResourceLoadState::Ready);
    ASSERT_TRUE(committed.TryGet());
    EXPECT_TRUE(committed.TryGet()->IsLoaded());
    EXPECT_TRUE(guard.GetManager().IsLoaded("observer.png"));
    EXPECT_EQ(guard.GetManager().GetRegistry()->GetCount(), 1u);
    EXPECT_EQ(readyEvents.load(std::memory_order_relaxed), 1u);
}

TEST(ResourcePreparedLoadingValidation, ShutdownCannotDispatchOwnerCallbacksFromWorkerThread)
{
    ManagerGuard guard(0);
    auto loader = std::make_unique<PreparedTestLoader>();
    ASSERT_TRUE(guard.AddLoader(std::move(loader)));

    std::atomic<bool> callbackCalled{false};
    std::thread::id callbackThread;
    guard.GetManager().LoadAsync<PreparedTestResource>(
        "shutdown-affinity.png",
        [&](ResourceHandle<PreparedTestResource> resource)
        {
            EXPECT_TRUE(resource);
            callbackThread = std::this_thread::get_id();
            callbackCalled.store(true, std::memory_order_release);
        });

    std::thread shutdownThread([&guard] { guard.GetManager().Shutdown(); });
    shutdownThread.join();
    EXPECT_TRUE(guard.GetManager().IsInitialized());
    EXPECT_FALSE(callbackCalled.load(std::memory_order_acquire));

    guard.GetManager().ProcessCompletedLoads();
    EXPECT_TRUE(callbackCalled.load(std::memory_order_acquire));
    EXPECT_EQ(callbackThread, std::this_thread::get_id());
}
