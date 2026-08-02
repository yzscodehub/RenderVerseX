#pragma once

/**
 * @file RenderPolicyTypes.h
 * @brief Backend-neutral value types for render-frame policy selection.
 */

#include "Core/Types.h"
#include "Render/GPUDriven/GPUDrivenQualification.h"
#include "RenderContracts/RenderFramePacket.h"
#include "RHI/RHIDefinitions.h"

namespace RVX
{
    /** @brief Requested rendering tier selected by frame policy resolution. */
    enum class GPUDrivenTier : uint8
    {
        Direct = 0,
        IndirectGrouped = 1,
        GPUResidentScene = 2,
        Meshlet = 3,
    };

    inline const char* GetGPUDrivenTierName(GPUDrivenTier tier)
    {
        switch (tier)
        {
            case GPUDrivenTier::Direct: return "Direct";
            case GPUDrivenTier::IndirectGrouped: return "IndirectGrouped";
            case GPUDrivenTier::GPUResidentScene: return "GPUResidentScene";
            case GPUDrivenTier::Meshlet: return "Meshlet";
            default: return "Invalid";
        }
    }

    /** @brief Visibility work selected for a render pass. */
    enum class RenderVisibilityMode : uint8
    {
        Cpu = 0,
        GpuFrustum = 1,
        GpuFrustumAndDistance = 2,
        GpuOcclusion = 3,
    };

    inline const char* GetRenderVisibilityModeName(RenderVisibilityMode mode)
    {
        switch (mode)
        {
            case RenderVisibilityMode::Cpu: return "Cpu";
            case RenderVisibilityMode::GpuFrustum: return "GpuFrustum";
            case RenderVisibilityMode::GpuFrustumAndDistance:
                return "GpuFrustumAndDistance";
            case RenderVisibilityMode::GpuOcclusion: return "GpuOcclusion";
            default: return "Invalid";
        }
    }

    /** @brief Draw submission mechanism selected for a render pass. */
    enum class RenderSubmissionMode : uint8
    {
        Direct = 0,
        FixedCountIndirect = 1,
        MultiDrawIndirectCount = 2,
        EncodedCommandBuffer = 3,
    };

    inline const char* GetRenderSubmissionModeName(RenderSubmissionMode mode)
    {
        switch (mode)
        {
            case RenderSubmissionMode::Direct: return "Direct";
            case RenderSubmissionMode::FixedCountIndirect:
                return "FixedCountIndirect";
            case RenderSubmissionMode::MultiDrawIndirectCount:
                return "MultiDrawIndirectCount";
            case RenderSubmissionMode::EncodedCommandBuffer:
                return "EncodedCommandBuffer";
            default: return "Invalid";
        }
    }

    /** @brief Render pass categories represented in policy plans and reports. */
    enum class RenderPassKind : uint8
    {
        None = 0,
        Depth = 1,
        Opaque = 2,
        Shadow = 3,
        Transparent = 4,
    };

    inline const char* GetRenderPassKindName(RenderPassKind kind)
    {
        switch (kind)
        {
            case RenderPassKind::None: return "None";
            case RenderPassKind::Depth: return "Depth";
            case RenderPassKind::Opaque: return "Opaque";
            case RenderPassKind::Shadow: return "Shadow";
            case RenderPassKind::Transparent: return "Transparent";
            default: return "Invalid";
        }
    }

    /** @brief Completion state reported after a planned pass is recorded. */
    enum class RenderExecutionStatus : uint8
    {
        NotAttempted = 0,
        Completed = 1,
        Failed = 2,
    };

    inline const char* GetRenderExecutionStatusName(RenderExecutionStatus status)
    {
        switch (status)
        {
            case RenderExecutionStatus::NotAttempted: return "NotAttempted";
            case RenderExecutionStatus::Completed: return "Completed";
            case RenderExecutionStatus::Failed: return "Failed";
            default: return "Invalid";
        }
    }

    /** @brief Stable explanation for a policy or execution outcome. */
    enum class RenderPolicyReason : uint8
    {
        None = 0,
        ConservativeDefault = 1,
        InvalidRequest = 2,
        ForcedDirect = 3,
        ForcedGPUDriven = 4,
        BackendUnsupported = 5,
        BackendNotQualified = 6,
        CapabilityUnavailable = 7,
        PipelineUnavailable = 8,
        ResourcesUnavailable = 9,
        PassUnsupported = 10,
        NoEligiblePackets = 11,
        WorkloadNotBeneficial = 12,
        PlannedFallback = 13,
        UnexpectedRecordingFailure = 14,
    };

    inline const char* GetRenderPolicyReasonName(RenderPolicyReason reason)
    {
        switch (reason)
        {
            case RenderPolicyReason::None: return "None";
            case RenderPolicyReason::ConservativeDefault:
                return "ConservativeDefault";
            case RenderPolicyReason::InvalidRequest: return "InvalidRequest";
            case RenderPolicyReason::ForcedDirect: return "ForcedDirect";
            case RenderPolicyReason::ForcedGPUDriven: return "ForcedGPUDriven";
            case RenderPolicyReason::BackendUnsupported:
                return "BackendUnsupported";
            case RenderPolicyReason::BackendNotQualified:
                return "BackendNotQualified";
            case RenderPolicyReason::CapabilityUnavailable:
                return "CapabilityUnavailable";
            case RenderPolicyReason::PipelineUnavailable:
                return "PipelineUnavailable";
            case RenderPolicyReason::ResourcesUnavailable:
                return "ResourcesUnavailable";
            case RenderPolicyReason::PassUnsupported: return "PassUnsupported";
            case RenderPolicyReason::NoEligiblePackets:
                return "NoEligiblePackets";
            case RenderPolicyReason::WorkloadNotBeneficial:
                return "WorkloadNotBeneficial";
            case RenderPolicyReason::PlannedFallback: return "PlannedFallback";
            case RenderPolicyReason::UnexpectedRecordingFailure:
                return "UnexpectedRecordingFailure";
            default: return "Invalid";
        }
    }

    /** @brief Contiguous packet range consumed by one planned render pass. */
    struct DrawPacketRange
    {
        uint32 first = 0;
        uint32 count = 0;
    };

    /** @brief External frame-level rendering policy request. */
    struct RenderFramePolicyRequest
    {
        uint64 frameSequence = 0;
        RenderGPUDrivenMode gpuDrivenMode = RenderGPUDrivenMode::Auto;
    };

    /** @brief Resolved tier for the frame's active view. */
    struct RenderViewPolicy
    {
        RenderGPUDrivenMode requestedMode = RenderGPUDrivenMode::Auto;
        GPUDrivenTier selectedTier = GPUDrivenTier::Direct;
        RenderPolicyReason reason = RenderPolicyReason::ConservativeDefault;
    };

    /** @brief Device capability snapshot consumed during policy resolution. */
    struct RenderCapabilitySnapshot
    {
        RHIBackendType backend = RHIBackendType::None;
        bool supportsComputeVisibility = false;
        bool supportsFixedCountIndirect = false;
        bool supportsIndirectDrawCount = false;
        bool supportsEncodedCommandBuffer = false;
    };

    /** @brief Reviewed backend qualification snapshot consumed by policy. */
    struct RenderQualificationSnapshot
    {
        GPUDrivenQualificationLevel level =
            GPUDrivenQualificationLevel::Unqualified;
        uint32 revision = 0;
        uint64 passedGateMask = 0;
        uint64 requiredGateMask =
            RVX_GPU_DRIVEN_REQUIRED_QUALIFICATION_GATE_MASK;
    };
} // namespace RVX
