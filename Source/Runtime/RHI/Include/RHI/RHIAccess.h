#pragma once

/** @file RHIAccess.h @brief Backend-neutral scoped resource access and lifetime snapshots */

#include "RHI/RHIQueueTopology.h"
#include "RHI/RHIResources.h"

#include <string>
#include <vector>

namespace RVX
{
    /** @brief Pipeline execution scopes participating in a resource dependency. */
    enum class RHIExecutionScope : uint32
    {
        None                  = 0,
        VertexInput           = 1u << 0,
        VertexShader          = 1u << 1,
        HullShader            = 1u << 2,
        DomainShader          = 1u << 3,
        GeometryShader        = 1u << 4,
        PixelShader           = 1u << 5,
        ComputeShader         = 1u << 6,
        MeshShader            = 1u << 7,
        AmplificationShader   = 1u << 8,
        RayTracingShader      = 1u << 9,
        ColorOutput           = 1u << 10,
        DepthStencil          = 1u << 11,
        Copy                  = 1u << 12,
        Indirect              = 1u << 13,
        AccelerationStructure = 1u << 14,
        Host                  = 1u << 15,
        AllGraphics           = VertexInput | VertexShader | HullShader |
                                DomainShader | GeometryShader | PixelShader |
                                MeshShader | AmplificationShader |
                                ColorOutput | DepthStencil,
        AllCommands           = 0xFFFFFFFFu,
    };

    /** @brief Memory operations made visible by a resource dependency. */
    enum class RHIMemoryAccess : uint32
    {
        None                       = 0,
        VertexRead                 = 1u << 0,
        IndexRead                  = 1u << 1,
        ConstantRead               = 1u << 2,
        ShaderRead                 = 1u << 3,
        ShaderWrite                = 1u << 4,
        ColorRead                  = 1u << 5,
        ColorWrite                 = 1u << 6,
        DepthStencilRead           = 1u << 7,
        DepthStencilWrite          = 1u << 8,
        CopyRead                   = 1u << 9,
        CopyWrite                  = 1u << 10,
        IndirectRead               = 1u << 11,
        AccelerationStructureRead  = 1u << 12,
        AccelerationStructureWrite = 1u << 13,
        HostRead                   = 1u << 14,
        HostWrite                  = 1u << 15,
        MemoryRead                 = 1u << 16,
        MemoryWrite                = 1u << 17,
    };

    /** @brief Backend-neutral physical usage/layout of a resource. */
    enum class RHIResourceLayout : uint8
    {
        Undefined = 0,
        General,
        Buffer,
        ShaderReadOnly,
        ColorAttachment,
        DepthStencilWrite,
        DepthStencilRead,
        CopySource,
        CopyDestination,
        Present,
        AccelerationStructure,
        ShaderBindingTable,
    };

    /** @brief Whether the resource contents may be consumed by a later read. */
    enum class RHIContentValidity : uint8
    {
        Invalid = 0,
        Valid,
        Unknown,
    };

    /** @brief Intent to preserve or discard prior contents independently of before-layout. */
    enum class RHIDiscardIntent : uint8
    {
        Preserve = 0,
        Discard,
    };

    /** @brief Native dependency categories required by one scoped access change. */
    enum class RHIDependencyKind : uint8
    {
        None       = 0,
        Transition = 1u << 0,
        Memory     = 1u << 1,
        Ownership  = 1u << 2,
        Aliasing   = 1u << 3,
        Discard    = 1u << 4,
    };

    constexpr RHIExecutionScope operator|(RHIExecutionScope lhs, RHIExecutionScope rhs)
    {
        return static_cast<RHIExecutionScope>(static_cast<uint32>(lhs) | static_cast<uint32>(rhs));
    }

    constexpr RHIExecutionScope operator&(RHIExecutionScope lhs, RHIExecutionScope rhs)
    {
        return static_cast<RHIExecutionScope>(static_cast<uint32>(lhs) & static_cast<uint32>(rhs));
    }

    constexpr RHIMemoryAccess operator|(RHIMemoryAccess lhs, RHIMemoryAccess rhs)
    {
        return static_cast<RHIMemoryAccess>(static_cast<uint32>(lhs) | static_cast<uint32>(rhs));
    }

    constexpr RHIMemoryAccess operator&(RHIMemoryAccess lhs, RHIMemoryAccess rhs)
    {
        return static_cast<RHIMemoryAccess>(static_cast<uint32>(lhs) & static_cast<uint32>(rhs));
    }

