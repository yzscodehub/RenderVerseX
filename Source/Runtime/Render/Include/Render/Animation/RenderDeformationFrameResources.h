#pragma once

/**
 * @file RenderDeformationFrameResources.h
 * @brief Immutable graph-recording skinning palette resources.
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Render/Graph/RenderGraph.h"
#include "RenderContracts/RenderFrameTypes.h"

#include <memory>
#include <string>
#include <vector>

namespace RVX
{
    class IRHIDevice;
    class RenderScene;

    /** Maximum palette cardinality accepted for one skinned render object. */
    inline constexpr uint32 RVX_RENDER_DEFORMATION_MAX_OBJECT_MATRICES = 65535U;
    /** Maximum palette cardinality packed by one graph recording. */
    inline constexpr uint32 RVX_RENDER_DEFORMATION_MAX_FRAME_MATRICES = 1048576U;

    /**
     * @brief Immutable provenance for the palette buffers belonging to one graph recording.
     *
     * This deliberately cannot use a frame sequence alone: a graph reset or a
     * second view recording of the same frame must never borrow its buffers.
     */
    struct RenderDeformationFrameIdentity
    {
        uint64 graphIdentity = 0;
        uint64 recordingGeneration = 0;
        uint64 frameSequence = 0;
        uint32 viewOrdinal = 0;
        uint64 recordEpoch = 0;

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return graphIdentity != 0 && recordingGeneration != 0 &&
                   frameSequence != 0 && recordEpoch != 0;
        }

        friend bool operator==(const RenderDeformationFrameIdentity&,
                               const RenderDeformationFrameIdentity&) = default;
    };

    /** @brief Packed current/previous palette range for one render object. */
    struct RenderDeformationPaletteSlice
    {
        uint64 objectId = 0;
        uint32 currentOffset = 0;
        uint32 previousOffset = 0;
        uint32 matrixCount = 0;
        RenderSkinningPaletteMetadata currentMetadata{};
        RenderSkinningPaletteMetadata previousMetadata{};
        bool previousValid = false;

        [[nodiscard]] bool IsStructurallyValid() const noexcept;
    };

    /**
     * @brief Strongly-owned, immutable palette buffers and exact graph handles.
     *
     * The object is completed before it becomes visible to pass recording.  A
     * zero-palette scene is a valid complete snapshot and intentionally owns no
     * RHI buffers or graph handles.
     */
    class RenderDeformationFrameResources final
    {
    public:
        [[nodiscard]] const RenderDeformationFrameIdentity& GetIdentity() const noexcept
        {
            return m_identity;
        }
        [[nodiscard]] const RHIBufferRef& GetCurrentPaletteBuffer() const noexcept
        {
            return m_currentPaletteBuffer;
        }
        [[nodiscard]] const RHIBufferRef& GetPreviousPaletteBuffer() const noexcept
        {
            return m_previousPaletteBuffer;
        }
        [[nodiscard]] RGBufferHandle GetCurrentPaletteHandle() const noexcept
        {
            return m_currentPaletteHandle;
        }
        [[nodiscard]] RGBufferHandle GetPreviousPaletteHandle() const noexcept
        {
            return m_previousPaletteHandle;
        }
        [[nodiscard]] const std::vector<RenderDeformationPaletteSlice>& GetSlices() const noexcept
        {
            return m_slices;
        }
        [[nodiscard]] uint32 GetMatrixCount() const noexcept
        {
            return m_matrixCount;
        }
        [[nodiscard]] uint64 GetContentHash() const noexcept
        {
            return m_contentHash;
        }

        [[nodiscard]] const RenderDeformationPaletteSlice* FindSlice(
            uint64 objectId) const noexcept;
        [[nodiscard]] bool IsComplete() const noexcept;
        [[nodiscard]] bool MatchesIdentity(
            const RenderDeformationFrameIdentity& identity) const noexcept;
        [[nodiscard]] bool MatchesGraphProvenance(
            const RenderGraph& graph) const noexcept;

    private:
        friend bool BuildRenderDeformationFrameResources(
            IRHIDevice* device,
            RenderGraph& graph,
            const RenderDeformationFrameIdentity& identity,
            const RenderScene& scene,
            std::shared_ptr<const RenderDeformationFrameResources>& outResources,
            std::string& outError);

        RenderDeformationFrameIdentity m_identity{};
        RHIBufferRef m_currentPaletteBuffer{};
        RHIBufferRef m_previousPaletteBuffer{};
        RGBufferHandle m_currentPaletteHandle{};
        RGBufferHandle m_previousPaletteHandle{};
        std::vector<RenderDeformationPaletteSlice> m_slices{};
        uint32 m_matrixCount = 0;
        uint64 m_contentHash = 0;
    };

    /**
     * @brief Build current and previous palette buffers for one graph recording.
     *
     * Only valid skinned objects are packed. Invalid current palette input,
     * duplicate object IDs, capacity overflow, map failure, and graph identity
     * mismatch fail without exposing a partial snapshot.
     */
    [[nodiscard]] bool BuildRenderDeformationFrameResources(
        IRHIDevice* device,
        RenderGraph& graph,
        const RenderDeformationFrameIdentity& identity,
        const RenderScene& scene,
        std::shared_ptr<const RenderDeformationFrameResources>& outResources,
        std::string& outError);
} // namespace RVX
