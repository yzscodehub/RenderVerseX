#pragma once

/** @file RenderGraphExecutor.h @brief One-shot physical RenderGraph execution */

#include "Render/Graph/RenderGraphCompiler.h"

namespace RVX
{
    /** @brief Preflights, realizes, records, and returns completion ownership. */
    class RenderGraphExecutor final
    {
    public:
        [[nodiscard]] RenderGraphExecution Prepare(
            CompiledRenderGraphPlan& plan,
            const RenderGraphExecutionEnvironment& environment) const;
    };
} // namespace RVX
