/** @file RHIAccess.cpp @brief Scoped resource access conversion and diagnostics */

#include "RHI/RHIAccess.h"

#include <sstream>

namespace RVX
{
    namespace
    {
        bool EqualRange(const RHISubresourceRange& lhs, const RHISubresourceRange& rhs)
        {
            return lhs.baseMipLevel == rhs.baseMipLevel &&
                   lhs.mipLevelCount == rhs.mipLevelCount &&
                   lhs.baseArrayLayer == rhs.baseArrayLayer &&
                   lhs.arrayLayerCount == rhs.arrayLayerCount &&
                   lhs.aspect == rhs.aspect;
        }
    } // namespace

    bool RHITextureSubresourceAccessSnapshot::operator==(
        const RHITextureSubresourceAccessSnapshot& other) const
    {
        return EqualRange(range, other.range) && access == other.access;
    }

    RHIExecutionScope GetRHIExecutionScope(RHIShaderStage stages)
    {
        RHIExecutionScope scope = RHIExecutionScope::None;
        if (HasFlag(stages, RHIShaderStage::Vertex))
            scope = scope | RHIExecutionScope::VertexShader;
        if (HasFlag(stages, RHIShaderStage::Hull))
            scope = scope | RHIExecutionScope::HullShader;
        if (HasFlag(stages, RHIShaderStage::Domain))
            scope = scope | RHIExecutionScope::DomainShader;
        if (HasFlag(stages, RHIShaderStage::Geometry))
            scope = scope | RHIExecutionScope::GeometryShader;
        if (HasFlag(stages, RHIShaderStage::Pixel))
            scope = scope | RHIExecutionScope::PixelShader;
        if (HasFlag(stages, RHIShaderStage::Compute))
            scope = scope | RHIExecutionScope::ComputeShader;
        if (HasFlag(stages, RHIShaderStage::Mesh))
            scope = scope | RHIExecutionScope::MeshShader;
        if (HasFlag(stages, RHIShaderStage::Amplification))
            scope = scope | RHIExecutionScope::AmplificationShader;
        if (HasFlag(stages, RHIShaderStage::AllRayTracing))
            scope = scope | RHIExecutionScope::RayTracingShader;
        return scope;
    }

