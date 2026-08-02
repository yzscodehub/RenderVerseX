#pragma once

/**
 * @file RenderFramePlanCompiler.h
 * @brief Pure compilation of prepared mesh-pass streams into a frame plan.
 */

#include "Render/Passes/MeshPassProcessor.h"
#include "Render/Policy/RenderPolicyResolver.h"

namespace RVX
{
    /** @brief Fail-closed result; a plan is publishable only when succeeded. */
    struct RenderFramePlanCompileResult
    {
        bool succeeded = false;
        RenderPolicyReason reason = RenderPolicyReason::InconsistentFacts;
        RenderFrameExecutionPlan plan{};

        bool operator==(const RenderFramePlanCompileResult&) const = default;
    };

    /**
     * @brief Compile one owned per-view plan from resolver output and prepared streams.
     *
     * The function retains no references. Task 5C deliberately compiles a
     * mixed GPU-candidate/Direct pass as whole-pass Direct; Task 7 introduces
     * stable packet identities and true hybrid submission.
     */
    [[nodiscard]] RenderFramePlanCompileResult CompileRenderFrameExecutionPlan(
        const RenderPolicyResolution& resolution,
        const SceneMeshPassPreparation& preparation);
} // namespace RVX
