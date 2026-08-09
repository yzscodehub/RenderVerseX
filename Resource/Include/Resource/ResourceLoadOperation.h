#pragma once

/**
 * @file ResourceLoadOperation.h
 * @brief Thread-safe shared state contract for deduplicated resource loads
 */

#include "Core/Diagnostics/Trace.h"
#include "Core/Types.h"
#include "Resource/IResource.h"
#include "Resource/ResourceHandle.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_set>

namespace RVX::Resource
{
    /** @brief Stable identity for one resource load request. Value zero is invalid. */
    class ResourceLoadRequestId
    {
    public:
        constexpr ResourceLoadRequestId() = default;

        constexpr bool IsValid() const { return m_value != 0; }
        constexpr uint64 GetValue() const { return m_value; }
        constexpr explicit operator bool() const { return IsValid(); }

        constexpr bool operator==(const ResourceLoadRequestId& other) const
        {
            return m_value == other.m_value;
        }

        constexpr bool operator!=(const ResourceLoadRequestId& other) const
        {
            return !(*this == other);
        }

        constexpr bool operator<(const ResourceLoadRequestId& other) const
        {
            return m_value < other.m_value;
        }

    private:
        explicit constexpr ResourceLoadRequestId(uint64 value)
            : m_value(value)
        {
        }

        uint64 m_value = 0;

        friend class ResourceLoadOperationOwner;
    };

    /**
     * @brief Cache identity for a source asset and every input that can affect its output.
     *
     * canonicalPath is expected to be a normalized, generic path. Use MakeAssetKey()
     * when the caller only has an input path.
     */
    struct AssetKey
    {
        std::string canonicalPath;
        ResourceType resourceType = ResourceType::Unknown;
        uint64 importOptionsHash = 0;
        uint64 platformProfileHash = 0;
        uint32 loaderSchemaVersion = 1;

        bool IsValid() const;

        bool operator==(const AssetKey& other) const;
        bool operator!=(const AssetKey& other) const { return !(*this == other); }
    };

    /** @brief Hash functor for AssetKey map/set use. */
    struct AssetKeyHash
    {
        size_t operator()(const AssetKey& key) const noexcept;
    };

    /** @brief Normalize a source path into the representation used by AssetKey. */
    std::string CanonicalizeAssetPath(const std::string& path);

    /** @brief Construct an AssetKey from an input path and all output-affecting inputs. */
    AssetKey MakeAssetKey(const std::string& path,
                          ResourceType resourceType,
                          uint64 importOptionsHash = 0,
                          uint64 platformProfileHash = 0,
                          uint32 loaderSchemaVersion = 1);

    enum class ResourceLoadState : uint8
    {
        Queued = 0,
        Loading,
        AwaitingPublish,
        Ready,
        Failed,
        Cancelled
    };

    /** @brief Fine-grained producer stage reported while an operation is loading. */
    enum class ResourceLoadStage : uint8
    {
        None = 0,
        Resolve,
        Read,
        Decode,
        Finalize,
        AwaitingPublish
    };

    struct ResourceLoadProgress
    {
        ResourceLoadStage stage = ResourceLoadStage::None;
        float32 fraction = 0.0f;
    };

    enum class ResourceLoadErrorCode : uint8
    {
        None = 0,
        InvalidRequest,
        LoaderUnavailable,
        LoaderFailure,
        PublishFailure,
        TypeMismatch,
        Cancelled
    };

    struct ResourceLoadError
    {
        ResourceLoadErrorCode code = ResourceLoadErrorCode::None;
        std::string message;

        bool HasError() const { return code != ResourceLoadErrorCode::None; }
    };

    enum class ResourceLoadPriority : uint8
    {
        Background = 0,
        Normal,
        High
    };

    /** @brief Scheduling policy attached to one operation. It has no cache-publish authority. */
    struct ResourceLoadOptions
    {
        ResourceLoadPriority priority = ResourceLoadPriority::Normal;
        bool allowCacheLookup = true;
        bool allowRetry = false;
        uint32 maxRetryCount = 0;

        /// Every input that can change imported output participates in the
        /// AssetKey.  Keeping these on the request rather than on a loader
        /// prevents different import profiles from accidentally sharing one
        /// in-flight operation.
        uint64 importOptionsHash = 0;
        uint64 platformProfileHash = 0;
        uint32 loaderSchemaVersion = 1;

        Diagnostics::TraceContext traceContext;
    };

