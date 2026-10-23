#include "RHI/RHIIndirectExecution.h"

#include "RHI/RHIBuffer.h"
#include "RHI/RHICapabilities.h"

#include <algorithm>
#include <limits>

namespace RVX
{
    namespace
    {
        constexpr bool IsAligned(uint64 value, uint32 alignment)
        {
            return alignment != 0 && value % alignment == 0;
        }

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

        RHIIndexedIndirectExecutionValidationResult Fail(
            RHIIndexedIndirectExecutionValidationCode code,
            const char* message)
        {
            return {code, message};
        }
    } // namespace

    const char* GetRHIIndexedIndirectExecutionValidationCodeName(
        RHIIndexedIndirectExecutionValidationCode code)
    {
        switch (code)
        {
            case RHIIndexedIndirectExecutionValidationCode::Success: return "Success";
            case RHIIndexedIndirectExecutionValidationCode::InvalidMode: return "InvalidMode";
            case RHIIndexedIndirectExecutionValidationCode::CapabilityUnsupported: return "CapabilityUnsupported";
            case RHIIndexedIndirectExecutionValidationCode::FixedCountHasCountBuffer: return "FixedCountHasCountBuffer";
            case RHIIndexedIndirectExecutionValidationCode::MissingCountBuffer: return "MissingCountBuffer";
            case RHIIndexedIndirectExecutionValidationCode::FirstInstanceUnsupported: return "FirstInstanceUnsupported";
            case RHIIndexedIndirectExecutionValidationCode::MissingArgumentBuffer: return "MissingArgumentBuffer";
            case RHIIndexedIndirectExecutionValidationCode::ArgumentBufferUsageMissing: return "ArgumentBufferUsageMissing";
            case RHIIndexedIndirectExecutionValidationCode::ArgumentStateInvalid: return "ArgumentStateInvalid";
            case RHIIndexedIndirectExecutionValidationCode::ArgumentOffsetMisaligned: return "ArgumentOffsetMisaligned";
            case RHIIndexedIndirectExecutionValidationCode::CommandStrideTooSmall: return "CommandStrideTooSmall";
            case RHIIndexedIndirectExecutionValidationCode::CommandStrideMisaligned: return "CommandStrideMisaligned";
            case RHIIndexedIndirectExecutionValidationCode::CommandStrideMustMatchCommandSize: return "CommandStrideMustMatchCommandSize";
            case RHIIndexedIndirectExecutionValidationCode::DrawCountExceedsCapability: return "DrawCountExceedsCapability";
            case RHIIndexedIndirectExecutionValidationCode::ArgumentRangeOverflow: return "ArgumentRangeOverflow";
            case RHIIndexedIndirectExecutionValidationCode::ArgumentRangeOutOfBounds: return "ArgumentRangeOutOfBounds";
            case RHIIndexedIndirectExecutionValidationCode::CountBufferUsageMissing: return "CountBufferUsageMissing";
            case RHIIndexedIndirectExecutionValidationCode::CountStateInvalid: return "CountStateInvalid";
            case RHIIndexedIndirectExecutionValidationCode::CountOffsetMisaligned: return "CountOffsetMisaligned";
            case RHIIndexedIndirectExecutionValidationCode::CountValueSizeInvalid: return "CountValueSizeInvalid";
            case RHIIndexedIndirectExecutionValidationCode::CountRangeOverflow: return "CountRangeOverflow";
            case RHIIndexedIndirectExecutionValidationCode::CountRangeOutOfBounds: return "CountRangeOutOfBounds";
            default: return "Invalid";
        }
    }

