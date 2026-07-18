#include "Render/GPUResourceManager.h"

#include "Resources/RenderResourceRegistry.h"

namespace RVX
{
    GPUResourceManager::GPUResourceManager()
        : m_registry(std::make_unique<RenderResourceRegistry>())
    {
    }

    GPUResourceManager::~GPUResourceManager()
    {
        Shutdown();
    }

    void GPUResourceManager::Initialize(IRHIDevice* device)
    {
        if (!m_registry)
        {
            m_registry = std::make_unique<RenderResourceRegistry>();
        }
        m_registry->InitializeLegacy(device);
    }

    void GPUResourceManager::Shutdown()
    {
        if (m_registry)
        {
            m_registry->ShutdownLegacy();
        }
    }

    bool GPUResourceManager::IsInitialized() const
    {
        return m_registry && m_registry->IsLegacyInitialized();
    }

    void GPUResourceManager::RequestUpload(
        IRenderMeshUploadSource* mesh,
        UploadPriority priority)
    {
        if (m_registry)
        {
            m_registry->RequestLegacyUpload(mesh, priority);
        }
    }

    void GPUResourceManager::RequestUpload(
        IRenderTextureUploadSource* texture,
        UploadPriority priority)
    {
        if (m_registry)
        {
            m_registry->RequestLegacyUpload(texture, priority);
        }
    }

    void GPUResourceManager::UploadImmediate(IRenderMeshUploadSource* mesh)
    {
        if (m_registry)
        {
            m_registry->UploadLegacyImmediate(mesh);
        }
    }

    void GPUResourceManager::UploadImmediate(
        IRenderTextureUploadSource* texture)
    {
        if (m_registry)
        {
            m_registry->UploadLegacyImmediate(texture);
        }
    }

    void GPUResourceManager::SetTextureInvalidatedCallback(
        TextureInvalidatedCallback callback)
    {
        if (m_registry)
        {
            m_registry->SetLegacyTextureInvalidatedCallback(
                std::move(callback));
        }
    }

    MeshGPUBuffers GPUResourceManager::GetMeshBuffers(uint64 meshId) const
    {
        return m_registry ? m_registry->GetLegacyMeshBuffers(meshId)
                          : MeshGPUBuffers{};
    }

    RHITexture* GPUResourceManager::GetTexture(uint64 textureId) const
    {
        return m_registry ? m_registry->GetLegacyTexture(textureId) : nullptr;
    }

    RHITexture* GPUResourceManager::GetTexture(
        IRenderTextureUploadSource* texture) const
    {
        return m_registry ? m_registry->GetLegacyTexture(texture) : nullptr;
    }

    bool GPUResourceManager::TransitionTexture(
        uint64 textureId,
        RHICommandContext& context,
        RHIResourceState desiredState)
    {
        return m_registry &&
               m_registry->TransitionLegacyTexture(textureId,
                                                   context,
                                                   desiredState);
    }

    bool GPUResourceManager::IsResident(uint64 id) const
    {
        return m_registry && m_registry->IsLegacyResident(id);
    }

    bool GPUResourceManager::IsResident(
        IRenderTextureUploadSource* texture) const
    {
        return m_registry && m_registry->IsLegacyResident(texture);
    }

    GPUResourceState GPUResourceManager::GetResourceState(uint64 id) const
    {
        return m_registry ? m_registry->GetLegacyResourceState(id)
                          : GPUResourceState::Unloaded;
    }

    bool GPUResourceManager::IsGPUReady(
        IRenderTextureUploadSource* texture) const
    {
        return m_registry && m_registry->IsLegacyGPUReady(texture);
    }

    void GPUResourceManager::ProcessPendingUploads(float timeBudgetMs)
    {
        if (m_registry)
        {
            m_registry->ProcessLegacyPendingUploads(timeBudgetMs);
        }
    }

    void GPUResourceManager::MarkUsed(uint64 id)
    {
        if (m_registry)
        {
            m_registry->MarkLegacyUsed(id);
        }
    }

    void GPUResourceManager::MarkUsed(IRenderTextureUploadSource* texture)
    {
        if (m_registry)
        {
            m_registry->MarkLegacyUsed(texture);
        }
    }

    void GPUResourceManager::EvictUnused(
        uint64 currentFrame,
        uint64 frameThreshold)
    {
        if (m_registry)
        {
            m_registry->EvictLegacyUnused(currentFrame, frameThreshold);
        }
    }

    void GPUResourceManager::SetMemoryBudget(size_t bytes)
    {
        if (m_registry)
        {
            m_registry->SetLegacyMemoryBudget(bytes);
        }
    }

    size_t GPUResourceManager::GetUsedMemory() const
    {
        return m_registry ? m_registry->GetLegacyUsedMemory() : 0;
    }

    size_t GPUResourceManager::GetMemoryBudget() const
    {
        return m_registry ? m_registry->GetLegacyMemoryBudget() : 0;
    }

    bool GPUResourceManager::IsOverBudget() const
    {
        return GetUsedMemory() > GetMemoryBudget();
    }

    GPUResourceManager::Stats GPUResourceManager::GetStats() const
    {
        if (!m_registry)
        {
            return {};
        }

        const LegacyGPUResourceStats stats = m_registry->GetLegacyStats();
        return {
            stats.residentMeshCount,
            stats.residentTextureCount,
            stats.pendingUploadCount,
            stats.queuedUploadCount,
            stats.uploadingCount,
            stats.failedUploadCount,
            stats.usedMemory,
            stats.memoryBudget,
        };
    }
} // namespace RVX
