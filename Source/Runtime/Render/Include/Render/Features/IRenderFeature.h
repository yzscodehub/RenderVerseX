#pragma once

/** @file IRenderFeature.h @brief Stable renderer feature composition interface. */

#include "Render/Features/RenderFeatureGraphBuilder.h"
#include "Render/Features/RenderFeatureTypes.h"
#include "Render/Features/RenderGraphBlackboard.h"

namespace RVX
{
    class IRenderFeature
    {
    public:
        virtual ~IRenderFeature() = default;

        [[nodiscard]] virtual RenderFeatureDescriptor Describe() const = 0;
        virtual void Prepare(const RenderFeatureContext& context) = 0;
        [[nodiscard]] virtual RenderFeatureReadiness ResolveReadiness() const = 0;
        virtual void AddPasses(RenderFeatureGraphBuilder& builder,
                               RenderGraphBlackboard& blackboard) = 0;
        /**
         * @brief Receive registry-owned evidence after this graph recording ran.
         *
         * The default keeps existing features source-compatible. Features that
         * expose detailed execution evidence must wait for this callback rather
         * than treating graph scheduling as execution.
         */
        virtual void OnGraphExecutionFinalized(
            const RenderFeatureGraphExecutionEvidence& evidence)
        {
            static_cast<void>(evidence);
        }
        [[nodiscard]] virtual RenderFeatureExecutionReceipt CollectReceipt()
            const = 0;
    };
} // namespace RVX
