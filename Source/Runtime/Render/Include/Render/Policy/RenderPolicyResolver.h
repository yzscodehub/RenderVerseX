#pragma once

/**
 * @file RenderPolicyResolver.h
 * @brief Pure, value-only resolution of frame GPU-driven policy.
 */

#include "Render/Policy/RenderFrameExecutionPlan.h"

#include <vector>

namespace RVX
{
    /** @brief Per-view owned facts captured before policy resolution. */
    struct RenderPolicyViewFacts
    {
        bool rendererAllowsGPUDriven = false;
        bool viewAllowsGPUDriven = false;
        bool implementationAvailable = false;
        RenderVisibilityMode requestedVisibility = RenderVisibilityMode::Cpu;
        RenderPolicyReadiness visibilityShaderReadiness =
            RenderPolicyReadiness::Unavailable;
        RenderPolicyReadiness visibilityPipelineReadiness =
            RenderPolicyReadiness::Unavailable;
        RenderPolicyReadiness sharedResourceReadiness =
            RenderPolicyReadiness::Unavailable;
        RenderPolicyReadiness requiredBindingReadiness =
            RenderPolicyReadiness::Unavailable;

        bool operator==(const RenderPolicyViewFacts&) const = default;
    };

    /**
     * @brief Value-only readiness facts for the optional GPU-resident scene tier.
     *
     * These facts describe the current frame's owned implementation, shader,
     * pipeline, resident-resource, and descriptor-binding readiness. They do
     * not retain a GPU-scene lease or any RHI owner.
     */
    struct RenderGPUResidentSceneFacts
    {
        RenderPolicyReadiness implementationReadiness =
            RenderPolicyReadiness::Unavailable;
        RenderPolicyReadiness shaderReadiness =
            RenderPolicyReadiness::Unavailable;
        RenderPolicyReadiness pipelineReadiness =
            RenderPolicyReadiness::Unavailable;
        RenderPolicyReadiness resourceReadiness =
            RenderPolicyReadiness::Unavailable;
        RenderPolicyReadiness bindingReadiness =
            RenderPolicyReadiness::Unavailable;
        uint64 requiredResidentVersion = 0;

        bool operator==(const RenderGPUResidentSceneFacts&) const = default;
    };

    /** @brief Per-pass owned facts captured from scene extraction and readiness probes. */
    struct RenderPassPolicyFacts
    {
        RenderPassKind pass = RenderPassKind::None;
        bool requested = false;
        bool supported = false;
        bool directAllowed = false;
        bool gpuDrivenAllowed = false;
        bool fixedCountIndirectAllowed = false;
        RenderPolicyReadiness directShaderReadiness =
            RenderPolicyReadiness::Unavailable;
        RenderPolicyReadiness directPipelineReadiness =
            RenderPolicyReadiness::Unavailable;
        RenderPolicyReadiness directResourceReadiness =
            RenderPolicyReadiness::Unavailable;
        RenderPolicyReadiness gpuDrivenShaderReadiness =
            RenderPolicyReadiness::Unavailable;
        RenderPolicyReadiness gpuDrivenPipelineReadiness =
            RenderPolicyReadiness::Unavailable;
        RenderPolicyReadiness gpuDrivenResourceReadiness =
            RenderPolicyReadiness::Unavailable;
        uint32 inputPacketCount = 0;
        uint32 relevantPacketCount = 0;
        uint32 candidatePacketCount = 0;
        uint32 directPacketCount = 0;
        uint32 skippedPacketCount = 0;
        uint32 drawGroupCount = 0;
        bool workloadBeneficial = false;

        bool operator==(const RenderPassPolicyFacts&) const = default;
    };

    /**
     * @brief Owned external facts accepted by the resolver.
     *
     * This is deliberately a value snapshot: it must not retain RHI, scene,
     * render-graph, pass, registry, pointer, or span ownership.
     */
    struct RenderPolicyResolverInput
    {
        RenderFramePolicyRequest request{};
        uint32 viewOrdinal = 0;
        RenderPolicyViewFacts view{};
        RenderGPUResidentSceneFacts gpuResidentScene{};
        RenderCapabilitySnapshot capabilities{};
        GPUDrivenBackendQualification qualification{};
        std::vector<RenderPassPolicyFacts> passes{};

        bool operator==(const RenderPolicyResolverInput& other) const
        {
            return request == other.request &&
                   viewOrdinal == other.viewOrdinal &&
                   view == other.view &&
                   gpuResidentScene == other.gpuResidentScene &&
                   capabilities == other.capabilities &&
                   qualification.schemaVersion == other.qualification.schemaVersion &&
                   qualification.backend == other.qualification.backend &&
                   qualification.revision == other.qualification.revision &&
                   qualification.passedGateMask == other.qualification.passedGateMask &&
                   qualification.requiredGateMask == other.qualification.requiredGateMask &&
                   passes == other.passes;
        }
    };

    /** @brief Canonical policy decision for one pass before packet-plan compilation. */
    struct RenderPassPolicyDecision
    {
        RenderPassKind pass = RenderPassKind::None;
        RenderVisibilityMode visibility = RenderVisibilityMode::Cpu;
        RenderSubmissionMode preferredSubmission =
            RenderSubmissionMode::Direct;
        RenderSubmissionMode fallbackSubmission =
            RenderSubmissionMode::Direct;
        RenderPolicyReason reason = RenderPolicyReason::ConservativeDefault;
        RenderPacketPartitionSummary partition{};

        bool operator==(const RenderPassPolicyDecision&) const = default;
    };

    /** @brief Immutable output of the pure policy resolver. */
    struct RenderPolicyResolution
    {
        uint64 frameSequence = 0;
        uint32 viewOrdinal = 0;
        RenderViewPolicy viewPolicy{};
        std::vector<RenderPassPolicyDecision> canonicalPassDecisions{};
        RenderCapabilitySnapshot capabilities{};
        RenderQualificationSnapshot qualification{};

        bool operator==(const RenderPolicyResolution&) const = default;
    };

    /** @brief Validate that external value facts are structurally self-consistent. */
    [[nodiscard]] bool ValidateRenderPolicyResolverInput(
        const RenderPolicyResolverInput& input);

    /** @brief Resolve policy without retaining or dereferencing external owners. */
    [[nodiscard]] RenderPolicyResolution ResolveRenderPolicy(
        const RenderPolicyResolverInput& input);

    /** @brief Validate a canonical resolver result before plan compilation. */
    [[nodiscard]] bool ValidateRenderPolicyResolution(
        const RenderPolicyResolution& resolution);

    /** @brief Validate frame-local packet references and all pass range partitions. */
    [[nodiscard]] bool ValidateRenderFrameExecutionPlan(
        const RenderFrameExecutionPlan& plan);
} // namespace RVX
