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

    /** @brief Validate one prepared stream before plan consumption. */
    [[nodiscard]] bool ValidateMeshPassPacketStream(
        const MeshPassPacketStream& stream);

    /** @brief Build the exact value identity for one prepared source packet. */
    [[nodiscard]] RenderDrawPacketId BuildRenderDrawPacketId(
        uint64 frameSequence,
        uint32 viewOrdinal,
        RenderPassKind pass,
        uint32 sourcePacketIndex,
        const MeshPassProcessorResult& source) noexcept;

    /** @brief Copy every prepared value consumed by lane validation/recording. */
    [[nodiscard]] RenderPreparedDrawPacketSignature
    BuildRenderPreparedDrawPacketSignature(
        const MeshPassProcessorResult& source) noexcept;

    /** @brief Match a compiled reference to the exact prepared source value. */
    [[nodiscard]] bool RenderDrawPacketReferenceMatchesSource(
        const RenderFrameExecutionPlan& plan,
        const RenderDrawPacketReference& reference,
        const MeshPassProcessorResult& source) noexcept;

    /**
     * @brief Compile one owned per-view plan from resolver output and prepared streams.
     *
     * The function retains no references. Mixed GPU-candidate/Direct depth or
     * opaque plans are compiled as planned hybrid lanes: GPU-sorted candidates,
     * source-ordered directs, and ordered skips.
     */
    [[nodiscard]] RenderFramePlanCompileResult CompileRenderFrameExecutionPlan(
        const RenderPolicyResolution& resolution,
        const SceneMeshPassPreparation& preparation);
} // namespace RVX
