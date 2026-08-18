#include "Resource/ResourceManager.h"
#include "Resource/HotReloadManager.h"
#include "Resource/Importer/GLTFImporter.h"
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

    ResourceContentIdentity MakeContentIdentity(
        std::string digest = "f009e85dae4e41d6d1cc9ba534a5948090c986993523280fd7e513df87c40300")
    {
        ResourceContentIdentity identity;
        identity.schemaVersion = RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
        identity.domain = ResourceContentIdentityDomain::Source;
        identity.scope = ResourceContentIdentityScope::SelfContainedArtifact;
        identity.algorithm = ResourceContentHashAlgorithm::SHA256;
        identity.digest = std::move(digest);
        identity.byteCount = 2045;
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

    EXPECT_EQ(ResourceManager::Get().Unload(second.GetSnapshot().assetKey),
              AssetResidencyReleaseResult::Unloaded);
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

TEST(ResourcePreparedLoadingValidation, ContentIdentityVerificationPrecedesEveryPublication)
{
    ManagerGuard guard(0);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    const ResourceContentIdentity observed = MakeContentIdentity();
    loaderPtr->observedContentIdentity = observed;
    ResourceManager::Get().RegisterLoader(ResourceType::Texture, std::move(loader));

    ResourceLoadOptions verifiedOptions;
    verifiedOptions.expectedContentIdentity = observed;
    auto verified = ResourceManager::Get().RequestAsync<PreparedTestResource>(
        "verified-identity.png", verifiedOptions);
    ASSERT_TRUE(verified);
    ResourceManager::Get().ProcessCompletedLoads();
    const ResourceHandle<PreparedTestResource> verifiedResource = verified.TryGet();
    ASSERT_TRUE(verifiedResource);
    const ResourceContentVerificationReceipt& verifiedReceipt =
        verifiedResource->GetContentVerificationReceipt();
    EXPECT_EQ(verifiedReceipt.status, ResourceContentVerificationStatus::Verified);
    EXPECT_TRUE(verifiedReceipt.IsVerified());
    EXPECT_EQ(verifiedReceipt.expected, observed);
    EXPECT_EQ(verifiedReceipt.observed, observed);
    EXPECT_STREQ(GetResourceContentVerificationStatusName(verifiedReceipt.status), "verified");
    const size_t publishedCount = ResourceManager::Get().GetRegistry()->GetCount();

    ResourceLoadOptions mismatchOptions;
    ResourceContentIdentity mismatch = observed;
    mismatch.digest[0] = 'e';
    mismatchOptions.expectedContentIdentity = mismatch;
    auto mismatched = ResourceManager::Get().RequestAsync<PreparedTestResource>(
        "verified-identity.png", mismatchOptions);
    ASSERT_TRUE(mismatched);
    ResourceManager::Get().ProcessCompletedLoads();
    EXPECT_EQ(mismatched.GetSnapshot().state, ResourceLoadState::Failed);
    EXPECT_EQ(mismatched.GetSnapshot().error.code, ResourceLoadErrorCode::ContentIdentityMismatch);
    EXPECT_FALSE(ResourceManager::Get().IsLoaded(mismatched.GetSnapshot().assetKey));
    EXPECT_EQ(ResourceManager::Get().GetRegistry()->GetCount(), publishedCount);
    EXPECT_EQ(ResourceManager::Get().GetCache().GetStats().totalResources, publishedCount);

    loaderPtr->observedContentIdentity = {};
    ResourceLoadOptions unavailableOptions;
    unavailableOptions.expectedContentIdentity = MakeContentIdentity(
        "a009e85dae4e41d6d1cc9ba534a5948090c986993523280fd7e513df87c40300");
    auto unavailable = ResourceManager::Get().RequestAsync<PreparedTestResource>(
        "unavailable-identity.png", unavailableOptions);
    ASSERT_TRUE(unavailable);
    ResourceManager::Get().ProcessCompletedLoads();
    EXPECT_EQ(unavailable.GetSnapshot().state, ResourceLoadState::Failed);
    EXPECT_EQ(unavailable.GetSnapshot().error.code,
              ResourceLoadErrorCode::ContentIdentityUnavailable);
    EXPECT_FALSE(ResourceManager::Get().IsLoaded(unavailable.GetSnapshot().assetKey));
    EXPECT_EQ(ResourceManager::Get().GetRegistry()->GetCount(), publishedCount);
}

TEST(ResourcePreparedLoadingValidation, UnrequestedObservedIdentityNeverClaimsVerification)
{
    ManagerGuard guard(0);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    loaderPtr->observedContentIdentity = MakeContentIdentity();
    ResourceManager::Get().RegisterLoader(ResourceType::Texture, std::move(loader));

    auto request = ResourceManager::Get().RequestAsync<PreparedTestResource>("observed-identity.png");
    ASSERT_TRUE(request);
    ResourceManager::Get().ProcessCompletedLoads();
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

    ManagerGuard guard(0);
    ResourceLoadOptions options;
    options.expectedContentIdentity = MakeContentIdentity();
    auto request = ResourceManager::Get().RequestAsync<ModelResource>(fixture.string(), options);
    ASSERT_TRUE(request);
    ResourceManager::Get().ProcessCompletedLoads();
    const ResourceHandle<ModelResource> model = request.TryGet();
    ASSERT_TRUE(model) << request.GetSnapshot().error.message;
    const ResourceContentVerificationReceipt& receipt = model->GetContentVerificationReceipt();
    EXPECT_EQ(receipt.status, ResourceContentVerificationStatus::Verified);
    EXPECT_EQ(receipt.observed.digest,
              "f009e85dae4e41d6d1cc9ba534a5948090c986993523280fd7e513df87c40300");
    EXPECT_EQ(receipt.observed.byteCount, 2045u);
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

TEST(ResourcePreparedLoadingValidation, OwnerPublicationExceptionRollsBackAndRetryUsesNewRequest)
{
    ManagerGuard guard(0);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    loaderPtr->throwOnSecondGetType = true;
    ResourceManager::Get().RegisterLoader(ResourceType::Texture, std::move(loader));

    auto failed = ResourceManager::Get().RequestAsync<PreparedTestResource>(
        "owner-publication-throw.png");
    ASSERT_TRUE(failed);
    const ResourceLoadRequestId failedRequestId = failed.GetRequestId();
    EXPECT_NO_THROW(ResourceManager::Get().ProcessCompletedLoads());
    EXPECT_EQ(failed.GetSnapshot().state, ResourceLoadState::Failed);
    EXPECT_EQ(failed.GetSnapshot().error.code, ResourceLoadErrorCode::PublishFailure);
    EXPECT_EQ(ResourceManager::Get().GetRegistry()->GetCount(), 0u);
    EXPECT_EQ(ResourceManager::Get().GetCache().GetStats().totalResources, 0u);

    loaderPtr->throwOnSecondGetType = false;
    auto retry = ResourceManager::Get().RequestAsync<PreparedTestResource>(
        "owner-publication-throw.png");
    ASSERT_TRUE(retry);
    EXPECT_NE(retry.GetRequestId(), failedRequestId);
    EXPECT_NO_THROW(ResourceManager::Get().ProcessCompletedLoads());
    EXPECT_EQ(retry.GetSnapshot().state, ResourceLoadState::Ready);
    ASSERT_TRUE(retry.TryGet());
    EXPECT_TRUE(ResourceManager::Get().IsLoaded(retry.GetSnapshot().assetKey));
}

TEST(ResourcePreparedLoadingValidation, ReservedRootIdentityCannotBeOverwrittenByAnEvictedDependency)
{
    ManagerGuard guard(0);
    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    const ResourceContentIdentity expectedIdentity = MakeContentIdentity();
    loaderPtr->observedContentIdentity = expectedIdentity;
    ResourceManager::Get().RegisterLoader(ResourceType::Texture, std::move(loader));

    ResourceLoadOptions verifiedOptions;
    verifiedOptions.expectedContentIdentity = expectedIdentity;
    auto verified = ResourceManager::Get().RequestAsync<PreparedTestResource>(
        "reserved-root-a.png", verifiedOptions);
    ASSERT_TRUE(verified);
    ResourceManager::Get().ProcessCompletedLoads();
    const ResourceHandle<PreparedTestResource> original = verified.TryGet();
    ASSERT_TRUE(original);
    const ResourceId reservedRootId = original.GetId();
    ASSERT_TRUE(original->GetContentVerificationReceipt().IsVerified());
    ASSERT_TRUE(ResourceManager::Get().GetCache().Remove(reservedRootId));
    EXPECT_FALSE(ResourceManager::Get().GetCache().Contains(reservedRootId));
    const auto originalMetadata = ResourceManager::Get().GetRegistry()->FindById(reservedRootId);
    ASSERT_TRUE(originalMetadata);

    loaderPtr->forcedDependencyResourceId = reservedRootId;
    auto conflicting = ResourceManager::Get().RequestAsync<PreparedTestResource>(
        "reserved-root-b.png");
    ASSERT_TRUE(conflicting);
    EXPECT_NO_THROW(ResourceManager::Get().ProcessCompletedLoads());
    EXPECT_EQ(conflicting.GetSnapshot().state, ResourceLoadState::Failed);
    EXPECT_EQ(conflicting.GetSnapshot().error.code, ResourceLoadErrorCode::PublishFailure);
    EXPECT_FALSE(ResourceManager::Get().GetCache().Contains(reservedRootId));
    const auto metadataAfterConflict = ResourceManager::Get().GetRegistry()->FindById(reservedRootId);
    ASSERT_TRUE(metadataAfterConflict);
    EXPECT_EQ(metadataAfterConflict->path, originalMetadata->path);
    EXPECT_EQ(metadataAfterConflict->type, originalMetadata->type);

    ResourceHandle<PreparedTestResource> unverifiedCacheEntry(new PreparedTestResource());
    unverifiedCacheEntry->SetId(reservedRootId);
    unverifiedCacheEntry->SetPath("reserved-root-a.png");
    unverifiedCacheEntry->SetName("UnverifiedReservedRoot");
    ASSERT_TRUE(ResourceManager::Get().GetCache().StorePreparedBatch(
        {unverifiedCacheEntry.Get()}));
    auto rejectedCacheHit = ResourceManager::Get().RequestAsync<PreparedTestResource>(
        "reserved-root-a.png", verifiedOptions);
    EXPECT_FALSE(rejectedCacheHit);
    EXPECT_EQ(ResourceManager::Get().GetCache().Get(reservedRootId),
              unverifiedCacheEntry.Get());
    ASSERT_TRUE(ResourceManager::Get().GetCache().Remove(reservedRootId));

    loaderPtr->forcedDependencyResourceId = InvalidResourceId;
    auto retry = ResourceManager::Get().RequestAsync<PreparedTestResource>(
        "reserved-root-a.png", verifiedOptions);
    ASSERT_TRUE(retry);
    EXPECT_NE(retry.GetRequestId(), verified.GetRequestId());
    EXPECT_NO_THROW(ResourceManager::Get().ProcessCompletedLoads());
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
    ManagerGuard guard(0);
    ResourceManager::Get().EnableHotReload(true);
    ASSERT_TRUE(HotReloadManager::Get().IsInitialized());

    auto loader = std::make_unique<PreparedTestLoader>();
    PreparedTestLoader* loaderPtr = loader.get();
    const ResourceContentIdentity expectedIdentity = MakeContentIdentity();
    loaderPtr->observedContentIdentity = expectedIdentity;
    ResourceManager::Get().RegisterLoader(ResourceType::Texture, std::move(loader));

    std::atomic<uint32> reloadedEvents{0};
    ResourceManager::Get().SetLifecycleEventCallback(
        [&reloadedEvents](const ResourceLifecycleEvent& event)
        {
            if (event.type == ResourceLifecycleEventType::Reloaded)
            {
                reloadedEvents.fetch_add(1, std::memory_order_relaxed);
            }
        });

    ResourceLoadOptions options;
    options.expectedContentIdentity = expectedIdentity;
    auto request = ResourceManager::Get().RequestAsync<PreparedTestResource>(
        "hot-reload-pinned.png", options);
    ASSERT_TRUE(request);
    ResourceManager::Get().ProcessCompletedLoads();
    const ResourceHandle<PreparedTestResource> pinned = request.TryGet();
    ASSERT_TRUE(pinned);
    IResource* const original = pinned.Get();
    const ResourceId resourceId = pinned.GetId();

    EXPECT_EQ(HotReloadManager::Get().GetResourceVersion(resourceId), 0u);
    EXPECT_FALSE(HotReloadManager::Get().ForceReload(resourceId));
    EXPECT_EQ(HotReloadManager::Get().GetStats().registeredResources, 0u);
    ResourceManager::Get().ProcessCompletedLoads();
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
