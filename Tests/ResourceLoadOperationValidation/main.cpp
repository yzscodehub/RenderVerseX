#include "Resource/ResourceLoadOperation.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace RVX;
using namespace RVX::Resource;

namespace
{
    class TestResource final : public IResource
    {
    public:
        explicit TestResource(ResourceType type)
            : m_type(type)
        {
        }

        ResourceType GetType() const override { return m_type; }
        const char* GetTypeName() const override { return GetResourceTypeName(m_type); }

    private:
        ResourceType m_type = ResourceType::Unknown;
    };

    class AlternateTextureResource final : public IResource
    {
    public:
        ResourceType GetType() const override { return ResourceType::Texture; }
        const char* GetTypeName() const override { return "AlternateTextureResource"; }
    };

    AssetKey MakeTextureKey(uint64 importOptionsHash = 7,
                            uint64 platformProfileHash = 11,
                            uint32 loaderSchemaVersion = 3)
    {
        return MakeAssetKey("Assets/Textures/Albedo.png",
                            ResourceType::Texture,
                            importOptionsHash,
                            platformProfileHash,
                            loaderSchemaVersion);
    }

    ResourceHandle<IResource> ToBaseHandle(const ResourceHandle<TestResource>& resource)
    {
        return ResourceHandle<IResource>(resource);
    }
} // namespace

TEST(ResourceLoadOperationValidation, AssetKeyIncludesEveryOutputAffectingInput)
{
    const AssetKey key = MakeAssetKey("Assets\\Textures\\..\\Textures\\Albedo.png",
                                      ResourceType::Texture,
                                      7,
                                      11,
                                      3);
    const AssetKey equivalent = MakeTextureKey();
    const AssetKey differentType = MakeAssetKey("Assets/Textures/Albedo.png",
                                                ResourceType::Material,
                                                7,
                                                11,
                                                3);
    const AssetKey differentImportOptions = MakeTextureKey(8, 11, 3);
    const AssetKey differentPlatformProfile = MakeTextureKey(7, 12, 3);
    const AssetKey differentLoaderSchema = MakeTextureKey(7, 11, 4);

    EXPECT_TRUE(key.IsValid());
    EXPECT_EQ(key, equivalent);
    EXPECT_NE(key, differentType);
    EXPECT_NE(key, differentImportOptions);
    EXPECT_NE(key, differentPlatformProfile);
    EXPECT_NE(key, differentLoaderSchema);

    std::unordered_set<AssetKey, AssetKeyHash> keys;
    keys.insert(key);
    keys.insert(equivalent);
    keys.insert(differentType);
    keys.insert(differentImportOptions);
    keys.insert(differentPlatformProfile);
    keys.insert(differentLoaderSchema);
    EXPECT_EQ(keys.size(), 5u);
}

TEST(ResourceLoadOperationValidation, RequestIdsAreMonotonicAndNeverReused)
{
    std::unordered_set<uint64> requestIds;
    uint64 previous = 0;

    for (uint32 index = 0; index < 256; ++index)
    {
        ResourceLoadOperationOwner operation = ResourceLoadOperationOwner::Create(MakeTextureKey());
        ASSERT_TRUE(operation);

        const uint64 requestId = operation.GetRequestId().GetValue();
        EXPECT_NE(requestId, 0u);
        EXPECT_GT(requestId, previous);
        EXPECT_TRUE(requestIds.insert(requestId).second);
        previous = requestId;
    }
}

TEST(ResourceLoadOperationValidation, SingleSubscriptionCancellationDoesNotCancelOtherSubscribers)
{
    ResourceLoadOperationOwner operation = ResourceLoadOperationOwner::Create(MakeTextureKey());
    ASSERT_TRUE(operation);

    auto first = operation.Subscribe<TestResource>();
    auto second = operation.Subscribe<TestResource>();
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(operation.GetSnapshot().subscriberCount, 2u);

    EXPECT_TRUE(first.Cancel());
    EXPECT_EQ(operation.GetSnapshot().subscriberCount, 1u);
    EXPECT_FALSE(operation.IsCancellationRequested());
    EXPECT_TRUE(second);

    ASSERT_TRUE(operation.BeginLoading());
    ASSERT_TRUE(operation.UpdateProgress(ResourceLoadStage::Read, 1.0f));
    ASSERT_TRUE(operation.BeginAwaitingPublish());

    ResourceHandle<TestResource> resource(new TestResource(ResourceType::Texture));
    ASSERT_TRUE(operation.CompleteReady(ToBaseHandle(resource)));

    const ResourceHandle<TestResource> loaded = second.TryGet();
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded.Get(), resource.Get());
}

