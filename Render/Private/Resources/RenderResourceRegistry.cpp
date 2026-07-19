#include "Resources/RenderResourceRegistry.h"

#include "Core/Assert.h"
#include "Render/GPUResourceManager.h"
#include "Resources/RenderRetirementQueue.h"
#include "Runtime/RenderResourceStatusTable.h"

#include <algorithm>

namespace RVX
{
    RenderResourceRegistry::~RenderResourceRegistry()
    {
        RVX_ASSERT_MSG(m_entries.empty(),
                       "Exact registry requires explicit Render shutdown");
        RVX_ASSERT_MSG(m_legacy == nullptr,
                       "Legacy registry requires explicit facade shutdown");
    }

    bool RenderResourceRegistry::Initialize(
        RenderResourceStatusTable* statusTable,
        RenderRetirementQueue* retirementQueue)
    {
        Shutdown();
        if (statusTable == nullptr || retirementQueue == nullptr)
        {
            return false;
        }
        m_statusTable = statusTable;
        m_retirementQueue = retirementQueue;
        return true;
    }

    void RenderResourceRegistry::Shutdown()
    {
        m_entries.clear();
        m_retirementQueue = nullptr;
        m_statusTable = nullptr;
    }

    bool RenderResourceRegistry::BeginPending(
        RenderResourceHandle handle,
        RenderResourceKind kind,
        const std::vector<RenderResourceHandle>& dependencies)
    {
        if (m_statusTable == nullptr || m_retirementQueue == nullptr ||
            !handle.IsValid() || kind == RenderResourceKind::Invalid)
        {
            return false;
        }

        const RenderResourceStatus status = m_statusTable->Query(handle);
        if (status.code != RenderResourceStatusCode::Current ||
            status.state != RenderResourcePublicState::Uploading)
        {
            return false;
        }

        auto existing = m_entries.find(handle.slot);
        if (existing != m_entries.end())
        {
            if (existing->second.generation != handle.generation ||
                existing->second.pending.has_value() ||
                existing->second.committed.has_value())
            {
                return false;
            }
        }

        Entry entry;
        entry.generation = handle.generation;
        entry.kind = kind;
        entry.dependencies = dependencies;
        switch (kind)
        {
            case RenderResourceKind::Mesh:
                entry.pending.emplace(RenderMeshResourceData{});
                break;
            case RenderResourceKind::Texture:
                entry.pending.emplace(RenderTextureResourceData{});
                break;
            case RenderResourceKind::Material:
                entry.pending.emplace(RenderMaterialResourceData{});
                break;
            case RenderResourceKind::Invalid:
            default:
                return false;
        }
        m_entries.insert_or_assign(handle.slot, std::move(entry));
        return true;
    }

    bool RenderResourceRegistry::AddPendingMeshBuffer(
        RenderResourceHandle handle,
        RenderMeshBufferSemantic semantic,
        RHIBufferRef buffer,
        uint64 estimatedBytes)
    {
        Entry* entry = FindExact(handle);
        if (entry == nullptr || !entry->pending || !buffer)
        {
            return false;
        }
        auto* mesh = std::get_if<RenderMeshResourceData>(&*entry->pending);
        if (mesh == nullptr)
        {
            return false;
        }
        for (const RenderOwnedBuffer& owned : mesh->buffers)
        {
            if (owned.semantic == semantic)
            {
                return false;
            }
        }
        mesh->buffers.push_back(
            RenderOwnedBuffer{semantic, std::move(buffer), estimatedBytes});
        return true;
    }

    bool RenderResourceRegistry::SetPendingMeshMetadata(
        RenderResourceHandle handle,
        const MeshUploadCreateInfo& createInfo,
        const std::vector<MeshUploadSubmesh>& submeshes)
    {
        Entry* entry = FindExact(handle);
        if (entry == nullptr || !entry->pending)
        {
            return false;
        }
        auto* mesh = std::get_if<RenderMeshResourceData>(&*entry->pending);
        if (mesh == nullptr)
        {
            return false;
        }
        mesh->createInfo = createInfo;
        mesh->submeshes = submeshes;
        return true;
    }

