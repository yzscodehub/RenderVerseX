#include "Core/Log.h"
#include "Resource/ResourceCache.h"
#include "Resource/ResourceManager.h"
#include "Resource/ResourceSubsystem.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

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
        ::testing::AddGlobalTestEnvironment(new LogEnvironment());

    class TestResource final : public IResource
    {
    public:
        explicit TestResource(ResourceType type = ResourceType::Texture)
            : m_type(type)
        {
        }

        ResourceType GetType() const override { return m_type; }
        const char* GetTypeName() const override { return "TestResource"; }

        void SetTestState(ResourceState state)
        {
            SetState(state);
        }

    private:
        ResourceType m_type = ResourceType::Unknown;
    };

    // Deliberately reports the same ResourceType as TestResource. The acquire
    // contract is exact concrete type, rather than a ResourceType category.
    class AlternateTestResource final : public IResource
    {
    public:
        ResourceType GetType() const override { return ResourceType::Texture; }
        const char* GetTypeName() const override { return "AlternateTestResource"; }

        void SetTestState(ResourceState state)
        {
            SetState(state);
        }
    };

    class CountingLoader final : public IResourceLoader
    {
    public:
        ResourceType GetResourceType() const override { return ResourceType::Custom; }

        std::vector<std::string> GetSupportedExtensions() const override
        {
            return {".acquiretest"};
        }

        IResource* Load(const std::string&) override
        {
            ++loadCallCount;
            return nullptr;
        }

        std::atomic<uint32> loadCallCount{0};
    };

    class ResourceSubsystemScope final
    {
    public:
        ResourceSubsystemScope()
        {
            ResourceManagerConfig config;
            config.asyncThreadCount = 0;
            m_subsystem.Initialize(config);
        }

        ~ResourceSubsystemScope()
        {
            m_subsystem.Deinitialize();
        }

        ResourceSubsystem& Get() { return m_subsystem; }

    private:
        ResourceSubsystem m_subsystem;
    };
} // namespace

TEST(ResourceLoadedAcquireValidation, CacheAcquiresOnlyExactLoadedMatchingEntry)
{
    ResourceCache cache;

    auto* loaded = new TestResource();
    loaded->SetId(101);
    loaded->SetTestState(ResourceState::Loaded);
    cache.Store(loaded);
    ASSERT_EQ(1u, loaded->GetRefCount());

    const ResourceCache::Stats initialStats = cache.GetStats();
    ResourceHandle<TestResource> acquired = cache.TryAcquireLoaded<TestResource>(101);
    ASSERT_TRUE(acquired);
    EXPECT_EQ(loaded, acquired.Get());
    EXPECT_EQ(2u, loaded->GetRefCount());
    EXPECT_EQ(101u, acquired.GetId());
    EXPECT_TRUE(acquired.IsLoaded());
    EXPECT_EQ(initialStats.hitCount + 1, cache.GetStats().hitCount);

    acquired = nullptr;
    EXPECT_EQ(1u, loaded->GetRefCount());

    // The stored object has the same ResourceType but not the exact requested
    // concrete type. Rejection must not retain it or change its lifecycle.
    EXPECT_FALSE(cache.TryAcquireLoaded<AlternateTestResource>(101));
    EXPECT_EQ(1u, loaded->GetRefCount());
    EXPECT_EQ(ResourceState::Loaded, loaded->GetState());

    auto* unloaded = new TestResource();
    unloaded->SetId(102);
    cache.Store(unloaded);
    EXPECT_FALSE(cache.TryAcquireLoaded<TestResource>(102));
    EXPECT_EQ(1u, unloaded->GetRefCount());
    EXPECT_EQ(ResourceState::Unloaded, unloaded->GetState());

    auto* failed = new TestResource();
    failed->SetId(103);
    failed->SetTestState(ResourceState::Failed);
    cache.Store(failed);
    EXPECT_FALSE(cache.TryAcquireLoaded<TestResource>(103));
    EXPECT_EQ(1u, failed->GetRefCount());
    EXPECT_EQ(ResourceState::Failed, failed->GetState());

    const ResourceCache::Stats beforeMiss = cache.GetStats();
    EXPECT_FALSE(cache.TryAcquireLoaded<TestResource>(999));
    const ResourceCache::Stats afterMiss = cache.GetStats();
    EXPECT_EQ(beforeMiss.missCount + 1, afterMiss.missCount);
    EXPECT_EQ(beforeMiss.totalResources, afterMiss.totalResources);
}

TEST(ResourceLoadedAcquireValidation, ManagerAndSubsystemNeverLoadOnAcquireMiss)
{
    ResourceSubsystemScope scope;
    ResourceSubsystem& subsystem = scope.Get();
    ResourceManager& manager = subsystem.GetManager();

    auto loader = std::make_unique<CountingLoader>();
    CountingLoader* const loaderObserver = loader.get();
    subsystem.RegisterLoader(ResourceType::Custom, std::move(loader));

    EXPECT_FALSE(manager.TryAcquireLoaded<TestResource>(701));
    EXPECT_FALSE(subsystem.TryAcquireLoaded<TestResource>(701));
    EXPECT_EQ(0u, loaderObserver->loadCallCount.load());

    auto* resource = new TestResource(ResourceType::Custom);
    resource->SetId(702);
    resource->SetTestState(ResourceState::Loaded);
    manager.GetCache().Store(resource);

    ResourceHandle<TestResource> fromSubsystem =
        subsystem.TryAcquireLoaded<TestResource>(702);
    ASSERT_TRUE(fromSubsystem);
    EXPECT_EQ(resource, fromSubsystem.Get());
    EXPECT_EQ(0u, loaderObserver->loadCallCount.load());

    EXPECT_FALSE(subsystem.TryAcquireLoaded<AlternateTestResource>(702));
    EXPECT_EQ(0u, loaderObserver->loadCallCount.load());
}