    constexpr RHIDependencyKind operator|(RHIDependencyKind lhs, RHIDependencyKind rhs)
    {
        return static_cast<RHIDependencyKind>(static_cast<uint8>(lhs) | static_cast<uint8>(rhs));
    }

    constexpr RHIDependencyKind operator&(RHIDependencyKind lhs, RHIDependencyKind rhs)
    {
        return static_cast<RHIDependencyKind>(static_cast<uint8>(lhs) & static_cast<uint8>(rhs));
    }

    constexpr bool HasAnyAccess(RHIMemoryAccess value, RHIMemoryAccess mask)
    {
        return static_cast<uint32>(value & mask) != 0;
    }

    constexpr bool HasDependencyKind(RHIDependencyKind value, RHIDependencyKind mask)
    {
        return static_cast<uint8>(value & mask) != 0;
    }

    /** @brief One uniform realized access at an ownership boundary. */
    struct RHIAccessSnapshot
    {
        RHIExecutionScope executionScope = RHIExecutionScope::None;
        RHIMemoryAccess memoryAccess = RHIMemoryAccess::None;
        RHIResourceLayout layout = RHIResourceLayout::Undefined;
        GPUQueueDomain domain = GPUQueueDomain::Graphics;
        RHIContentValidity contentValidity = RHIContentValidity::Invalid;

        bool operator==(const RHIAccessSnapshot&) const = default;
    };

    struct RHITextureSubresourceAccessSnapshot
    {
        RHISubresourceRange range = RHISubresourceRange::All();
        RHIAccessSnapshot access;

        bool operator==(const RHITextureSubresourceAccessSnapshot& other) const;
    };

    /** @brief Owned texture handoff, including non-uniform subresource accesses. */
    struct RHITextureAccessSnapshot
    {
        RHIAccessSnapshot uniformAccess;
        std::vector<RHITextureSubresourceAccessSnapshot> subresourceOverrides;

        bool operator==(const RHITextureAccessSnapshot&) const = default;
    };

    struct RHIBufferRangeAccessSnapshot
    {
        uint64 offset = 0;
        uint64 size = RVX_WHOLE_SIZE;
        RHIAccessSnapshot access;

        bool operator==(const RHIBufferRangeAccessSnapshot&) const = default;
    };

    /** @brief Owned buffer handoff, including non-uniform byte-range accesses. */
    struct RHIBufferAccessSnapshot
    {
        RHIAccessSnapshot uniformAccess;
        std::vector<RHIBufferRangeAccessSnapshot> rangeOverrides;

        bool operator==(const RHIBufferAccessSnapshot&) const = default;
    };

    RHIExecutionScope GetRHIExecutionScope(RHIShaderStage stages);
    RHIAccessSnapshot MakeRHIAccessSnapshot(
        RHIResourceState state,
        RHIShaderStage stages = RHIShaderStage::All,
        GPUQueueDomain domain = GPUQueueDomain::Graphics,
        RHIContentValidity contentValidity = RHIContentValidity::Valid);
    RHITextureAccessSnapshot MakeRHITextureAccessSnapshot(
        RHIResourceState state,
        RHIShaderStage stages = RHIShaderStage::All,
        GPUQueueDomain domain = GPUQueueDomain::Graphics,
        RHIContentValidity contentValidity = RHIContentValidity::Valid);
    RHIBufferAccessSnapshot MakeRHIBufferAccessSnapshot(
        RHIResourceState state,
        RHIShaderStage stages = RHIShaderStage::All,
        GPUQueueDomain domain = GPUQueueDomain::Graphics,
        RHIContentValidity contentValidity = RHIContentValidity::Valid);

    RHIResourceState ProjectRHIResourceState(const RHIAccessSnapshot& access);
    bool RHIAccessIncludesRead(const RHIAccessSnapshot& access);
    bool RHIAccessIncludesWrite(const RHIAccessSnapshot& access);
    RHIDependencyKind ClassifyRHIDependency(
        const RHIAccessSnapshot& before,
        const RHIAccessSnapshot& after,
        RHIDiscardIntent discardIntent = RHIDiscardIntent::Preserve);
    std::string DescribeRHIAccessSnapshot(const RHIAccessSnapshot& access);

} // namespace RVX