    bool RenderResourceRegistry::SetPendingTexture(
        RenderResourceHandle handle,
        RHITextureRef texture,
        uint64 estimatedBytes)
    {
        Entry* entry = FindExact(handle);
        if (entry == nullptr || !entry->pending || !texture)
        {
            return false;
        }
        auto* data = std::get_if<RenderTextureResourceData>(&*entry->pending);
        if (data == nullptr || data->texture)
        {
            return false;
        }
        data->texture = std::move(texture);
        data->estimatedBytes = estimatedBytes;
        return true;
    }

    bool RenderResourceRegistry::SetPendingMaterialConstants(
        RenderResourceHandle handle,
        RHIBufferRef constants,
        uint64 estimatedBytes)
    {
        Entry* entry = FindExact(handle);
        if (entry == nullptr || !entry->pending || !constants)
        {
            return false;
        }
        auto* data = std::get_if<RenderMaterialResourceData>(&*entry->pending);
        if (data == nullptr || data->constants)
        {
            return false;
        }
        data->constants = std::move(constants);
        data->constantBytes = estimatedBytes;
        return true;
    }

    bool RenderResourceRegistry::SetPendingMaterialMetadata(
        RenderResourceHandle handle,
        const MaterialUploadPayload& payload)
    {
        Entry* entry = FindExact(handle);
        if (entry == nullptr || !entry->pending)
        {
            return false;
        }
        auto* data = std::get_if<RenderMaterialResourceData>(&*entry->pending);
        if (data == nullptr || data->metadataValid)
        {
            return false;
        }
        data->sourceData = payload.sourceData;
        data->textureBindings = payload.textureBindings;
        data->metadataValid = true;
        return true;
    }

    bool RenderResourceRegistry::AddPendingMaterialSampler(
        RenderResourceHandle handle,
        RHISamplerRef sampler)
    {
        Entry* entry = FindExact(handle);
        if (entry == nullptr || !entry->pending || !sampler)
        {
            return false;
        }
        auto* data = std::get_if<RenderMaterialResourceData>(&*entry->pending);
        if (data == nullptr)
        {
            return false;
        }
        data->samplers.push_back(std::move(sampler));
        return true;
    }

    bool RenderResourceRegistry::SetPendingCompletion(
        RenderResourceHandle handle,
        const GPUCompletionToken& completion)
    {
        Entry* entry = FindExact(handle);
        if (entry == nullptr || !entry->pending)
        {
            return false;
        }
        return MergeGPUCompletionToken(entry->lastUse, completion);
    }

    bool RenderResourceRegistry::Commit(RenderResourceHandle handle)
    {
        Entry* entry = FindExact(handle);
        if (entry == nullptr || !entry->pending || entry->committed)
        {
            return false;
        }
        entry->committed = std::move(entry->pending);
        entry->pending.reset();
        return true;
    }

    bool RenderResourceRegistry::RetirePending(RenderResourceHandle handle)
    {
        Entry* entry = FindExact(handle);
        if (entry == nullptr || !entry->pending)
        {
            return false;
        }
        if (!RetireData(*entry->pending, entry->lastUse))
        {
            return false;
        }
        entry->pending.reset();
        if (!entry->committed)
        {
            m_entries.erase(handle.slot);
        }
        return true;
    }

    bool RenderResourceRegistry::Release(RenderResourceHandle handle)
    {
        Entry* entry = FindExact(handle);
        if (entry == nullptr)
        {
            return true;
        }
        if (entry->pending && !RetireData(*entry->pending, entry->lastUse))
        {
            return false;
        }
        if (entry->committed && !RetireData(*entry->committed, entry->lastUse))
        {
            return false;
        }
        m_entries.erase(handle.slot);
        return true;
    }

    bool RenderResourceRegistry::MergeLastUse(
        RenderResourceHandle handle,
        const GPUCompletionToken& completion)
    {
        Entry* entry = FindExact(handle);
        return entry != nullptr &&
               MergeGPUCompletionToken(entry->lastUse, completion);
    }