TEST(ResourceLoadOperationValidation, TypeMismatchDoesNotTerminateAndLateSubscriberCanReadReadyResource)
{
    ResourceLoadOptions options;
    options.traceContext.correlationId = 42;
    ResourceLoadOperationOwner operation = ResourceLoadOperationOwner::Create(MakeTextureKey(), options);
    ASSERT_TRUE(operation);

    auto initialSubscriber = operation.Subscribe<TestResource>();
    ASSERT_TRUE(initialSubscriber);
    ASSERT_TRUE(operation.BeginLoading());
    ASSERT_TRUE(operation.BeginAwaitingPublish());

    ResourceHandle<TestResource> mismatchedResource(new TestResource(ResourceType::Material));
    EXPECT_FALSE(operation.CompleteReady(ToBaseHandle(mismatchedResource)));
    const ResourceLoadSnapshot mismatchSnapshot = operation.GetSnapshot();
    EXPECT_EQ(mismatchSnapshot.state, ResourceLoadState::AwaitingPublish);
    EXPECT_FALSE(mismatchSnapshot.IsTerminal());
    EXPECT_EQ(mismatchSnapshot.error.code, ResourceLoadErrorCode::None);
    EXPECT_EQ(mismatchSnapshot.options.traceContext.correlationId, 42u);

    ResourceHandle<TestResource> resource(new TestResource(ResourceType::Texture));
    ASSERT_TRUE(operation.CompleteReady(ToBaseHandle(resource)));

    auto lateSubscriber = operation.Subscribe<TestResource>();
    ASSERT_TRUE(lateSubscriber);
    const ResourceHandle<TestResource> firstResult = initialSubscriber.TryGet();
    const ResourceHandle<TestResource> lateResult = lateSubscriber.TryGet();
    ASSERT_TRUE(firstResult);
    ASSERT_TRUE(lateResult);
    EXPECT_EQ(firstResult.Get(), resource.Get());
    EXPECT_EQ(lateResult.Get(), resource.Get());
}

TEST(ResourceLoadOperationValidation, TypedSubscriberNeverReportsFalseReady)
{
    ResourceLoadOperationOwner operation = ResourceLoadOperationOwner::Create(MakeTextureKey());
    ASSERT_TRUE(operation);
    auto correct = operation.Subscribe<TestResource>();
    auto wrong = operation.Subscribe<AlternateTextureResource>();
    ASSERT_TRUE(correct);
    ASSERT_TRUE(wrong);
    ASSERT_TRUE(operation.BeginLoading());
    ASSERT_TRUE(operation.BeginAwaitingPublish());

    ResourceHandle<TestResource> resource(new TestResource(ResourceType::Texture));
    ASSERT_TRUE(operation.CompleteReady(ToBaseHandle(resource)));
    EXPECT_EQ(operation.GetSnapshot().state, ResourceLoadState::Ready);
    EXPECT_EQ(correct.GetSnapshot().state, ResourceLoadState::Ready);
    EXPECT_EQ(wrong.GetSnapshot().state, ResourceLoadState::Failed);
    EXPECT_EQ(wrong.GetSnapshot().error.code, ResourceLoadErrorCode::TypeMismatch);
    EXPECT_TRUE(correct.TryGet());
    EXPECT_FALSE(wrong.TryGet());
}

TEST(ResourceLoadOperationValidation, ReadyOperationPinsItsResourceUntilSubscribersReleaseIt)
{
    ResourceLoadOperationOwner operation = ResourceLoadOperationOwner::Create(MakeTextureKey());
    ASSERT_TRUE(operation);

    auto subscriber = operation.Subscribe<TestResource>();
    ASSERT_TRUE(subscriber);
    ASSERT_TRUE(operation.BeginLoading());
    ASSERT_TRUE(operation.BeginAwaitingPublish());

    ResourceHandle<TestResource> original(new TestResource(ResourceType::Texture));
    TestResource* const expectedResource = original.Get();
    ResourceHandle<IResource> baseResource = ToBaseHandle(original);
    ASSERT_TRUE(operation.CompleteReady(baseResource));
    original.Reset();
    baseResource.Reset();

    const ResourceHandle<TestResource> pinned = subscriber.TryGet();
    ASSERT_TRUE(pinned);
    EXPECT_EQ(pinned.Get(), expectedResource);

    operation = ResourceLoadOperationOwner{};
    const ResourceHandle<TestResource> pinnedAfterOwnerRelease = subscriber.TryGet();
    ASSERT_TRUE(pinnedAfterOwnerRelease);
    EXPECT_EQ(pinnedAfterOwnerRelease.Get(), expectedResource);
}

