#include "Resources/RenderResourceRegistry.h"

#include "Core/Assert.h"
#include "Resources/RenderRetirementQueue.h"
#include "Runtime/RenderResourceStatusTable.h"
#include "RHI/RHICommandContext.h"

#include <algorithm>

namespace RVX
{
    RenderResourceRegistry::~RenderResourceRegistry()
    {
        RVX_ASSERT_MSG(m_entries.empty(),
                       "Exact registry requires explicit Render shutdown");
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
        BumpContentRevision();
        return true;
    }

    void RenderResourceRegistry::Shutdown()
    {
        const bool hadState = !m_entries.empty() || m_statusTable != nullptr ||
                              m_retirementQueue != nullptr;
        m_entries.clear();
        m_retirementQueue = nullptr;
        m_statusTable = nullptr;
        if (hadState)
            BumpContentRevision();
    }

    bool RenderResourceRegistry::BeginPending(
        RenderResourceHandle handle,
        RenderResourceKind kind,
        const std::vector<RenderResourceHandle>& dependencies,
        RenderResourceContentOperation operation,
        uint64 sourceRevision)
    {
        if (m_statusTable == nullptr || m_retirementQueue == nullptr ||
            !handle.IsValid() || kind == RenderResourceKind::Invalid ||
            (operation != RenderResourceContentOperation::Create &&
             operation != RenderResourceContentOperation::Replace) ||
            (operation == RenderResourceContentOperation::Replace &&
             sourceRevision == 0))
        {
            return false;
        }

        const RenderResourceStatus status = m_statusTable->Query(handle);
        const RenderResourcePublicState expectedState =
            operation == RenderResourceContentOperation::Replace
                ? RenderResourcePublicState::Replacing
                : RenderResourcePublicState::Uploading;
        if (status.code != RenderResourceStatusCode::Current ||
            status.state != expectedState)
        {
            return false;
        }

        auto existing = m_entries.find(handle.slot);
        if (operation == RenderResourceContentOperation::Create)
        {
            if (existing != m_entries.end() &&
                (existing->second.generation != handle.generation ||
                 existing->second.pending.has_value() ||
                 existing->second.committed.has_value()))
            {
                return false;
            }
        }
        else if (existing == m_entries.end() ||
                 existing->second.generation != handle.generation ||
                 existing->second.kind != kind ||
                 existing->second.pending.has_value() ||
                 !existing->second.committed.has_value() ||
                 sourceRevision <= existing->second.lastAcceptedSourceRevision)
        {
            return false;
        }

        auto makePendingData = [kind]() -> std::optional<RenderResourceGPUData>
        {
            switch (kind)
            {
                case RenderResourceKind::Mesh:
                    return RenderMeshResourceData{};
                case RenderResourceKind::Texture:
                    return RenderTextureResourceData{};
                case RenderResourceKind::Material:
                    return RenderMaterialResourceData{};
                case RenderResourceKind::Invalid:
                default:
                    return std::nullopt;
            }
        };
        std::optional<RenderResourceGPUData> pending = makePendingData();
        if (!pending)
        {
            return false;
        }

        if (operation == RenderResourceContentOperation::Replace)
        {
            Entry& entry = existing->second;
            entry.pending = std::move(pending);
            entry.pendingOperation = operation;
            entry.pendingSourceRevision = sourceRevision;
            entry.lastAcceptedSourceRevision = sourceRevision;
            entry.pendingDependencies = dependencies;
            return true;
        }

        Entry entry;
        entry.generation = handle.generation;
        entry.kind = kind;
        entry.pendingDependencies = dependencies;
        entry.pending = std::move(pending);
        entry.pendingOperation = operation;
        entry.pendingSourceRevision = sourceRevision;
        entry.lastAcceptedSourceRevision = sourceRevision;
        m_entries.insert_or_assign(handle.slot, std::move(entry));
        return true;
    }