    bool RenderResourceRegistry::MergeLastUseClosure(
        std::span<const RenderResourceHandle> roots,
        const GPUCompletionToken& completion)
    {
        std::vector<RenderResourceHandle> closure;
        const auto collect = [this, &closure](auto&& self,
                                               RenderResourceHandle handle)
            -> bool
        {
            if (!handle.IsValid() ||
                std::find(closure.begin(), closure.end(), handle) !=
                    closure.end())
            {
                return true;
            }
            const Entry* entry = FindExact(handle);
            if (entry == nullptr)
            {
                return false;
            }
            closure.push_back(handle);
            for (RenderResourceHandle dependency : entry->dependencies)
            {
                if (!self(self, dependency))
                {
                    return false;
                }
            }
            return true;
        };
        for (RenderResourceHandle root : roots)
        {
            if (!collect(collect, root))
            {
                return false;
            }
        }

        std::vector<GPUCompletionToken> merged;
        merged.reserve(closure.size());
        for (RenderResourceHandle handle : closure)
        {
            const Entry* entry = FindExact(handle);
            GPUCompletionToken candidate = entry->lastUse;
            if (!MergeGPUCompletionToken(candidate, completion))
            {
                return false;
            }
            merged.push_back(candidate);
        }
        for (size_t index = 0; index < closure.size(); ++index)
        {
            FindExact(closure[index])->lastUse = merged[index];
        }
        return true;
    }

    const RenderMeshResourceData* RenderResourceRegistry::ResolveMesh(
        RenderResourceHandle handle) const
    {
        const Entry* entry = FindExact(handle);
        if (entry == nullptr || !entry->committed ||
            !IsReady(handle, RenderResourceKind::Mesh))
        {
            return nullptr;
        }
        return std::get_if<RenderMeshResourceData>(&*entry->committed);
    }

    const RenderTextureResourceData* RenderResourceRegistry::ResolveTexture(
        RenderResourceHandle handle) const
    {
        const Entry* entry = FindExact(handle);
        if (entry == nullptr || !entry->committed ||
            !IsReady(handle, RenderResourceKind::Texture))
        {
            return nullptr;
        }
        return std::get_if<RenderTextureResourceData>(&*entry->committed);
    }

    const RenderMaterialResourceData* RenderResourceRegistry::ResolveMaterial(
        RenderResourceHandle handle) const
    {
        const Entry* entry = FindExact(handle);
        if (entry == nullptr || !entry->committed ||
            !IsReady(handle, RenderResourceKind::Material))
        {
            return nullptr;
        }
        return std::get_if<RenderMaterialResourceData>(&*entry->committed);
    }

    MeshGPUBuffers RenderResourceRegistry::ResolveMeshBuffers(
        RenderResourceHandle handle) const
    {
        MeshGPUBuffers buffers;
        const RenderMeshResourceData* mesh = ResolveMesh(handle);
        if (mesh == nullptr)
        {
            return buffers;
        }
        for (const RenderOwnedBuffer& owned : mesh->buffers)
        {
            RHIBuffer* buffer = owned.buffer.Get();
            switch (owned.semantic)
            {
                case RenderMeshBufferSemantic::Position:
                    buffers.positionBuffer = buffer;
                    break;
                case RenderMeshBufferSemantic::Normal:
                    buffers.normalBuffer = buffer;
                    buffers.hasNormals = true;
                    break;
                case RenderMeshBufferSemantic::UV:
                    buffers.uvBuffer = buffer;
                    buffers.hasUVs = true;
                    break;
                case RenderMeshBufferSemantic::Tangent:
                    buffers.tangentBuffer = buffer;
                    buffers.hasTangents = true;
                    break;
                case RenderMeshBufferSemantic::BoneIndices:
                    buffers.boneIndicesBuffer = buffer;
                    buffers.hasBoneIndices = true;
                    break;
                case RenderMeshBufferSemantic::BoneWeights:
                    buffers.boneWeightsBuffer = buffer;
                    buffers.hasBoneWeights = true;
                    break;
                case RenderMeshBufferSemantic::Index:
                    buffers.indexBuffer = buffer;
                    break;
            }
        }
        buffers.submeshes.reserve(mesh->submeshes.size());
        for (const MeshUploadSubmesh& submesh : mesh->submeshes)
        {
            buffers.submeshes.push_back(SubmeshGPUInfo{
                submesh.indexOffset,
                submesh.indexCount,
                submesh.baseVertex});
        }
        if (buffers.submeshes.empty() && mesh->createInfo.indexCount != 0)
        {
            buffers.submeshes.push_back(SubmeshGPUInfo{
                0,
                static_cast<uint32>(mesh->createInfo.indexCount),
                0});
        }
        buffers.isResident = buffers.positionBuffer != nullptr &&
                             buffers.indexBuffer != nullptr;
        return buffers;
    }

