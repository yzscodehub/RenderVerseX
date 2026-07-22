#pragma once

/** @file RenderOwnerSnapshotRetirement.h @brief Helpers for completion-safe owner replacement */

#include "Core/Assert.h"
#include "Core/RefCounted.h"
#include "Resources/RenderRetirementQueue.h"

#include <vector>

namespace RVX
{
    using RenderOwnerRetirementList = std::vector<Ref<RefCounted>>;

    /** @brief Move an owner's old strong reference into its pending snapshot list. */
    template<typename T>
    void QueueRenderOwnerRetirement(
        Ref<T>& object,
        RenderOwnerRetirementList& pending)
    {
        if (object)
        {
            pending.emplace_back(std::move(object));
        }
    }

    /** @brief Transfer all pending owner replacements using exact prior-submit evidence. */
    inline void FlushRenderOwnerRetirements(
        RenderOwnerRetirementList& pending,
        const GPUCompletionToken& completion,
        RenderRetirementQueue& retirement)
    {
        for (Ref<RefCounted>& object : pending)
        {
            RenderRetirementEntry entry{completion, std::move(object), 0};
            RVX_ASSERT_MSG(retirement.Enqueue(std::move(entry)),
                           "Owner snapshot retirement transfer failed");
        }
        pending.clear();
    }
} // namespace RVX
