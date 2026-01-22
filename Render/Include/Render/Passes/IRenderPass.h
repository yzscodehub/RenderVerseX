#pragma once

/**
 * @file IRenderPass.h
 * @brief Render pass interface - base class for all render passes
 * 
 * IRenderPass integrates with RenderGraph for automatic resource state
 * tracking, barrier insertion, and memory aliasing.
 */

#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "Render/Graph/RenderGraph.h"

namespace RVX
{
    class RenderGraphBuilder;
    class RenderGraph;
    struct ViewData;

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
         * @brief Check if the pass is enabled
         * @return true if the pass should execute
         */
        virtual bool IsEnabled() const { return true; }

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
            // Default implementation captures 'this' and 'view' to bridge
            // the IRenderPass interface with RenderGraph's callback system
            struct PassData
            {
                IRenderPass* pass;
                const ViewData* viewData;
            };

            graph.AddPass<PassData>(
                GetName(),
                GetPassType(),
                [this, &view](RenderGraphBuilder& builder, PassData& data)
                {
                    data.pass = this;
                    data.viewData = &view;
                    this->Setup(builder, view);
                },
                [](const PassData& data, RHICommandContext& ctx)
                {
                    data.pass->Execute(ctx, *data.viewData);
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
