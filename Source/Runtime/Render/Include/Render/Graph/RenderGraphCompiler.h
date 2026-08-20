#pragma once

/** @file RenderGraphCompiler.h @brief Allocation-free final RenderGraph plan compiler */

#include "Render/Graph/RenderGraph.h"

namespace RVX
{
    class RenderGraphExecutor;

    /** @brief Immutable capability token for one finalized graph plan. */
    class CompiledRenderGraphPlan final
    {
    public:
        CompiledRenderGraphPlan() = default;
        CompiledRenderGraphPlan(CompiledRenderGraphPlan&&) noexcept = default;
        CompiledRenderGraphPlan& operator=(CompiledRenderGraphPlan&&) noexcept = default;
        CompiledRenderGraphPlan(const CompiledRenderGraphPlan&) = delete;
        CompiledRenderGraphPlan& operator=(const CompiledRenderGraphPlan&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return m_graph != nullptr && m_planHash != 0;
        }

        [[nodiscard]] uint64 GetPlanHash() const noexcept { return m_planHash; }
        [[nodiscard]] RGQueuePolicy GetFinalQueuePolicy() const noexcept
        {
            return m_finalQueuePolicy;
        }

    private:
        friend class RenderGraphCompiler;
        friend class RenderGraphExecutor;

        RenderGraph* m_graph = nullptr;
        uint64 m_graphIdentity = 0;
        uint64 m_recordingGeneration = 0;
        uint64 m_planHash = 0;
        RGQueuePolicy m_finalQueuePolicy = RGQueuePolicy::GraphicsOnly;
    };

    /** @brief Pure planning facade; never receives a device or transient pool. */
    class RenderGraphCompiler final
    {
    public:
        [[nodiscard]] CompiledRenderGraphPlan Compile(
            RenderGraph& definition,
            const RenderGraphCompileOptions& options) const;
    };
} // namespace RVX