TEST(ResourceLoadOperationValidation, LastSubscriberRequestsCooperativeCancellationUntilOwnerCompletesIt)
{
    ResourceLoadOperationOwner operation = ResourceLoadOperationOwner::Create(MakeTextureKey());
    ASSERT_TRUE(operation);

    auto subscriber = operation.Subscribe<TestResource>();
    ASSERT_TRUE(subscriber);
    ASSERT_TRUE(operation.BeginLoading());

    EXPECT_TRUE(subscriber.Cancel());
    const ResourceLoadSnapshot requestedCancellation = operation.GetSnapshot();
    EXPECT_EQ(requestedCancellation.state, ResourceLoadState::Loading);
    EXPECT_TRUE(requestedCancellation.cancellationRequested);
    EXPECT_TRUE(operation.IsCancellationRequested());
    EXPECT_FALSE(operation.BeginAwaitingPublish());

    ASSERT_TRUE(operation.CompleteCancelled("No active subscribers"));
    const ResourceLoadSnapshot cancelled = operation.GetSnapshot();
    EXPECT_EQ(cancelled.state, ResourceLoadState::Cancelled);
    EXPECT_TRUE(cancelled.IsTerminal());
    EXPECT_EQ(cancelled.error.code, ResourceLoadErrorCode::Cancelled);
    EXPECT_EQ(cancelled.error.message, "No active subscribers");
    EXPECT_FALSE(operation.CompleteCancelled("duplicate"));
}

TEST(ResourceLoadOperationValidation, OwnerPublicationGateLinearizesLastSubscriberCancellation)
{
    ResourceLoadOperationOwner committed = ResourceLoadOperationOwner::Create(MakeTextureKey());
    ASSERT_TRUE(committed);
    auto committedSubscriber = committed.Subscribe<TestResource>();
    ASSERT_TRUE(committedSubscriber);
    ASSERT_TRUE(committed.BeginLoading());
    ASSERT_TRUE(committed.BeginAwaitingPublish());
    ASSERT_TRUE(committed.BeginOwnerPublication());
    EXPECT_TRUE(committedSubscriber.Cancel());
    EXPECT_FALSE(committed.IsCancellationRequested());

    ResourceHandle<TestResource> resource(new TestResource(ResourceType::Texture));
    EXPECT_TRUE(committed.CompleteReady(ToBaseHandle(resource)));
    EXPECT_EQ(committed.GetSnapshot().state, ResourceLoadState::Ready);
    EXPECT_FALSE(committed.CompleteCancelled("too late"));

    ResourceLoadOperationOwner cancelled = ResourceLoadOperationOwner::Create(MakeTextureKey());
    ASSERT_TRUE(cancelled);
    auto cancelledSubscriber = cancelled.Subscribe<TestResource>();
    ASSERT_TRUE(cancelledSubscriber);
    ASSERT_TRUE(cancelled.BeginLoading());
    ASSERT_TRUE(cancelled.BeginAwaitingPublish());
    EXPECT_TRUE(cancelledSubscriber.Cancel());
    EXPECT_FALSE(cancelled.BeginOwnerPublication());
    EXPECT_TRUE(cancelled.CompleteCancelled("cancel won before publication"));
    EXPECT_EQ(cancelled.GetSnapshot().state, ResourceLoadState::Cancelled);
}

TEST(ResourceLoadOperationValidation, FailedAndCancelledOperationsAreTerminalAndDoNotYieldResources)
{
    ResourceLoadOperationOwner failed = ResourceLoadOperationOwner::Create(MakeTextureKey());
    ASSERT_TRUE(failed);
    auto failedSubscriber = failed.Subscribe<TestResource>();
    ASSERT_TRUE(failedSubscriber);
    ASSERT_TRUE(failed.BeginLoading());
    ASSERT_TRUE(failed.CompleteFailed(
        {ResourceLoadErrorCode::LoaderFailure, "The loader could not read the asset."}));

    const ResourceLoadSnapshot failure = failed.GetSnapshot();
    EXPECT_EQ(failure.state, ResourceLoadState::Failed);
    EXPECT_TRUE(failure.IsTerminal());
    EXPECT_EQ(failure.error.code, ResourceLoadErrorCode::LoaderFailure);
    EXPECT_FALSE(failedSubscriber.TryGet());
    EXPECT_FALSE(failed.BeginAwaitingPublish());
    EXPECT_FALSE(failed.CompleteFailed({ResourceLoadErrorCode::LoaderFailure, "duplicate"}));

    ResourceLoadOperationOwner cancelled = ResourceLoadOperationOwner::Create(MakeTextureKey());
    ASSERT_TRUE(cancelled);
    auto cancelledSubscriber = cancelled.Subscribe<TestResource>();
    ASSERT_TRUE(cancelledSubscriber);
    ASSERT_TRUE(cancelled.BeginLoading());
    ASSERT_TRUE(cancelled.CompleteCancelled());
    EXPECT_EQ(cancelled.GetSnapshot().state, ResourceLoadState::Cancelled);
    EXPECT_FALSE(cancelledSubscriber.TryGet());
    EXPECT_FALSE(cancelled.BeginLoading());
}

