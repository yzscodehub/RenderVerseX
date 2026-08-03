#pragma once

/**
 * @file DX12IndirectExecution.h
 * @brief Pure validation helpers shared by DX12 raw indirect entry points and tests.
 */

#include "RHI/RHIBuffer.h"
#include "RHI/RHIIndirectExecution.h"

namespace RVX
{
    enum class DX12IndirectValidationCode : uint8
    {
        Success = 0,
        InvalidSemantic,
        StateInvalidationUnsupported,
        MissingArgumentBuffer,
        ArgumentUsageMissing,
        ArgumentOffsetMisaligned,
        CommandStrideInvalid,
        ArgumentRangeOverflow,
        ArgumentRangeOutOfBounds,
        MissingCountBuffer,
        CountUsageMissing,
        CountOffsetMisaligned,
        CountRangeOverflow,
        CountRangeOutOfBounds,
    };

    struct DX12IndirectValidationResult
    {
        DX12IndirectValidationCode code = DX12IndirectValidationCode::Success;
        const char* message = "";

        bool IsValid() const { return code == DX12IndirectValidationCode::Success; }
        explicit operator bool() const { return IsValid(); }
    };

    uint32 GetDX12IndirectCommandStride(RHIIndirectCommandSemantic semantic);

    uint32 NormalizeDX12IndirectCommandStride(
        RHIIndirectCommandSemantic semantic,
        uint32 commandStride);

    DX12IndirectValidationResult ValidateDX12IndirectArgumentRange(
        const RHIBuffer* buffer,
        uint64 offset,
        uint32 commandCount,
        uint32 commandStride,
        RHIIndirectCommandSemantic semantic);

    DX12IndirectValidationResult ValidateDX12IndirectCountRange(
        const RHIBuffer* countBuffer,
        uint64 countOffset);

    DX12IndirectValidationResult ValidateDX12IndirectCommandLayout(
        const RHIIndirectCommandLayout& layout);
} // namespace RVX
