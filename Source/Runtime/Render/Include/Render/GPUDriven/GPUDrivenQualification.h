#pragma once

/**
 * @file GPUDrivenQualification.h
 * @brief Versioned production-qualification manifest for GPU-driven backends.
 */

#include "Core/Types.h"
#include "RHI/RHIDefinitions.h"

#include <array>

namespace RVX
{
    /** @brief Maturity level derived from the reviewed qualification evidence. */
    enum class GPUDrivenQualificationLevel : uint8
    {
        Unqualified = 0,
        Candidate,
        Qualified,
    };

    inline const char* GetGPUDrivenQualificationLevelName(
        GPUDrivenQualificationLevel level)
    {
        switch (level)
        {
            case GPUDrivenQualificationLevel::Unqualified: return "Unqualified";
            case GPUDrivenQualificationLevel::Candidate: return "Candidate";
            case GPUDrivenQualificationLevel::Qualified: return "Qualified";
            default: return "Invalid";
        }
    }

    /**
     * @brief Stable evidence gates required before Auto may select GPU-driven.
     *
     * A gate is recorded only after its automated test and review evidence have
     * landed. Capability probing remains a separate per-device runtime check.
     */
    enum class GPUDrivenQualificationGate : uint64
    {
        None = 0,
        RHIContractConformance = 1ull << 0,
        ShaderPipelineContracts = 1ull << 1,
        DescriptorIntegrity = 1ull << 2,
        ResourceStateValidation = 1ull << 3,
        MultiBatchMaterialRouting = 1ull << 4,
        IndirectExecutionSmoke = 1ull << 5,
        DirectFallbackSmoke = 1ull << 6,
        DeterministicVisualGolden = 1ull << 7,
        GPUBasedValidation = 1ull << 8,
        RepeatedFrameResize = 1ull << 9,
        CrossPathImageParity = 1ull << 10,
        RealAssetRegression = 1ull << 11,
        AdapterDriverMatrix = 1ull << 12,
    };

    inline const char* GetGPUDrivenQualificationGateName(
        GPUDrivenQualificationGate gate)
    {
        switch (gate)
        {
            case GPUDrivenQualificationGate::None: return "None";
            case GPUDrivenQualificationGate::RHIContractConformance:
                return "RHIContractConformance";
            case GPUDrivenQualificationGate::ShaderPipelineContracts:
                return "ShaderPipelineContracts";
            case GPUDrivenQualificationGate::DescriptorIntegrity:
                return "DescriptorIntegrity";
            case GPUDrivenQualificationGate::ResourceStateValidation:
                return "ResourceStateValidation";
            case GPUDrivenQualificationGate::MultiBatchMaterialRouting:
                return "MultiBatchMaterialRouting";
            case GPUDrivenQualificationGate::IndirectExecutionSmoke:
                return "IndirectExecutionSmoke";
            case GPUDrivenQualificationGate::DirectFallbackSmoke:
                return "DirectFallbackSmoke";
            case GPUDrivenQualificationGate::DeterministicVisualGolden:
                return "DeterministicVisualGolden";
            case GPUDrivenQualificationGate::GPUBasedValidation:
                return "GPUBasedValidation";
            case GPUDrivenQualificationGate::RepeatedFrameResize:
                return "RepeatedFrameResize";
            case GPUDrivenQualificationGate::CrossPathImageParity:
                return "CrossPathImageParity";
            case GPUDrivenQualificationGate::RealAssetRegression:
                return "RealAssetRegression";
            case GPUDrivenQualificationGate::AdapterDriverMatrix:
                return "AdapterDriverMatrix";
            default: return "Invalid";
        }
    }

    inline constexpr std::array<GPUDrivenQualificationGate, 13>
        RVX_GPU_DRIVEN_QUALIFICATION_GATES = {
            GPUDrivenQualificationGate::RHIContractConformance,
            GPUDrivenQualificationGate::ShaderPipelineContracts,
            GPUDrivenQualificationGate::DescriptorIntegrity,
            GPUDrivenQualificationGate::ResourceStateValidation,
            GPUDrivenQualificationGate::MultiBatchMaterialRouting,
            GPUDrivenQualificationGate::IndirectExecutionSmoke,
            GPUDrivenQualificationGate::DirectFallbackSmoke,
            GPUDrivenQualificationGate::DeterministicVisualGolden,
            GPUDrivenQualificationGate::GPUBasedValidation,
            GPUDrivenQualificationGate::RepeatedFrameResize,
            GPUDrivenQualificationGate::CrossPathImageParity,
            GPUDrivenQualificationGate::RealAssetRegression,
            GPUDrivenQualificationGate::AdapterDriverMatrix,
        };

    inline constexpr uint64 GetGPUDrivenQualificationGateMask(
        GPUDrivenQualificationGate gate)
    {
        return static_cast<uint64>(gate);
    }

    inline constexpr uint64 RVX_GPU_DRIVEN_REQUIRED_QUALIFICATION_GATE_MASK =
        (1ull << RVX_GPU_DRIVEN_QUALIFICATION_GATES.size()) - 1ull;

    static_assert(
        GetGPUDrivenQualificationGateMask(
            GPUDrivenQualificationGate::AdapterDriverMatrix) ==
        (1ull << (RVX_GPU_DRIVEN_QUALIFICATION_GATES.size() - 1)),
        "Qualification gate values must remain contiguous and match the gate inventory.");

