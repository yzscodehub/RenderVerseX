#pragma once

/** @file RenderGraphValidationAccess.h @brief Test-only access to graph internals */

#include "Render/Graph/RenderGraph.h"

#include <utility>

namespace RVX
{
    /**
     * @brief Source-only validation adapter for focused graph contract tests.
     *
     * Production rendering must use RenderGraphCompiler and
     * RenderGraphExecutor. This adapter deliberately does not provide a
     * runtime mode switch and is never linked into engine code.
     */
    class RenderGraphValidationAccess final
    {
    public:
        static void SetDevice(RenderGraph& graph, IRHIDevice* device)
        {
            graph.ConfigureDeviceForValidation(device);
        }

        static void SetTransientResourcePool(
            RenderGraph& graph,
            TransientResourcePool* pool)
        {
            graph.ConfigurePoolForValidation(pool);
        }

        static bool SetQueueExecutionMode(
            RenderGraph& graph,
            RenderGraph::QueueExecutionMode mode)
        {
            return graph.SetQueueExecutionModeForValidation(mode);
        }

        static void SetParallelRecordingEnabled(
            RenderGraph& graph,
            bool enabled)
        {
            graph.SetParallelRecordingEnabledForValidation(enabled);
        }

        static void SetMemoryAliasingEnabled(
            RenderGraph& graph,
            bool enabled)
        {
            graph.SetMemoryAliasingEnabledForValidation(enabled);
        }

        static void Compile(RenderGraph& graph)
        {
            graph.CompilePlanInternal();
        }

        static void Compile(
            RenderGraph& graph,
            const RenderGraphCompileOptions& options)
        {
            graph.CompilePlanInternal(options);
        }

        static void Execute(
            RenderGraph& graph,
            RHICommandContext& context)
        {
            graph.RecordGraphicsPlanInternal(context);
        }

        static bool RecordQueueSubmission(
            RenderGraph& graph,
            RenderGraph::RecordedQueueSubmission& submission)
        {
            return graph.RecordQueueSubmissionInternal(submission);
        }

        static RenderGraphExecution TakeExecution(RenderGraph& graph)
        {
            return graph.TakeExecutionInternal();
        }

        static RenderGraphExecution TakeExecution(
            RenderGraph& graph,
            RenderGraph::RecordedQueueSubmission&& submission)
        {
            return graph.TakeExecutionInternal(std::move(submission));
        }

        static RHITexture* GetTexture(
            const RenderGraph& graph,
            RGTextureHandle handle)
        {
            return graph.ResolveTexture(handle);
        }

        static RHIBuffer* GetBuffer(
            const RenderGraph& graph,
            RGBufferHandle handle)
        {
            return graph.ResolveBuffer(handle);
        }

        static void Reset(RenderGraph& graph)
        {
            graph.ResetDefinitionForValidation();
        }

        static RGTextureHandle ImportTexture(
            RenderGraph& graph,
            RHITexture* texture,
            RHIResourceState initialState)
        {
            return graph.ImportTextureBorrowedForValidation(
                texture,
                MakeRHITextureAccessSnapshot(initialState));
        }

        static RGTextureHandle ImportTexture(
            RenderGraph& graph,
            RHITexture* texture,
            const RHITextureAccessSnapshot& initialAccess)
        {
            return graph.ImportTextureBorrowedForValidation(
                texture,
                initialAccess);
        }

        static RGBufferHandle ImportBuffer(
            RenderGraph& graph,
            RHIBuffer* buffer,
            RHIResourceState initialState)
        {
            return graph.ImportBufferBorrowedForValidation(
                buffer,
                MakeRHIBufferAccessSnapshot(initialState));
        }

        static RGBufferHandle ImportBuffer(
            RenderGraph& graph,
            RHIBuffer* buffer,
            const RHIBufferAccessSnapshot& initialAccess)
        {
            return graph.ImportBufferBorrowedForValidation(
                buffer,
                initialAccess);
        }
    };
} // namespace RVX
