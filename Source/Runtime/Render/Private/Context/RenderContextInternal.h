#pragma once

/** @file RenderContextInternal.h @brief Render-private context composition access */

#include "Render/Context/RenderContext.h"

namespace RVX
{
    class RenderSubmissionTracker;

    struct RenderContextInternalAccess
    {
        static RenderSubmissionTracker* GetSubmissionTracker(
            RenderContext& context)
        {
            return context.m_frameSynchronizer.m_submissionTracker.get();
        }
    };
} // namespace RVX
