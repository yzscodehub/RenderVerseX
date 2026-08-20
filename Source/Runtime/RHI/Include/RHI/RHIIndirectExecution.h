#pragma once

/**
 * @file RHIIndirectExecution.h
 * @brief Backend-neutral contract and validation for indexed indirect execution.
 */

#include "RHI/RHIDefinitions.h"

namespace RVX
{
    struct RHICapabilities;
    class RHIBuffer;

    /**
     * @brief Standard indexed indirect command payload shared by all backends.
     *
     * This is the single public layout source for D3D12, Vulkan, Metal, DX11,
     * and OpenGL indexed indirect execution.
     */
    struct IndirectDrawIndexedCommand
    {
        uint32 indexCount = 0;
        uint32 instanceCount = 0;
        uint32 firstIndex = 0;
        int32 vertexOffset = 0;
        uint32 firstInstance = 0;
    };

    static_assert(sizeof(IndirectDrawIndexedCommand) == 20,
                  "Indexed indirect command layout must remain ABI-compatible.");

    /** @brief How an indexed indirect execution obtains its draw count. */
    enum class RHIIndirectExecutionMode : uint8
    {
        FixedCount = 0,
        CountBuffer = 1,
    };

    /** @brief Semantic command payload selected by an indirect command signature. */
    enum class RHIIndirectCommandSemantic : uint8
    {
        Draw = 0,
        DrawIndexed,
        Dispatch,
    };

    /**
     * @brief Pipeline state invalidated by an indirect signature's root arguments.
     *
     * The current standard layouts contain no root arguments, so they must
     * declare None. Backends reject non-None layouts until their corresponding
     * root-argument replay/invalidation behavior is implemented.
     */
    enum class RHIIndirectCommandStateInvalidation : uint8
    {
        None = 0,
        RootConstants = 1u << 0,
        RootDescriptors = 1u << 1,
        DescriptorTables = 1u << 2,
    };

    /** @brief Backend-neutral description of one indirect command payload layout. */
    struct RHIIndirectCommandLayout
    {
        RHIIndirectCommandSemantic semantic = RHIIndirectCommandSemantic::DrawIndexed;
        uint32 commandStride = 0;
        RHIIndirectCommandStateInvalidation stateInvalidation =
            RHIIndirectCommandStateInvalidation::None;

        bool operator==(const RHIIndirectCommandLayout&) const = default;
    };

    /**
     * @brief Typed backend-native encoded-command buffer boundary.
     *
     * This deliberately exposes neither a Metal implementation nor an
     * untyped native pointer. Task 13 may provide a backend object derived
     * from this contract without widening the public RHI with void* semantics.
     */
    class RHIEncodedCommandBuffer
    {
    public:
        virtual ~RHIEncodedCommandBuffer() = default;
        virtual RHIBackendType GetBackendType() const = 0;
    };

    /** @brief Typed future execution request for a backend-native command buffer. */
    struct RHIEncodedCommandBufferExecutionDesc
    {
        RHIEncodedCommandBuffer* commandBuffer = nullptr;

        bool operator==(const RHIEncodedCommandBufferExecutionDesc&) const = default;
    };

    /** @brief Backend-published limits for the standard indexed indirect command layout. */
    struct RHIIndexedIndirectExecutionCapabilities
    {
        bool supportsFixedCount = false;
        bool supportsCountBuffer = false;
        bool supportsFirstInstance = false;
        bool requiresExactCommandStride = false;
        uint32 indexedCommandSize = 0;
        uint32 minCommandStride = 0;
        uint32 commandStrideAlignment = 0;
        uint32 argumentOffsetAlignment = 0;
        uint32 countOffsetAlignment = 0;
        uint32 maxDrawCount = 0;
        uint32 countValueSize = 0;
        RHIResourceState requiredArgumentState = RHIResourceState::IndirectArgument;
        RHIResourceState requiredCountState = RHIResourceState::IndirectArgument;

        bool operator==(const RHIIndexedIndirectExecutionCapabilities&) const = default;
    };

    /**
     * @brief Value descriptor for one standard indexed-indirect execution.
     *
     * CountBuffer executions consume a GPU-produced uint32 count and execute
     * min(gpuCount, maxDrawCount). Validation never reads GPU memory.
     */
    struct RHIIndexedIndirectExecutionDesc
    {
        RHIIndirectExecutionMode mode = RHIIndirectExecutionMode::FixedCount;
        RHIBuffer* argumentBuffer = nullptr;
        uint64 argumentOffset = 0;
        uint32 commandStride = 0;
        uint32 maxDrawCount = 0;
        bool requiresFirstInstance = false;
        RHIResourceState argumentState = RHIResourceState::IndirectArgument;

        RHIBuffer* countBuffer = nullptr;
        uint64 countOffset = 0;
        RHIResourceState countState = RHIResourceState::IndirectArgument;

        bool operator==(const RHIIndexedIndirectExecutionDesc&) const = default;
    };

    /** @brief Stable result code for indexed-indirect descriptor validation. */
    enum class RHIIndexedIndirectExecutionValidationCode : uint8
    {
        Success = 0,
        InvalidMode,
        CapabilityUnsupported,
        FixedCountHasCountBuffer,
        MissingCountBuffer,
        FirstInstanceUnsupported,
        MissingArgumentBuffer,
        ArgumentBufferUsageMissing,
        ArgumentStateInvalid,
        ArgumentOffsetMisaligned,
        CommandStrideTooSmall,
        CommandStrideMisaligned,
        CommandStrideMustMatchCommandSize,
        DrawCountExceedsCapability,
        ArgumentRangeOverflow,
        ArgumentRangeOutOfBounds,
        CountBufferUsageMissing,
        CountStateInvalid,
        CountOffsetMisaligned,
        CountValueSizeInvalid,
        CountRangeOverflow,
        CountRangeOutOfBounds,
    };

    /** @brief Structured, stable descriptor-validation result. */
    struct RHIIndexedIndirectExecutionValidationResult
    {
        RHIIndexedIndirectExecutionValidationCode code =
            RHIIndexedIndirectExecutionValidationCode::Success;
        const char* message = "";

        bool IsValid() const
        {
            return code == RHIIndexedIndirectExecutionValidationCode::Success;
        }

        explicit operator bool() const { return IsValid(); }
    };

    const char* GetRHIIndexedIndirectExecutionValidationCodeName(
        RHIIndexedIndirectExecutionValidationCode code);

    /** @brief Validate an indexed-indirect descriptor against public device capabilities. */
    RHIIndexedIndirectExecutionValidationResult ValidateRHIIndexedIndirectExecutionDesc(
        const RHICapabilities& capabilities,
        const RHIIndexedIndirectExecutionDesc& desc);

    /**
     * @brief Resolve the count semantic without reading a GPU buffer.
     *
     * The caller supplies a count already produced/consumed on the GPU path.
     * Count-buffer execution is always clamped to maxDrawCount.
     */
    uint32 ResolveRHIIndexedIndirectExecutionDrawCount(
        const RHIIndexedIndirectExecutionDesc& desc,
        uint32 gpuCount);
} // namespace RVX
