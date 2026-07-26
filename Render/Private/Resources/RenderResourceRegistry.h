#pragma once

/** @file RenderResourceRegistry.h @brief Exact-generation Render-owned RHI resources */

#include "RenderContracts/IRenderResourceGateway.h"
#include "RenderContracts/ResourceUploadRequest.h"
#include "Render/Resources/RenderResourceTypes.h"
#include "Resources/RenderSubmissionTracker.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHISampler.h"
#include "RHI/RHITexture.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <variant>
#include <vector>

namespace RVX
{
    class RenderResourceStatusTable;
    class RenderRetirementQueue;
    class RHICommandContext;
    enum class RenderMeshBufferSemantic : uint8
    {
        Position = 0,
        Normal,
        UV,
        Tangent,
        BoneIndices,
        BoneWeights,
        Index,
    };

    struct RenderOwnedBuffer
    {
        RenderMeshBufferSemantic semantic = RenderMeshBufferSemantic::Position;
        RHIBufferRef buffer;
        uint64 estimatedBytes = 0;
    };

    struct RenderMeshResourceData
    {
        std::vector<RenderOwnedBuffer> buffers;
        MeshUploadCreateInfo createInfo;
        std::vector<MeshUploadSubmesh> submeshes;
    };

    struct RenderTextureResourceData
    {
        RHITextureRef texture;
        uint64 estimatedBytes = 0;
        RHIResourceState state = RHIResourceState::Common;
    };

    struct RenderMaterialResourceData
    {
        RHIBufferRef constants;
        uint64 constantBytes = 0;
        MaterialSourceData sourceData;
        std::vector<MaterialUploadTextureBinding> textureBindings;
        std::vector<RHISamplerRef> samplers;
        bool metadataValid = false;
    };

    using RenderResourceGPUData = std::variant<
        RenderMeshResourceData,
        RenderTextureResourceData,
        RenderMaterialResourceData>;

    /** @brief Sole owner of pending and committed resource RHI references. */
    class RenderResourceRegistry final
    {
    public:
        RenderResourceRegistry() = default;
        ~RenderResourceRegistry();

        RenderResourceRegistry(const RenderResourceRegistry&) = delete;
        RenderResourceRegistry& operator=(const RenderResourceRegistry&) = delete;

        [[nodiscard]] bool Initialize(RenderResourceStatusTable* statusTable,
                                      RenderRetirementQueue* retirementQueue);
        void Shutdown();

        [[nodiscard]] bool BeginPending(
            RenderResourceHandle handle,
            RenderResourceKind kind,
            const std::vector<RenderResourceHandle>& dependencies);
        [[nodiscard]] bool AddPendingMeshBuffer(
            RenderResourceHandle handle,
            RenderMeshBufferSemantic semantic,
            RHIBufferRef buffer,
            uint64 estimatedBytes);
        [[nodiscard]] bool SetPendingMeshMetadata(
            RenderResourceHandle handle,
            const MeshUploadCreateInfo& createInfo,
            const std::vector<MeshUploadSubmesh>& submeshes);
        [[nodiscard]] bool SetPendingTexture(
            RenderResourceHandle handle,
            RHITextureRef texture,
            uint64 estimatedBytes);
        [[nodiscard]] bool SetPendingMaterialConstants(
            RenderResourceHandle handle,
            RHIBufferRef constants,
            uint64 estimatedBytes);
        [[nodiscard]] bool SetPendingMaterialMetadata(
            RenderResourceHandle handle,
            const MaterialUploadPayload& payload);
        [[nodiscard]] bool AddPendingMaterialSampler(
            RenderResourceHandle handle,
            RHISamplerRef sampler);
        [[nodiscard]] bool SetPendingCompletion(
            RenderResourceHandle handle,
            const GPUCompletionToken& completion);
        [[nodiscard]] bool Commit(RenderResourceHandle handle);
        [[nodiscard]] bool RetirePending(RenderResourceHandle handle);
        [[nodiscard]] bool Release(RenderResourceHandle handle);
        [[nodiscard]] bool MergeLastUse(
            RenderResourceHandle handle,
            const GPUCompletionToken& completion);
        [[nodiscard]] bool MergeLastUseClosure(
            std::span<const RenderResourceHandle> roots,
            const GPUCompletionToken& completion);

        [[nodiscard]] const RenderMeshResourceData* ResolveMesh(
            RenderResourceHandle handle) const;
        [[nodiscard]] const RenderTextureResourceData* ResolveTexture(
            RenderResourceHandle handle) const;
        [[nodiscard]] const RenderMaterialResourceData* ResolveMaterial(
            RenderResourceHandle handle) const;
        [[nodiscard]] MeshGPUBuffers ResolveMeshBuffers(
            RenderResourceHandle handle) const;
        [[nodiscard]] RHITexture* ResolveTextureObject(
            RenderResourceHandle handle) const;
        [[nodiscard]] bool TransitionTexture(
            RenderResourceHandle handle,
            RHICommandContext& context,
            RHIResourceState desiredState);
        [[nodiscard]] const std::vector<RenderResourceHandle>*
            GetDependencies(RenderResourceHandle handle) const;
        [[nodiscard]] GPUCompletionToken GetLastUse(
            RenderResourceHandle handle) const;
        [[nodiscard]] bool HasExactEntry(RenderResourceHandle handle) const;
        [[nodiscard]] bool IsGPUReadyExact(RenderResourceHandle handle) const;
        [[nodiscard]] bool HasPending(RenderResourceHandle handle) const;
        [[nodiscard]] uint32 GetEntryCount() const;
        [[nodiscard]] RenderResourceRegistryStats GetStats() const;

    private:
        struct Entry
        {
            uint32 generation = 0;
            RenderResourceKind kind = RenderResourceKind::Invalid;
            std::vector<RenderResourceHandle> dependencies;
            std::optional<RenderResourceGPUData> pending;
            std::optional<RenderResourceGPUData> committed;
            GPUCompletionToken lastUse;
        };

        Entry* FindExact(RenderResourceHandle handle);
        const Entry* FindExact(RenderResourceHandle handle) const;
        [[nodiscard]] bool RetireData(RenderResourceGPUData& data,
                                      const GPUCompletionToken& completion);
        [[nodiscard]] bool IsReady(RenderResourceHandle handle,
                                   RenderResourceKind kind) const;

        RenderResourceStatusTable* m_statusTable = nullptr;
        RenderRetirementQueue* m_retirementQueue = nullptr;
        std::unordered_map<uint32, Entry> m_entries;
    };
} // namespace RVX