    RHITexture* RenderResourceRegistry::ResolveTextureObject(
        RenderResourceHandle handle) const
    {
        const RenderTextureResourceData* texture = ResolveTexture(handle);
        return texture == nullptr ? nullptr : texture->texture.Get();
    }

    const std::vector<RenderResourceHandle>*
        RenderResourceRegistry::GetDependencies(
            RenderResourceHandle handle) const
    {
        const Entry* entry = FindExact(handle);
        return entry == nullptr ? nullptr : &entry->dependencies;
    }

    GPUCompletionToken RenderResourceRegistry::GetLastUse(
        RenderResourceHandle handle) const
    {
        const Entry* entry = FindExact(handle);
        return entry == nullptr ? GPUCompletionToken{} : entry->lastUse;
    }

    bool RenderResourceRegistry::HasExactEntry(
        RenderResourceHandle handle) const
    {
        return FindExact(handle) != nullptr;
    }

    bool RenderResourceRegistry::IsGPUReadyExact(
        RenderResourceHandle handle) const
    {
        const Entry* entry = FindExact(handle);
        if (entry == nullptr || !entry->committed || m_statusTable == nullptr)
        {
            return false;
        }
        const RenderResourceStatus status = m_statusTable->Query(handle);
        return status.code == RenderResourceStatusCode::Current &&
               status.state == RenderResourcePublicState::GPUReady;
    }

    bool RenderResourceRegistry::HasPending(
        RenderResourceHandle handle) const
    {
        const Entry* entry = FindExact(handle);
        return entry != nullptr && entry->pending.has_value();
    }

    uint32 RenderResourceRegistry::GetEntryCount() const
    {
        return static_cast<uint32>(m_entries.size());
    }

    RenderResourceRegistry::Entry* RenderResourceRegistry::FindExact(
        RenderResourceHandle handle)
    {
        auto it = m_entries.find(handle.slot);
        return it != m_entries.end() &&
                       it->second.generation == handle.generation
                   ? &it->second
                   : nullptr;
    }

    const RenderResourceRegistry::Entry* RenderResourceRegistry::FindExact(
        RenderResourceHandle handle) const
    {
        const auto it = m_entries.find(handle.slot);
        return it != m_entries.end() &&
                       it->second.generation == handle.generation
                   ? &it->second
                   : nullptr;
    }

    bool RenderResourceRegistry::RetireData(
        RenderResourceGPUData& data,
        const GPUCompletionToken& completion)
    {
        auto retire = [this, &completion](auto& typedRef,
                                          uint64 estimatedBytes)
        {
            if (!typedRef)
            {
                return true;
            }
            Ref<RefCounted> object(typedRef);
            if (!m_retirementQueue->Enqueue(
                    RenderRetirementEntry{completion,
                                          std::move(object),
                                          estimatedBytes}))
            {
                return false;
            }
            typedRef.Reset();
            return true;
        };

        if (auto* mesh = std::get_if<RenderMeshResourceData>(&data))
        {
            for (RenderOwnedBuffer& owned : mesh->buffers)
            {
                if (!retire(owned.buffer, owned.estimatedBytes))
                {
                    return false;
                }
            }
            return true;
        }
        if (auto* texture = std::get_if<RenderTextureResourceData>(&data))
        {
            return retire(texture->texture, texture->estimatedBytes);
        }
        auto* material = std::get_if<RenderMaterialResourceData>(&data);
        if (material == nullptr ||
            !retire(material->constants, material->constantBytes))
        {
            return false;
        }
        for (RHISamplerRef& sampler : material->samplers)
        {
            if (!retire(sampler, 0))
            {
                return false;
            }
        }
        return true;
    }

    bool RenderResourceRegistry::IsReady(
        RenderResourceHandle handle,
        RenderResourceKind kind) const
    {
        const Entry* entry = FindExact(handle);
        if (entry == nullptr || entry->kind != kind || m_statusTable == nullptr)
        {
            return false;
        }
        const RenderResourceStatus status = m_statusTable->Query(handle);
        return status.code == RenderResourceStatusCode::Current &&
               status.state == RenderResourcePublicState::GPUReady;
    }
} // namespace RVX
