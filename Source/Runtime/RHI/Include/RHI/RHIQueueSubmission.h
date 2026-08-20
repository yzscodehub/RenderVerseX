#pragma once

#include "RHI/RHICommandContext.h"

#include <string>
#include <vector>

namespace RVX
{
    /** @brief One independently submitted queue batch in topological order. */
    struct RHIQueueSubmissionBatch
    {
        RHICommandQueueType queueType = RHICommandQueueType::Graphics;
        std::vector<RHICommandContext*> contexts;
        std::vector<uint32> prerequisiteBatchIndices;
    };

    /**
     * @brief Backend-neutral multi-queue submission DAG.
     *
     * Batches are stored in topological order. Every prerequisite index must
     * refer to an earlier batch. The terminal batch must be the final batch,
     * execute on Graphics, and transitively depend on every other batch so an
     * externally signaled frame fence represents completion of the whole DAG.
     */
    struct RHIQueueSubmissionPlan
    {
        std::vector<RHIQueueSubmissionBatch> batches;
        uint32 terminalGraphicsBatchIndex = RVX_INVALID_INDEX;
    };

    struct RHIQueueSubmissionPlanValidationResult
    {
        bool valid = false;
        std::string message;

        explicit operator bool() const { return valid; }
    };

    /** @brief Validate topology, context ownership, and terminal completion. */
    RHIQueueSubmissionPlanValidationResult ValidateRHIQueueSubmissionPlan(
        const RHIQueueSubmissionPlan& plan);

} // namespace RVX
