#include "Resource/ResourceLoadOperation.h"

#include <atomic>
#include <filesystem>
#include <limits>
#include <utility>

namespace RVX::Resource
{
namespace
{
    std::atomic<uint64> s_nextRequestId{1};

    size_t CombineHash(size_t seed, size_t value)
    {
        return seed ^ (value + static_cast<size_t>(0x9e3779b9u) +
                       (seed << 6u) + (seed >> 2u));
    }

    bool IsValidLoadingStage(ResourceLoadStage stage)
    {
        return stage >= ResourceLoadStage::Resolve &&
               stage <= ResourceLoadStage::Finalize;
    }
} // namespace

bool AssetKey::IsValid() const
{
    return !canonicalPath.empty() && resourceType != ResourceType::Unknown &&
           (expectedContentIdentity.IsEmpty() || expectedContentIdentity.IsValid());
}

bool AssetKey::operator==(const AssetKey& other) const
{
    return canonicalPath == other.canonicalPath &&
           resourceType == other.resourceType &&
           importOptionsHash == other.importOptionsHash &&
           platformProfileHash == other.platformProfileHash &&
           loaderSchemaVersion == other.loaderSchemaVersion &&
           expectedContentIdentity == other.expectedContentIdentity;
}

size_t AssetKeyHash::operator()(const AssetKey& key) const noexcept
{
    size_t hashValue = std::hash<std::string>{}(key.canonicalPath);
    hashValue = CombineHash(hashValue, std::hash<uint32>{}(static_cast<uint32>(key.resourceType)));
    hashValue = CombineHash(hashValue, std::hash<uint64>{}(key.importOptionsHash));
    hashValue = CombineHash(hashValue, std::hash<uint64>{}(key.platformProfileHash));
    hashValue = CombineHash(hashValue, std::hash<uint32>{}(key.loaderSchemaVersion));
    hashValue = CombineHash(
        hashValue,
        std::hash<uint32>{}(key.expectedContentIdentity.schemaVersion));
    hashValue = CombineHash(
        hashValue,
        std::hash<uint32>{}(static_cast<uint32>(key.expectedContentIdentity.domain)));
    hashValue = CombineHash(
        hashValue,
        std::hash<uint32>{}(static_cast<uint32>(key.expectedContentIdentity.scope)));
    hashValue = CombineHash(
        hashValue,
        std::hash<uint32>{}(static_cast<uint32>(key.expectedContentIdentity.algorithm)));
    hashValue = CombineHash(
        hashValue,
        std::hash<std::string>{}(key.expectedContentIdentity.digest));
    hashValue = CombineHash(
        hashValue,
        std::hash<uint64>{}(key.expectedContentIdentity.byteCount));
    return CombineHash(
        hashValue,
        std::hash<uint32>{}(key.expectedContentIdentity.fileCount));
}

std::string CanonicalizeAssetPath(const std::string& path)
{
    if (path.empty())
        return {};

    std::string genericPath = path;
    for (char& character : genericPath)
    {
        if (character == '\\')
        {
            character = '/';
        }
    }

    std::string normalized;
    try
    {
        std::filesystem::path normalizedPath =
            std::filesystem::path(genericPath).lexically_normal();
        normalizedPath.make_preferred();
        normalized = normalizedPath.string();
    }
    catch (const std::filesystem::filesystem_error&)
    {
        return {};
    }
    if (normalized == ".")
        return {};

    return normalized;
}

AssetKey MakeAssetKey(const std::string& path,
                      ResourceType resourceType,
                      uint64 importOptionsHash,
                      uint64 platformProfileHash,
                      uint32 loaderSchemaVersion,
                      ResourceContentIdentity expectedContentIdentity)
{
    AssetKey key;
    key.canonicalPath = CanonicalizeAssetPath(path);
    key.resourceType = resourceType;
    key.importOptionsHash = importOptionsHash;
    key.platformProfileHash = platformProfileHash;
    key.loaderSchemaVersion = loaderSchemaVersion;
    key.expectedContentIdentity = std::move(expectedContentIdentity);
    return key;
}

ResourceLoadOperation::ResourceLoadOperation(ResourceLoadRequestId requestId,
                                             AssetKey assetKey,
                                             ResourceLoadOptions options)
    : m_requestId(requestId)
    , m_assetKey(std::move(assetKey))
    , m_options(options)
{
}

ResourceLoadSnapshot ResourceLoadOperation::GetSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    ResourceLoadSnapshot snapshot;
    snapshot.requestId = m_requestId;
    snapshot.assetKey = m_assetKey;
    snapshot.state = m_state;
    snapshot.progress = m_progress;
    snapshot.error = m_error;
    snapshot.options = m_options;
    snapshot.subscriberCount = static_cast<uint32>(m_subscribers.size());
    snapshot.cancellationRequested = m_cancellationRequested;
    return snapshot;
}

