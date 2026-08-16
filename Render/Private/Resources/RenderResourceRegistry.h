#pragma once

/** @file RenderResourceRegistry.h @brief Exact-generation Render-owned RHI resources */

#include "RenderContracts/IRenderResourceGateway.h"
#include "RenderContracts/ResourceUploadRequest.h"
#include "Render/Resources/RenderResourceTypes.h"
#include "Resources/RenderSubmissionTracker.h"
#include "RHI/RHIAccess.h"
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
        RHIBufferAccessSnapshot accessSnapshot = MakeRHIBufferAccessSnapshot(
            RHIResourceState::Common,
            RHIShaderStage::All,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Valid);
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
        RHITextureAccessSnapshot accessSnapshot = MakeRHITextureAccessSnapshot(
            RHIResourceState::Common,
            RHIShaderStage::All,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Valid);
    };

    struct RenderMaterialResourceData
    {
        RHIBufferRef constants;
        uint64 constantBytes = 0;
        RHIBufferAccessSnapshot constantsAccessSnapshot = MakeRHIBufferAccessSnapshot(
            RHIResourceState::Common,
            RHIShaderStage::All,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Valid);
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
            const std::vector<RenderResourceHandle>& dependencies,
            RenderResourceContentOperation operation =
                RenderResourceContentOperation::Create,
            uint64 sourceRevision = 0);
        /**
         * @brief Begin an upload with its stable source asset identity.
         *
         * Production uploads must use this overload. The compatibility
         * overload above remains for legacy Render-private tests that create
         * synthetic resources without an AssetId; those entries cannot
         * contribute cross-process raster transcript semantics.
         */
        [[nodiscard]] bool BeginPending(
            RenderResourceHandle handle,
            RenderResourceKind kind,
            const std::vector<RenderResourceHandle>& dependencies,
            AssetId assetId,
            RenderResourceContentOperation operation =
                RenderResourceContentOperation::Create,
            uint64 sourceRevision = 0);
        [[nodiscard]] bool AddPendingMeshBuffer(
            RenderResourceHandle handle,
            RenderMeshBufferSemantic semantic,
            RHIBufferRef buffer,
            uint64 estimatedBytes,
            const RHIBufferAccessSnapshot& finalAccess =
                MakeRHIBufferAccessSnapshot(RHIResourceState::Common));
        [[nodiscard]] bool SetPendingMeshMetadata(
            RenderResourceHandle handle,
            const MeshUploadCreateInfo& createInfo,
            const std::vector<MeshUploadSubmesh>& submeshes);
        [[nodiscard]] bool SetPendingTexture(
            RenderResourceHandle handle,
            RHITextureRef texture,
            uint64 estimatedBytes,
            const RHITextureAccessSnapshot& finalAccess =
                MakeRHITextureAccessSnapshot(RHIResourceState::Common));
        [[nodiscard]] bool SetPendingMaterialConstants(
            RenderResourceHandle handle,
            RHIBufferRef constants,
            uint64 estimatedBytes,
            const RHIBufferAccessSnapshot& finalAccess =
                MakeRHIBufferAccessSnapshot(RHIResourceState::Common));
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
        /** @brief Commit the access realized by RenderGraph for an imported texture. */
        [[nodiscard]] bool CommitTextureAccessSnapshot(
            RenderResourceHandle handle,
            const RHITextureAccessSnapshot& accessSnapshot);
        [[nodiscard]] const std::vector<RenderResourceHandle>*
            GetDependencies(RenderResourceHandle handle) const;
        [[nodiscard]] GPUCompletionToken GetLastUse(
            RenderResourceHandle handle) const;
        [[nodiscard]] bool HasExactEntry(RenderResourceHandle handle) const;
        /** @brief Return the exact entry kind, or Invalid for an absent/stale handle. */
        [[nodiscard]] RenderResourceKind GetExactKind(
            RenderResourceHandle handle) const noexcept;
        /** @brief Render-private AssetId for one exact resource generation. */
        [[nodiscard]] AssetId GetExactAssetId(
            RenderResourceHandle handle) const noexcept;
        /**
         * @brief Combine a ready exact mesh AssetId with post-resolution
         * material evidence from MaterialSystem.
         *
         * This intentionally cannot inspect a material handle: source data
         * does not reveal the descriptor/table fallback outcome actually used
         * by the draw. A zero material key remains unavailable evidence.
         */
        [[nodiscard]] std::optional<uint64>
            CombineRasterMeshAndMaterialSemanticIdentity(
                RenderResourceHandle mesh,
                uint64 rasterMaterialSemanticKey) const noexcept;
        [[nodiscard]] bool IsGPUReadyExact(RenderResourceHandle handle) const;
        /** @brief Query the public lifecycle state for an exact generation. */
        [[nodiscard]] RenderResourceStatus QueryStatus(
            RenderResourceHandle handle) const noexcept;
        [[nodiscard]] bool HasPending(RenderResourceHandle handle) const;
        /** @brief Last successfully committed content source revision. */
        [[nodiscard]] uint64 GetCommittedSourceRevision(
            RenderResourceHandle handle) const noexcept;
        /** @brief Source revision reserved by the in-flight content update. */
        [[nodiscard]] uint64 GetPendingSourceRevision(
            RenderResourceHandle handle) const noexcept;
        [[nodiscard]] uint32 GetEntryCount() const;
        [[nodiscard]] RenderResourceRegistryStats GetStats() const;
        /** @brief Monotonic revision for readiness/content identity changes. */
        [[nodiscard]] uint64 GetContentRevision() const noexcept
        {
            return m_contentRevision;
        }
        /** @brief Exact entry content revision, or zero when the generation is absent. */
        [[nodiscard]] uint64 GetContentRevision(
            RenderResourceHandle handle) const noexcept;

    private:
        struct Entry
        {
            uint32 generation = 0;
            AssetId assetId{};
            uint64 contentRevision = 0;
            RenderResourceKind kind = RenderResourceKind::Invalid;
            std::vector<RenderResourceHandle> dependencies;
            std::vector<RenderResourceHandle> pendingDependencies;
            std::optional<RenderResourceGPUData> pending;
            std::optional<RenderResourceGPUData> committed;
            RenderResourceContentOperation pendingOperation =
                RenderResourceContentOperation::Create;
            uint64 pendingSourceRevision = 0;
            uint64 committedSourceRevision = 0;
            /** Strict high-water mark, retained across failed replacements. */
            uint64 lastAcceptedSourceRevision = 0;
            GPUCompletionToken lastUse;
        };

        Entry* FindExact(RenderResourceHandle handle);
        const Entry* FindExact(RenderResourceHandle handle) const;
        [[nodiscard]] bool RetireData(RenderResourceGPUData& data,
                                      const GPUCompletionToken& completion);
        [[nodiscard]] bool IsReady(RenderResourceHandle handle,
                                   RenderResourceKind kind) const;
        uint64 BumpContentRevision() noexcept;

        RenderResourceStatusTable* m_statusTable = nullptr;
        RenderRetirementQueue* m_retirementQueue = nullptr;
        std::unordered_map<uint32, Entry> m_entries;
        uint64 m_contentRevision = 1;
    };
} // namespace RVX