    /** @brief Immutable copy of the thread-safe operation state. */
    struct ResourceLoadSnapshot
    {
        ResourceLoadRequestId requestId;
        AssetKey assetKey;
        ResourceLoadState state = ResourceLoadState::Queued;
        ResourceLoadProgress progress;
        ResourceLoadError error;
        ResourceLoadOptions options;
        uint32 subscriberCount = 0;
        bool cancellationRequested = false;

        bool IsTerminal() const
        {
            return state == ResourceLoadState::Ready ||
                   state == ResourceLoadState::Failed ||
                   state == ResourceLoadState::Cancelled;
        }
    };

    class ResourceLoadOperationOwner;

    template<typename T>
    class ResourceLoadHandle;

    /**
     * @brief Shared operation state. Mutations are intentionally private to the owner.
     *
     * This object stages a completed ResourceHandle for subscribers. It never references
     * ResourceCache and therefore cannot publish a worker result into the cache.
     */
    class ResourceLoadOperation final
        : public std::enable_shared_from_this<ResourceLoadOperation>
    {
    public:
        ResourceLoadOperation(const ResourceLoadOperation&) = delete;
        ResourceLoadOperation& operator=(const ResourceLoadOperation&) = delete;

        ResourceLoadSnapshot GetSnapshot() const;

    private:
        ResourceLoadOperation(ResourceLoadRequestId requestId,
                              AssetKey assetKey,
                              ResourceLoadOptions options);

        uint64 AddSubscriber();
        bool CancelSubscriber(uint64 subscriberId);
        bool IsSubscriberActive(uint64 subscriberId) const;
        ResourceHandle<IResource> TryGetResource(uint64 subscriberId) const;

        bool BeginLoading();
        bool UpdateProgress(ResourceLoadStage stage, float32 fraction);
        bool BeginAwaitingPublish();
        bool BeginOwnerPublication();
        bool CompleteReady(const ResourceHandle<IResource>& resource);
        bool CompleteFailed(ResourceLoadError error);
        bool CompleteCancelled(std::string reason);
        bool IsCancellationRequested() const;

        bool IsTerminalLocked() const;

        mutable std::mutex m_mutex;
        ResourceLoadRequestId m_requestId;
        AssetKey m_assetKey;
        ResourceLoadState m_state = ResourceLoadState::Queued;
        ResourceLoadProgress m_progress;
        ResourceLoadError m_error;
        ResourceLoadOptions m_options;
        ResourceHandle<IResource> m_readyResource;
        std::unordered_set<uint64> m_subscribers;
        uint64 m_nextSubscriberId = 1;
        bool m_hadSubscriber = false;
        bool m_cancellationRequested = false;
        bool m_ownerPublicationStarted = false;

        friend class ResourceLoadOperationOwner;

        template<typename T>
        friend class ResourceLoadHandle;
    };

    /**
     * @brief Manager-owned controller for a ResourceLoadOperation.
     *
     * The controller only advances operation state and stages a completed resource for
     * handles. Cache publication remains an explicit ResourceManager responsibility.
     */
    class ResourceLoadOperationOwner
    {
    public:
        ResourceLoadOperationOwner() = default;

        static ResourceLoadOperationOwner Create(AssetKey assetKey,
                                                 ResourceLoadOptions options = {});

        bool IsValid() const { return m_operation != nullptr; }
        explicit operator bool() const { return IsValid(); }

        ResourceLoadRequestId GetRequestId() const;
        ResourceLoadSnapshot GetSnapshot() const;
        bool IsCancellationRequested() const;

        template<typename T>
        ResourceLoadHandle<T> Subscribe();

        bool BeginLoading();
        bool UpdateProgress(ResourceLoadStage stage, float32 fraction);
        bool BeginAwaitingPublish();
        /** @brief Linearize owner publication against last-subscriber cancellation. */
        bool BeginOwnerPublication();
        bool CompleteReady(const ResourceHandle<IResource>& resource);
        bool CompleteFailed(ResourceLoadError error);
        bool CompleteCancelled(std::string reason = {});

    private:
        explicit ResourceLoadOperationOwner(std::shared_ptr<ResourceLoadOperation> operation)
            : m_operation(std::move(operation))
        {
        }

        static ResourceLoadRequestId AllocateRequestId();

        std::shared_ptr<ResourceLoadOperation> m_operation;
    };