uint64 ResourceLoadOperation::AddSubscriber()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_cancellationRequested ||
        m_state == ResourceLoadState::Failed ||
        m_state == ResourceLoadState::Cancelled ||
        m_nextSubscriberId == 0)
    {
        return 0;
    }

    const uint64 subscriberId = m_nextSubscriberId;
    ++m_nextSubscriberId;
    m_subscribers.insert(subscriberId);
    m_hadSubscriber = true;
    return subscriberId;
}

bool ResourceLoadOperation::CancelSubscriber(uint64 subscriberId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (subscriberId == 0 || m_subscribers.erase(subscriberId) == 0)
        return false;

    if (m_hadSubscriber && m_subscribers.empty() && !IsTerminalLocked() &&
        !m_ownerPublicationStarted)
    {
        m_cancellationRequested = true;
    }

    return true;
}

bool ResourceLoadOperation::IsSubscriberActive(uint64 subscriberId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return subscriberId != 0 && m_subscribers.contains(subscriberId);
}

ResourceHandle<IResource> ResourceLoadOperation::TryGetResource(uint64 subscriberId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != ResourceLoadState::Ready ||
        !m_subscribers.contains(subscriberId))
    {
        return {};
    }

    return m_readyResource;
}

bool ResourceLoadOperation::BeginLoading()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != ResourceLoadState::Queued || m_cancellationRequested)
        return false;

    m_state = ResourceLoadState::Loading;
    m_progress = {ResourceLoadStage::Resolve, 0.0f};
    return true;
}

bool ResourceLoadOperation::UpdateProgress(ResourceLoadStage stage, float32 fraction)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != ResourceLoadState::Loading ||
        m_cancellationRequested ||
        !IsValidLoadingStage(stage) ||
        fraction < 0.0f ||
        fraction > 1.0f ||
        stage < m_progress.stage ||
        (stage == m_progress.stage && fraction < m_progress.fraction))
    {
        return false;
    }

    m_progress = {stage, fraction};
    return true;
}

bool ResourceLoadOperation::BeginAwaitingPublish()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != ResourceLoadState::Loading || m_cancellationRequested)
        return false;

    m_state = ResourceLoadState::AwaitingPublish;
    m_progress = {ResourceLoadStage::AwaitingPublish, 1.0f};
    return true;
}

bool ResourceLoadOperation::BeginOwnerPublication()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != ResourceLoadState::AwaitingPublish ||
        m_cancellationRequested || m_ownerPublicationStarted)
    {
        return false;
    }

    m_ownerPublicationStarted = true;
    return true;
}

bool ResourceLoadOperation::CompleteReady(const ResourceHandle<IResource>& resource)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != ResourceLoadState::AwaitingPublish ||
        (m_cancellationRequested && !m_ownerPublicationStarted) ||
        !resource ||
        resource->GetType() != m_assetKey.resourceType)
    {
        return false;
    }

    m_readyResource = resource;
    m_error = {};
    m_state = ResourceLoadState::Ready;
    return true;
}

