#pragma once

/**
 * @file ResourceSubsystem.h
 * @brief Update-thread owner of CPU resources and immutable render uploads.
 */

#include "Core/Subsystem/EngineSubsystem.h"
#include "RenderContracts/IRenderResourceGateway.h"
#include "Resource/ResourceHandle.h"
#include "Resource/ResourceManager.h"

#include <functional>
#include <future>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace RVX::Resource
{
    enum class RenderResourceResolveCode : uint8
    {
        Resolved = 0,
        NotFound = 1,
        KindMismatch = 2,
        StaleGeneration = 3
    };

    struct RenderResourceResolveResult
    {
        RenderResourceResolveCode code = RenderResourceResolveCode::NotFound;
        RenderResourceHandle handle{};
        RenderResourceStatus status{};
    };

    /** @brief Update-side resource-to-render publication diagnostics. */
    struct ResourceRenderStats
    {
        uint64 readyEvents = 0;
        uint64 reloadEvents = 0;
        uint64 unloadEvents = 0;
        uint64 reservations = 0;
        uint64 existingReservations = 0;
        uint64 enqueueAccepted = 0;
        uint64 queueFullByCount = 0;
        uint64 queueFullByBytes = 0;
        uint64 retryAttempts = 0;
        uint64 releasesAccepted = 0;
        uint64 terminalRequestsReclaimed = 0;
        uint64 buildFailures = 0;
        uint64 gatewayRejections = 0;
        uint64 wrongThreadMutations = 0;
        size_t pendingResourceCount = 0;
        size_t pendingUploadCount = 0;
        size_t retainedRequestCount = 0;
    };

    /**
     * @brief Sole update-thread producer for the render-resource gateway.
     */
    class ResourceSubsystem : public EngineSubsystem
    {
    public:
        // =====================================================================
        // Construction and EngineSubsystem
        // =====================================================================
        ResourceSubsystem() = default;
        ~ResourceSubsystem() override = default;

        const char* GetName() const override { return "ResourceSubsystem"; }
        void Initialize() override;
        void Initialize(const ResourceManagerConfig& config);
        void Deinitialize() override;
        bool ShouldTick() const override { return true; }
        void Tick(float deltaTime) override;

        /** @brief Inject Render's non-owning narrow gateway. */
        void SetRenderResourceGateway(IRenderResourceGateway* gateway);

        /** @brief Resolve an AssetId to its exact current generational handle. */
        RenderResourceResolveResult ResolveRenderResource(
            AssetId assetId,
            RenderResourceKind kind) const;

        /** @brief Publish a runtime-created CPU resource through the normal gateway. */
        [[nodiscard]] bool PublishRenderResource(
            ResourceHandle<IResource> resource);

        /**
         * @brief Seal new upload production while preserving mappings needed
         * by already accepted render frames until the terminal drain.
         */
        void BeginRenderShutdown();

        /** @brief Release retained request payloads after observing terminal state. */
        void DrainTerminalRenderRequests();

        ResourceRenderStats GetRenderResourceStats() const;

        // =====================================================================
        // Resource Loading
        // =====================================================================
        template<typename T>
        ResourceHandle<T> Load(const std::string& path)
        {
            if (!RequireUpdateThread("Load"))
                return {};
            return ResourceManager::Get().Load<T>(path);
        }

        template<typename T>
        ResourceHandle<T> Load(ResourceId id)
        {
            if (!RequireUpdateThread("Load"))
                return {};
            return ResourceManager::Get().Load<T>(id);
        }

        template<typename T>
        std::future<ResourceHandle<T>> LoadAsync(const std::string& path)
        {
            if (!RequireUpdateThread("LoadAsync"))
                return {};
            return ResourceManager::Get().LoadAsync<T>(path);
        }

        template<typename T>
        void LoadAsync(const std::string& path,
                       std::function<void(ResourceHandle<T>)> callback)
        {
            if (!RequireUpdateThread("LoadAsync"))
                return;
            ResourceManager::Get().LoadAsync<T>(path, std::move(callback));
        }

        bool IsLoaded(const std::string& path) const;
        bool IsLoaded(ResourceId id) const;
        void Unload(const std::string& path);
        void Unload(ResourceId id);
        void UnloadUnused();
        void RegisterLoader(ResourceType type,
                            std::unique_ptr<IResourceLoader> loader);
        void EnableHotReload(bool enable);
        void OnResourceReloaded(
            std::function<void(ResourceId, IResource*)> callback);
        void SetCacheLimit(size_t bytes);
        void ClearCache();
        ResourceManager::Stats GetStats() const;
        ResourceManager& GetManager();
        const ResourceManager& GetManager() const;

    private:
        // =====================================================================
        // Update-side State
        // =====================================================================
        struct PendingResource
        {
            AssetId assetId{};
            RenderResourceKind kind = RenderResourceKind::Invalid;
            ResourceHandle<IResource> resource;
            RenderUploadPriority priority = RenderUploadPriority::Normal;
            uint64 fifoOrder = 0;
            uint64 sourceRevision = 0;
        };

        struct PendingUpload
        {
            AssetId assetId{};
            RenderResourceKind kind = RenderResourceKind::Invalid;
            RenderResourceHandle handle{};
            ResourceUploadRequestRef request{};
            RenderUploadPriority priority = RenderUploadPriority::Normal;
            uint64 fifoOrder = 0;
        };

        struct TrackedResource
        {
            RenderResourceKind kind = RenderResourceKind::Invalid;
            RenderResourceHandle handle{};
        };

        struct RetainedRequest
        {
            AssetId assetId{};
            RenderResourceHandle handle{};
            ResourceUploadRequestRef request{};
        };

        // =====================================================================
        // Internal Methods
        // =====================================================================
        bool IsOnUpdateThread() const;
        bool RequireUpdateThread(const char* operation);
        void HandleLifecycleEvent(const ResourceLifecycleEvent& event);
        void QueueReadyResource(const ResourceLifecycleEvent& event);
        [[nodiscard]] bool QueueRenderResourceTree(
            ResourceHandle<IResource> resource);
        void ProcessPendingResources();
        void ProcessPendingUploads();
        bool TryBuildPendingResource(PendingResource& pending);
        RenderResourceHandle ResolveDependency(AssetId assetId,
                                               RenderResourceKind kind) const;
        void ReleaseAsset(AssetId assetId);
        void RemovePendingAsset(AssetId assetId);
        static RenderResourceKind ToRenderResourceKind(ResourceType type);
        static RenderUploadPriority GetUploadPriority(ResourceType type);

        // =====================================================================
        // Members
        // =====================================================================
        ResourceManagerConfig m_config{};
        IRenderResourceGateway* m_gateway = nullptr;
        std::thread::id m_updateThreadId{};
        bool m_initialized = false;
        bool m_renderShuttingDown = false;
        uint64 m_nextRequestSequence = 1;
        uint64 m_nextFifoOrder = 1;
        uint64 m_nextSourceRevision = 1;

        std::vector<PendingResource> m_pendingResources{};
        std::vector<PendingUpload> m_pendingUploads{};
        std::unordered_map<AssetId, TrackedResource, AssetIdHash>
            m_trackedResources{};
        std::unordered_map<RenderResourceHandle,
                           RetainedRequest,
                           RenderResourceHandleHash>
            m_retainedRequests{};
        std::vector<ResourceUploadRequestRef> m_localTerminalRequests{};
        ResourceRenderStats m_renderStats{};
    };
} // namespace RVX::Resource

namespace RVX
{
    using Resource::RenderResourceResolveCode;
    using Resource::RenderResourceResolveResult;
    using Resource::ResourceRenderStats;
    using Resource::ResourceSubsystem;
} // namespace RVX
