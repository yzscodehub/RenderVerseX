/**
 * @file RenderDeformationFrameResources.cpp
 * @brief Recording-scoped skinning palette packing and upload.
 */

#include "Render/Animation/RenderDeformationFrameResources.h"

#include "Core/Diagnostics/ContentHash.h"
#include "Render/Renderer/RenderScene.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHIDevice.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>

namespace RVX
{
namespace
{
    [[nodiscard]] bool IsMetadataShapeValid(
        const RenderSkinningPaletteMetadata& metadata,
        uint32 expectedCount) noexcept
    {
        return metadata.providerComponentId != 0 &&
               metadata.sourceModelResourceId != 0 &&
               metadata.poseSequence != 0 && metadata.paletteHash != 0 &&
               expectedCount != 0 && metadata.paletteCount == expectedCount;
    }

    [[nodiscard]] bool ArePalettesCompatible(
        const RenderSkinningPaletteMetadata& current,
        const RenderSkinningPaletteMetadata& previous) noexcept
    {
        return current.providerComponentId == previous.providerComponentId &&
               current.sourceModelResourceId == previous.sourceModelResourceId &&
               current.paletteCount == previous.paletteCount;
    }

    [[nodiscard]] bool ComputePaletteByteSize(
        uint32 matrixCount,
        uint64& outByteSize) noexcept
    {
        outByteSize = 0;
        if (matrixCount == 0)
            return true;

        constexpr uint64 stride = sizeof(Mat4);
        if (matrixCount > std::numeric_limits<uint64>::max() / stride)
            return false;

        outByteSize = static_cast<uint64>(matrixCount) * stride;
        return outByteSize != 0;
    }

    [[nodiscard]] RHIBufferRef CreatePaletteBuffer(
        IRHIDevice& device,
        uint64 byteSize,
        const char* debugName)
    {
        if (byteSize == 0 || byteSize % sizeof(Mat4) != 0)
            return {};

        RHIBufferDesc description;
        description.size = byteSize;
        description.usage = RHIBufferUsage::Structured |
                            RHIBufferUsage::ShaderResource;
        description.memoryType = RHIMemoryType::Upload;
        description.stride = sizeof(Mat4);
        description.debugName = debugName;
        return device.CreateBuffer(description);
    }

    [[nodiscard]] bool UploadPalette(
        RHIBuffer& buffer,
        std::span<const Mat4> palette) noexcept
    {
        if (palette.empty() ||
            palette.size() > std::numeric_limits<uint32>::max())
            return false;

        uint64 byteSize = 0;
        if (!ComputePaletteByteSize(static_cast<uint32>(palette.size()), byteSize) ||
            buffer.GetSize() != byteSize ||
            buffer.GetStride() != sizeof(Mat4) ||
            buffer.GetMemoryType() != RHIMemoryType::Upload)
        {
            return false;
        }

        void* mapped = buffer.Map();
        if (mapped == nullptr)
            return false;

        std::memcpy(mapped, palette.data(), static_cast<size_t>(byteSize));
        return buffer.CommitMappedWrite();
    }

    void MixMetadata(uint64& hash,
                     const RenderSkinningPaletteMetadata& metadata) noexcept
    {
        Diagnostics::MixContentHashValue(hash, metadata.providerComponentId);
        Diagnostics::MixContentHashValue(hash, metadata.sourceModelResourceId);
        Diagnostics::MixContentHashValue(hash, metadata.poseSequence);
        Diagnostics::MixContentHashValue(hash, metadata.paletteHash);
        Diagnostics::MixContentHashValue(hash, metadata.paletteCount);
    }

    [[nodiscard]] uint64 ComputeContentHash(
        uint32 matrixCount,
        std::span<const RenderDeformationPaletteSlice> slices) noexcept
    {
        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        Diagnostics::MixContentHashString(hash, "RVX.RenderDeformationFrameResources.v1");
        Diagnostics::MixContentHashValue(hash, matrixCount);
        for (const RenderDeformationPaletteSlice& slice : slices)
        {
            Diagnostics::MixContentHashValue(hash, slice.objectId);
            Diagnostics::MixContentHashValue(hash, slice.currentOffset);
            Diagnostics::MixContentHashValue(hash, slice.previousOffset);
            Diagnostics::MixContentHashValue(hash, slice.matrixCount);
            MixMetadata(hash, slice.currentMetadata);
            MixMetadata(hash, slice.previousMetadata);
            Diagnostics::MixContentHashValue(hash, slice.previousValid ? 1U : 0U);
        }
        return hash;
    }

