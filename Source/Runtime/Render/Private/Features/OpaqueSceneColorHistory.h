#pragma once

#include "Render/Features/RenderFeatureGraphBuilder.h"
#include "Render/Passes/RenderPassRecordContext.h"
#include "Render/PostProcess/TAA.h"

namespace RVX
{
    /** One composition-owned, present-transactional opaque refraction history. */
    class OpaqueSceneColorHistory final
    {
    public:
        OpaqueSceneColorHistory();
        void SetResources(PipelineCache* cache, ResourceViewCache* views);
        void Prepare(RenderFrameIdentity frame, RenderViewIdentity view,
                     const PostProcessFrameInputs& inputs);
        bool Record(RenderFeatureGraphBuilder& builder,
                    const RenderPassRecordContext& context,
                    RenderFrameIdentity frame, RenderViewIdentity view,
                    RGTextureHandle& output, std::string& failure);
        bool RequiresCommit() const noexcept;
        bool CanCommit(const RenderViewHistoryIdentity& identity) const noexcept;
        bool Commit(const RenderViewHistoryIdentity& identity) noexcept;
        void Abandon() noexcept;
        void Shutdown();
        bool WasExecuted() const noexcept { return m_resolve.GetLastResolveStats().executed; }
    private:
        TAA m_resolve{TemporalResolvePurpose::OpaqueRefraction};
        PostProcessFrameInputs m_inputs{};
        RenderFrameIdentity m_frame{};
        RenderViewIdentity m_view{};
        RenderViewHistoryIdentity m_committedIdentity{};
        bool m_recorded = false;
    };
}