    bool RenderResourceRegistry::AddPendingMeshBuffer(
        RenderResourceHandle handle,
        RenderMeshBufferSemantic semantic,
        RHIBufferRef buffer,
        uint64 estimatedBytes,
        const RHIBufferAccessSnapshot& finalAccess)
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
        mesh->buffers.push_back(RenderOwnedBuffer{
            semantic,
            std::move(buffer),
            estimatedBytes,
            finalAccess});
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
        uint64 estimatedBytes,
        const RHITextureAccessSnapshot& finalAccess)
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
        data->accessSnapshot = finalAccess;
        return true;
    }

    bool RenderResourceRegistry::SetPendingMaterialConstants(
        RenderResourceHandle handle,
        RHIBufferRef constants,
        uint64 estimatedBytes,
        const RHIBufferAccessSnapshot& finalAccess)
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
        data->constantsAccessSnapshot = finalAccess;
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
        if (entry == nullptr || !entry->pending)
        {
            return false;
        }
        if (entry->pendingOperation == RenderResourceContentOperation::Create)
        {
            if (entry->committed)
            {
                return false;
            }
        }
        else if (entry->pendingOperation == RenderResourceContentOperation::Replace)
        {
            if (!entry->committed)
            {
                return false;
            }
            // A replacement becomes visible only after every prior use of the
            // committed content and the replacement upload are covered by the
            // entry token. RetireData transfers old strong references before
            // the new content is published.
            if (!RetireData(*entry->committed, entry->lastUse))
            {
                return false;
            }
        }
        else
        {
            return false;
        }

        entry->committed = std::move(entry->pending);
        entry->pending.reset();
        entry->dependencies = std::move(entry->pendingDependencies);
        entry->pendingDependencies.clear();
        entry->committedSourceRevision = entry->pendingSourceRevision;
        entry->pendingSourceRevision = 0;
        entry->pendingOperation = RenderResourceContentOperation::Create;
        entry->contentRevision = BumpContentRevision();
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
        entry->pendingDependencies.clear();
        entry->pendingSourceRevision = 0;
        entry->pendingOperation = RenderResourceContentOperation::Create;
        const bool hasCommitted = entry->committed.has_value();
        if (!hasCommitted)
        {
            m_entries.erase(handle.slot);
        }
        // A failed replacement preserves the prior committed data and its
        // content revision. Consumers must not rebuild against an update that
        // never became visible.
        if (!hasCommitted)
            BumpContentRevision();
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
        BumpContentRevision();
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
        buffers.hasTangents = buffers.tangentBuffer != nullptr &&
                              mesh->createInfo.hasTangentBasis;
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

    bool RenderResourceRegistry::CommitTextureAccessSnapshot(
        RenderResourceHandle handle,
        const RHITextureAccessSnapshot& accessSnapshot)
    {
        Entry* entry = FindExact(handle);
        if (entry == nullptr || !entry->committed)
        {
            return false;
        }
        auto* texture = std::get_if<RenderTextureResourceData>(
            &*entry->committed);
        if (texture == nullptr || !texture->texture)
        {
            return false;
        }
        texture->accessSnapshot = accessSnapshot;
        return true;
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
               (status.state == RenderResourcePublicState::GPUReady ||
                status.state == RenderResourcePublicState::ReplacementQueued ||
                status.state == RenderResourcePublicState::Replacing);
    }

    RenderResourceStatus RenderResourceRegistry::QueryStatus(
        RenderResourceHandle handle) const noexcept
    {
        return m_statusTable != nullptr
                   ? m_statusTable->Query(handle)
                   : RenderResourceStatus{};
    }

    bool RenderResourceRegistry::HasPending(
        RenderResourceHandle handle) const
    {
        const Entry* entry = FindExact(handle);
        return entry != nullptr && entry->pending.has_value();
    }

    uint64 RenderResourceRegistry::GetCommittedSourceRevision(
        RenderResourceHandle handle) const noexcept
    {
        const Entry* entry = FindExact(handle);
        return entry != nullptr ? entry->committedSourceRevision : 0;
    }

    uint64 RenderResourceRegistry::GetPendingSourceRevision(
        RenderResourceHandle handle) const noexcept
    {
        const Entry* entry = FindExact(handle);
        return entry != nullptr ? entry->pendingSourceRevision : 0;
    }

    uint32 RenderResourceRegistry::GetEntryCount() const
    {
        return static_cast<uint32>(m_entries.size());
    }

    RenderResourceRegistryStats RenderResourceRegistry::GetStats() const
    {
        RenderResourceRegistryStats stats;
        for (const auto& [slot, entry] : m_entries)
        {
            static_cast<void>(slot);
            if (entry.pending)
            {
                ++stats.pendingUploadCount;
            }
            if (!entry.committed)
            {
                continue;
            }
            if (const auto* mesh = std::get_if<RenderMeshResourceData>(
                    &*entry.committed))
            {
                ++stats.residentMeshCount;
                for (const RenderOwnedBuffer& buffer : mesh->buffers)
                {
                    stats.usedMemory += static_cast<size_t>(
                        buffer.estimatedBytes);
                }
            }
            else if (const auto* texture =
                         std::get_if<RenderTextureResourceData>(
                             &*entry.committed))
            {
                ++stats.residentTextureCount;
                stats.usedMemory += static_cast<size_t>(
                    texture->estimatedBytes);
            }
            else if (const auto* material =
                         std::get_if<RenderMaterialResourceData>(
                             &*entry.committed))
            {
                ++stats.residentMaterialCount;
                stats.usedMemory += static_cast<size_t>(
                    material->constantBytes);
            }
        }
        return stats;
    }

    uint64 RenderResourceRegistry::GetContentRevision(
        RenderResourceHandle handle) const noexcept
    {
        const Entry* entry = FindExact(handle);
        return entry != nullptr ? entry->contentRevision : 0;
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
        std::vector<RenderRetirementEntry> retirements;
        auto retain = [&retirements, &completion](const auto& typedRef,
                                                   uint64 estimatedBytes)
        {
            if (!typedRef)
            {
                return true;
            }
            Ref<RefCounted> object(typedRef);
            retirements.push_back(RenderRetirementEntry{
                completion, std::move(object), estimatedBytes});
            return true;
        };

        try
        {
            if (auto* mesh = std::get_if<RenderMeshResourceData>(&data))
            {
                retirements.reserve(mesh->buffers.size());
                for (const RenderOwnedBuffer& owned : mesh->buffers)
                {
                    if (!retain(owned.buffer, owned.estimatedBytes))
                    {
                        return false;
                    }
                }
            }
            else if (auto* texture =
                         std::get_if<RenderTextureResourceData>(&data))
            {
                if (!retain(texture->texture, texture->estimatedBytes))
                {
                    return false;
                }
            }
            else if (auto* material =
                         std::get_if<RenderMaterialResourceData>(&data))
            {
                retirements.reserve(material->samplers.size() + 1U);
                if (!retain(material->constants, material->constantBytes))
                {
                    return false;
                }
                for (const RHISamplerRef& sampler : material->samplers)
                {
                    if (!retain(sampler, 0))
                    {
                        return false;
                    }
                }
            }
            else
            {
                return false;
            }
        }
        catch (...)
        {
            return false;
        }

        if (!retirements.empty() &&
            !m_retirementQueue->EnqueueBatch(std::move(retirements)))
        {
            return false;
        }

        if (auto* mesh = std::get_if<RenderMeshResourceData>(&data))
        {
            for (RenderOwnedBuffer& owned : mesh->buffers)
            {
                owned.buffer.Reset();
            }
        }
        else if (auto* texture =
                     std::get_if<RenderTextureResourceData>(&data))
        {
            texture->texture.Reset();
        }
        else if (auto* material =
                     std::get_if<RenderMaterialResourceData>(&data))
        {
            material->constants.Reset();
            for (RHISamplerRef& sampler : material->samplers)
            {
                sampler.Reset();
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
               entry->committed.has_value() &&
               (status.state == RenderResourcePublicState::GPUReady ||
                status.state == RenderResourcePublicState::ReplacementQueued ||
                status.state == RenderResourcePublicState::Replacing);
    }

    uint64 RenderResourceRegistry::BumpContentRevision() noexcept
    {
        ++m_contentRevision;
        if (m_contentRevision == 0)
            ++m_contentRevision;
        return m_contentRevision;
    }
} // namespace RVX
