#include "Render/Submission/RenderSubmissionStrategy.h"

namespace RVX
{
    RenderSubmissionResult DirectRenderSubmissionStrategy::Submit(
        RHICommandContext& context,
        const RenderSubmissionRequest& request) const
    {
        RenderSubmissionResult result;
        if (request.kind != RenderSubmissionKind::DirectIndexed)
        {
            result.validationCode = RenderSubmissionValidationCode::UnsupportedSubmissionKind;
            return result;
        }

        const RenderDirectIndexedSubmissionDesc& desc = request.directIndexed;
        result.executedDrawCountAvailable = true;
        if (desc.indexCount == 0 || desc.instanceCount == 0)
        {
            return result;
        }

        context.DrawIndexed(desc.indexCount,
                            desc.instanceCount,
                            desc.firstIndex,
                            desc.vertexOffset,
                            desc.firstInstance);
        result.recorded = true;
        result.submittedDrawUpperBound = 1;
        result.executedDrawCount = 1;
        return result;
    }

    RenderSubmissionResult IndexedIndirectRenderSubmissionStrategy::Submit(
        RHICommandContext& context,
        const RenderSubmissionRequest& request) const
    {
        RenderSubmissionResult result;
        if (request.kind != RenderSubmissionKind::IndexedIndirect)
        {
            result.validationCode =
                RenderSubmissionValidationCode::UnsupportedSubmissionKind;
            return result;
        }
        if (request.capabilities == nullptr)
        {
            result.validationCode = RenderSubmissionValidationCode::InvalidRequest;
            return result;
        }

        const RHIIndexedIndirectExecutionDesc& desc = request.indexedIndirect;
        const RHIIndexedIndirectExecutionValidationResult validation =
            ValidateRHIIndexedIndirectExecutionDesc(*request.capabilities, desc);
        result.indexedIndirectValidationCode = validation.code;
        if (!validation)
        {
            result.validationCode =
                RenderSubmissionValidationCode::IndexedIndirectValidationFailed;
            return result;
        }

        result.executedDrawCountAvailable =
            desc.mode == RHIIndirectExecutionMode::FixedCount;
        if (desc.maxDrawCount == 0)
        {
            return result;
        }

        switch (desc.mode)
        {
            case RHIIndirectExecutionMode::FixedCount:
                context.DrawIndexedIndirect(desc.argumentBuffer,
                                            desc.argumentOffset,
                                            desc.maxDrawCount,
                                            desc.commandStride);
                result.recorded = true;
                result.submittedDrawUpperBound = desc.maxDrawCount;
                result.executedDrawCount = desc.maxDrawCount;
                return result;

            case RHIIndirectExecutionMode::CountBuffer:
                context.DrawIndexedIndirectCount(desc.argumentBuffer,
                                                 desc.argumentOffset,
                                                 desc.countBuffer,
                                                 desc.countOffset,
                                                 desc.maxDrawCount,
                                                 desc.commandStride);
                result.recorded = true;
                result.submittedDrawUpperBound = desc.maxDrawCount;
                return result;

            default:
                result.validationCode =
                    RenderSubmissionValidationCode::IndexedIndirectValidationFailed;
                result.indexedIndirectValidationCode =
                    RHIIndexedIndirectExecutionValidationCode::InvalidMode;
                return result;
        }
    }
} // namespace RVX