    RHIAccessSnapshot MakeRHIAccessSnapshot(
        RHIResourceState state,
        RHIShaderStage stages,
        GPUQueueDomain domain,
        RHIContentValidity contentValidity)
    {
        RHIAccessSnapshot result;
        result.domain = domain;
        result.contentValidity = contentValidity;

        const RHIExecutionScope shaderScope = GetRHIExecutionScope(stages);
        switch (state)
        {
            case RHIResourceState::Undefined:
                result.contentValidity = RHIContentValidity::Invalid;
                break;
            case RHIResourceState::Common:
                result.executionScope = RHIExecutionScope::AllCommands;
                result.memoryAccess = RHIMemoryAccess::MemoryRead | RHIMemoryAccess::MemoryWrite;
                result.layout = RHIResourceLayout::General;
                break;
            case RHIResourceState::VertexBuffer:
                result.executionScope = RHIExecutionScope::VertexInput;
                result.memoryAccess = RHIMemoryAccess::VertexRead;
                result.layout = RHIResourceLayout::Buffer;
                break;
            case RHIResourceState::IndexBuffer:
                result.executionScope = RHIExecutionScope::VertexInput;
                result.memoryAccess = RHIMemoryAccess::IndexRead;
                result.layout = RHIResourceLayout::Buffer;
                break;
            case RHIResourceState::ConstantBuffer:
                result.executionScope = shaderScope;
                result.memoryAccess = RHIMemoryAccess::ConstantRead;
                result.layout = RHIResourceLayout::Buffer;
                break;
            case RHIResourceState::ShaderResource:
                result.executionScope = shaderScope;
                result.memoryAccess = RHIMemoryAccess::ShaderRead;
                result.layout = RHIResourceLayout::ShaderReadOnly;
                break;
            case RHIResourceState::UnorderedAccess:
                result.executionScope = shaderScope;
                result.memoryAccess = RHIMemoryAccess::ShaderRead | RHIMemoryAccess::ShaderWrite;
                result.layout = RHIResourceLayout::General;
                break;
            case RHIResourceState::RenderTarget:
                result.executionScope = RHIExecutionScope::ColorOutput;
                result.memoryAccess = RHIMemoryAccess::ColorRead | RHIMemoryAccess::ColorWrite;
                result.layout = RHIResourceLayout::ColorAttachment;
                break;
            case RHIResourceState::DepthWrite:
                result.executionScope = RHIExecutionScope::DepthStencil;
                result.memoryAccess = RHIMemoryAccess::DepthStencilRead | RHIMemoryAccess::DepthStencilWrite;
                result.layout = RHIResourceLayout::DepthStencilWrite;
                break;
            case RHIResourceState::DepthRead:
                result.executionScope = RHIExecutionScope::DepthStencil;
                result.memoryAccess = RHIMemoryAccess::DepthStencilRead;
                result.layout = RHIResourceLayout::DepthStencilRead;
                break;
            case RHIResourceState::CopyDest:
                result.executionScope = RHIExecutionScope::Copy;
                result.memoryAccess = RHIMemoryAccess::CopyWrite;
                result.layout = RHIResourceLayout::CopyDestination;
                break;
            case RHIResourceState::CopySource:
                result.executionScope = RHIExecutionScope::Copy;
                result.memoryAccess = RHIMemoryAccess::CopyRead;
                result.layout = RHIResourceLayout::CopySource;
                break;
            case RHIResourceState::Present:
                result.executionScope = RHIExecutionScope::AllCommands;
                result.memoryAccess = RHIMemoryAccess::MemoryRead;
                result.layout = RHIResourceLayout::Present;
                break;
            case RHIResourceState::IndirectArgument:
                result.executionScope = RHIExecutionScope::Indirect;
                result.memoryAccess = RHIMemoryAccess::IndirectRead;
                result.layout = RHIResourceLayout::Buffer;
                break;
            case RHIResourceState::AccelerationStructureBuildRead:
                result.executionScope = RHIExecutionScope::AccelerationStructure;
                result.memoryAccess = RHIMemoryAccess::AccelerationStructureRead;
                result.layout = RHIResourceLayout::Buffer;
                break;
            case RHIResourceState::AccelerationStructureBuildWrite:
                result.executionScope = RHIExecutionScope::AccelerationStructure;
                result.memoryAccess = RHIMemoryAccess::AccelerationStructureWrite;
                result.layout = RHIResourceLayout::Buffer;
                break;
            case RHIResourceState::AccelerationStructureRead:
                result.executionScope = shaderScope | RHIExecutionScope::AccelerationStructure;
                result.memoryAccess = RHIMemoryAccess::AccelerationStructureRead;
                result.layout = RHIResourceLayout::AccelerationStructure;
                break;
            case RHIResourceState::ShaderBindingTable:
                result.executionScope = RHIExecutionScope::RayTracingShader;
                result.memoryAccess = RHIMemoryAccess::ShaderRead;
                result.layout = RHIResourceLayout::ShaderBindingTable;
                break;
        }
        return result;
    }

    RHITextureAccessSnapshot MakeRHITextureAccessSnapshot(
        RHIResourceState state,
        RHIShaderStage stages,
        GPUQueueDomain domain,
        RHIContentValidity contentValidity)
    {
        return {MakeRHIAccessSnapshot(state, stages, domain, contentValidity), {}};
    }

    RHIBufferAccessSnapshot MakeRHIBufferAccessSnapshot(
        RHIResourceState state,
        RHIShaderStage stages,
        GPUQueueDomain domain,
        RHIContentValidity contentValidity)
    {
        return {MakeRHIAccessSnapshot(state, stages, domain, contentValidity), {}};
    }

