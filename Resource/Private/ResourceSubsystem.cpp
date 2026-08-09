#include "Resource/ResourceSubsystem.h"

#include "Core/Assert.h"
#include "Core/Log.h"
#include "Resource/RenderUploadRequestBuilder.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace RVX::Resource
{
namespace
{
    bool IsTerminalStatus(const RenderResourceStatus& status)
    {
        if (status.code == RenderResourceStatusCode::StaleGeneration ||
            status.code == RenderResourceStatusCode::InvalidHandle)
        {
            return true;
        }
        return status.state == RenderResourcePublicState::GPUReady ||
               status.state == RenderResourcePublicState::Failed ||
               status.state == RenderResourcePublicState::Released;
    }

    bool HigherPriority(RenderUploadPriority left,
                        RenderUploadPriority right)
    {
        return static_cast<uint8>(left) > static_cast<uint8>(right);
    }
} // namespace

    void ResourceSubsystem::Initialize()
    {
        if (m_initialized)
            return;

        m_updateThreadId = std::this_thread::get_id();
        m_renderShuttingDown = false;
        ResourceManager& manager = ResourceManager::Get();
        manager.Initialize(m_config);
        manager.SetLifecycleEventCallback(
            [this](const ResourceLifecycleEvent& event)
            {
                HandleLifecycleEvent(event);
            });
        m_initialized = true;
    }

    void ResourceSubsystem::Initialize(const ResourceManagerConfig& config)
    {
        m_config = config;
        Initialize();
    }

    void ResourceSubsystem::Deinitialize()
    {
        if (!m_initialized)
            return;
        if (!RequireUpdateThread("Deinitialize"))
            return;

        BeginRenderShutdown();
        ResourceManager& manager = ResourceManager::Get();
        manager.Shutdown();
        DrainTerminalRenderRequests();
        manager.SetLifecycleEventCallback({});

        RVX_VERIFY(m_retainedRequests.empty(),
                   "ResourceSubsystem deinitialized before all render requests reached a terminal state");
        m_pendingResources.clear();
        m_pendingUploads.clear();
        m_localTerminalRequests.clear();
        m_retainedRequests.clear();
        m_trackedResources.clear();
        m_gateway = nullptr;
        m_initialized = false;
        m_updateThreadId = {};
    }

    void ResourceSubsystem::Tick(float deltaTime)
    {
        (void)deltaTime;
        if (!m_initialized || !RequireUpdateThread("Tick"))
            return;

        ResourceManager& manager = ResourceManager::Get();
        manager.ProcessCompletedLoads();
        if (m_config.enableHotReload)
        {
            manager.CheckForChanges();
            manager.ProcessCompletedLoads();
        }

        DrainTerminalRenderRequests();
        if (!m_renderShuttingDown)
        {
            ProcessPendingResources();
            ProcessPendingUploads();
        }
    }

    void ResourceSubsystem::SetRenderResourceGateway(
        IRenderResourceGateway* gateway)
    {
        if (m_initialized && !RequireUpdateThread("SetRenderResourceGateway"))
            return;
        if (m_renderShuttingDown && gateway != nullptr)
        {
            ++m_renderStats.gatewayRejections;
            RVX_VERIFY(false,
                       "ResourceSubsystem rejected a gateway mutation after render shutdown began");
            return;
        }
        m_gateway = gateway;
    }

    RenderResourceResolveResult ResourceSubsystem::ResolveRenderResource(
        AssetId assetId,
        RenderResourceKind kind) const
    {
        RenderResourceResolveResult result;
        const auto tracked = m_trackedResources.find(assetId);
        if (tracked == m_trackedResources.end())
            return result;
        if (tracked->second.kind != kind)
        {
            result.code = RenderResourceResolveCode::KindMismatch;
            return result;
        }

        result.handle = tracked->second.handle;
        if (m_gateway == nullptr)
            return result;
        result.status = m_gateway->QueryResourceStatus(result.handle);
        if (result.status.code == RenderResourceStatusCode::StaleGeneration ||
            result.status.code == RenderResourceStatusCode::InvalidHandle)
        {
            result.code = RenderResourceResolveCode::StaleGeneration;
            return result;
        }
        result.code = RenderResourceResolveCode::Resolved;
        return result;
    }

    bool ResourceSubsystem::PublishRenderResource(
        ResourceHandle<IResource> resource)
    {
        if (!m_initialized || m_renderShuttingDown ||
            !RequireUpdateThread("PublishRenderResource"))
        {
            return false;
        }
        return QueueRenderResourceTree(std::move(resource));
    }

    void ResourceSubsystem::BeginRenderShutdown()
    {
        if (!m_initialized || m_renderShuttingDown)
            return;
        if (!RequireUpdateThread("BeginRenderShutdown"))
            return;

        m_renderShuttingDown = true;
        m_pendingResources.clear();
        for (PendingUpload& pending : m_pendingUploads)
        {
            if (pending.request)
                m_localTerminalRequests.push_back(std::move(pending.request));
        }
        m_pendingUploads.clear();
    }

    void ResourceSubsystem::DrainTerminalRenderRequests()
    {
        if (m_initialized && !RequireUpdateThread(
                                 "DrainTerminalRenderRequests"))
        {
            return;
        }

        const size_t localCount = m_localTerminalRequests.size();
        m_localTerminalRequests.clear();
        m_renderStats.terminalRequestsReclaimed += localCount;

        if (m_gateway == nullptr)
            return;

        auto request = m_retainedRequests.begin();
        while (request != m_retainedRequests.end())
        {
            const RenderResourceStatus status =
                m_gateway->QueryResourceStatus(request->second.handle);
            if (!IsTerminalStatus(status))
            {
                ++request;
                continue;
            }

            if (status.state == RenderResourcePublicState::Released ||
                status.code != RenderResourceStatusCode::Current)
            {
                const auto tracked =
                    m_trackedResources.find(request->second.assetId);
                if (tracked != m_trackedResources.end() &&
                    tracked->second.handle == request->second.handle)
                {
                    m_trackedResources.erase(tracked);
                }
            }
            request = m_retainedRequests.erase(request);
            ++m_renderStats.terminalRequestsReclaimed;
        }

        // BeginRenderShutdown deliberately preserves current mappings while
        // the dedicated renderer drains accepted frames. Once Render has
        // stopped, its registry owns no live GPU resources and these lookup
        // mappings can be discarded without enqueueing releases ahead of the
        // frames that still reference them.
        if (m_renderShuttingDown)
        {
            m_trackedResources.clear();
        }
    }

    ResourceRenderStats ResourceSubsystem::GetRenderResourceStats() const
    {
        ResourceRenderStats stats = m_renderStats;
        stats.pendingResourceCount = m_pendingResources.size();
        stats.pendingUploadCount = m_pendingUploads.size();
        stats.retainedRequestCount = m_retainedRequests.size();
        return stats;
    }

    bool ResourceSubsystem::IsLoaded(const std::string& path) const
    {
        return ResourceManager::Get().IsLoaded(path);
    }

    bool ResourceSubsystem::IsLoaded(ResourceId id) const
    {
        return ResourceManager::Get().IsLoaded(id);
    }

    void ResourceSubsystem::Unload(const std::string& path)
    {
        if (RequireUpdateThread("Unload"))
            ResourceManager::Get().Unload(path);
    }

    void ResourceSubsystem::Unload(ResourceId id)
    {
        if (RequireUpdateThread("Unload"))
            ResourceManager::Get().Unload(id);
    }

    void ResourceSubsystem::UnloadUnused()
    {
        if (RequireUpdateThread("UnloadUnused"))
            ResourceManager::Get().UnloadUnused();
    }

    void ResourceSubsystem::RegisterLoader(
        ResourceType type,
        std::unique_ptr<IResourceLoader> loader)
    {
        if (RequireUpdateThread("RegisterLoader"))
            ResourceManager::Get().RegisterLoader(type, std::move(loader));
    }

    void ResourceSubsystem::EnableHotReload(bool enable)
    {
        if (!RequireUpdateThread("EnableHotReload"))
            return;
        m_config.enableHotReload = enable;
        ResourceManager::Get().EnableHotReload(enable);
    }

    void ResourceSubsystem::OnResourceReloaded(
        std::function<void(ResourceId, IResource*)> callback)
    {
        if (RequireUpdateThread("OnResourceReloaded"))
        {
            ResourceManager::Get().OnResourceReloaded(std::move(callback));
        }
    }

    void ResourceSubsystem::SetCacheLimit(size_t bytes)
    {
        if (RequireUpdateThread("SetCacheLimit"))
            ResourceManager::Get().SetCacheLimit(bytes);
    }

    void ResourceSubsystem::ClearCache()
    {
        if (RequireUpdateThread("ClearCache"))
            ResourceManager::Get().ClearCache();
    }

    ResourceManager::Stats ResourceSubsystem::GetStats() const
    {
        return ResourceManager::Get().GetStats();
    }

    ResourceManager& ResourceSubsystem::GetManager()
    {
        return ResourceManager::Get();
    }

    const ResourceManager& ResourceSubsystem::GetManager() const
    {
        return ResourceManager::Get();
    }

    bool ResourceSubsystem::IsOnUpdateThread() const
    {
        return m_updateThreadId == std::thread::id{} ||
               m_updateThreadId == std::this_thread::get_id();
    }

    bool ResourceSubsystem::RequireUpdateThread(const char* operation)
    {
        if (IsOnUpdateThread())
            return true;
        ++m_renderStats.wrongThreadMutations;
        RVX_VERIFY(false,
                   "ResourceSubsystem operation '{}' must run on its registered update thread",
                   operation == nullptr ? "unknown" : operation);
        return false;
    }

    void ResourceSubsystem::HandleLifecycleEvent(
        const ResourceLifecycleEvent& event)
    {
        if (!RequireUpdateThread("HandleLifecycleEvent"))
            return;

        switch (event.type)
        {
            case ResourceLifecycleEventType::Ready:
                ++m_renderStats.readyEvents;
                QueueReadyResource(event);
                break;
            case ResourceLifecycleEventType::Reloaded:
                ++m_renderStats.reloadEvents;
                ReleaseAsset(AssetId{event.resourceId});
                QueueReadyResource(event);
                break;
            case ResourceLifecycleEventType::BeforeUnload:
                ++m_renderStats.unloadEvents;
                ReleaseAsset(AssetId{event.resourceId});
                break;
        }
    }

    void ResourceSubsystem::QueueReadyResource(
        const ResourceLifecycleEvent& event)
    {
        if (m_renderShuttingDown || !event.resource ||
            event.resourceId == InvalidResourceId)
        {
            return;
        }

        static_cast<void>(QueueRenderResourceTree(event.resource));
    }

    bool ResourceSubsystem::QueueRenderResourceTree(
        ResourceHandle<IResource> rootResource)
    {
        if (!rootResource ||
            rootResource.GetId() == InvalidResourceId)
        {
            return false;
        }

        const uint64 sourceRevision = m_nextSourceRevision++;
        std::vector<ResourceHandle<IResource>> resourcesToQueue{
            std::move(rootResource)};
        std::unordered_set<ResourceId> visited;
        size_t nextResource = 0;
        bool renderResourceFound = false;
        while (nextResource < resourcesToQueue.size())
        {
            ResourceHandle<IResource> resource =
                resourcesToQueue[nextResource++];
            if (!resource || resource.GetId() == InvalidResourceId ||
                !visited.insert(resource.GetId()).second)
            {
                continue;
            }

            for (ResourceId dependencyId : resource->GetAllDependencies())
            {
                if (dependencyId == InvalidResourceId ||
                    visited.contains(dependencyId))
                {
                    continue;
                }
                if (IResource* dependency =
                        ResourceManager::Get().GetCache().Get(dependencyId))
                {
                    resourcesToQueue.emplace_back(dependency);
                }
            }

            const RenderResourceKind kind =
                ToRenderResourceKind(resource->GetType());
            if (kind == RenderResourceKind::Invalid)
                continue;
            renderResourceFound = true;

            const AssetId assetId{resource.GetId()};
            const auto tracked = m_trackedResources.find(assetId);
            if (tracked != m_trackedResources.end())
            {
                if (tracked->second.kind != kind)
                    ++m_renderStats.gatewayRejections;
                continue;
            }
            const auto pending = std::find_if(
                m_pendingResources.begin(),
                m_pendingResources.end(),
                [assetId](const PendingResource& value)
                {
                    return value.assetId == assetId;
                });
            if (pending != m_pendingResources.end())
                continue;

            m_pendingResources.push_back(PendingResource{
                assetId,
                kind,
                resource,
                GetUploadPriority(resource->GetType()),
                m_nextFifoOrder++,
                sourceRevision});
        }
        return renderResourceFound;
    }

    void ResourceSubsystem::ProcessPendingResources()
    {
        std::stable_sort(
            m_pendingResources.begin(),
            m_pendingResources.end(),
            [](const PendingResource& left, const PendingResource& right)
            {
                if (left.priority != right.priority)
                    return HigherPriority(left.priority, right.priority);
                return left.fifoOrder < right.fifoOrder;
            });

        size_t index = 0;
        while (index < m_pendingResources.size())
        {
            if (TryBuildPendingResource(m_pendingResources[index]))
            {
                m_pendingResources.erase(m_pendingResources.begin() + index);
            }
            else
            {
                ++index;
            }
        }
    }

    void ResourceSubsystem::ProcessPendingUploads()
    {
        if (m_gateway == nullptr)
            return;

        std::stable_sort(
            m_pendingUploads.begin(),
            m_pendingUploads.end(),
            [](const PendingUpload& left, const PendingUpload& right)
            {
                if (left.priority != right.priority)
                    return HigherPriority(left.priority, right.priority);
                return left.fifoOrder < right.fifoOrder;
            });

        size_t index = 0;
        while (index < m_pendingUploads.size())
        {
            PendingUpload& pending = m_pendingUploads[index];
            const RenderUploadEnqueueResult result =
                m_gateway->TryEnqueueUpload(pending.request);
            if (result.code == RenderUploadEnqueueCode::Accepted)
            {
                m_retainedRequests.emplace(
                    pending.handle,
                    RetainedRequest{pending.assetId,
                                    pending.handle,
                                    std::move(pending.request)});
                m_pendingUploads.erase(m_pendingUploads.begin() + index);
                ++m_renderStats.enqueueAccepted;
                continue;
            }
            if (result.code == RenderUploadEnqueueCode::QueueFullByCount ||
                result.code == RenderUploadEnqueueCode::QueueFullByBytes)
            {
                if (result.code == RenderUploadEnqueueCode::QueueFullByCount)
                    ++m_renderStats.queueFullByCount;
                else
                    ++m_renderStats.queueFullByBytes;
                ++m_renderStats.retryAttempts;
                break;
            }

            ++m_renderStats.gatewayRejections;
            const RenderResourceStatus status =
                m_gateway->QueryResourceStatus(pending.handle);
            bool waitForTerminal = IsTerminalStatus(status) ||
                                   status.state ==
                                       RenderResourcePublicState::Evicting;
            if (!waitForTerminal)
            {
                const RenderReleaseResult release =
                    m_gateway->RequestRelease(pending.handle);
                waitForTerminal =
                    release.code == RenderReleaseCode::Accepted ||
                    release.code == RenderReleaseCode::AlreadyPending;
                if (release.code == RenderReleaseCode::Accepted)
                    ++m_renderStats.releasesAccepted;
            }

            if (waitForTerminal)
            {
                m_retainedRequests.emplace(
                    pending.handle,
                    RetainedRequest{pending.assetId,
                                    pending.handle,
                                    std::move(pending.request)});
            }
            else
            {
                m_localTerminalRequests.push_back(std::move(pending.request));
            }
            const auto tracked = m_trackedResources.find(pending.assetId);
            if (tracked != m_trackedResources.end() &&
                tracked->second.handle == pending.handle)
            {
                m_trackedResources.erase(tracked);
            }
            m_pendingUploads.erase(m_pendingUploads.begin() + index);
        }
    }

    bool ResourceSubsystem::TryBuildPendingResource(PendingResource& pending)
    {
        if (m_gateway == nullptr || !pending.resource)
            return pending.resource == nullptr;

        RenderResourceReserveResult reserve;
        const auto tracked = m_trackedResources.find(pending.assetId);
        if (tracked == m_trackedResources.end())
        {
            reserve = m_gateway->ReserveResource(pending.assetId,
                                                 pending.kind);
            if (reserve.code == RenderResourceReserveCode::CapacityExceeded)
            {
                ++m_renderStats.retryAttempts;
                return false;
            }
            if (reserve.code != RenderResourceReserveCode::Reserved &&
                reserve.code != RenderResourceReserveCode::Existing)
            {
                ++m_renderStats.gatewayRejections;
                return true;
            }
            if (reserve.code == RenderResourceReserveCode::Reserved)
                ++m_renderStats.reservations;
            else
                ++m_renderStats.existingReservations;
            m_trackedResources[pending.assetId] =
                TrackedResource{pending.kind, reserve.handle};
        }
        else
        {
            if (tracked->second.kind != pending.kind)
            {
                ++m_renderStats.gatewayRejections;
                return true;
            }
            reserve.handle = tracked->second.handle;
            reserve.status = m_gateway->QueryResourceStatus(reserve.handle);
            reserve.code = RenderResourceReserveCode::Existing;
        }

        const RenderResourceStatus status =
            m_gateway->QueryResourceStatus(reserve.handle);
        if (status.code != RenderResourceStatusCode::Current)
        {
            m_trackedResources.erase(pending.assetId);
            ++m_renderStats.gatewayRejections;
            return false;
        }
        if (status.state != RenderResourcePublicState::Reserved)
            return true;

        const auto queued = std::find_if(
            m_pendingUploads.begin(),
            m_pendingUploads.end(),
            [handle = reserve.handle](const PendingUpload& upload)
            {
                return upload.handle == handle;
            });
        if (queued != m_pendingUploads.end() ||
            m_retainedRequests.find(reserve.handle) !=
                m_retainedRequests.end())
        {
            return true;
        }

        const RenderUploadRequestBuildResult build =
            RenderUploadRequestBuilder::Build(
                *pending.resource,
                reserve.handle,
                m_nextRequestSequence++,
                [this](AssetId assetId, RenderResourceKind kind)
                {
                    return ResolveDependency(assetId, kind);
                },
                pending.priority,
                pending.sourceRevision);
        if (build.code ==
            RenderUploadRequestBuildCode::DependencyUnavailable)
        {
            ++m_renderStats.retryAttempts;
            return false;
        }
        if (build.code != RenderUploadRequestBuildCode::Built ||
            !build.request)
        {
            ++m_renderStats.buildFailures;
            const RenderReleaseResult release =
                m_gateway->RequestRelease(reserve.handle);
            if (release.code == RenderReleaseCode::Accepted)
                ++m_renderStats.releasesAccepted;
            m_trackedResources.erase(pending.assetId);
            return true;
        }

        m_pendingUploads.push_back(PendingUpload{
            pending.assetId,
            pending.kind,
            reserve.handle,
            build.request,
            pending.priority,
            pending.fifoOrder});
        return true;
    }

    RenderResourceHandle ResourceSubsystem::ResolveDependency(
        AssetId assetId,
        RenderResourceKind kind) const
    {
        const auto tracked = m_trackedResources.find(assetId);
        if (tracked == m_trackedResources.end() ||
            tracked->second.kind != kind)
        {
            return {};
        }
        const RenderResourceStatus status =
            m_gateway->QueryResourceStatus(tracked->second.handle);
        // Composite uploads are submitted only after their dependencies are
        // usable by Render. Enqueuing a material beside its texture uploads
        // would otherwise turn ordinary asynchronous ordering into a terminal
        // DependencyUnavailable failure on the Render Thread.
        return status.code == RenderResourceStatusCode::Current &&
                       status.state == RenderResourcePublicState::GPUReady
                   ? tracked->second.handle
                   : RenderResourceHandle{};
    }

    void ResourceSubsystem::ReleaseAsset(AssetId assetId)
    {
        RemovePendingAsset(assetId);
        const auto tracked = m_trackedResources.find(assetId);
        if (tracked == m_trackedResources.end())
            return;
        if (m_gateway == nullptr)
        {
            m_trackedResources.erase(tracked);
            return;
        }

        const RenderReleaseResult release =
            m_gateway->RequestRelease(tracked->second.handle);
        if (release.code == RenderReleaseCode::Accepted)
        {
            ++m_renderStats.releasesAccepted;
            m_trackedResources.erase(tracked);
        }
        else if (release.code == RenderReleaseCode::AlreadyPending ||
                 release.code == RenderReleaseCode::StaleGeneration)
        {
            m_trackedResources.erase(tracked);
        }
        else
        {
            ++m_renderStats.gatewayRejections;
        }
    }

    void ResourceSubsystem::RemovePendingAsset(AssetId assetId)
    {
        std::erase_if(m_pendingResources,
                      [assetId](const PendingResource& pending)
                      {
                          return pending.assetId == assetId;
                      });
        auto upload = m_pendingUploads.begin();
        while (upload != m_pendingUploads.end())
        {
            if (upload->assetId != assetId)
            {
                ++upload;
                continue;
            }
            if (upload->request)
            {
                m_localTerminalRequests.push_back(
                    std::move(upload->request));
            }
            upload = m_pendingUploads.erase(upload);
        }
    }

    RenderResourceKind ResourceSubsystem::ToRenderResourceKind(
        ResourceType type)
    {
        switch (type)
        {
            case ResourceType::Mesh: return RenderResourceKind::Mesh;
            case ResourceType::Texture: return RenderResourceKind::Texture;
            case ResourceType::Material: return RenderResourceKind::Material;
            default: return RenderResourceKind::Invalid;
        }
    }

    RenderUploadPriority ResourceSubsystem::GetUploadPriority(
        ResourceType type)
    {
        return type == ResourceType::Texture ? RenderUploadPriority::High
                                             : RenderUploadPriority::Normal;
    }
} // namespace RVX::Resource
