#include "DX12IndirectExecution.h"

#include "DX12Common.h"

#include <limits>

namespace RVX
{
    namespace
    {
        bool TryAdd(uint64 left, uint64 right, uint64& result)
        {
            if (left > std::numeric_limits<uint64>::max() - right)
            {
                return false;
            }
            result = left + right;
            return true;
        }

        bool TryMultiply(uint64 left, uint64 right, uint64& result)
        {
            if (left != 0 && right > std::numeric_limits<uint64>::max() / left)
            {
                return false;
            }
            result = left * right;
            return true;
        }
    } // namespace

    uint32 GetDX12IndirectCommandStride(RHIIndirectCommandSemantic semantic)
    {
        switch (semantic)
        {
            case RHIIndirectCommandSemantic::Draw:
                return sizeof(D3D12_DRAW_ARGUMENTS);
            case RHIIndirectCommandSemantic::DrawIndexed:
                return sizeof(IndirectDrawIndexedCommand);
            case RHIIndirectCommandSemantic::Dispatch:
                return sizeof(D3D12_DISPATCH_ARGUMENTS);
            default:
                return 0;
        }
    }

    uint32 NormalizeDX12IndirectCommandStride(
        RHIIndirectCommandSemantic semantic,
        uint32 commandStride)
    {
        return commandStride != 0
            ? commandStride
            : GetDX12IndirectCommandStride(semantic);
    }

    DX12IndirectValidationResult ValidateDX12IndirectArgumentRange(
        const RHIBuffer* buffer,
        uint64 offset,
        uint32 commandCount,
        uint32 commandStride,
        RHIIndirectCommandSemantic semantic)
    {
        if (commandCount == 0)
        {
            return {};
        }

        const uint32 expectedStride = GetDX12IndirectCommandStride(semantic);
        if (expectedStride == 0)
        {
            return {DX12IndirectValidationCode::InvalidSemantic,
                    "indirect command semantic is invalid"};
        }
        if (buffer == nullptr)
        {
            return {DX12IndirectValidationCode::MissingArgumentBuffer,
                    "argument buffer is null"};
        }
        if (!HasFlag(buffer->GetUsage(), RHIBufferUsage::IndirectArgs))
        {
            return {DX12IndirectValidationCode::ArgumentUsageMissing,
                    "argument buffer lacks IndirectArgs usage"};
        }
        if (offset % alignof(uint32) != 0)
        {
            return {DX12IndirectValidationCode::ArgumentOffsetMisaligned,
                    "argument offset is not uint32 aligned"};
        }
        if (commandStride != expectedStride)
        {
            return {DX12IndirectValidationCode::CommandStrideInvalid,
                    "command stride does not match the semantic layout"};
        }

        uint64 lastCommandOffset = 0;
        uint64 argumentEnd = 0;
        if (!TryMultiply(commandStride, commandCount - 1, lastCommandOffset) ||
            !TryAdd(offset, lastCommandOffset, lastCommandOffset) ||
            !TryAdd(lastCommandOffset, expectedStride, argumentEnd))
        {
            return {DX12IndirectValidationCode::ArgumentRangeOverflow,
                    "argument range overflows uint64"};
        }
        if (argumentEnd > buffer->GetSize())
        {
            return {DX12IndirectValidationCode::ArgumentRangeOutOfBounds,
                    "argument range exceeds the buffer"};
        }
        return {};
    }

    DX12IndirectValidationResult ValidateDX12IndirectCountRange(
        const RHIBuffer* countBuffer,
        uint64 countOffset)
    {
        if (countBuffer == nullptr)
        {
            return {DX12IndirectValidationCode::MissingCountBuffer,
                    "count buffer is null"};
        }
        if (!HasFlag(countBuffer->GetUsage(), RHIBufferUsage::IndirectArgs))
        {
            return {DX12IndirectValidationCode::CountUsageMissing,
                    "count buffer lacks IndirectArgs usage"};
        }
        if (countOffset % alignof(uint32) != 0)
        {
            return {DX12IndirectValidationCode::CountOffsetMisaligned,
                    "count offset is not uint32 aligned"};
        }

        uint64 countEnd = 0;
        if (!TryAdd(countOffset, sizeof(uint32), countEnd))
        {
            return {DX12IndirectValidationCode::CountRangeOverflow,
                    "count range overflows uint64"};
        }
        if (countEnd > countBuffer->GetSize())
        {
            return {DX12IndirectValidationCode::CountRangeOutOfBounds,
                    "count range exceeds the buffer"};
        }
        return {};
    }

    DX12IndirectValidationResult ValidateDX12IndirectCommandLayout(
        const RHIIndirectCommandLayout& layout)
    {
        if (layout.stateInvalidation != RHIIndirectCommandStateInvalidation::None)
        {
            return {DX12IndirectValidationCode::StateInvalidationUnsupported,
                    "root-argument state invalidation is not implemented"};
        }
        const uint32 expectedStride = GetDX12IndirectCommandStride(layout.semantic);
        if (expectedStride == 0)
        {
            return {DX12IndirectValidationCode::InvalidSemantic,
                    "indirect command semantic is invalid"};
        }
        if (layout.commandStride != expectedStride)
        {
            return {DX12IndirectValidationCode::CommandStrideInvalid,
                    "command stride does not match the semantic layout"};
        }
        return {};
    }
} // namespace RVX
