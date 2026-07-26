#include "Common/RenderRuntimeTestHarness.h"

#include "Render/Resources/RenderResourceTypes.h"
#include "Resource/RenderUploadRequestBuilder.h"
#include "Resources/RenderResourceRegistry.h"
#include "Resources/RenderRetirementQueue.h"
#include "Resources/RenderSubmissionTracker.h"
#include "Resources/RenderUploadProcessor.h"
#include "RHI/RHIDevice.h"
#include "Runtime/RenderResourceGateway.h"

namespace RVX
{
namespace
{
    RenderResourceKind ToRenderResourceKind(Resource::ResourceType type)
    {
        switch (type)
        {
            case Resource::ResourceType::Mesh:
                return RenderResourceKind::Mesh;
            case Resource::ResourceType::Texture:
                return RenderResourceKind::Texture;
            case Resource::ResourceType::Material:
                return RenderResourceKind::Material;
            default:
                return RenderResourceKind::Invalid;
        }
    }
} // namespace

RenderRuntimeTestHarness::RenderRuntimeTestHarness() = default;

RenderRuntimeTestHarness::~RenderRuntimeTestHarness()
{
    Shutdown();
}

bool RenderRuntimeTestHarness::Initialize(IRHIDevice* device)
{
    Shutdown();
    if (device == nullptr)
        return false;

    RenderTransportConfig config;
    config.statusSlotCapacity = 4096;
    m_gateway = std::make_unique<RenderResourceGateway>(config);
    m_tracker = std::make_unique<RenderSubmissionTracker>();
    m_retirement = std::make_unique<RenderRetirementQueue>();
    m_registry = std::make_unique<RenderResourceRegistry>();
    m_processor = std::make_unique<RenderUploadProcessor>();
    m_device = device;

    if (!m_tracker->Initialize(device) ||
        !m_retirement->Initialize(m_tracker.get()) ||
        !m_registry->Initialize(&m_gateway->GetStatusTable(),
                                m_retirement.get()) ||
        !m_processor->Initialize(device,
                                 &m_gateway->GetStatusTable(),
                                 m_registry.get(),
                                 m_tracker.get()))
    {
        Shutdown();
        return false;
    }
    return true;
}

void RenderRuntimeTestHarness::Shutdown()
{
    if (m_device != nullptr)
    {
        m_device->WaitIdle();
    }
    if (m_processor)
    {
        static_cast<void>(m_processor->PollCompletion());
        m_processor->Shutdown();
    }
    if (m_retirement)
    {
        static_cast<void>(m_retirement->Poll());
    }
    if (m_registry)
    {
        m_registry->Shutdown();
    }
    if (m_retirement)
    {
        static_cast<void>(m_retirement->ForceDeviceLostTeardown());
    }
    if (m_tracker)
    {
        m_tracker->Shutdown();
    }

    m_handles.clear();
    m_processor.reset();
    m_registry.reset();
    m_retirement.reset();
    m_tracker.reset();
    m_gateway.reset();
    m_device = nullptr;
    m_nextSequence = 1;
}

RenderResourceHandle RenderRuntimeTestHarness::Reserve(
    Resource::IResource* resource)
{
    if (m_gateway == nullptr || resource == nullptr ||
        resource->GetId() == Resource::InvalidResourceId)
    {
        return {};
    }
    const RenderResourceKind kind = ToRenderResourceKind(resource->GetType());
    if (kind == RenderResourceKind::Invalid)
        return {};
    const RenderResourceReserveResult reserve = m_gateway->ReserveResource(
        AssetId{resource->GetId()}, kind);
    if (reserve.code != RenderResourceReserveCode::Reserved &&
        reserve.code != RenderResourceReserveCode::Existing)
    {
        return {};
    }
    m_handles.insert_or_assign(resource->GetId(), reserve.handle);
    return reserve.handle;
}

bool RenderRuntimeTestHarness::UploadImmediate(Resource::IResource* resource)
{
    if (m_device == nullptr || m_gateway == nullptr || resource == nullptr ||
        resource->GetId() == Resource::InvalidResourceId)
    {
        return false;
    }

    const RenderResourceHandle handle = Reserve(resource);
    if (!handle.IsValid())
        return false;
    const RenderResourceStatus reservedStatus =
        m_gateway->QueryResourceStatus(handle);
    if (reservedStatus.state == RenderResourcePublicState::GPUReady)
        return true;
    if (reservedStatus.state != RenderResourcePublicState::Reserved)
        return false;

    const RenderUploadRequestBuildResult built =
        RenderUploadRequestBuilder::Build(
            *resource,
            handle,
            m_nextSequence++,
            [this](AssetId asset, RenderResourceKind dependencyKind)
            {
                const RenderResourceHandle handle = GetHandle(asset.value);
                if (!handle.IsValid() ||
                    GetResourceState(asset.value) !=
                        RenderResourcePublicState::GPUReady)
                {
                    return RenderResourceHandle{};
                }
                const RenderResourceStatus status =
                    m_gateway->QueryResourceStatus(handle);
                return status.code == RenderResourceStatusCode::Current &&
                               dependencyKind != RenderResourceKind::Invalid
                           ? handle
                           : RenderResourceHandle{};
            });
    if (built.code != RenderUploadRequestBuildCode::Built ||
        built.request == nullptr)
    {
        return false;
    }
    if (m_gateway->TryEnqueueUpload(built.request).code !=
        RenderUploadEnqueueCode::Accepted)
    {
        return false;
    }

    ResourceUploadRequestRef request = m_gateway->TryDequeueUpload();
    if (m_processor->ProcessUpload(std::move(request)) !=
        RenderUploadProcessCode::Accepted)
    {
        return false;
    }
    m_device->WaitIdle();
    const GPUCompletionStatus completion = m_processor->PollCompletion();
    return completion != GPUCompletionStatus::Pending &&
           IsGPUReady(resource->GetId());
}

RenderResourceHandle RenderRuntimeTestHarness::ResolveOrUpload(
    Resource::IResource* resource)
{
    if (resource == nullptr)
        return {};
    const RenderResourceHandle existing = GetHandle(resource->GetId());
    if (existing.IsValid() && IsGPUReady(resource->GetId()))
        return existing;
    return UploadImmediate(resource) ? GetHandle(resource->GetId())
                                     : RenderResourceHandle{};
}

RenderResourceHandle RenderRuntimeTestHarness::GetHandle(
    uint64 resourceId) const
{
    const auto it = m_handles.find(resourceId);
    return it == m_handles.end() ? RenderResourceHandle{} : it->second;
}

RenderResourcePublicState RenderRuntimeTestHarness::GetResourceState(
    uint64 resourceId) const
{
    if (m_gateway == nullptr)
        return RenderResourcePublicState::Released;
    const RenderResourceHandle handle = GetHandle(resourceId);
    if (!handle.IsValid())
        return RenderResourcePublicState::Released;
    const RenderResourceStatus status = m_gateway->QueryResourceStatus(handle);
    return status.code == RenderResourceStatusCode::Current
               ? status.state
               : RenderResourcePublicState::Released;
}

bool RenderRuntimeTestHarness::IsGPUReady(uint64 resourceId) const
{
    return GetResourceState(resourceId) == RenderResourcePublicState::GPUReady;
}

RHITexture* RenderRuntimeTestHarness::GetTexture(uint64 resourceId) const
{
    return m_registry == nullptr
               ? nullptr
               : m_registry->ResolveTextureObject(GetHandle(resourceId));
}

MeshGPUBuffers RenderRuntimeTestHarness::GetMeshBuffers(
    uint64 resourceId) const
{
    return m_registry == nullptr
               ? MeshGPUBuffers{}
               : m_registry->ResolveMeshBuffers(GetHandle(resourceId));
}

RenderResourceRegistry& RenderRuntimeTestHarness::GetRegistry()
{
    return *m_registry;
}

const RenderResourceRegistry& RenderRuntimeTestHarness::GetRegistry() const
{
    return *m_registry;
}
} // namespace RVX
