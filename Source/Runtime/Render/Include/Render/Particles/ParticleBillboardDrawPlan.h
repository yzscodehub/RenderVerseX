#pragma once

/**
 * @file ParticleBillboardDrawPlan.h
 * @brief Immutable, view-specific CPU draw planning for particle billboards.
 */

#include "Core/Types.h"
#include "Render/Renderer/ViewData.h"
#include "RenderContracts/ParticleRenderSnapshot.h"

#include <string>
#include <vector>

namespace RVX
{
    /** @brief Why a particle snapshot cannot become a billboard draw plan. */
    enum class ParticleBillboardDrawPlanFailure : uint8
    {
        None = 0,
        IncompleteSnapshot,
        InvalidChannelEnvelope,
        DuplicateSourceIdentity,
        DuplicateSystemInstanceIdentity,
        MetadataOnlyPayload,
        UnsupportedSimulationBackend,
        InvalidCpuPacket,
        UnsupportedRenderMode,
        UnsupportedSoftParticles,
        UnsupportedBlendMode,
        InvalidParticleCount,
        InvalidParticleData,
    };

    /** @brief One immutable billboard instance after view-dependent ordering. */
    struct ParticleBillboardDrawItem
    {
        ParticleRenderParticleData particle{};
        ParticleRenderSnapshotBlendMode blendMode =
            ParticleRenderSnapshotBlendMode::AlphaBlend;
        /** Stable source asset identity; never a RenderScene allocation ID. */
        uint64 systemAssetId = 0;
        /** Packet-sealed owner identity used for deterministic alpha ordering. */
        uint64 deterministicInstanceKey = 0;
        /** Producer identity used only after the packet key to break corrupt collisions. */
        RenderFeatureSourceIdentity source{};
        /** RenderScene IDs remain diagnostic and uniqueness-only. */
        uint64 systemId = 0;
        uint64 instanceId = 0;
        float viewDepth = 0.0f;
    };

    /** @brief A contiguous batch preserving the globally sorted draw order. */
    struct ParticleBillboardDrawBatch
    {
        ParticleRenderSnapshotBlendMode blendMode =
            ParticleRenderSnapshotBlendMode::AlphaBlend;
        uint32 firstItem = 0;
        uint32 itemCount = 0;
    };

    /**
     * @brief Pure per-view billboard submission plan.
     *
     * The plan owns its sorted upload payload.  It never mutates a CPU packet,
     * which means multiple views may build plans from the same immutable packet
     * without interfering with one another.
     */
    struct ParticleBillboardDrawPlan
    {
        bool valid = false;
        ParticleBillboardDrawPlanFailure failure =
            ParticleBillboardDrawPlanFailure::None;
        std::string diagnostic;
        std::vector<ParticleBillboardDrawItem> items;
        std::vector<ParticleBillboardDrawBatch> batches;

        [[nodiscard]] uint32 GetInstanceCount() const noexcept
        {
            return static_cast<uint32>(items.size());
        }

        [[nodiscard]] uint32 GetDrawCount() const noexcept
        {
            return static_cast<uint32>(batches.size());
        }
    };

    [[nodiscard]] const char* GetParticleBillboardDrawPlanFailureName(
        ParticleBillboardDrawPlanFailure failure) noexcept;

    /**
     * @brief Validate and sort all CPU billboard particles for @p view.
     *
     * The result is fail-closed: one malformed or unsupported requested item
     * rejects the complete snapshot instead of rendering an arbitrary subset.
     */
    [[nodiscard]] ParticleBillboardDrawPlan BuildParticleBillboardDrawPlan(
        const ParticleRenderSnapshot& snapshot,
        const ViewData& view);
} // namespace RVX