    RHIIndexedIndirectExecutionValidationResult ValidateRHIIndexedIndirectExecutionDesc(
        const RHICapabilities& capabilities,
        const RHIIndexedIndirectExecutionDesc& desc)
    {
        const RHIIndexedIndirectExecutionCapabilities& execution =
            capabilities.indexedIndirectExecution;

        switch (desc.mode)
        {
            case RHIIndirectExecutionMode::FixedCount:
                if (desc.countBuffer != nullptr)
                {
                    return Fail(RHIIndexedIndirectExecutionValidationCode::FixedCountHasCountBuffer,
                                "fixed-count execution cannot carry a count buffer");
                }
                break;
            case RHIIndirectExecutionMode::CountBuffer:
                if (desc.countBuffer == nullptr)
                {
                    return Fail(RHIIndexedIndirectExecutionValidationCode::MissingCountBuffer,
                                "count-buffer execution requires a count buffer");
                }
                break;
            default:
                return Fail(RHIIndexedIndirectExecutionValidationCode::InvalidMode,
                            "indexed indirect execution mode is invalid");
        }

        // Zero work is intentionally a legal no-op: it neither dereferences a
        // buffer nor requires the backend to expose an executable draw path.
        if (desc.maxDrawCount == 0)
        {
            return {};
        }

        const bool modeSupported =
            desc.mode == RHIIndirectExecutionMode::FixedCount
                ? execution.supportsFixedCount
                : execution.supportsCountBuffer;
        if (!modeSupported)
        {
            return Fail(RHIIndexedIndirectExecutionValidationCode::CapabilityUnsupported,
                        "selected indexed indirect execution mode is unsupported");
        }
        if (desc.requiresFirstInstance && !execution.supportsFirstInstance)
        {
            return Fail(RHIIndexedIndirectExecutionValidationCode::FirstInstanceUnsupported,
                        "indexed indirect first-instance is unsupported");
        }
        if (desc.argumentBuffer == nullptr)
        {
            return Fail(RHIIndexedIndirectExecutionValidationCode::MissingArgumentBuffer,
                        "indexed indirect execution requires an argument buffer");
        }
        if (!HasFlag(desc.argumentBuffer->GetUsage(), RHIBufferUsage::IndirectArgs))
        {
            return Fail(RHIIndexedIndirectExecutionValidationCode::ArgumentBufferUsageMissing,
                        "argument buffer must include IndirectArgs usage");
        }
        if (desc.argumentState != execution.requiredArgumentState)
        {
            return Fail(RHIIndexedIndirectExecutionValidationCode::ArgumentStateInvalid,
                        "argument buffer must be declared in the required indirect state");
        }
        if (!IsAligned(desc.argumentOffset, execution.argumentOffsetAlignment))
        {
            return Fail(RHIIndexedIndirectExecutionValidationCode::ArgumentOffsetMisaligned,
                        "argument offset does not satisfy indirect execution alignment");
        }
        if (desc.commandStride < execution.minCommandStride ||
            desc.commandStride < execution.indexedCommandSize)
        {
            return Fail(RHIIndexedIndirectExecutionValidationCode::CommandStrideTooSmall,
                        "indexed indirect command stride is smaller than the published minimum");
        }
        if (!IsAligned(desc.commandStride, execution.commandStrideAlignment))
        {
            return Fail(RHIIndexedIndirectExecutionValidationCode::CommandStrideMisaligned,
                        "indexed indirect command stride does not satisfy alignment");
        }
        if (execution.requiresExactCommandStride &&
            desc.commandStride != execution.indexedCommandSize)
        {
            return Fail(RHIIndexedIndirectExecutionValidationCode::CommandStrideMustMatchCommandSize,
                        "indexed indirect command stride must match the backend command signature");
        }
        if (desc.maxDrawCount > execution.maxDrawCount)
        {
            return Fail(RHIIndexedIndirectExecutionValidationCode::DrawCountExceedsCapability,
                        "indexed indirect draw count exceeds the published maximum");
        }

        uint64 lastCommandOffset = 0;
        uint64 argumentEnd = 0;
        if (!TryMultiply(desc.commandStride, desc.maxDrawCount - 1, lastCommandOffset) ||
            !TryAdd(desc.argumentOffset, lastCommandOffset, lastCommandOffset) ||
            !TryAdd(lastCommandOffset, execution.indexedCommandSize, argumentEnd))
        {
            return Fail(RHIIndexedIndirectExecutionValidationCode::ArgumentRangeOverflow,
                        "indexed indirect argument range overflows uint64");
        }
        if (argumentEnd > desc.argumentBuffer->GetSize())
        {
            return Fail(RHIIndexedIndirectExecutionValidationCode::ArgumentRangeOutOfBounds,
                        "indexed indirect argument range exceeds the argument buffer");
        }

        if (desc.mode == RHIIndirectExecutionMode::CountBuffer)
        {
            if (!HasFlag(desc.countBuffer->GetUsage(), RHIBufferUsage::IndirectArgs))
            {
                return Fail(RHIIndexedIndirectExecutionValidationCode::CountBufferUsageMissing,
                            "count buffer must include IndirectArgs usage");
            }
            if (desc.countState != execution.requiredCountState)
            {
                return Fail(RHIIndexedIndirectExecutionValidationCode::CountStateInvalid,
                            "count buffer must be declared in the required indirect state");
            }
            if (!IsAligned(desc.countOffset, execution.countOffsetAlignment))
            {
                return Fail(RHIIndexedIndirectExecutionValidationCode::CountOffsetMisaligned,
                            "count offset does not satisfy indirect execution alignment");
            }
            if (execution.countValueSize != sizeof(uint32))
            {
                return Fail(RHIIndexedIndirectExecutionValidationCode::CountValueSizeInvalid,
                            "count-buffer execution requires a uint32 count value");
            }

            uint64 countEnd = 0;
            if (!TryAdd(desc.countOffset, execution.countValueSize, countEnd))
            {
                return Fail(RHIIndexedIndirectExecutionValidationCode::CountRangeOverflow,
                            "indexed indirect count range overflows uint64");
            }
            if (countEnd > desc.countBuffer->GetSize())
            {
                return Fail(RHIIndexedIndirectExecutionValidationCode::CountRangeOutOfBounds,
                            "indexed indirect count range exceeds the count buffer");
            }
        }

        return {};
    }

    uint32 ResolveRHIIndexedIndirectExecutionDrawCount(
        const RHIIndexedIndirectExecutionDesc& desc,
        uint32 gpuCount)
    {
        switch (desc.mode)
        {
            case RHIIndirectExecutionMode::FixedCount:
                return desc.maxDrawCount;
            case RHIIndirectExecutionMode::CountBuffer:
                return std::min(gpuCount, desc.maxDrawCount);
            default:
                return 0;
        }
    }
} // namespace RVX
