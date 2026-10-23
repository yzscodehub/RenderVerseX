#include "RHI/RHIQueueSubmission.h"

#include <algorithm>
#include <unordered_set>

namespace RVX
{
    RHIQueueSubmissionPlanValidationResult ValidateRHIQueueSubmissionPlan(
        const RHIQueueSubmissionPlan& plan)
    {
        RHIQueueSubmissionPlanValidationResult result;
        if (plan.batches.empty())
        {
            result.message = "queue submission plan has no batches";
            return result;
        }
        if (plan.terminalGraphicsBatchIndex >= plan.batches.size())
        {
            result.message = "terminal Graphics batch index is invalid";
            return result;
        }
        if (plan.terminalGraphicsBatchIndex + 1u != plan.batches.size())
        {
            result.message = "terminal Graphics batch must be the final topological batch";
            return result;
        }
        if (plan.batches[plan.terminalGraphicsBatchIndex].queueType !=
            RHICommandQueueType::Graphics)
        {
            result.message = "terminal batch must execute on the Graphics queue";
            return result;
        }

        std::unordered_set<RHICommandContext*> uniqueContexts;
        for (uint32 batchIndex = 0;
             batchIndex < static_cast<uint32>(plan.batches.size());
             ++batchIndex)
        {
            const RHIQueueSubmissionBatch& batch = plan.batches[batchIndex];
            if (static_cast<uint8>(batch.queueType) >
                static_cast<uint8>(RHICommandQueueType::Copy))
            {
                result.message = "queue submission batch has an invalid queue type";
                return result;
            }
            if (batch.contexts.empty())
            {
                result.message = "queue submission batch has no command contexts";
                return result;
            }
            for (RHICommandContext* context : batch.contexts)
            {
                if (!context)
                {
                    result.message = "queue submission batch contains a null command context";
                    return result;
                }
                if (context->GetQueueType() != batch.queueType)
                {
                    result.message = "command context queue does not match its submission batch";
                    return result;
                }
                if (!uniqueContexts.insert(context).second)
                {
                    result.message = "command context appears in more than one submission batch";
                    return result;
                }
            }

            std::unordered_set<uint32> uniquePrerequisites;
            for (uint32 prerequisite : batch.prerequisiteBatchIndices)
            {
                if (prerequisite >= batchIndex)
                {
                    result.message =
                        "queue submission prerequisite must refer to an earlier batch";
                    return result;
                }
                if (!uniquePrerequisites.insert(prerequisite).second)
                {
                    result.message = "queue submission batch contains a duplicate prerequisite";
                    return result;
                }
            }
        }

        std::vector<uint8> reachesTerminal(plan.batches.size(), 0);
        reachesTerminal[plan.terminalGraphicsBatchIndex] = 1;
        std::vector<uint32> stack = {plan.terminalGraphicsBatchIndex};
        while (!stack.empty())
        {
            const uint32 batchIndex = stack.back();
            stack.pop_back();
            for (uint32 prerequisite :
                 plan.batches[batchIndex].prerequisiteBatchIndices)
            {
                if (reachesTerminal[prerequisite] == 0)
                {
                    reachesTerminal[prerequisite] = 1;
                    stack.push_back(prerequisite);
                }
            }
        }
        if (std::find(reachesTerminal.begin(), reachesTerminal.end(), 0) !=
            reachesTerminal.end())
        {
            result.message =
                "terminal Graphics batch does not join every submission branch";
            return result;
        }

        result.valid = true;
        result.message = "queue submission plan is valid";
        return result;
    }

} // namespace RVX
