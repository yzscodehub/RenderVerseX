#pragma once

#include "RenderContracts/RenderFrameTypes.h"
#include "RenderContracts/RenderIdentity.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace RVX
{
    /** Value-only identity of exactly the submitted mesh/material/texture draw. */
    struct TransparentTemporalDrawIdentity
    {
        uint64 objectId = 0;
        RenderResourceHandle mesh{};
        uint64 meshRevision = 0;
        uint32 submeshIndex = 0;
        uint32 indexOffset = 0;
        uint32 indexCount = 0;
        int32 baseVertex = 0;
        RenderResourceHandle material{};
        uint64 materialRevision = 0;
        uint64 descriptorKey = 0;
        uint64 descriptorRevision = 0;
        uint64 layoutHash = 0;
        uint32 textureFlags = 0;
        uint32 policy = 0;
        bool valid = false;
        std::vector<uint64> textureIdentityWords;
        bool operator==(const TransparentTemporalDrawIdentity&) const = default;
    };

    /** Last-presented, value-only history. Preparing a frame never mutates it. */
    class TransparentTemporalHistory
    {
    public:
        [[nodiscard]] bool CanReuse(uint64 frame, const RenderViewIdentity& view,
            const TransparentTemporalDrawIdentity& draw, bool viewHistoryAvailable) const
        {
            return viewHistoryAvailable && view.IsValid() && draw.valid &&
                draw.objectId != 0 && draw.mesh.IsValid() && draw.meshRevision != 0 &&
                draw.material.IsValid() && draw.materialRevision != 0 &&
                draw.descriptorKey != 0 && draw.descriptorRevision != 0 &&
                draw.layoutHash != 0 && draw.indexCount != 0 &&
                m_frame != 0 && frame > m_frame && view == m_view &&
                std::count(m_draws.begin(), m_draws.end(), draw) == 1;
        }

        void Commit(uint64 frame, const RenderViewIdentity& view,
            std::vector<TransparentTemporalDrawIdentity> draws, bool presentationQualified)
        {
            const bool accepted = presentationQualified && frame > m_frame && view.IsValid();
            m_draws = accepted ? std::move(draws) : std::vector<TransparentTemporalDrawIdentity>{};
            m_frame = accepted ? frame : 0;
            m_view = accepted ? view : RenderViewIdentity{};
        }

    private:
        uint64 m_frame = 0;
        RenderViewIdentity m_view{};
        std::vector<TransparentTemporalDrawIdentity> m_draws;
    };
}