    RHIResourceState ProjectRHIResourceState(const RHIAccessSnapshot& access)
    {
        switch (access.layout)
        {
            case RHIResourceLayout::Undefined: return RHIResourceState::Undefined;
            case RHIResourceLayout::ShaderReadOnly: return RHIResourceState::ShaderResource;
            case RHIResourceLayout::ColorAttachment: return RHIResourceState::RenderTarget;
            case RHIResourceLayout::DepthStencilWrite: return RHIResourceState::DepthWrite;
            case RHIResourceLayout::DepthStencilRead: return RHIResourceState::DepthRead;
            case RHIResourceLayout::CopySource: return RHIResourceState::CopySource;
            case RHIResourceLayout::CopyDestination: return RHIResourceState::CopyDest;
            case RHIResourceLayout::Present: return RHIResourceState::Present;
            case RHIResourceLayout::AccelerationStructure: return RHIResourceState::AccelerationStructureRead;
            case RHIResourceLayout::ShaderBindingTable: return RHIResourceState::ShaderBindingTable;
            case RHIResourceLayout::Buffer:
                if (HasAnyAccess(access.memoryAccess, RHIMemoryAccess::VertexRead)) return RHIResourceState::VertexBuffer;
                if (HasAnyAccess(access.memoryAccess, RHIMemoryAccess::IndexRead)) return RHIResourceState::IndexBuffer;
                if (HasAnyAccess(access.memoryAccess, RHIMemoryAccess::ConstantRead)) return RHIResourceState::ConstantBuffer;
                if (HasAnyAccess(access.memoryAccess, RHIMemoryAccess::IndirectRead)) return RHIResourceState::IndirectArgument;
                if (HasAnyAccess(access.memoryAccess, RHIMemoryAccess::AccelerationStructureWrite)) return RHIResourceState::AccelerationStructureBuildWrite;
                if (HasAnyAccess(access.memoryAccess, RHIMemoryAccess::AccelerationStructureRead)) return RHIResourceState::AccelerationStructureBuildRead;
                return RHIResourceState::Common;
            case RHIResourceLayout::General:
                if (HasAnyAccess(access.memoryAccess,
                                 RHIMemoryAccess::ShaderRead |
                                 RHIMemoryAccess::ShaderWrite))
                {
                    return RHIResourceState::UnorderedAccess;
                }
                return RHIResourceState::Common;
        }
        return RHIResourceState::Common;
    }

    bool RHIAccessIncludesRead(const RHIAccessSnapshot& access)
    {
        constexpr RHIMemoryAccess reads =
            RHIMemoryAccess::VertexRead | RHIMemoryAccess::IndexRead |
            RHIMemoryAccess::ConstantRead | RHIMemoryAccess::ShaderRead |
            RHIMemoryAccess::ColorRead | RHIMemoryAccess::DepthStencilRead |
            RHIMemoryAccess::CopyRead | RHIMemoryAccess::IndirectRead |
            RHIMemoryAccess::AccelerationStructureRead | RHIMemoryAccess::HostRead |
            RHIMemoryAccess::MemoryRead;
        return HasAnyAccess(access.memoryAccess, reads);
    }

    bool RHIAccessIncludesWrite(const RHIAccessSnapshot& access)
    {
        constexpr RHIMemoryAccess writes =
            RHIMemoryAccess::ShaderWrite | RHIMemoryAccess::ColorWrite |
            RHIMemoryAccess::DepthStencilWrite | RHIMemoryAccess::CopyWrite |
            RHIMemoryAccess::AccelerationStructureWrite | RHIMemoryAccess::HostWrite |
            RHIMemoryAccess::MemoryWrite;
        return HasAnyAccess(access.memoryAccess, writes);
    }

    RHIDependencyKind ClassifyRHIDependency(
        const RHIAccessSnapshot& before,
        const RHIAccessSnapshot& after,
        RHIDiscardIntent discardIntent)
    {
        RHIDependencyKind result = RHIDependencyKind::None;
        if (before.layout != after.layout)
            result = result | RHIDependencyKind::Transition;
        if (before.domain != after.domain)
            result = result | RHIDependencyKind::Ownership;
        const bool beforeReads = RHIAccessIncludesRead(before);
        const bool beforeWrites = RHIAccessIncludesWrite(before);
        const bool afterReads = RHIAccessIncludesRead(after);
        const bool afterWrites = RHIAccessIncludesWrite(after);
        if ((beforeWrites && (afterReads || afterWrites)) ||
            (beforeReads && afterWrites))
        {
            result = result | RHIDependencyKind::Memory;
        }
        if (discardIntent == RHIDiscardIntent::Discard)
            result = result | RHIDependencyKind::Discard;
        return result;
    }

    std::string DescribeRHIAccessSnapshot(const RHIAccessSnapshot& access)
    {
        std::ostringstream stream;
        stream << "scope=0x" << std::hex << static_cast<uint32>(access.executionScope)
               << ",access=0x" << static_cast<uint32>(access.memoryAccess)
               << std::dec << ",layout=" << static_cast<uint32>(access.layout)
               << ",domain=" << GetGPUQueueDomainName(access.domain)
               << ",contents=" << static_cast<uint32>(access.contentValidity);
        return stream.str();
    }

} // namespace RVX
