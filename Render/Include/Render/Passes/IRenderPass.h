#pragma once

/**
 * @file IRenderPass.h
 * @brief Render pass interface - base class for all render passes
 * 
 * IRenderPass integrates with RenderGraph for automatic resource state
 * tracking, barrier insertion, and memory aliasing.
 */

#include "Render/Graph/RenderGraph.h"
#include "Render/Passes/RenderPassRecordContext.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"

#include <string>

namespace RVX
{
    class RenderGraphBuilder;
    class RenderGraph;
    struct ViewData;

    /**
     * @brief Observable state for one render pass in the main chain.
     */
    struct RenderPassStatus
    {
        std::string name;
        int32_t priority = 0;
        bool requestedEnabled = false;
        bool supported = true;
        bool enabled = false;
        std::string unsupportedReason;
    };

    /**
     * @brief Render pass interface
     * 
     * IRenderPass is the base interface for all render passes. Each pass
     * is responsible for declaring its resource dependencies (Setup) and
     * recording GPU commands (Execute).
     * 
     * This is similar to Unity's ScriptableRenderPass.
     * 
     * Usage:
     * @code
     * class MyPass : public IRenderPass
     * {
     * public:
     *     const char* GetName() const override { return "MyPass"; }
     * 
     *     void Setup(RenderGraphBuilder& builder, const ViewData& view) override
     *     {
     *         // Declare resource usage
     *         m_colorTarget = builder.Write(view.colorTarget);
     *     }
     * 
     *     void Execute(RHICommandContext& ctx, const ViewData& view) override
     *     {
     *         // Record commands
     *         ctx.SetPipeline(m_pipeline);
     *         ctx.Draw(...);
     *     }
     * };
     * @endcode
     */
    class IRenderPass
    {
    public:
        virtual ~IRenderPass() = default;

        /**
         * @brief Get the pass name
         * @return Pass name string
         */
        virtual const char* GetName() const = 0;

        /**
         * @brief Setup phase - declare resource dependencies
         * @param builder The render graph builder
         * @param view The current view data
         * 
         * Called during render graph construction. The pass should declare
         * all resources it will read or write.
         */
        virtual void Setup(RenderGraphBuilder& builder, const ViewData& view) = 0;

        /**
         * @brief Execute phase - record GPU commands
         * @param ctx The command context to record into
         * @param view The current view data
         * 
         * Called during render graph execution. The pass should record
         * all GPU commands.
         */
        virtual void Execute(RHICommandContext& ctx, const ViewData& view) = 0;

        /**
         * @brief Get pass priority for sorting
         * @return Priority value (lower = earlier execution)
         * 
         * Standard priorities:
         * - DepthPrepass: 100
         * - ShadowPass: 200
         * - OpaquePass: 300
         * - SkyboxPass: 400
         * - TransparentPass: 500
         * - PostProcess: 1000
         */
        virtual int32_t GetPriority() const { return 0; }

        /**
         * @brief Check if the pass was requested even when unsupported
         */
        virtual bool IsRequestedEnabled() const { return true; }

        /**
         * @brief Check whether this pass has a real executable implementation
         */
        virtual bool IsSupported() const { return true; }

        /**
         * @brief Human-readable reason when unsupported
         */
        virtual const std::string& GetUnsupportedReason() const
        {
            static const std::string emptyReason;
            return emptyReason;
        }

        /**
         * @brief Check if the pass is enabled
         * @return true if the pass should execute
         */
        virtual bool IsEnabled() const { return IsRequestedEnabled() && IsSupported(); }

        /**
         * @brief Get a snapshot of this pass' chain status
         */
        virtual RenderPassStatus GetStatus() const
        {
            RenderPassStatus status;
            status.name = GetName() ? GetName() : "";
            status.priority = GetPriority();
            status.requestedEnabled = IsRequestedEnabled();
            status.supported = IsSupported();
            status.enabled = IsEnabled();
            status.unsupportedReason = GetUnsupportedReason();
            return status;
        }

        /**
         * @brief Called when the pass is added to the renderer
         * @param device The RHI device for resource creation
         */
        virtual void OnAdd(IRHIDevice* device) { (void)device; }

        /**
         * @brief Called when the pass is removed from the renderer
         */
        virtual void OnRemove() {}

        /**
         * @brief Get the RenderGraph pass type
         * @return Pass type (Graphics, Compute, or Copy)
         */
        virtual RenderGraphPassType GetPassType() const { return RenderGraphPassType::Graphics; }

        /**
         * @brief Register this pass with a RenderGraph
         * @param graph The render graph to register with
         * @param view The current view data
         * 
         * This method wraps Setup and Execute into a RenderGraph pass, enabling
         * automatic barrier management, pass culling, and memory aliasing.
         * 
         * Override this method for custom pass data types or special behavior.
         */
        virtual void AddToGraph(RenderGraph& graph, const ViewData& view)
        {
            AddToGraph(graph, MakeRenderPassRecordContext(graph, view));
        }

        /**
         * @brief Register this pass using an immutable record context.
         *
         * The default adapter is retained for unmigrated passes.  It captures
         * ViewData by value and never retains a caller-owned ViewData address.
         */
        virtual void AddToGraph(RenderGraph& graph,
                                const RenderPassRecordContext& context)
        {
            struct PassData
            {
                IRenderPass* pass;
                RenderPassExecutionData execution;
                bool contextValid = false;
            };

            const RenderPassExecutionData execution =
                MakeRenderPassExecutionData(context);
            const bool contextValid = context.MatchesTargetGraph(graph) &&
                execution.MatchesTargetGraph(graph);

            graph.AddPass<PassData>(
                GetName(),
                GetPassType(),
                [this, execution, contextValid](RenderGraphBuilder& builder,
                                                 PassData& data)
                {
                    data.pass = this;
                    data.execution = execution;
                    data.contextValid = contextValid;
                    if (!data.contextValid)
                    {
                        return;
                    }
                    this->Setup(builder, data.execution.view);
                },
                [](const PassData& data, RHICommandContext& ctx)
                {
                    if (data.contextValid)
                    {
                        data.pass->Execute(ctx, data.execution.view);
                    }
                });
        }
    };

    // Standard pass priorities
    namespace PassPriority
    {
        constexpr int32_t DepthPrepass    = 100;
        constexpr int32_t Shadow          = 200;
        constexpr int32_t Opaque          = 300;
        constexpr int32_t Skybox          = 400;
        constexpr int32_t Transparent     = 500;
        constexpr int32_t PostProcess     = 1000;
        constexpr int32_t Debug           = 2000;
    }

} // namespace RVX
