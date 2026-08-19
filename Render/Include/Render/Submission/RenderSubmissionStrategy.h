#pragma once

/**
 * @file RenderSubmissionStrategy.h
 * @brief Stateless recording strategies for already-prepared render submissions.
 */

#include "RHI/RHICapabilities.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIIndirectExecution.h"

namespace RVX
{
    /** @brief Submission kind accepted by the common strategy interface. */
    enum class RenderSubmissionKind : uint8
    {
        DirectIndexed = 0,
        IndexedIndirect,
        EncodedCommandBuffer,
    };

    /** @brief Stable result classification for strategy-side request handling. */
    enum class RenderSubmissionValidationCode : uint8
    {
        Success = 0,
        InvalidRequest,
        UnsupportedSubmissionKind,
        IndexedIndirectValidationFailed,
    };

    /** @brief Value payload for a direct indexed draw after the pass bound state. */
    struct RenderDirectIndexedSubmissionDesc
    {
        uint32 indexCount = 0;
        uint32 instanceCount = 1;
        uint32 firstIndex = 0;
        int32 vertexOffset = 0;
        uint32 firstInstance = 0;
    };

    /** @brief Value request consumed by a render submission strategy. */
    struct RenderSubmissionRequest
    {
        RenderSubmissionKind kind = RenderSubmissionKind::DirectIndexed;
        RenderDirectIndexedSubmissionDesc directIndexed;
        RHIIndexedIndirectExecutionDesc indexedIndirect;
        RHIEncodedCommandBufferExecutionDesc encodedCommandBuffer;
        const RHICapabilities* capabilities = nullptr;
    };

    /** @brief Common result semantics for direct and indirect recording. */
    struct RenderSubmissionResult
    {
        bool recorded = false;
        uint32 submittedDrawUpperBound = 0;
        bool executedDrawCountAvailable = false;
        uint32 executedDrawCount = 0;
        RenderSubmissionValidationCode validationCode =
            RenderSubmissionValidationCode::Success;
        RHIIndexedIndirectExecutionValidationCode indexedIndirectValidationCode =
            RHIIndexedIndirectExecutionValidationCode::Success;
    };

    /**
     * @brief Records a prepared submission without selecting policy or binding state.
     *
     * Callers own visibility, grouping, materials, pipelines, RenderGraph state,
     * and all bindings; they invoke this only after those decisions are complete.
     */
    class IRenderSubmissionStrategy
    {
    public:
        virtual ~IRenderSubmissionStrategy() = default;

        virtual RenderSubmissionResult Submit(
            RHICommandContext& context,
            const RenderSubmissionRequest& request) const = 0;
    };

    /** @brief Records a single prepared direct indexed draw. */
    class DirectRenderSubmissionStrategy final : public IRenderSubmissionStrategy
    {
    public:
        RenderSubmissionResult Submit(
            RHICommandContext& context,
            const RenderSubmissionRequest& request) const override;
    };

    /** @brief Records one validated standard indexed indirect execution. */
    class IndexedIndirectRenderSubmissionStrategy final : public IRenderSubmissionStrategy
    {
    public:
        RenderSubmissionResult Submit(
            RHICommandContext& context,
            const RenderSubmissionRequest& request) const override;
    };
} // namespace RVX
