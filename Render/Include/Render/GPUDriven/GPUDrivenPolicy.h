#pragma once

/**
 * @file GPUDrivenPolicy.h
 * @brief Capability- and qualification-aware GPU-driven path selection.
 */

#include "Render/GPUDriven/GPUDrivenQualification.h"
#include "RenderContracts/RenderFramePacket.h"
#include "RHI/RHICapabilities.h"
#include "RHI/RHIDevice.h"

namespace RVX
{
    /** @brief Stable reason explaining the resolved GPU-driven path. */
    enum class GPUDrivenPolicyReason : uint8
    {
        None = 0,
        InvalidMode,
        ForcedDisabled,
        BackendNotQualified,
        ComputePipelineUnsupported,
        DescriptorSetsUnsupported,
        IndirectDrawCountUnsupported,
        PipelineUnavailable,
    };

    inline const char* GetGPUDrivenPolicyReasonName(GPUDrivenPolicyReason reason)
    {
        switch (reason)
        {
            case GPUDrivenPolicyReason::None: return "None";
            case GPUDrivenPolicyReason::InvalidMode: return "InvalidMode";
            case GPUDrivenPolicyReason::ForcedDisabled: return "ForcedDisabled";
            case GPUDrivenPolicyReason::BackendNotQualified: return "BackendNotQualified";
            case GPUDrivenPolicyReason::ComputePipelineUnsupported: return "ComputePipelineUnsupported";
            case GPUDrivenPolicyReason::DescriptorSetsUnsupported: return "DescriptorSetsUnsupported";
            case GPUDrivenPolicyReason::IndirectDrawCountUnsupported: return "IndirectDrawCountUnsupported";
            case GPUDrivenPolicyReason::PipelineUnavailable: return "PipelineUnavailable";
            default: return "Invalid";
        }
    }

    /** @brief Inputs used to resolve a requested GPU-driven mode. */
    struct GPUDrivenPolicyInput
    {
        RenderGPUDrivenMode requestedMode = RenderGPUDrivenMode::Auto;
        RHIBackendType backend = RHIBackendType::None;
        bool supportsComputePipeline = false;
        bool supportsDescriptorSets = false;
        bool supportsIndirectDrawCount = false;
        bool pipelineReady = false;
    };

    /** @brief Immutable decision consumed by renderer passes and diagnostics. */
    struct GPUDrivenPolicyDecision
    {
        RenderGPUDrivenMode requestedMode = RenderGPUDrivenMode::Auto;
        GPUDrivenPolicyReason reason = GPUDrivenPolicyReason::BackendNotQualified;
        GPUDrivenQualificationLevel qualificationLevel =
            GPUDrivenQualificationLevel::Unqualified;
        uint32 qualificationRevision = 0;
        uint64 passedQualificationGateMask = 0;
        uint64 requiredQualificationGateMask =
            RVX_GPU_DRIVEN_REQUIRED_QUALIFICATION_GATE_MASK;
        uint64 missingQualificationGateMask =
            RVX_GPU_DRIVEN_REQUIRED_QUALIFICATION_GATE_MASK;
        bool enabled = false;
        bool backendQualified = false;
        bool capabilitiesReady = false;
        bool pipelineReady = false;
    };

    /**
     * @brief Whether a backend has passed the real-model production gate.
     *
     * Auto remains quarantined until a backend passes cross-path image
     * equivalence, validation-layer, and real-asset regression gates. Forced
     * mode remains available to develop and validate a quarantined backend.
     */
    inline bool IsGPUDrivenBackendQualified(RHIBackendType backend)
    {
        return GetGPUDrivenBackendQualification(backend).IsQualified();
    }

    /** @brief Resolve the requested mode without silently bypassing requirements. */
    inline GPUDrivenPolicyDecision ResolveGPUDrivenPolicy(
        const GPUDrivenPolicyInput& input)
    {
        GPUDrivenPolicyDecision decision;
        decision.requestedMode = input.requestedMode;
        const GPUDrivenBackendQualification qualification =
            GetGPUDrivenBackendQualification(input.backend);
        decision.qualificationLevel = qualification.GetLevel();
        decision.qualificationRevision = qualification.revision;
        decision.passedQualificationGateMask = qualification.passedGateMask;
        decision.requiredQualificationGateMask = qualification.requiredGateMask;
        decision.missingQualificationGateMask = qualification.GetMissingGateMask();
        decision.backendQualified = qualification.IsQualified();
        decision.pipelineReady = input.pipelineReady;

        switch (input.requestedMode)
        {
            case RenderGPUDrivenMode::Auto:
            case RenderGPUDrivenMode::ForceEnabled:
            case RenderGPUDrivenMode::ForceDisabled:
                break;
            default:
                decision.reason = GPUDrivenPolicyReason::InvalidMode;
                return decision;
        }

        if (input.requestedMode == RenderGPUDrivenMode::ForceDisabled)
        {
            decision.reason = GPUDrivenPolicyReason::ForcedDisabled;
            return decision;
        }

        if (!input.supportsComputePipeline)
        {
            decision.reason = GPUDrivenPolicyReason::ComputePipelineUnsupported;
            return decision;
        }
        if (!input.supportsDescriptorSets)
        {
            decision.reason = GPUDrivenPolicyReason::DescriptorSetsUnsupported;
            return decision;
        }
        if (!input.supportsIndirectDrawCount)
        {
            decision.reason = GPUDrivenPolicyReason::IndirectDrawCountUnsupported;
            return decision;
        }
        decision.capabilitiesReady = true;

        if (input.requestedMode == RenderGPUDrivenMode::Auto &&
            !decision.backendQualified)
        {
            decision.reason = GPUDrivenPolicyReason::BackendNotQualified;
            return decision;
        }

        if (!input.pipelineReady)
        {
            decision.reason = GPUDrivenPolicyReason::PipelineUnavailable;
            return decision;
        }

        decision.enabled = true;
        decision.reason = GPUDrivenPolicyReason::None;
        return decision;
    }

    /** @brief Build policy input from the active device and pipeline state. */
    inline GPUDrivenPolicyInput MakeGPUDrivenPolicyInput(
        RenderGPUDrivenMode mode,
        const IRHIDevice* device,
        bool pipelineReady)
    {
        GPUDrivenPolicyInput input;
        input.requestedMode = mode;
        input.pipelineReady = pipelineReady;
        if (!device)
        {
            return input;
        }

        const RHICapabilities& capabilities = device->GetCapabilities();
        input.backend = device->GetBackendType();
        input.supportsComputePipeline = capabilities.supportsComputePipeline;
        input.supportsDescriptorSets = capabilities.supportsDescriptorSets;
        input.supportsIndirectDrawCount = capabilities.supportsIndirectDrawCount;
        return input;
    }
} // namespace RVX
