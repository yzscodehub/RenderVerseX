#pragma once

/**
 * @file RenderPolicyDiagnostics.h
 * @brief Value-only diagnostics projection for render policy selection.
 */

#include "Render/Policy/RenderFrameExecutionPlan.h"

namespace RVX
{
    /** @brief Request, selected plan, and observed report for one frame policy. */
    struct RenderPolicyDiagnostics
    {
        bool requestAvailable = false;
        bool planAvailable = false;
        bool reportAvailable = false;
        RenderFramePolicyRequest request{};
        RenderFrameExecutionPlan selectedPlan{};
        RenderFrameExecutionReport executionReport{};
    };
} // namespace RVX