    inline constexpr uint32 RVX_GPU_DRIVEN_QUALIFICATION_SCHEMA_VERSION = 1;

    /** @brief Reviewed, immutable qualification evidence for one backend. */
    struct GPUDrivenBackendQualification
    {
        uint32 schemaVersion = RVX_GPU_DRIVEN_QUALIFICATION_SCHEMA_VERSION;
        RHIBackendType backend = RHIBackendType::None;
        uint32 revision = 0;
        uint64 passedGateMask = 0;
        uint64 requiredGateMask = RVX_GPU_DRIVEN_REQUIRED_QUALIFICATION_GATE_MASK;

        [[nodiscard]] uint64 GetMissingGateMask() const
        {
            return requiredGateMask & ~passedGateMask;
        }

        [[nodiscard]] bool IsValidManifest() const
        {
            return schemaVersion == RVX_GPU_DRIVEN_QUALIFICATION_SCHEMA_VERSION &&
                   requiredGateMask ==
                       RVX_GPU_DRIVEN_REQUIRED_QUALIFICATION_GATE_MASK &&
                   (passedGateMask & ~requiredGateMask) == 0;
        }

        [[nodiscard]] bool HasPassed(GPUDrivenQualificationGate gate) const
        {
            const uint64 gateMask = GetGPUDrivenQualificationGateMask(gate);
            return gateMask != 0 && (passedGateMask & gateMask) == gateMask;
        }

        [[nodiscard]] bool IsQualified() const
        {
            return IsValidManifest() && revision != 0 &&
                   GetMissingGateMask() == 0;
        }

        [[nodiscard]] GPUDrivenQualificationLevel GetLevel() const
        {
            if (IsQualified())
            {
                return GPUDrivenQualificationLevel::Qualified;
            }
            return IsValidManifest() && revision != 0 && passedGateMask != 0
                ? GPUDrivenQualificationLevel::Candidate
                : GPUDrivenQualificationLevel::Unqualified;
        }
    };

    /**
     * @brief Return the reviewed backend qualification manifest.
     *
     * DX12 has completed the M2 correctness closure and visible-set cross-path
     * parity. Vulkan has completed its Tier-1 native indirect-count, shader,
     * repeated-resize, and Direct/GPU parity slice. Both remain Candidates
     * until the remaining gates close, so Auto stays on the direct path.
     */
    inline GPUDrivenBackendQualification GetGPUDrivenBackendQualification(
        RHIBackendType backend)
    {
        GPUDrivenBackendQualification qualification;
        qualification.backend = backend;

        if (backend == RHIBackendType::DX12)
        {
            qualification.revision = 2;
            qualification.passedGateMask =
                GetGPUDrivenQualificationGateMask(
                    GPUDrivenQualificationGate::RHIContractConformance) |
                GetGPUDrivenQualificationGateMask(
                    GPUDrivenQualificationGate::ShaderPipelineContracts) |
                GetGPUDrivenQualificationGateMask(
                    GPUDrivenQualificationGate::DescriptorIntegrity) |
                GetGPUDrivenQualificationGateMask(
                    GPUDrivenQualificationGate::ResourceStateValidation) |
                GetGPUDrivenQualificationGateMask(
                    GPUDrivenQualificationGate::MultiBatchMaterialRouting) |
                GetGPUDrivenQualificationGateMask(
                    GPUDrivenQualificationGate::IndirectExecutionSmoke) |
                GetGPUDrivenQualificationGateMask(
                    GPUDrivenQualificationGate::DirectFallbackSmoke) |
                GetGPUDrivenQualificationGateMask(
                    GPUDrivenQualificationGate::DeterministicVisualGolden) |
                GetGPUDrivenQualificationGateMask(
                    GPUDrivenQualificationGate::GPUBasedValidation) |
                GetGPUDrivenQualificationGateMask(
                    GPUDrivenQualificationGate::RepeatedFrameResize) |
                GetGPUDrivenQualificationGateMask(
                    GPUDrivenQualificationGate::CrossPathImageParity);
        }
        else if (backend == RHIBackendType::Vulkan)
        {
            qualification.revision = 2;
            qualification.passedGateMask =
                GetGPUDrivenQualificationGateMask(
                    GPUDrivenQualificationGate::RHIContractConformance) |
                GetGPUDrivenQualificationGateMask(
                    GPUDrivenQualificationGate::ShaderPipelineContracts) |
                GetGPUDrivenQualificationGateMask(
                    GPUDrivenQualificationGate::DescriptorIntegrity) |
                GetGPUDrivenQualificationGateMask(
                    GPUDrivenQualificationGate::ResourceStateValidation) |
                GetGPUDrivenQualificationGateMask(
                    GPUDrivenQualificationGate::IndirectExecutionSmoke) |
                GetGPUDrivenQualificationGateMask(
                    GPUDrivenQualificationGate::DirectFallbackSmoke) |
                GetGPUDrivenQualificationGateMask(
                    GPUDrivenQualificationGate::RepeatedFrameResize) |
                GetGPUDrivenQualificationGateMask(
                    GPUDrivenQualificationGate::CrossPathImageParity);
        }

        return qualification;
    }
} // namespace RVX