    [[nodiscard]] bool HasCurrentGraphProvenance(
        const RGBufferHandle& handle,
        const RenderDeformationFrameIdentity& identity) noexcept
    {
        return handle.IsValid() && handle.graphIdentity == identity.graphIdentity &&
               handle.recordingGeneration == identity.recordingGeneration;
    }
} // namespace

bool RenderDeformationPaletteSlice::IsStructurallyValid() const noexcept
{
    if (objectId == 0 || matrixCount == 0 ||
        matrixCount > RVX_RENDER_DEFORMATION_MAX_OBJECT_MATRICES ||
        !IsMetadataShapeValid(currentMetadata, matrixCount))
    {
        return false;
    }

    if (!previousValid)
        return previousMetadata.paletteCount == 0;

    return IsMetadataShapeValid(previousMetadata, matrixCount) &&
           ArePalettesCompatible(currentMetadata, previousMetadata);
}

const RenderDeformationPaletteSlice* RenderDeformationFrameResources::FindSlice(
    uint64 objectId) const noexcept
{
    if (objectId == 0)
        return nullptr;

    const auto position = std::lower_bound(
        m_slices.begin(),
        m_slices.end(),
        objectId,
        [](const RenderDeformationPaletteSlice& slice, uint64 value)
        {
            return slice.objectId < value;
        });
    return position != m_slices.end() && position->objectId == objectId
        ? &*position
        : nullptr;
}

bool RenderDeformationFrameResources::IsComplete() const noexcept
{
    if (!m_identity.IsValid() || m_contentHash == 0 ||
        m_matrixCount > RVX_RENDER_DEFORMATION_MAX_FRAME_MATRICES)
    {
        return false;
    }

    if (m_matrixCount == 0)
    {
        return m_slices.empty() && !m_currentPaletteBuffer &&
               !m_previousPaletteBuffer && !m_currentPaletteHandle.IsValid() &&
               !m_previousPaletteHandle.IsValid();
    }

    uint64 byteSize = 0;
    if (!ComputePaletteByteSize(m_matrixCount, byteSize) ||
        !m_currentPaletteBuffer || !m_previousPaletteBuffer ||
        m_currentPaletteBuffer->GetSize() != byteSize ||
        m_previousPaletteBuffer->GetSize() != byteSize ||
        m_currentPaletteBuffer->GetStride() != sizeof(Mat4) ||
        m_previousPaletteBuffer->GetStride() != sizeof(Mat4) ||
        !HasCurrentGraphProvenance(m_currentPaletteHandle, m_identity) ||
        !HasCurrentGraphProvenance(m_previousPaletteHandle, m_identity))
    {
        return false;
    }

    uint64 nextOffset = 0;
    uint64 previousObjectId = 0;
    for (const RenderDeformationPaletteSlice& slice : m_slices)
    {
        if (!slice.IsStructurallyValid() || slice.objectId <= previousObjectId ||
            slice.currentOffset != nextOffset ||
            slice.previousOffset != nextOffset ||
            nextOffset > m_matrixCount ||
            slice.matrixCount > m_matrixCount - nextOffset)
        {
            return false;
        }
        previousObjectId = slice.objectId;
        nextOffset += slice.matrixCount;
    }
    return nextOffset == m_matrixCount;
}

bool RenderDeformationFrameResources::MatchesIdentity(
    const RenderDeformationFrameIdentity& identity) const noexcept
{
    return IsComplete() && m_identity == identity;
}

bool RenderDeformationFrameResources::MatchesGraphProvenance(
    const RenderGraph& graph) const noexcept
{
    return IsComplete() && m_identity.graphIdentity == graph.GetGraphIdentity() &&
           m_identity.recordingGeneration == graph.GetRecordingGeneration();
}

bool BuildRenderDeformationFrameResources(
    IRHIDevice* device,
    RenderGraph& graph,
    const RenderDeformationFrameIdentity& identity,
    const RenderScene& scene,
    std::shared_ptr<const RenderDeformationFrameResources>& outResources,
    std::string& outError)
{
    outResources.reset();
    outError.clear();

    if (!identity.IsValid() || identity.graphIdentity != graph.GetGraphIdentity() ||
        identity.recordingGeneration != graph.GetRecordingGeneration())
    {
        outError = "render deformation identity does not match the active graph recording";
        return false;
    }

    try
    {
        struct PendingPalette
        {
            const RenderObject* object = nullptr;
        };

        std::vector<PendingPalette> pending;
        pending.reserve(scene.GetObjects().size());
        for (const RenderObject& object : scene.GetObjects())
        {
            if (!object.HasSkinningData())
                continue;

            if (object.entityId == 0 || !object.HasValidSkinningPalette() ||
                object.skinningMatrices.size() >
                    RVX_RENDER_DEFORMATION_MAX_OBJECT_MATRICES)
            {
                outError = "skinned render object has an invalid current palette";
                return false;
            }

            pending.push_back({&object});
        }

        std::sort(pending.begin(), pending.end(),
                  [](const PendingPalette& left, const PendingPalette& right)
                  {
                      return left.object->entityId < right.object->entityId;
                  });

        uint64 totalMatrixCount = 0;
        uint64 previousObjectId = 0;
        std::vector<RenderDeformationPaletteSlice> slices;
        std::vector<Mat4> currentMatrices;
        std::vector<Mat4> previousMatrices;
        slices.reserve(pending.size());
        for (const PendingPalette& pendingPalette : pending)
        {
            const RenderObject& object = *pendingPalette.object;
            if (object.entityId == previousObjectId)
            {
                outError = "duplicate object ID in deformation palette packing";
                return false;
            }
            previousObjectId = object.entityId;

            const uint32 matrixCount = static_cast<uint32>(
                object.skinningMatrices.size());
            if (matrixCount == 0 || matrixCount >
                RVX_RENDER_DEFORMATION_MAX_OBJECT_MATRICES ||
                totalMatrixCount > RVX_RENDER_DEFORMATION_MAX_FRAME_MATRICES -
                    matrixCount)
            {
                outError = "deformation palette count exceeds the recording limit";
                return false;
            }

            RenderDeformationPaletteSlice slice;
            slice.objectId = object.entityId;
            slice.currentOffset = static_cast<uint32>(totalMatrixCount);
            slice.previousOffset = static_cast<uint32>(totalMatrixCount);
            slice.matrixCount = matrixCount;
            slice.currentMetadata = object.skinningPalette;

            const bool previousCompatible =
                object.HasValidPreviousSkinningPalette() &&
                ArePalettesCompatible(object.skinningPalette,
                                      object.previousSkinningPalette) &&
                object.previousSkinningMatrices.size() == matrixCount;
            if (previousCompatible)
            {
                slice.previousMetadata = object.previousSkinningPalette;
                slice.previousValid = true;
            }

            currentMatrices.insert(currentMatrices.end(),
                                   object.skinningMatrices.begin(),
                                   object.skinningMatrices.end());
            if (previousCompatible)
            {
                previousMatrices.insert(previousMatrices.end(),
                                        object.previousSkinningMatrices.begin(),
                                        object.previousSkinningMatrices.end());
            }
            else
            {
                // Safe GPU input for an invalid history; the velocity shader
                // must still consult previousValid before emitting motion.
                previousMatrices.insert(previousMatrices.end(),
                                        object.skinningMatrices.begin(),
                                        object.skinningMatrices.end());
            }
            slices.push_back(slice);
            totalMatrixCount += matrixCount;
        }

        auto resources = std::make_shared<RenderDeformationFrameResources>();
        resources->m_identity = identity;
        resources->m_matrixCount = static_cast<uint32>(totalMatrixCount);
        resources->m_slices = std::move(slices);
        resources->m_contentHash = ComputeContentHash(
            resources->m_matrixCount, resources->m_slices);

        if (resources->m_matrixCount == 0)
        {
            if (!resources->IsComplete())
            {
                outError = "empty deformation snapshot failed completeness validation";
                return false;
            }
            outResources = std::move(resources);
            return true;
        }

        if (device == nullptr)
        {
            outError = "skinned deformation resources require an RHI device";
            return false;
        }

        uint64 byteSize = 0;
        if (!ComputePaletteByteSize(resources->m_matrixCount, byteSize) ||
            currentMatrices.size() != resources->m_matrixCount ||
            previousMatrices.size() != resources->m_matrixCount)
        {
            outError = "deformation palette packing overflowed its byte range";
            return false;
        }

        resources->m_currentPaletteBuffer = CreatePaletteBuffer(
            *device, byteSize, "RenderDeformationCurrentPalette");
        resources->m_previousPaletteBuffer = CreatePaletteBuffer(
            *device, byteSize, "RenderDeformationPreviousPalette");
        if (!resources->m_currentPaletteBuffer ||
            !resources->m_previousPaletteBuffer ||
            !UploadPalette(*resources->m_currentPaletteBuffer, currentMatrices) ||
            !UploadPalette(*resources->m_previousPaletteBuffer, previousMatrices))
        {
            outError = "failed to create or upload deformation palette buffers";
            return false;
        }

        resources->m_currentPaletteHandle = graph.ImportBuffer(
            resources->m_currentPaletteBuffer, RHIResourceState::ShaderResource);
        resources->m_previousPaletteHandle = graph.ImportBuffer(
            resources->m_previousPaletteBuffer, RHIResourceState::ShaderResource);
        if (!resources->IsComplete())
        {
            outError = "deformation palette graph import did not preserve provenance";
            return false;
        }

        outResources = std::move(resources);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        outError = "out of memory while building deformation palette resources";
    }
    catch (...)
    {
        outError = "unexpected failure while building deformation palette resources";
    }
    return false;
}
} // namespace RVX