TEST(ResourceLoadOperationValidation, IllegalDuplicateAndRollbackTransitionsFailClosed)
{
    ResourceLoadOperationOwner operation = ResourceLoadOperationOwner::Create(MakeTextureKey());
    ASSERT_TRUE(operation);
    auto subscriber = operation.Subscribe<TestResource>();
    ASSERT_TRUE(subscriber);

    EXPECT_FALSE(operation.BeginAwaitingPublish());
    EXPECT_FALSE(operation.CompleteFailed({ResourceLoadErrorCode::LoaderFailure, "too early"}));
    EXPECT_EQ(operation.GetSnapshot().state, ResourceLoadState::Queued);

    ASSERT_TRUE(operation.BeginLoading());
    EXPECT_FALSE(operation.UpdateProgress(ResourceLoadStage::None, 0.0f));
    ASSERT_TRUE(operation.UpdateProgress(ResourceLoadStage::Read, 0.5f));
    EXPECT_FALSE(operation.UpdateProgress(ResourceLoadStage::Resolve, 1.0f));
    EXPECT_FALSE(operation.UpdateProgress(ResourceLoadStage::Read, 0.25f));
    EXPECT_FALSE(operation.BeginLoading());

    ResourceHandle<TestResource> resource(new TestResource(ResourceType::Texture));
    EXPECT_FALSE(operation.CompleteReady(ToBaseHandle(resource)));
    EXPECT_EQ(operation.GetSnapshot().state, ResourceLoadState::Loading);

    ASSERT_TRUE(operation.CompleteFailed({ResourceLoadErrorCode::LoaderFailure, "load failed"}));
    EXPECT_EQ(operation.GetSnapshot().state, ResourceLoadState::Failed);
    EXPECT_FALSE(operation.BeginAwaitingPublish());
    EXPECT_FALSE(operation.CompleteCancelled());
    EXPECT_EQ(operation.GetSnapshot().state, ResourceLoadState::Failed);
}

TEST(ResourceLoadOperationValidation, ConcurrentSnapshotsRemainSelfConsistent)
{
    ResourceLoadOperationOwner operation = ResourceLoadOperationOwner::Create(MakeTextureKey());
    ASSERT_TRUE(operation);
    auto subscriber = operation.Subscribe<TestResource>();
    ASSERT_TRUE(subscriber);
    ASSERT_TRUE(operation.BeginLoading());

    std::atomic<bool> start{false};
    std::atomic<bool> finish{false};
    std::atomic<uint32> invalidSnapshots{0};
    std::vector<std::thread> observers;
    observers.reserve(4);

    for (uint32 index = 0; index < 4; ++index)
    {
        observers.emplace_back(
            [&operation, &start, &finish, &invalidSnapshots]()
            {
                while (!start.load(std::memory_order_acquire))
                {
                }

                while (!finish.load(std::memory_order_acquire))
                {
                    const ResourceLoadSnapshot snapshot = operation.GetSnapshot();
                    if (!snapshot.requestId.IsValid() ||
                        snapshot.assetKey != MakeTextureKey() ||
                        snapshot.progress.fraction < 0.0f ||
                        snapshot.progress.fraction > 1.0f)
                    {
                        invalidSnapshots.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
    }

    start.store(true, std::memory_order_release);
    for (uint32 index = 0; index < 256; ++index)
    {
        const float32 fraction = static_cast<float32>(index) / 255.0f;
        ASSERT_TRUE(operation.UpdateProgress(ResourceLoadStage::Read, fraction));
    }
    ASSERT_TRUE(operation.UpdateProgress(ResourceLoadStage::Decode, 0.0f));
    ASSERT_TRUE(operation.UpdateProgress(ResourceLoadStage::Decode, 1.0f));

    finish.store(true, std::memory_order_release);
    for (std::thread& observer : observers)
    {
        observer.join();
    }

    EXPECT_EQ(invalidSnapshots.load(std::memory_order_relaxed), 0u);
}