bool ResourceLoadOperation::CompleteFailed(ResourceLoadError error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if ((m_state != ResourceLoadState::Loading &&
         m_state != ResourceLoadState::AwaitingPublish) ||
        (m_cancellationRequested && !m_ownerPublicationStarted) ||
        error.code == ResourceLoadErrorCode::None ||
        error.code == ResourceLoadErrorCode::Cancelled)
    {
        return false;
    }

    m_error = std::move(error);
    m_state = ResourceLoadState::Failed;
    return true;
}

bool ResourceLoadOperation::CompleteCancelled(std::string reason)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (IsTerminalLocked() || m_ownerPublicationStarted)
        return false;

    m_readyResource.Reset();
    m_cancellationRequested = true;
    m_error.code = ResourceLoadErrorCode::Cancelled;
    m_error.message = std::move(reason);
    m_state = ResourceLoadState::Cancelled;
    return true;
}

bool ResourceLoadOperation::IsCancellationRequested() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cancellationRequested;
}

bool ResourceLoadOperation::IsTerminalLocked() const
{
    return m_state == ResourceLoadState::Ready ||
           m_state == ResourceLoadState::Failed ||
           m_state == ResourceLoadState::Cancelled;
}

ResourceLoadRequestId ResourceLoadOperationOwner::AllocateRequestId()
{
    uint64 current = s_nextRequestId.load(std::memory_order_relaxed);
    while (current != 0)
    {
        const uint64 next = current == std::numeric_limits<uint64>::max()
                                ? 0
                                : current + 1;
        if (s_nextRequestId.compare_exchange_weak(current,
                                                   next,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed))
        {
            return ResourceLoadRequestId(current);
        }
    }

    return {};
}

ResourceLoadOperationOwner ResourceLoadOperationOwner::Create(AssetKey assetKey,
                                                               ResourceLoadOptions options)
{
    if (!assetKey.IsValid())
        return {};

    const ResourceLoadRequestId requestId = AllocateRequestId();
    if (!requestId)
        return {};

    return ResourceLoadOperationOwner(
        std::shared_ptr<ResourceLoadOperation>(
            new ResourceLoadOperation(requestId, std::move(assetKey), options)));
}

ResourceLoadRequestId ResourceLoadOperationOwner::GetRequestId() const
{
    return m_operation ? m_operation->GetSnapshot().requestId : ResourceLoadRequestId{};
}

ResourceLoadSnapshot ResourceLoadOperationOwner::GetSnapshot() const
{
    return m_operation ? m_operation->GetSnapshot() : ResourceLoadSnapshot{};
}

bool ResourceLoadOperationOwner::IsCancellationRequested() const
{
    return m_operation && m_operation->IsCancellationRequested();
}

bool ResourceLoadOperationOwner::BeginLoading()
{
    return m_operation && m_operation->BeginLoading();
}

bool ResourceLoadOperationOwner::UpdateProgress(ResourceLoadStage stage, float32 fraction)
{
    return m_operation && m_operation->UpdateProgress(stage, fraction);
}

bool ResourceLoadOperationOwner::BeginAwaitingPublish()
{
    return m_operation && m_operation->BeginAwaitingPublish();
}

bool ResourceLoadOperationOwner::BeginOwnerPublication()
{
    return m_operation && m_operation->BeginOwnerPublication();
}

bool ResourceLoadOperationOwner::CompleteReady(const ResourceHandle<IResource>& resource)
{
    return m_operation && m_operation->CompleteReady(resource);
}

bool ResourceLoadOperationOwner::CompleteFailed(ResourceLoadError error)
{
    return m_operation && m_operation->CompleteFailed(std::move(error));
}

bool ResourceLoadOperationOwner::CompleteCancelled(std::string reason)
{
    return m_operation && m_operation->CompleteCancelled(std::move(reason));
}
} // namespace RVX::Resource