TEST(ResourceLoadedAcquireValidation, StrongHandleRemainsValidAcrossDeterministicRemoval)
{
    ResourceCache cache;
    auto* resource = new TestResource();
    resource->SetId(801);
    resource->SetTestState(ResourceState::Loaded);
    cache.Store(resource);

    std::promise<void> acquiredPromise;
    std::future<void> acquiredFuture = acquiredPromise.get_future();
    std::promise<void> validateAfterRemovalPromise;
    std::shared_future<void> validateAfterRemoval =
        validateAfterRemovalPromise.get_future().share();
    std::atomic<bool> acquired{false};
    std::atomic<bool> validAfterRemoval{false};

    std::thread acquirer([&]
    {
        ResourceHandle<TestResource> handle =
            cache.TryAcquireLoaded<TestResource>(801);
        acquired.store(static_cast<bool>(handle), std::memory_order_release);
        acquiredPromise.set_value();

        validateAfterRemoval.wait();
        validAfterRemoval.store(
            handle && handle.GetId() == 801 && handle.IsLoaded(),
            std::memory_order_release);
    });

    const bool acquiredWithinTimeout =
        acquiredFuture.wait_for(std::chrono::seconds(5)) ==
        std::future_status::ready;
    EXPECT_TRUE(acquiredWithinTimeout);
    if (!acquiredWithinTimeout)
    {
        validateAfterRemovalPromise.set_value();
        acquirer.join();
        return;
    }

    const bool acquisitionSucceeded = acquired.load(std::memory_order_acquire);
    EXPECT_TRUE(acquisitionSucceeded);
    if (!acquisitionSucceeded)
    {
        validateAfterRemovalPromise.set_value();
        acquirer.join();
        return;
    }
    EXPECT_EQ(2u, resource->GetRefCount());
    EXPECT_TRUE(cache.Remove(801));
    EXPECT_FALSE(cache.TryAcquireLoaded<TestResource>(801));
    EXPECT_EQ(1u, resource->GetRefCount());

    validateAfterRemovalPromise.set_value();
    acquirer.join();
    EXPECT_TRUE(validAfterRemoval.load(std::memory_order_acquire));
}

TEST(ResourceLoadedAcquireValidation, StrongHandleRemainsValidAcrossShutdownAndAdmissionsClose)
{
    ResourceSubsystemScope scope;
    ResourceManager& manager = scope.Get().GetManager();

    auto* resource = new TestResource();
    resource->SetId(901);
    resource->SetTestState(ResourceState::Loaded);
    manager.GetCache().Store(resource);

    std::promise<void> acquiredPromise;
    std::future<void> acquiredFuture = acquiredPromise.get_future();
    std::promise<void> validateAfterShutdownPromise;
    std::shared_future<void> validateAfterShutdown =
        validateAfterShutdownPromise.get_future().share();
    std::atomic<bool> acquired{false};
    std::atomic<bool> validAfterShutdown{false};
    std::atomic<bool> admissionRejectedAfterShutdown{false};

    std::thread acquirer([&]
    {
        ResourceHandle<TestResource> handle =
            manager.TryAcquireLoaded<TestResource>(901);
        acquired.store(static_cast<bool>(handle), std::memory_order_release);
        acquiredPromise.set_value();

        validateAfterShutdown.wait();
        validAfterShutdown.store(
            handle && handle.GetId() == 901 && handle.IsLoaded(),
            std::memory_order_release);
        admissionRejectedAfterShutdown.store(
            !manager.TryAcquireLoaded<TestResource>(901),
            std::memory_order_release);
    });

    const bool acquiredWithinTimeout =
        acquiredFuture.wait_for(std::chrono::seconds(5)) ==
        std::future_status::ready;
    EXPECT_TRUE(acquiredWithinTimeout);
    if (!acquiredWithinTimeout)
    {
        validateAfterShutdownPromise.set_value();
        acquirer.join();
        return;
    }

    const bool acquisitionSucceeded = acquired.load(std::memory_order_acquire);
    EXPECT_TRUE(acquisitionSucceeded);
    if (!acquisitionSucceeded)
    {
        validateAfterShutdownPromise.set_value();
        acquirer.join();
        return;
    }
    EXPECT_EQ(2u, resource->GetRefCount());
    scope.Get().Deinitialize();
    EXPECT_FALSE(manager.TryAcquireLoaded<TestResource>(901));

    validateAfterShutdownPromise.set_value();
    acquirer.join();

    EXPECT_TRUE(validAfterShutdown.load(std::memory_order_acquire));
    EXPECT_TRUE(admissionRejectedAfterShutdown.load(std::memory_order_acquire));
}