    /**
     * @brief Move-only subscription to a shared typed resource load operation.
     *
     * Cancelling or destroying one handle only removes that subscription. When the last
     * live subscription leaves a non-terminal operation, it requests cooperative
     * cancellation without forcing a worker thread to stop unsafely.
     */
    template<typename T>
    class ResourceLoadHandle
    {
        static_assert(std::is_base_of_v<IResource, T>, "T must derive from IResource");

    public:
        ResourceLoadHandle() = default;
        ResourceLoadHandle(const ResourceLoadHandle&) = delete;
        ResourceLoadHandle& operator=(const ResourceLoadHandle&) = delete;

        ResourceLoadHandle(ResourceLoadHandle&& other) noexcept
            : m_operation(std::move(other.m_operation))
            , m_subscriberId(other.m_subscriberId)
        {
            other.m_subscriberId = 0;
        }

        ResourceLoadHandle& operator=(ResourceLoadHandle&& other) noexcept
        {
            if (this != &other)
            {
                Cancel();
                m_operation = std::move(other.m_operation);
                m_subscriberId = other.m_subscriberId;
                other.m_subscriberId = 0;
            }
            return *this;
        }

        ~ResourceLoadHandle()
        {
            Cancel();
        }

        bool IsValid() const
        {
            return m_operation && m_subscriberId != 0 &&
                   m_operation->IsSubscriberActive(m_subscriberId);
        }

        explicit operator bool() const { return IsValid(); }

        ResourceLoadRequestId GetRequestId() const
        {
            return m_operation ? m_operation->GetSnapshot().requestId : ResourceLoadRequestId{};
        }

        ResourceLoadSnapshot GetSnapshot() const
        {
            if (!m_operation)
                return {};

            ResourceLoadSnapshot snapshot = m_operation->GetSnapshot();
            if (snapshot.state == ResourceLoadState::Ready && m_subscriberId != 0)
            {
                const ResourceHandle<IResource> resource =
                    m_operation->TryGetResource(m_subscriberId);
                if (resource && dynamic_cast<T*>(resource.Get()) == nullptr)
                {
                    // Type compatibility is subscriber-specific: do not poison
                    // the shared operation for correctly typed subscribers, but
                    // never report a false Ready state to this typed handle.
                    snapshot.state = ResourceLoadState::Failed;
                    snapshot.error = {ResourceLoadErrorCode::TypeMismatch,
                                      "The published resource does not match this subscription type."};
                }
            }
            return snapshot;
        }

        /** @brief Remove only this subscription. Returns false for an already-cancelled handle. */
        bool Cancel()
        {
            if (!m_operation || m_subscriberId == 0)
                return false;

            const bool cancelled = m_operation->CancelSubscriber(m_subscriberId);
            m_subscriberId = 0;
            m_operation.reset();
            return cancelled;
        }

        /** @brief Return a strong typed handle only when this subscription observes Ready. */
        ResourceHandle<T> TryGet() const
        {
            if (!m_operation || m_subscriberId == 0)
                return {};

            ResourceHandle<IResource> resource = m_operation->TryGetResource(m_subscriberId);
            if (!resource)
                return {};

            T* typedResource = dynamic_cast<T*>(resource.Get());
            return typedResource ? ResourceHandle<T>(typedResource) : ResourceHandle<T>{};
        }

    private:
        ResourceLoadHandle(std::shared_ptr<ResourceLoadOperation> operation,
                           uint64 subscriberId)
            : m_operation(std::move(operation))
            , m_subscriberId(subscriberId)
        {
        }

        std::shared_ptr<ResourceLoadOperation> m_operation;
        uint64 m_subscriberId = 0;

        friend class ResourceLoadOperationOwner;
    };

    template<typename T>
    ResourceLoadHandle<T> ResourceLoadOperationOwner::Subscribe()
    {
        static_assert(std::is_base_of_v<IResource, T>, "T must derive from IResource");

        if (!m_operation)
            return {};

        const uint64 subscriberId = m_operation->AddSubscriber();
        return subscriberId != 0 ? ResourceLoadHandle<T>(m_operation, subscriberId)
                                 : ResourceLoadHandle<T>{};
    }
} // namespace RVX::Resource

namespace std
{
    template<>
    struct hash<RVX::Resource::ResourceLoadRequestId>
    {
        size_t operator()(const RVX::Resource::ResourceLoadRequestId& requestId) const noexcept
        {
            return hash<RVX::uint64>{}(requestId.GetValue());
        }
    };
} // namespace std
