#include "DX12CommandContext.h"
#include "DX12Device.h"
#include "DX12IndirectExecution.h"
#include "DX12Resources.h"
#include "DX12Pipeline.h"
#include "DX12Query.h"

#include <algorithm>
#include <limits>

namespace RVX
{
    namespace
    {
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS ToD3D12ASBuildFlags(
            RHIAccelerationStructureBuildFlags flags,
            bool performUpdate = false)
        {
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS result =
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;

            if (HasFlag(flags, RHIAccelerationStructureBuildFlags::AllowUpdate))
                result |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
            if (HasFlag(flags, RHIAccelerationStructureBuildFlags::AllowCompaction))
                result |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
            if (HasFlag(flags, RHIAccelerationStructureBuildFlags::PreferFastTrace))
                result |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            if (HasFlag(flags, RHIAccelerationStructureBuildFlags::PreferFastBuild))
                result |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
            if (HasFlag(flags, RHIAccelerationStructureBuildFlags::MinimizeMemory))
                result |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY;
            if (performUpdate)
                result |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;

            return result;
        }

        bool ValidateDX12ASBuildFlags(const RHICapabilities& capabilities,
                                      RHIAccelerationStructureBuildFlags flags,
                                      const char* operation)
        {
            if (HasFlag(flags, RHIAccelerationStructureBuildFlags::AllowCompaction) &&
                !capabilities.supportsAccelerationStructureCompaction)
            {
                RVX_RHI_ERROR("DX12CommandContext: {} rejected because AllowCompaction requires acceleration structure compaction support",
                              operation);
                return false;
            }

            return true;
        }

        bool ValidateDX12RayTracingCommandState(bool isRecording,
                                                bool inRenderPass,
                                                const char* operation)
        {
            if (!isRecording)
            {
                RVX_RHI_ERROR("DX12CommandContext: {} requires active command recording",
                              operation);
                return false;
            }

            if (inRenderPass)
            {
                RVX_RHI_ERROR("DX12CommandContext: {} cannot run inside a render pass",
                              operation);
                return false;
            }

            return true;
        }

        bool ValidateDX12ASBuildResources(const char* operation,
                                          RHIAccelerationStructureType expectedType,
                                          const RHIAccelerationStructureBuildSizes& sizes,
                                          bool update,
                                          DX12AccelerationStructure* dstAS,
                                          DX12AccelerationStructure* srcAS,
                                          DX12Buffer* scratch,
                                          uint64 scratchOffset)
        {
            if (!sizes.IsValid())
            {
                RVX_RHI_ERROR("DX12CommandContext: {} failed because acceleration structure build sizes are invalid",
                              operation);
                return false;
            }

            if (update && !sizes.HasValidUpdateScratchSize())
            {
                RVX_RHI_ERROR("DX12CommandContext: {} update requires valid acceleration structure update scratch size",
                              operation);
                return false;
            }

            if (dstAS->GetType() != expectedType)
            {
                RVX_RHI_ERROR("DX12CommandContext: {} destination AS has the wrong type",
                              operation);
                return false;
            }

            if (update && srcAS && srcAS->GetType() != expectedType)
            {
                RVX_RHI_ERROR("DX12CommandContext: {} source AS has the wrong type for update",
                              operation);
                return false;
            }

            if (dstAS->GetSize() < sizes.accelerationStructureSize)
            {
                RVX_RHI_ERROR("DX12CommandContext: {} destination AS is too small ({} < {})",
                              operation,
                              dstAS->GetSize(),
                              sizes.accelerationStructureSize);
                return false;
            }

            if (update && srcAS && srcAS->GetSize() < sizes.accelerationStructureSize)
            {
                RVX_RHI_ERROR("DX12CommandContext: {} source AS is too small for update ({} < {})",
                              operation,
                              srcAS->GetSize(),
                              sizes.accelerationStructureSize);
                return false;
            }

            if ((scratchOffset % D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT) != 0)
            {
                RVX_RHI_ERROR("DX12CommandContext: {} scratch offset must be {}-byte aligned",
                              operation,
                              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
                return false;
            }

            if (scratch->GetMemoryType() != RHIMemoryType::Default)
            {
                RVX_RHI_ERROR("DX12CommandContext: {} scratch buffer must use default memory",
                              operation);
                return false;
            }

            if (!HasFlag(scratch->GetUsage(), RHIBufferUsage::UnorderedAccess) ||
                !HasFlag(scratch->GetUsage(), RHIBufferUsage::DeviceAddress))
            {
                RVX_RHI_ERROR("DX12CommandContext: {} scratch buffer requires unordered access and device address usage",
                              operation);
                return false;
            }

            const uint64 requiredScratchSize = update ? sizes.updateScratchSize : sizes.buildScratchSize;
            if (requiredScratchSize == 0)
            {
                RVX_RHI_ERROR("DX12CommandContext: {} requires non-zero {} scratch size",
                              operation,
                              update ? "update" : "build");
                return false;
            }

            if (scratch->GetSize() < scratchOffset || scratch->GetSize() - scratchOffset < requiredScratchSize)
            {
                RVX_RHI_ERROR("DX12CommandContext: {} scratch buffer range is too small (available {}, required {})",
                              operation,
                              scratch->GetSize() < scratchOffset ? 0 : scratch->GetSize() - scratchOffset,
                              requiredScratchSize);
                return false;
            }

            return true;
        }

        D3D12_RAYTRACING_GEOMETRY_FLAGS ToD3D12GeometryFlags(RHIRayTracingGeometryFlags flags)
        {
            D3D12_RAYTRACING_GEOMETRY_FLAGS result = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
            if (HasFlag(flags, RHIRayTracingGeometryFlags::Opaque))
                result |= D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            if (HasFlag(flags, RHIRayTracingGeometryFlags::NoDuplicateAnyHitInvocation))
                result |= D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION;
            return result;
        }

        bool TryAddDX12GPUVirtualAddress(D3D12_GPU_VIRTUAL_ADDRESS baseAddress,
                                         uint64 offset,
                                         D3D12_GPU_VIRTUAL_ADDRESS& result)
        {
            result = 0;
            if (baseAddress == 0)
            {
                return false;
            }

            const uint64 baseAddress64 = static_cast<uint64>(baseAddress);
            if (offset > std::numeric_limits<uint64>::max() - baseAddress64)
            {
                return false;
            }

            result = static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(baseAddress64 + offset);
            return result != 0;
        }

        D3D12_GPU_VIRTUAL_ADDRESS GetDX12BufferAddress(RHIBuffer* buffer, uint64 offset = 0)
        {
            auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);
            if (!dx12Buffer || !dx12Buffer->GetResource())
                return 0;

            D3D12_GPU_VIRTUAL_ADDRESS address = 0;
            return TryAddDX12GPUVirtualAddress(dx12Buffer->GetGPUVirtualAddress(), offset, address) ? address : 0;
        }

        bool ValidateDX12BufferAddressRange(const char* operation,
                                            DX12Buffer* buffer,
                                            uint64 offset,
                                            uint64 requiredBytes,
                                            const char* label)
        {
            if (!buffer || !buffer->GetResource() || buffer->GetGPUVirtualAddress() == 0)
            {
                RVX_RHI_ERROR("DX12CommandContext: {} requires a GPU-addressable {}",
                              operation,
                              label);
                return false;
            }

            if (!IsRHIRayTracingBufferRangeValid(*buffer, offset, requiredBytes))
            {
                RVX_RHI_ERROR("DX12CommandContext: {} {} range exceeds buffer size",
                              operation,
                              label);
                return false;
            }

            D3D12_GPU_VIRTUAL_ADDRESS address = 0;
            if (!TryAddDX12GPUVirtualAddress(buffer->GetGPUVirtualAddress(), offset, address))
            {
                RVX_RHI_ERROR("DX12CommandContext: {} {} GPU address overflowed",
                              operation,
                              label);
                return false;
            }

            return true;
        }

        DX12QueryPool* ValidateDX12QueryPoolForContext(
            const char* operation,
            const DX12CommandContext& context,
            bool requiresRecording,
            bool isRecording,
            RHIQueryPool* pool,
            RHIQueryType expectedType)
        {
            if (requiresRecording && !isRecording)
            {
                RVX_RHI_ERROR("DX12CommandContext: {} requires active command recording",
                              operation);
                return nullptr;
            }

            if (pool == nullptr)
            {
                RVX_RHI_ERROR("DX12CommandContext: {} requires a query pool", operation);
                return nullptr;
            }

            auto* dx12Pool = dynamic_cast<DX12QueryPool*>(pool);
            if (dx12Pool == nullptr || dx12Pool->GetHeap() == nullptr)
            {
                RVX_RHI_ERROR("DX12CommandContext: {} requires a live DX12 query pool", operation);
                return nullptr;
            }

            const RHIQueryValidationResult metadataValidation =
                ValidateRHIQueryPoolMetadata(
                    *dx12Pool,
                    expectedType,
                    context.GetQueueType());
            if (!metadataValidation)
            {
                RVX_RHI_ERROR("DX12CommandContext: {} rejected because {}",
                              operation,
                              metadataValidation.message);
                return nullptr;
            }

            return dx12Pool;
        }

        bool ValidateDX12QueryRange(
            const char* operation,
            const RHIQueryPool& pool,
            uint32 firstQuery,
            uint32 queryCount)
        {
            const RHIQueryValidationResult rangeValidation =
                ValidateRHIQueryRange(pool, firstQuery, queryCount);
            if (!rangeValidation)
            {
                RVX_RHI_ERROR("DX12CommandContext: {} rejected because {}",
                              operation,
                              rangeValidation.message);
                return false;
            }

            return true;
        }

        bool ValidateDX12BLASGeometryInputAddresses(const RHIBottomLevelASDesc& desc)
        {
            for (uint32 geometryIndex = 0; geometryIndex < desc.geometries.size(); ++geometryIndex)
            {
                const RHIRayTracingGeometryDesc& geometry = desc.geometries[geometryIndex];
                if (geometry.type == RHIRayTracingGeometryType::Triangles)
                {
                    const RHIRayTracingTrianglesDesc& triangles = geometry.triangles;
                    const uint32 vertexFormatSize = GetFormatBytesPerPixel(triangles.vertexFormat);
                    uint64 vertexRangeBytes = 0;
                    if (!TryGetRHIRayTracingStridedRangeSize(triangles.vertexCount,
                                                             triangles.vertexStride,
                                                             vertexFormatSize,
                                                             vertexRangeBytes))
                    {
                        RVX_RHI_ERROR("DX12CommandContext: BLAS geometry {} vertex range size overflowed",
                                      geometryIndex);
                        return false;
                    }

                    if (!ValidateDX12BufferAddressRange("BLAS geometry input",
                                                        static_cast<DX12Buffer*>(triangles.vertexBuffer),
                                                        triangles.vertexOffset,
                                                        vertexRangeBytes,
                                                        "vertex buffer"))
                    {
                        return false;
                    }

                    if (triangles.indexBuffer)
                    {
                        const uint32 indexElementSize = triangles.indexFormat == RHIFormat::R16_UINT ? 2u : 4u;
                        const uint64 indexRangeBytes = static_cast<uint64>(triangles.indexCount) * indexElementSize;
                        if (!ValidateDX12BufferAddressRange("BLAS geometry input",
                                                            static_cast<DX12Buffer*>(triangles.indexBuffer),
                                                            triangles.indexOffset,
                                                            indexRangeBytes,
                                                            "index buffer"))
                        {
                            return false;
                        }
                    }

                    if (triangles.transformBuffer &&
                        !ValidateDX12BufferAddressRange("BLAS geometry input",
                                                        static_cast<DX12Buffer*>(triangles.transformBuffer),
                                                        triangles.transformOffset,
                                                        sizeof(float) * 12,
                                                        "transform buffer"))
                    {
                        return false;
                    }
                }
                else
                {
                    const RHIRayTracingAABBDesc& aabbs = geometry.aabbs;
                    uint64 aabbRangeBytes = 0;
                    if (!TryGetRHIRayTracingStridedRangeSize(aabbs.count,
                                                             aabbs.stride,
                                                             24,
                                                             aabbRangeBytes))
                    {
                        RVX_RHI_ERROR("DX12CommandContext: BLAS geometry {} AABB range size overflowed",
                                      geometryIndex);
                        return false;
                    }

                    if (!ValidateDX12BufferAddressRange("BLAS geometry input",
                                                        static_cast<DX12Buffer*>(aabbs.aabbBuffer),
                                                        aabbs.offset,
                                                        aabbRangeBytes,
                                                        "AABB buffer"))
                    {
                        return false;
                    }
                }
            }

            return true;
        }

        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> BuildDX12GeometryDescs(
            const RHIBottomLevelASDesc& desc)
        {
            std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
            geometries.reserve(desc.geometries.size());

            for (const RHIRayTracingGeometryDesc& geometry : desc.geometries)
            {
                D3D12_RAYTRACING_GEOMETRY_DESC d3dGeometry = {};
                d3dGeometry.Flags = ToD3D12GeometryFlags(geometry.flags);

                if (geometry.type == RHIRayTracingGeometryType::Triangles)
                {
                    const RHIRayTracingTrianglesDesc& triangles = geometry.triangles;
                    d3dGeometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                    d3dGeometry.Triangles.VertexBuffer.StartAddress =
                        GetDX12BufferAddress(triangles.vertexBuffer, triangles.vertexOffset);
                    d3dGeometry.Triangles.VertexBuffer.StrideInBytes = triangles.vertexStride;
                    d3dGeometry.Triangles.VertexCount = triangles.vertexCount;
                    d3dGeometry.Triangles.VertexFormat = ToDXGIFormat(triangles.vertexFormat);
                    d3dGeometry.Triangles.IndexBuffer =
                        triangles.indexBuffer ? GetDX12BufferAddress(triangles.indexBuffer, triangles.indexOffset) : 0;
                    d3dGeometry.Triangles.IndexCount = triangles.indexBuffer ? triangles.indexCount : 0;
                    d3dGeometry.Triangles.IndexFormat =
                        triangles.indexBuffer ? ToDXGIFormat(triangles.indexFormat) : DXGI_FORMAT_UNKNOWN;
                    d3dGeometry.Triangles.Transform3x4 =
                        triangles.transformBuffer ? GetDX12BufferAddress(triangles.transformBuffer, triangles.transformOffset) : 0;
                }
                else
                {
                    const RHIRayTracingAABBDesc& aabbs = geometry.aabbs;
                    d3dGeometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
                    d3dGeometry.AABBs.AABBCount = aabbs.count;
                    d3dGeometry.AABBs.AABBs.StartAddress = GetDX12BufferAddress(aabbs.aabbBuffer, aabbs.offset);
                    d3dGeometry.AABBs.AABBs.StrideInBytes = aabbs.stride;
                }

                geometries.push_back(d3dGeometry);
            }

            return geometries;
        }

        constexpr bool HasExecutionScope(
            RHIExecutionScope value,
            RHIExecutionScope mask)
        {
            return static_cast<uint32>(value & mask) != 0;
        }

        D3D12_BARRIER_SYNC ToD3D12BarrierSync(RHIExecutionScope scope)
        {
            if (scope == RHIExecutionScope::None)
                return D3D12_BARRIER_SYNC_NONE;
            if (scope == RHIExecutionScope::AllCommands ||
                HasExecutionScope(scope, RHIExecutionScope::Host))
            {
                return D3D12_BARRIER_SYNC_ALL;
            }

            D3D12_BARRIER_SYNC result = D3D12_BARRIER_SYNC_NONE;
            if (HasExecutionScope(scope, RHIExecutionScope::VertexInput))
            {
                result |= D3D12_BARRIER_SYNC_INDEX_INPUT;
                result |= D3D12_BARRIER_SYNC_VERTEX_SHADING;
            }
            if (HasExecutionScope(
                    scope,
                    RHIExecutionScope::VertexShader |
                        RHIExecutionScope::HullShader |
                        RHIExecutionScope::DomainShader |
                        RHIExecutionScope::GeometryShader |
                        RHIExecutionScope::MeshShader |
                        RHIExecutionScope::AmplificationShader))
            {
                result |= D3D12_BARRIER_SYNC_VERTEX_SHADING;
            }
            if (HasExecutionScope(scope, RHIExecutionScope::PixelShader))
                result |= D3D12_BARRIER_SYNC_PIXEL_SHADING;
            if (HasExecutionScope(scope, RHIExecutionScope::ComputeShader))
                result |= D3D12_BARRIER_SYNC_COMPUTE_SHADING;
            if (HasExecutionScope(scope, RHIExecutionScope::RayTracingShader))
                result |= D3D12_BARRIER_SYNC_RAYTRACING;
            if (HasExecutionScope(scope, RHIExecutionScope::ColorOutput))
                result |= D3D12_BARRIER_SYNC_RENDER_TARGET;
            if (HasExecutionScope(scope, RHIExecutionScope::DepthStencil))
                result |= D3D12_BARRIER_SYNC_DEPTH_STENCIL;
            if (HasExecutionScope(scope, RHIExecutionScope::Copy))
                result |= D3D12_BARRIER_SYNC_COPY;
            if (HasExecutionScope(scope, RHIExecutionScope::Indirect))
                result |= D3D12_BARRIER_SYNC_EXECUTE_INDIRECT;
            if (HasExecutionScope(scope, RHIExecutionScope::AccelerationStructure))
            {
                result |= D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE;
                result |= D3D12_BARRIER_SYNC_RAYTRACING;
            }
            return result == D3D12_BARRIER_SYNC_NONE
                ? D3D12_BARRIER_SYNC_ALL
                : result;
        }

        D3D12_BARRIER_ACCESS ToD3D12BarrierAccess(RHIMemoryAccess access)
        {
            if (access == RHIMemoryAccess::None)
                return D3D12_BARRIER_ACCESS_NO_ACCESS;

            D3D12_BARRIER_ACCESS result = D3D12_BARRIER_ACCESS_COMMON;
            if (HasAnyAccess(access, RHIMemoryAccess::VertexRead))
                result |= D3D12_BARRIER_ACCESS_VERTEX_BUFFER;
            if (HasAnyAccess(access, RHIMemoryAccess::IndexRead))
                result |= D3D12_BARRIER_ACCESS_INDEX_BUFFER;
            if (HasAnyAccess(access, RHIMemoryAccess::ConstantRead))
                result |= D3D12_BARRIER_ACCESS_CONSTANT_BUFFER;

            const bool shaderWrite = HasAnyAccess(
                access, RHIMemoryAccess::ShaderWrite);
            if (shaderWrite)
            {
                result |= D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
            }
            else if (HasAnyAccess(access, RHIMemoryAccess::ShaderRead))
            {
                result |= D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
            }

            if (HasAnyAccess(access, RHIMemoryAccess::ColorWrite |
                                         RHIMemoryAccess::ColorRead))
            {
                result |= D3D12_BARRIER_ACCESS_RENDER_TARGET;
            }
            if (HasAnyAccess(access, RHIMemoryAccess::DepthStencilWrite))
            {
                result |= D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
            }
            else if (HasAnyAccess(access, RHIMemoryAccess::DepthStencilRead))
            {
                result |= D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ;
            }
            if (HasAnyAccess(access, RHIMemoryAccess::CopyRead))
                result |= D3D12_BARRIER_ACCESS_COPY_SOURCE;
            if (HasAnyAccess(access, RHIMemoryAccess::CopyWrite))
                result |= D3D12_BARRIER_ACCESS_COPY_DEST;
            if (HasAnyAccess(access, RHIMemoryAccess::IndirectRead))
                result |= D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT;
            if (HasAnyAccess(access, RHIMemoryAccess::AccelerationStructureRead))
            {
                result |= D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ;
            }
            if (HasAnyAccess(access, RHIMemoryAccess::AccelerationStructureWrite))
            {
                result |= D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE;
            }
            return result;
        }

        D3D12_BARRIER_LAYOUT ToD3D12BarrierLayout(
            const RHIAccessSnapshot& access)
        {
            switch (access.layout)
            {
                case RHIResourceLayout::Undefined:
                    return D3D12_BARRIER_LAYOUT_UNDEFINED;
                case RHIResourceLayout::General:
                    return HasAnyAccess(
                               access.memoryAccess,
                               RHIMemoryAccess::ShaderWrite)
                        ? D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS
                        : D3D12_BARRIER_LAYOUT_COMMON;
                case RHIResourceLayout::ShaderReadOnly:
                    return D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
                case RHIResourceLayout::ColorAttachment:
                    return D3D12_BARRIER_LAYOUT_RENDER_TARGET;
                case RHIResourceLayout::DepthStencilWrite:
                    return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
                case RHIResourceLayout::DepthStencilRead:
                    return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ;
                case RHIResourceLayout::CopySource:
                    return D3D12_BARRIER_LAYOUT_COPY_SOURCE;
                case RHIResourceLayout::CopyDestination:
                    return D3D12_BARRIER_LAYOUT_COPY_DEST;
                case RHIResourceLayout::Present:
                    return D3D12_BARRIER_LAYOUT_PRESENT;
                case RHIResourceLayout::Buffer:
                case RHIResourceLayout::AccelerationStructure:
                case RHIResourceLayout::ShaderBindingTable:
                    return D3D12_BARRIER_LAYOUT_COMMON;
            }
            return D3D12_BARRIER_LAYOUT_COMMON;
        }

        RHIAccessSnapshot ResolveBarrierAccess(
            bool hasScopedAccess,
            const RHIAccessSnapshot& scopedAccess,
            RHIResourceState state)
        {
            return hasScopedAccess
                ? scopedAccess
                : MakeRHIAccessSnapshot(state);
        }

        D3D12_RESOURCE_STATES ToD3D12LegacyResourceState(
            RHIResourceState state,
            bool hasScopedAccess,
            const RHIAccessSnapshot& scopedAccess)
        {
            if (!hasScopedAccess || state != RHIResourceState::ShaderResource)
            {
                return ToD3D12ResourceState(state);
            }

            // A legacy compute command list rejects PIXEL_SHADER_RESOURCE.
            // Graphics root descriptor tables, however, are emitted with broad
            // shader visibility and the debug layer requires both read bits at
            // bind time even when the graph access names one graphics stage.
            // Queue-domain projection therefore preserves the established
            // all-graphics state while narrowing only native compute work.
            return scopedAccess.domain == GPUQueueDomain::Compute
                ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                : ToD3D12ResourceState(state);
        }

        void ResolveEnhancedBarrierAccess(
            const RHIAccessSnapshot& before,
            const RHIAccessSnapshot& after,
            RHIDiscardIntent discardIntent,
            D3D12_BARRIER_SYNC& syncBefore,
            D3D12_BARRIER_SYNC& syncAfter,
            D3D12_BARRIER_ACCESS& accessBefore,
            D3D12_BARRIER_ACCESS& accessAfter)
        {
            syncBefore = ToD3D12BarrierSync(before.executionScope);
            syncAfter = ToD3D12BarrierSync(after.executionScope);
            accessBefore = ToD3D12BarrierAccess(before.memoryAccess);
            accessAfter = ToD3D12BarrierAccess(after.memoryAccess);

            // Queue fences make the prior domain complete. The acquire-side
            // barrier only has to establish layout/access for the new domain.
            if (before.domain != after.domain)
            {
                syncBefore = D3D12_BARRIER_SYNC_NONE;
                accessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS;
            }

            if (discardIntent == RHIDiscardIntent::Discard)
            {
                accessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS;
            }

            if (accessBefore != D3D12_BARRIER_ACCESS_NO_ACCESS &&
                syncBefore == D3D12_BARRIER_SYNC_NONE)
            {
                syncBefore = D3D12_BARRIER_SYNC_ALL;
            }
            if (accessAfter != D3D12_BARRIER_ACCESS_NO_ACCESS &&
                syncAfter == D3D12_BARRIER_SYNC_NONE)
            {
                syncAfter = D3D12_BARRIER_SYNC_ALL;
            }
        }

        uint32 GetDX12TexturePlaneCount(RHIFormat format)
        {
            return format == RHIFormat::D24_UNORM_S8_UINT ||
                       format == RHIFormat::D32_FLOAT_S8_UINT
                ? 2u
                : 1u;
        }

        bool ResolveEnhancedSubresourceRange(
            const DX12Texture& texture,
            const RHISubresourceRange& range,
            D3D12_BARRIER_SUBRESOURCE_RANGE& nativeRange)
        {
            const bool allMips = range.mipLevelCount == RVX_ALL_MIPS;
            const bool allLayers = range.arrayLayerCount == RVX_ALL_LAYERS;
            const bool allAspects = range.aspect == RHITextureAspect::Color ||
                                    range.aspect == RHITextureAspect::DepthStencil;
            if (range.baseMipLevel == 0 && allMips &&
                range.baseArrayLayer == 0 && allLayers && allAspects)
            {
                nativeRange.IndexOrFirstMipLevel = 0xffffffffu;
                nativeRange.NumMipLevels = 0;
                return true;
            }

            if (range.baseMipLevel >= texture.GetMipLevels() ||
                range.baseArrayLayer >= texture.GetArraySize())
            {
                RVX_RHI_ERROR("DX12CommandContext: texture barrier subresource range starts outside the texture");
                return false;
            }

            nativeRange.IndexOrFirstMipLevel = range.baseMipLevel;
            nativeRange.NumMipLevels = allMips
                ? texture.GetMipLevels() - range.baseMipLevel
                : std::min(range.mipLevelCount,
                           texture.GetMipLevels() - range.baseMipLevel);
            nativeRange.FirstArraySlice = range.baseArrayLayer;
            nativeRange.NumArraySlices = allLayers
                ? texture.GetArraySize() - range.baseArrayLayer
                : std::min(range.arrayLayerCount,
                           texture.GetArraySize() - range.baseArrayLayer);

            const uint32 planeCount = GetDX12TexturePlaneCount(
                texture.GetFormat());
            switch (range.aspect)
            {
                case RHITextureAspect::Stencil:
                    if (planeCount < 2)
                    {
                        RVX_RHI_ERROR("DX12CommandContext: stencil barrier requested for a format without a stencil plane");
                        return false;
                    }
                    nativeRange.FirstPlane = 1;
                    nativeRange.NumPlanes = 1;
                    break;
                case RHITextureAspect::Depth:
                    nativeRange.FirstPlane = 0;
                    nativeRange.NumPlanes = 1;
                    break;
                case RHITextureAspect::DepthStencil:
                    nativeRange.FirstPlane = 0;
                    nativeRange.NumPlanes = planeCount;
                    break;
                case RHITextureAspect::Color:
                    nativeRange.FirstPlane = 0;
                    nativeRange.NumPlanes = 1;
                    break;
            }
            return nativeRange.NumMipLevels > 0 &&
                   nativeRange.NumArraySlices > 0 &&
                   nativeRange.NumPlanes > 0;
        }

        ID3D12Resource* GetDX12AliasingResource(RHIResource* resource)
        {
            if (auto* buffer = dynamic_cast<DX12Buffer*>(resource))
            {
                return buffer->GetResource();
            }
            if (auto* texture = dynamic_cast<DX12Texture*>(resource))
            {
                return texture->GetResource();
            }
            return nullptr;
        }
    } // namespace

    // =============================================================================
    // Constructor / Destructor
    // =============================================================================
    DX12CommandContext::DX12CommandContext(DX12Device* device, RHICommandQueueType type)
        : m_device(device)
        , m_queueType(type)
    {
        auto d3dDevice = device->GetD3DDevice();

        switch (type)
        {
            case RHICommandQueueType::Graphics: m_listType = D3D12_COMMAND_LIST_TYPE_DIRECT; break;
            case RHICommandQueueType::Compute:  m_listType = D3D12_COMMAND_LIST_TYPE_COMPUTE; break;
            case RHICommandQueueType::Copy:     m_listType = D3D12_COMMAND_LIST_TYPE_COPY; break;
            default: m_listType = D3D12_COMMAND_LIST_TYPE_DIRECT; break;
        }

        m_commandAllocator = device->GetAllocatorPool().Acquire(m_listType);
        RVX_ASSERT_MSG(m_commandAllocator, "DX12CommandContext: Failed to acquire command allocator");
        DX12_CHECK(d3dDevice->CreateCommandList(0, m_listType, m_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList)));

        if (device->GetCapabilities().dx12.barrierDialect ==
            DX12BarrierDialect::Enhanced)
        {
            const HRESULT enhancedResult = m_commandList.As(
                &m_enhancedCommandList);
            RVX_ASSERT_MSG(
                SUCCEEDED(enhancedResult) && m_enhancedCommandList,
                "DX12CommandContext: device declared Enhanced Barriers but ID3D12GraphicsCommandList7 is unavailable");
        }

        // Command list starts in recording state, close it
        m_commandList->Close();
        m_isRecording = false;
    }

    DX12CommandContext::~DX12CommandContext()
    {
    }

    // =============================================================================
    // Lifecycle
    // =============================================================================
    void DX12CommandContext::Begin()
    {
        if (m_isRecording)
        {
            RVX_RHI_WARN("CommandContext::Begin called while already recording");
            return;
        }

        if (!m_commandAllocator)
        {
            m_commandAllocator = m_device->GetAllocatorPool().Acquire(m_listType);
            RVX_ASSERT_MSG(m_commandAllocator, "DX12CommandContext::Begin failed to acquire command allocator");
            if (!m_commandAllocator)
            {
                return;
            }
        }

        DX12_CHECK(m_commandAllocator->Reset());
        DX12_CHECK(m_commandList->Reset(m_commandAllocator.Get(), nullptr));

        m_isRecording = true;
        m_currentPipeline = nullptr;
        m_boundRayTracingDescriptorSetLayouts.clear();

        if (m_queueType != RHICommandQueueType::Copy)
        {
            ID3D12DescriptorHeap* heaps[] = {
                m_device->GetDescriptorHeapManager().GetCbvSrvUavHeap(),
                m_device->GetDescriptorHeapManager().GetSamplerHeap()
            };
            m_commandList->SetDescriptorHeaps(2, heaps);
        }
    }

    void DX12CommandContext::End()
    {
        if (!m_isRecording)
        {
            RVX_RHI_WARN("CommandContext::End called while not recording");
            return;
        }

        FlushBarriers();

        HRESULT hr = m_commandList->Close();
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("m_commandList->Close(): HRESULT = 0x{:08X}", static_cast<uint32>(hr));

            // Dump D3D12 info queue messages
            ComPtr<ID3D12InfoQueue> infoQueue;
            if (SUCCEEDED(m_device->GetD3DDevice()->QueryInterface(IID_PPV_ARGS(&infoQueue))))
            {
                const UINT64 messageCount = infoQueue->GetNumStoredMessages();
                for (UINT64 i = 0; i < messageCount; ++i)
                {
                    SIZE_T messageLength = 0;
                    infoQueue->GetMessage(i, nullptr, &messageLength);
                    if (messageLength == 0) continue;

                    std::vector<char> messageData(messageLength);
                    auto* message = reinterpret_cast<D3D12_MESSAGE*>(messageData.data());
                    if (SUCCEEDED(infoQueue->GetMessage(i, message, &messageLength)))
                    {
                        if (message->pDescription)
                        {
                            RVX_RHI_ERROR("  D3D12: {}", message->pDescription);
                        }
                    }
                }
                infoQueue->ClearStoredMessages();
            }

            RVX_ASSERT(false);
        }
        m_isRecording = false;
    }

    void DX12CommandContext::Reset()
    {
        if (m_isRecording)
        {
            End();
        }
    }

    ComPtr<ID3D12CommandAllocator> DX12CommandContext::DetachCommandAllocator()
    {
        ComPtr<ID3D12CommandAllocator> allocator = m_commandAllocator;
        m_commandAllocator.Reset();
        return allocator;
    }

    // =============================================================================
    // Debug Markers
    // Uses ID3D12GraphicsCommandList::BeginEvent/EndEvent/SetMarker which are
    // compatible with PIX, RenderDoc, and other GPU profilers.
    // These APIs are available on Windows SDK and don't require the PIX header.
    // =============================================================================
    void DX12CommandContext::BeginEvent(const char* name, uint32 color)
    {
        if (!name) return;

        // Convert ARGB color to PIX-compatible format (already ARGB)
        // PIX uses ARGB format where the upper byte is alpha
        UINT64 pixColor = static_cast<UINT64>(color);

        wchar_t wname[256];
        MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 256);

        // Use the built-in D3D12 debug marker API
        // This is picked up by PIX, RenderDoc, NSight, etc.
        m_commandList->BeginEvent(static_cast<UINT>(pixColor), wname,
            static_cast<UINT>((wcslen(wname) + 1) * sizeof(wchar_t)));
    }

    void DX12CommandContext::EndEvent()
    {
        m_commandList->EndEvent();
    }

    void DX12CommandContext::SetMarker(const char* name, uint32 color)
    {
        if (!name) return;

        UINT64 pixColor = static_cast<UINT64>(color);

        wchar_t wname[256];
        MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 256);

        m_commandList->SetMarker(static_cast<UINT>(pixColor), wname,
            static_cast<UINT>((wcslen(wname) + 1) * sizeof(wchar_t)));
    }

    // =============================================================================
    // Resource Barriers
    // =============================================================================
    bool DX12CommandContext::UsesEnhancedBarriers() const
    {
        return m_enhancedCommandList &&
               m_device &&
               m_device->GetCapabilities().dx12.barrierDialect ==
                   DX12BarrierDialect::Enhanced;
    }

    void DX12CommandContext::QueueEnhancedBarrier(
        const D3D12_BUFFER_BARRIER& barrier)
    {
        if (!m_pendingTextureBarriers.empty() ||
            !m_pendingGlobalBarriers.empty())
        {
            FlushBarriers();
        }
        m_pendingBufferBarriers.push_back(barrier);
    }

    void DX12CommandContext::QueueEnhancedBarrier(
        const D3D12_TEXTURE_BARRIER& barrier)
    {
        if (!m_pendingBufferBarriers.empty() ||
            !m_pendingGlobalBarriers.empty())
        {
            FlushBarriers();
        }
        m_pendingTextureBarriers.push_back(barrier);
    }

    void DX12CommandContext::QueueEnhancedBarrier(
        const D3D12_GLOBAL_BARRIER& barrier)
    {
        if (!m_pendingBufferBarriers.empty() ||
            !m_pendingTextureBarriers.empty())
        {
            FlushBarriers();
        }
        m_pendingGlobalBarriers.push_back(barrier);
    }

    void DX12CommandContext::BufferBarrier(const RHIBufferBarrier& barrier)
    {
        if (!barrier.buffer)
        {
            return;
        }

        auto* dx12Buffer = static_cast<DX12Buffer*>(barrier.buffer);
        if (!dx12Buffer || !dx12Buffer->GetResource())
        {
            return;
        }

        // D3D12 fixes committed Upload and Readback resources in
        // GENERIC_READ and COPY_DEST respectively. They cannot participate in
        // state transitions (including the Common split used for cross-queue
        // ownership). Queue Signal/Wait still provides the required ordering,
        // so the physical barrier is intentionally empty for these heaps.
        if (dx12Buffer->GetMemoryType() != RHIMemoryType::Default)
        {
            if (barrier.hasScopedAccess &&
                barrier.accessBefore.domain != barrier.accessAfter.domain)
            {
                GPUQueueDomain contextDomain = GPUQueueDomain::Graphics;
                if (!TryGetGPUQueueDomain(
                        m_device->GetCapabilities().queueTopology,
                        GetQueueType(),
                        contextDomain))
                {
                    RVX_RHI_ERROR(
                        "DX12 fixed-state buffer ownership barrier used an invalid queue domain");
                    return;
                }
                if (contextDomain != barrier.accessBefore.domain &&
                    contextDomain != barrier.accessAfter.domain)
                {
                    RVX_RHI_ERROR(
                        "DX12 fixed-state buffer ownership barrier recorded on an unrelated queue");
                }
            }
            return;
        }

        RHIAccessSnapshot scopedBefore = barrier.accessBefore;
        RHIAccessSnapshot scopedAfter = barrier.accessAfter;
        RHIResourceState legacyBefore = barrier.stateBefore;
        RHIResourceState legacyAfter = barrier.stateAfter;

        if (barrier.hasScopedAccess &&
            barrier.accessBefore.domain != barrier.accessAfter.domain)
        {
            GPUQueueDomain contextDomain = GPUQueueDomain::Graphics;
            if (!TryGetGPUQueueDomain(
                    m_device->GetCapabilities().queueTopology,
                    GetQueueType(),
                    contextDomain))
            {
                RVX_RHI_ERROR("DX12 buffer ownership barrier used an invalid queue domain");
                return;
            }
            if (contextDomain == barrier.accessBefore.domain)
            {
                scopedAfter = MakeRHIAccessSnapshot(
                    RHIResourceState::Common,
                    RHIShaderStage::None,
                    contextDomain,
                    barrier.accessBefore.contentValidity);
                legacyAfter = RHIResourceState::Common;
            }
            else if (contextDomain == barrier.accessAfter.domain)
            {
                scopedBefore = MakeRHIAccessSnapshot(
                    RHIResourceState::Common,
                    RHIShaderStage::None,
                    contextDomain,
                    barrier.accessBefore.contentValidity);
                legacyBefore = RHIResourceState::Common;
            }
            else
            {
                RVX_RHI_ERROR("DX12 buffer ownership barrier recorded on an unrelated queue");
                return;
            }
        }

        if (UsesEnhancedBarriers())
        {
            if (barrier.hasScopedAccess &&
                barrier.dependencyKind == RHIDependencyKind::None)
            {
                return;
            }
            if (!barrier.hasScopedAccess && legacyBefore == legacyAfter)
            {
                return;
            }

            const RHIAccessSnapshot before = ResolveBarrierAccess(
                barrier.hasScopedAccess,
                scopedBefore,
                legacyBefore);
            const RHIAccessSnapshot after = ResolveBarrierAccess(
                barrier.hasScopedAccess,
                scopedAfter,
                legacyAfter);

            D3D12_BUFFER_BARRIER nativeBarrier = {};
            ResolveEnhancedBarrierAccess(
                before,
                after,
                barrier.discardIntent,
                nativeBarrier.SyncBefore,
                nativeBarrier.SyncAfter,
                nativeBarrier.AccessBefore,
                nativeBarrier.AccessAfter);
            nativeBarrier.pResource = dx12Buffer->GetResource();
            const uint64 bufferSize = dx12Buffer->GetSize();
            if (barrier.offset >= bufferSize)
            {
                RVX_RHI_ERROR(
                    "DX12 enhanced buffer barrier range starts beyond the resource (offset={}, size={})",
                    barrier.offset,
                    bufferSize);
                return;
            }
            const uint64 remainingSize = bufferSize - barrier.offset;
            const uint64 requestedSize =
                barrier.size == RVX_WHOLE_SIZE
                    ? remainingSize
                    : std::min(barrier.size, remainingSize);
            if (requestedSize == 0)
            {
                RVX_RHI_ERROR("DX12 enhanced buffer barrier resolved to an empty range");
                return;
            }
            nativeBarrier.Offset = barrier.offset;
            nativeBarrier.Size = requestedSize;
            QueueEnhancedBarrier(nativeBarrier);
            return;
        }

        const D3D12_RESOURCE_STATES nativeBefore =
            ToD3D12LegacyResourceState(
                legacyBefore, barrier.hasScopedAccess, scopedBefore);
        const D3D12_RESOURCE_STATES nativeAfter =
            ToD3D12LegacyResourceState(
                legacyAfter, barrier.hasScopedAccess, scopedAfter);
        if (nativeBefore == nativeAfter)
        {
            if (!barrier.hasScopedAccess ||
                !HasDependencyKind(barrier.dependencyKind, RHIDependencyKind::Memory) ||
                !HasAnyAccess(
                    scopedBefore.memoryAccess |
                        scopedAfter.memoryAccess,
                    RHIMemoryAccess::ShaderWrite))
            {
                return;
            }

            D3D12_RESOURCE_BARRIER uavBarrier = {};
            uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uavBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            uavBarrier.UAV.pResource = dx12Buffer->GetResource();
            m_pendingLegacyBarriers.push_back(uavBarrier);
            return;
        }

        D3D12_RESOURCE_BARRIER d3dBarrier = {};
        d3dBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        d3dBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        d3dBarrier.Transition.pResource = dx12Buffer->GetResource();
        d3dBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        d3dBarrier.Transition.StateBefore = nativeBefore;
        d3dBarrier.Transition.StateAfter = nativeAfter;

        m_pendingLegacyBarriers.push_back(d3dBarrier);
    }

    void DX12CommandContext::TextureBarrier(const RHITextureBarrier& barrier)
    {
        if (!barrier.texture)
        {
            return;
        }

        auto* dx12Texture = static_cast<DX12Texture*>(barrier.texture);
        if (!dx12Texture || !dx12Texture->GetResource())
        {
            return;
        }

        RHIAccessSnapshot scopedBefore = barrier.accessBefore;
        RHIAccessSnapshot scopedAfter = barrier.accessAfter;
        RHIResourceState legacyBefore = barrier.stateBefore;
        RHIResourceState legacyAfter = barrier.stateAfter;

        if (barrier.hasScopedAccess &&
            barrier.accessBefore.domain != barrier.accessAfter.domain)
        {
            GPUQueueDomain contextDomain = GPUQueueDomain::Graphics;
            if (!TryGetGPUQueueDomain(
                    m_device->GetCapabilities().queueTopology,
                    GetQueueType(),
                    contextDomain))
            {
                RVX_RHI_ERROR("DX12 texture ownership barrier used an invalid queue domain");
                return;
            }
            if (contextDomain == barrier.accessBefore.domain)
            {
                scopedAfter = MakeRHIAccessSnapshot(
                    RHIResourceState::Common,
                    RHIShaderStage::None,
                    contextDomain,
                    barrier.accessBefore.contentValidity);
                legacyAfter = RHIResourceState::Common;
            }
            else if (contextDomain == barrier.accessAfter.domain)
            {
                scopedBefore = MakeRHIAccessSnapshot(
                    RHIResourceState::Common,
                    RHIShaderStage::None,
                    contextDomain,
                    barrier.accessBefore.contentValidity);
                legacyBefore = RHIResourceState::Common;
            }
            else
            {
                RVX_RHI_ERROR("DX12 texture ownership barrier recorded on an unrelated queue");
                return;
            }
        }

        if (UsesEnhancedBarriers())
        {
            if (barrier.hasScopedAccess &&
                barrier.dependencyKind == RHIDependencyKind::None)
            {
                return;
            }
            if (!barrier.hasScopedAccess && legacyBefore == legacyAfter)
            {
                return;
            }

            const RHIAccessSnapshot before = ResolveBarrierAccess(
                barrier.hasScopedAccess,
                scopedBefore,
                legacyBefore);
            const RHIAccessSnapshot after = ResolveBarrierAccess(
                barrier.hasScopedAccess,
                scopedAfter,
                legacyAfter);

            D3D12_TEXTURE_BARRIER nativeBarrier = {};
            ResolveEnhancedBarrierAccess(
                before,
                after,
                barrier.discardIntent,
                nativeBarrier.SyncBefore,
                nativeBarrier.SyncAfter,
                nativeBarrier.AccessBefore,
                nativeBarrier.AccessAfter);
            nativeBarrier.LayoutBefore =
                barrier.discardIntent == RHIDiscardIntent::Discard
                ? D3D12_BARRIER_LAYOUT_UNDEFINED
                : ToD3D12BarrierLayout(before);
            nativeBarrier.LayoutAfter = ToD3D12BarrierLayout(after);
            nativeBarrier.pResource = dx12Texture->GetResource();
            nativeBarrier.Flags =
                barrier.discardIntent == RHIDiscardIntent::Discard
                ? D3D12_TEXTURE_BARRIER_FLAG_DISCARD
                : D3D12_TEXTURE_BARRIER_FLAG_NONE;
            if (!ResolveEnhancedSubresourceRange(
                    *dx12Texture,
                    barrier.subresourceRange,
                    nativeBarrier.Subresources))
            {
                return;
            }
            QueueEnhancedBarrier(nativeBarrier);
            return;
        }

        const D3D12_RESOURCE_STATES nativeBefore =
            ToD3D12LegacyResourceState(
                legacyBefore, barrier.hasScopedAccess, scopedBefore);
        const D3D12_RESOURCE_STATES nativeAfter =
            ToD3D12LegacyResourceState(
                legacyAfter, barrier.hasScopedAccess, scopedAfter);
        if (nativeBefore == nativeAfter)
        {
            if (!barrier.hasScopedAccess ||
                !HasDependencyKind(barrier.dependencyKind, RHIDependencyKind::Memory) ||
                !HasAnyAccess(
                    scopedBefore.memoryAccess |
                        scopedAfter.memoryAccess,
                    RHIMemoryAccess::ShaderWrite))
            {
                return;
            }

            D3D12_RESOURCE_BARRIER uavBarrier = {};
            uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uavBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            uavBarrier.UAV.pResource = dx12Texture->GetResource();
            m_pendingLegacyBarriers.push_back(uavBarrier);
            return;
        }

        D3D12_RESOURCE_BARRIER d3dBarrier = {};
        d3dBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        d3dBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        d3dBarrier.Transition.pResource = dx12Texture->GetResource();
        d3dBarrier.Transition.StateBefore = nativeBefore;
        d3dBarrier.Transition.StateAfter = nativeAfter;

        // Handle subresource range
        const auto& range = barrier.subresourceRange;
        if (range.mipLevelCount == RVX_ALL_MIPS && range.arrayLayerCount == RVX_ALL_LAYERS)
        {
            d3dBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            m_pendingLegacyBarriers.push_back(d3dBarrier);
        }
        else
        {
            uint32 mipCount = (range.mipLevelCount == RVX_ALL_MIPS)
                ? dx12Texture->GetMipLevels() - range.baseMipLevel
                : range.mipLevelCount;
            uint32 layerCount = (range.arrayLayerCount == RVX_ALL_LAYERS)
                ? dx12Texture->GetArraySize() - range.baseArrayLayer
                : range.arrayLayerCount;

            for (uint32 layer = 0; layer < layerCount; ++layer)
            {
                for (uint32 mip = 0; mip < mipCount; ++mip)
                {
                    d3dBarrier.Transition.Subresource = dx12Texture->GetSubresourceIndex(
                        range.baseMipLevel + mip,
                        range.baseArrayLayer + layer);
                    m_pendingLegacyBarriers.push_back(d3dBarrier);
                }
            }
        }
    }

    void DX12CommandContext::Barriers(
        std::span<const RHIBufferBarrier> bufferBarriers,
        std::span<const RHITextureBarrier> textureBarriers)
    {
        for (const auto& barrier : bufferBarriers)
        {
            BufferBarrier(barrier);
        }
        for (const auto& barrier : textureBarriers)
        {
            TextureBarrier(barrier);
        }
    }

    void DX12CommandContext::FlushBarriers()
    {
        if (!m_pendingLegacyBarriers.empty())
        {
            m_commandList->ResourceBarrier(
                static_cast<UINT>(m_pendingLegacyBarriers.size()),
                m_pendingLegacyBarriers.data());
            m_pendingLegacyBarriers.clear();
        }

        if (!UsesEnhancedBarriers())
        {
            return;
        }

        std::array<D3D12_BARRIER_GROUP, 3> groups{};
        uint32 groupCount = 0;
        if (!m_pendingGlobalBarriers.empty())
        {
            D3D12_BARRIER_GROUP& group = groups[groupCount++];
            group.Type = D3D12_BARRIER_TYPE_GLOBAL;
            group.NumBarriers = static_cast<UINT32>(
                m_pendingGlobalBarriers.size());
            group.pGlobalBarriers = m_pendingGlobalBarriers.data();
        }
        if (!m_pendingBufferBarriers.empty())
        {
            D3D12_BARRIER_GROUP& group = groups[groupCount++];
            group.Type = D3D12_BARRIER_TYPE_BUFFER;
            group.NumBarriers = static_cast<UINT32>(
                m_pendingBufferBarriers.size());
            group.pBufferBarriers = m_pendingBufferBarriers.data();
        }
        if (!m_pendingTextureBarriers.empty())
        {
            D3D12_BARRIER_GROUP& group = groups[groupCount++];
            group.Type = D3D12_BARRIER_TYPE_TEXTURE;
            group.NumBarriers = static_cast<UINT32>(
                m_pendingTextureBarriers.size());
            group.pTextureBarriers = m_pendingTextureBarriers.data();
        }
        if (groupCount > 0)
        {
            m_enhancedCommandList->Barrier(groupCount, groups.data());
            m_pendingGlobalBarriers.clear();
            m_pendingBufferBarriers.clear();
            m_pendingTextureBarriers.clear();
        }
    }

    void DX12CommandContext::InsertAccelerationStructureBarrier(
        DX12AccelerationStructure* accelerationStructure)
    {
        if (!accelerationStructure || !accelerationStructure->GetResource())
            return;

        FlushBarriers();
        if (UsesEnhancedBarriers())
        {
            D3D12_BUFFER_BARRIER barrier = {};
            barrier.SyncBefore =
                D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE;
            barrier.SyncAfter =
                D3D12_BARRIER_SYNC_ALL_SHADING |
                D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE |
                D3D12_BARRIER_SYNC_COPY_RAYTRACING_ACCELERATION_STRUCTURE |
                D3D12_BARRIER_SYNC_EMIT_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO;
            barrier.AccessBefore =
                D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE;
            barrier.AccessAfter =
                D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ |
                D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE;
            barrier.pResource = accelerationStructure->GetResource();
            barrier.Offset = 0;
            barrier.Size = UINT64_MAX;
            QueueEnhancedBarrier(barrier);
        }
        else
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.UAV.pResource = accelerationStructure->GetResource();
            m_pendingLegacyBarriers.push_back(barrier);
        }
        FlushBarriers();
    }

    // =============================================================================
    // Render Pass
    // =============================================================================
    void DX12CommandContext::BeginRenderPass(const RHIRenderPassDesc& desc)
    {
        FlushBarriers();

        m_renderPassColorFormats.fill(RHIFormat::Unknown);
        m_renderPassColorAttachmentCount = desc.colorAttachmentCount;
        m_renderPassDepthFormat = RHIFormat::Unknown;
        m_renderPassSampleCount = RHISampleCount::Count1;
        m_renderPassAttachmentSnapshotValid = true;

        bool sampleCountInitialized = false;
        const auto validateSampleCount = [this, &sampleCountInitialized](
                                             RHITexture* texture,
                                             const char* attachmentLabel)
        {
            if (!texture)
            {
                RVX_RHI_ERROR("DX12CommandContext: render pass {} attachment has no texture",
                              attachmentLabel);
                m_renderPassAttachmentSnapshotValid = false;
                return;
            }
            if (!sampleCountInitialized)
            {
                m_renderPassSampleCount = texture->GetSampleCount();
                sampleCountInitialized = true;
                return;
            }
            if (m_renderPassSampleCount != texture->GetSampleCount())
            {
                RVX_RHI_ERROR("DX12CommandContext: render pass attachments use different sample counts");
                m_renderPassAttachmentSnapshotValid = false;
            }
        };

        if (desc.colorAttachmentCount > RVX_MAX_RENDER_TARGETS)
        {
            RVX_RHI_ERROR("DX12CommandContext: render pass color attachment count {} exceeds {}",
                          desc.colorAttachmentCount,
                          RVX_MAX_RENDER_TARGETS);
            m_renderPassAttachmentSnapshotValid = false;
        }
        for (uint32 i = 0; i < std::min(desc.colorAttachmentCount,
                                        static_cast<uint32>(RVX_MAX_RENDER_TARGETS)); ++i)
        {
            RHITextureView* view = desc.colorAttachments[i].view;
            if (!view)
            {
                RVX_RHI_ERROR("DX12CommandContext: render pass color attachment {} is null", i);
                m_renderPassAttachmentSnapshotValid = false;
                continue;
            }
            RHITexture* texture = view->GetTexture();
            m_renderPassColorFormats[i] = view->GetFormat() == RHIFormat::Unknown
                ? (texture ? texture->GetFormat() : RHIFormat::Unknown)
                : view->GetFormat();
            validateSampleCount(texture, "color");

            if (texture && desc.colorAttachments[i].loadOp == RHILoadOp::Clear)
            {
                const auto* dx12Texture = static_cast<const DX12Texture*>(texture);
                const RHIOptimizedClearValue& optimized =
                    dx12Texture->GetDesc().optimizedClearValue;
                if (optimized.type == RHIOptimizedClearValueType::Color &&
                    !AreRHIClearColorsEqual(optimized.color,
                                            desc.colorAttachments[i].clearColor))
                {
                    RVX_RHI_ERROR("DX12CommandContext: color attachment {} clear does not match its optimized clear value",
                                  i);
                }
            }
        }
        if (desc.hasDepthStencil)
        {
            RHITextureView* view = desc.depthStencilAttachment.view;
            if (!view)
            {
                RVX_RHI_ERROR("DX12CommandContext: render pass depth attachment is null");
                m_renderPassAttachmentSnapshotValid = false;
            }
            else
            {
                RHITexture* texture = view->GetTexture();
                m_renderPassDepthFormat = view->GetFormat() == RHIFormat::Unknown
                    ? (texture ? texture->GetFormat() : RHIFormat::Unknown)
                    : view->GetFormat();
                validateSampleCount(texture, "depth");
                if (texture &&
                    desc.depthStencilAttachment.depthLoadOp == RHILoadOp::Clear)
                {
                    const auto* dx12Texture = static_cast<const DX12Texture*>(texture);
                    const RHIOptimizedClearValue& optimized =
                        dx12Texture->GetDesc().optimizedClearValue;
                    if (optimized.type == RHIOptimizedClearValueType::DepthStencil &&
                        !AreRHIClearDepthStencilValuesEqual(
                            optimized.depthStencil,
                            desc.depthStencilAttachment.clearValue))
                    {
                        RVX_RHI_ERROR("DX12CommandContext: depth attachment clear does not match its optimized clear value");
                    }
                }
            }
        }

        if (!m_renderPassAttachmentSnapshotValid)
        {
            return;
        }

        m_inRenderPass = true;

        // Collect RTVs
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[RVX_MAX_RENDER_TARGETS];
        for (uint32 i = 0; i < desc.colorAttachmentCount; ++i)
        {
            auto* dx12View = static_cast<DX12TextureView*>(desc.colorAttachments[i].view);
            rtvHandles[i] = dx12View->GetRTVHandle().cpuHandle;

            // Handle load op
            if (desc.colorAttachments[i].loadOp == RHILoadOp::Clear)
            {
                const auto& clearColor = desc.colorAttachments[i].clearColor;
                float color[4] = { clearColor.r, clearColor.g, clearColor.b, clearColor.a };
                m_commandList->ClearRenderTargetView(rtvHandles[i], color, 0, nullptr);
            }
        }

        // DSV
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};
        D3D12_CPU_DESCRIPTOR_HANDLE* pDsvHandle = nullptr;
        if (desc.hasDepthStencil && desc.depthStencilAttachment.view)
        {
            auto* dx12View = static_cast<DX12TextureView*>(desc.depthStencilAttachment.view);
            dsvHandle = dx12View->GetDSVHandle().cpuHandle;
            pDsvHandle = &dsvHandle;

            D3D12_CLEAR_FLAGS clearFlags = {};
            if (desc.depthStencilAttachment.depthLoadOp == RHILoadOp::Clear)
            {
                clearFlags |= D3D12_CLEAR_FLAG_DEPTH;
            }
            if (desc.depthStencilAttachment.stencilLoadOp == RHILoadOp::Clear)
            {
                clearFlags |= D3D12_CLEAR_FLAG_STENCIL;
            }

            if (clearFlags != 0)
            {
                m_commandList->ClearDepthStencilView(
                    dsvHandle,
                    clearFlags,
                    desc.depthStencilAttachment.clearValue.depth,
                    desc.depthStencilAttachment.clearValue.stencil,
                    0, nullptr);
            }
        }

        // Set render targets
        m_commandList->OMSetRenderTargets(
            desc.colorAttachmentCount,
            desc.colorAttachmentCount > 0 ? rtvHandles : nullptr,
            FALSE,
            pDsvHandle);

        // Set viewport and scissor if render area specified
        if (desc.renderArea.width > 0 && desc.renderArea.height > 0)
        {
            D3D12_VIEWPORT viewport = {};
            viewport.TopLeftX = static_cast<float>(desc.renderArea.x);
            viewport.TopLeftY = static_cast<float>(desc.renderArea.y);
            viewport.Width = static_cast<float>(desc.renderArea.width);
            viewport.Height = static_cast<float>(desc.renderArea.height);
            viewport.MinDepth = 0.0f;
            viewport.MaxDepth = 1.0f;
            m_commandList->RSSetViewports(1, &viewport);

            D3D12_RECT scissor = {};
            scissor.left = desc.renderArea.x;
            scissor.top = desc.renderArea.y;
            scissor.right = desc.renderArea.x + desc.renderArea.width;
            scissor.bottom = desc.renderArea.y + desc.renderArea.height;
            m_commandList->RSSetScissorRects(1, &scissor);
        }
    }

    void DX12CommandContext::EndRenderPass()
    {
        m_inRenderPass = false;
        m_renderPassAttachmentSnapshotValid = false;
        m_renderPassColorAttachmentCount = 0;
        m_renderPassDepthFormat = RHIFormat::Unknown;
    }

    // =============================================================================
    // Pipeline Binding
    // =============================================================================
    void DX12CommandContext::SetPipeline(RHIPipeline* pipeline)
    {
        m_currentPipeline = nullptr;
        m_boundRayTracingDescriptorSetLayouts.clear();

        if (!pipeline)
        {
            RVX_RHI_ERROR("DX12CommandContext: SetPipeline requires a pipeline");
            return;
        }

        auto* dx12Pipeline = static_cast<DX12Pipeline*>(pipeline);
        if (!dx12Pipeline->IsValid())
        {
            RVX_RHI_ERROR("DX12CommandContext: SetPipeline requires a valid native pipeline");
            return;
        }
        if (dx12Pipeline->IsRayTracing())
        {
            if (m_queueType == RHICommandQueueType::Copy)
            {
                RVX_RHI_ERROR("DX12CommandContext: ray tracing pipeline binding cannot run on the copy queue");
                return;
            }

            if (!ValidateDX12RayTracingCommandState(m_isRecording, m_inRenderPass, "ray tracing pipeline binding"))
            {
                return;
            }

            if (!dx12Pipeline->IsValid() || !dx12Pipeline->GetStateObject())
            {
                RVX_RHI_ERROR("DX12CommandContext: ray tracing pipeline binding requires a valid DX12 state object");
                return;
            }

            if (!dx12Pipeline->GetPipelineLayout() || !dx12Pipeline->GetRootSignature())
            {
                RVX_RHI_ERROR("DX12CommandContext: ray tracing pipeline binding requires a pipeline layout and global root signature");
                return;
            }

            ComPtr<ID3D12GraphicsCommandList4> commandList4;
            if (FAILED(m_commandList.As(&commandList4)) || !commandList4)
            {
                RVX_RHI_ERROR("DX12CommandContext: ray tracing pipeline binding requires ID3D12GraphicsCommandList4");
                return;
            }

            commandList4->SetPipelineState1(dx12Pipeline->GetStateObject());
            m_commandList->SetComputeRootSignature(dx12Pipeline->GetRootSignature());
            if (auto* pipelineLayout = dx12Pipeline->GetPipelineLayout())
            {
                m_boundRayTracingDescriptorSetLayouts.resize(pipelineLayout->GetSetLayoutCount(), nullptr);
            }
            m_currentPipeline = dx12Pipeline;
            return;
        }

        if (!dx12Pipeline->IsCompute() && m_inRenderPass &&
            m_renderPassAttachmentSnapshotValid)
        {
            bool compatible =
                dx12Pipeline->GetRenderTargetCount() ==
                    m_renderPassColorAttachmentCount &&
                dx12Pipeline->GetDepthStencilFormat() ==
                    m_renderPassDepthFormat &&
                dx12Pipeline->GetSampleCount() == m_renderPassSampleCount;
            for (uint32 i = 0;
                 compatible && i < m_renderPassColorAttachmentCount;
                 ++i)
            {
                compatible = dx12Pipeline->GetRenderTargetFormat(i) ==
                             m_renderPassColorFormats[i];
            }
            if (!compatible)
            {
                RVX_RHI_ERROR("DX12CommandContext: graphics pipeline '{}' is incompatible with the active render-pass attachment formats or sample count",
                              dx12Pipeline->GetDebugName());
                return;
            }
        }

        m_currentPipeline = dx12Pipeline;
        m_commandList->SetPipelineState(dx12Pipeline->GetPipelineState());

        if (dx12Pipeline->UsesComputeRootSignature())
        {
            m_commandList->SetComputeRootSignature(dx12Pipeline->GetRootSignature());
        }
        else
        {
            m_commandList->SetGraphicsRootSignature(dx12Pipeline->GetRootSignature());
            m_commandList->IASetPrimitiveTopology(dx12Pipeline->GetPrimitiveTopology());
        }
    }

    // =============================================================================
    // Vertex/Index Buffers
    // =============================================================================
    void DX12CommandContext::SetVertexBuffer(uint32 slot, RHIBuffer* buffer, uint64 offset)
    {
        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);

        D3D12_VERTEX_BUFFER_VIEW vbView = {};
        vbView.BufferLocation = dx12Buffer->GetGPUVirtualAddress() + offset;
        vbView.SizeInBytes = static_cast<UINT>(dx12Buffer->GetSize() - offset);
        vbView.StrideInBytes = dx12Buffer->GetStride();

        m_commandList->IASetVertexBuffers(slot, 1, &vbView);
    }

    void DX12CommandContext::SetVertexBuffers(uint32 startSlot, std::span<RHIBuffer* const> buffers, std::span<const uint64> offsets)
    {
        std::vector<D3D12_VERTEX_BUFFER_VIEW> vbViews(buffers.size());

        for (size_t i = 0; i < buffers.size(); ++i)
        {
            auto* dx12Buffer = static_cast<DX12Buffer*>(buffers[i]);
            uint64 offset = (i < offsets.size()) ? offsets[i] : 0;

            vbViews[i].BufferLocation = dx12Buffer->GetGPUVirtualAddress() + offset;
            vbViews[i].SizeInBytes = static_cast<UINT>(dx12Buffer->GetSize() - offset);
            vbViews[i].StrideInBytes = dx12Buffer->GetStride();
        }

        m_commandList->IASetVertexBuffers(startSlot, static_cast<UINT>(vbViews.size()), vbViews.data());
    }

    void DX12CommandContext::SetIndexBuffer(RHIBuffer* buffer, RHIFormat format, uint64 offset)
    {
        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);

        D3D12_INDEX_BUFFER_VIEW ibView = {};
        ibView.BufferLocation = dx12Buffer->GetGPUVirtualAddress() + offset;
        ibView.SizeInBytes = static_cast<UINT>(dx12Buffer->GetSize() - offset);
        ibView.Format = (format == RHIFormat::R16_UINT) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;

        m_commandList->IASetIndexBuffer(&ibView);
    }

    // =============================================================================
    // Descriptor Sets
    // =============================================================================
    void DX12CommandContext::SetDescriptorSet(uint32 slot, RHIDescriptorSet* set, std::span<const uint32> dynamicOffsets)
    {
        if (!m_currentPipeline || !set) return;

        const bool bindingRayTracingPipeline = m_currentPipeline->IsRayTracing();
        if (bindingRayTracingPipeline &&
            !ValidateDX12RayTracingCommandState(m_isRecording, m_inRenderPass, "ray tracing descriptor set binding"))
        {
            return;
        }

        auto* dx12Set = static_cast<DX12DescriptorSet*>(set);
        if (!dx12Set->IsValid())
        {
            RVX_RHI_ERROR("DX12CommandContext: descriptor set binding requires a complete valid DX12 descriptor snapshot");
            return;
        }

        auto* pipelineLayout = m_currentPipeline->GetPipelineLayout();
        auto* setLayout = dx12Set->GetLayout();
        if (!pipelineLayout)
        {
            RVX_RHI_ERROR("DX12CommandContext: descriptor set binding requires a pipeline layout");
            return;
        }

        DX12DescriptorSetLayout* expectedLayout = pipelineLayout->GetSetLayout(slot);
        if (!setLayout || expectedLayout != setLayout ||
            !dx12Set->IsReadyForBinding(expectedLayout))
        {
            RVX_RHI_ERROR("DX12CommandContext: descriptor set layout does not match pipeline slot {}", slot);
            return;
        }

        if (dynamicOffsets.size() != dx12Set->GetRequiredDynamicOffsetCount())
        {
            RVX_RHI_ERROR(
                "DX12CommandContext: descriptor set {} requires {} dynamic offsets, received {}",
                slot,
                dx12Set->GetRequiredDynamicOffsetCount(),
                dynamicOffsets.size());
            return;
        }

        if (bindingRayTracingPipeline)
        {
            if (slot >= m_boundRayTracingDescriptorSetLayouts.size())
            {
                m_boundRayTracingDescriptorSetLayouts.resize(slot + 1, nullptr);
            }
        }

        const auto& bindings = dx12Set->GetBindings();

        if (pipelineLayout)
        {
            // Bind descriptor tables (SRV/UAV + Sampler)
            uint32 srvUavTableIndex = pipelineLayout->GetSrvUavTableIndex(slot);
            uint32 samplerTableIndex = pipelineLayout->GetSamplerTableIndex(slot);
            if (srvUavTableIndex != UINT32_MAX && dx12Set->HasCbvSrvUavTable())
            {
                if (m_currentPipeline->UsesComputeRootSignature())
                {
                    m_commandList->SetComputeRootDescriptorTable(srvUavTableIndex, dx12Set->GetCbvSrvUavGpuHandle());
                }
                else
                {
                    m_commandList->SetGraphicsRootDescriptorTable(srvUavTableIndex, dx12Set->GetCbvSrvUavGpuHandle());
                }
            }

            if (samplerTableIndex != UINT32_MAX && dx12Set->HasSamplerTable())
            {
                if (m_currentPipeline->UsesComputeRootSignature())
                {
                    m_commandList->SetComputeRootDescriptorTable(samplerTableIndex, dx12Set->GetSamplerGpuHandle());
                }
                else
                {
                    m_commandList->SetGraphicsRootDescriptorTable(samplerTableIndex, dx12Set->GetSamplerGpuHandle());
                }
            }

            // Bind root CBVs for uniform buffers
            // Try to bind buffer as root CBV if it exists in the root signature
            for (const auto& binding : bindings)
            {
                if (!binding.buffer)
                    continue;

                // Try to get root CBV index - if it exists, bind the buffer as CBV
                uint32 rootIndex = pipelineLayout->GetRootCBVIndex(slot, binding.binding);
                if (rootIndex == UINT32_MAX)
                    continue;

                // Handle dynamic offset if applicable
                uint64 dynamicOffset = 0;
                if (setLayout)
                {
                    uint32 dynamicIndex = setLayout->GetDynamicBindingIndex(binding.binding);
                    if (dynamicIndex != UINT32_MAX && dynamicIndex < dynamicOffsets.size())
                    {
                        dynamicOffset = dynamicOffsets[dynamicIndex];
                    }
                }

                auto* dx12Buffer = static_cast<DX12Buffer*>(binding.buffer);
                D3D12_GPU_VIRTUAL_ADDRESS gpuAddr = dx12Buffer->GetGPUVirtualAddress() + binding.offset + dynamicOffset;

                if (m_currentPipeline->UsesComputeRootSignature())
                {
                    m_commandList->SetComputeRootConstantBufferView(rootIndex, gpuAddr);
                }
                else
                {
                    m_commandList->SetGraphicsRootConstantBufferView(rootIndex, gpuAddr);
                }
            }
        }
        else
        {
            // Fallback: root index == binding
            for (const auto& binding : bindings)
            {
                if (!binding.buffer)
                    continue;

                auto* dx12Buffer = static_cast<DX12Buffer*>(binding.buffer);
                D3D12_GPU_VIRTUAL_ADDRESS gpuAddr = dx12Buffer->GetGPUVirtualAddress() + binding.offset;
                uint32 rootIndex = binding.binding;

                if (m_currentPipeline->UsesComputeRootSignature())
                {
                    m_commandList->SetComputeRootConstantBufferView(rootIndex, gpuAddr);
                }
                else
                {
                    m_commandList->SetGraphicsRootConstantBufferView(rootIndex, gpuAddr);
                }
            }
        }

        if (bindingRayTracingPipeline)
        {
            m_boundRayTracingDescriptorSetLayouts[slot] = setLayout;
        }
        dx12Set->MarkBound();
    }

    void DX12CommandContext::SetPushConstants(const void* data, uint32 size, uint32 offset)
    {
        if (!m_currentPipeline || !data || size == 0) return;

        if (m_currentPipeline->IsRayTracing() &&
            !ValidateDX12RayTracingCommandState(m_isRecording, m_inRenderPass, "ray tracing push constants binding"))
        {
            return;
        }

        // Get push constant root index from pipeline layout
        auto* pipelineLayout = m_currentPipeline->GetPipelineLayout();
        uint32 rootIndex = pipelineLayout ? pipelineLayout->GetPushConstantRootIndex() : UINT32_MAX;
        if (rootIndex == UINT32_MAX)
            return;

        if (m_currentPipeline->UsesComputeRootSignature())
        {
            m_commandList->SetComputeRoot32BitConstants(rootIndex, size / 4, data, offset / 4);
        }
        else
        {
            m_commandList->SetGraphicsRoot32BitConstants(rootIndex, size / 4, data, offset / 4);
        }
    }

    // =============================================================================
    // Viewport/Scissor
    // =============================================================================
    void DX12CommandContext::SetViewport(const RHIViewport& viewport)
    {
        D3D12_VIEWPORT vp = {};
        vp.TopLeftX = viewport.x;
        vp.TopLeftY = viewport.y;
        vp.Width = viewport.width;
        vp.Height = viewport.height;
        vp.MinDepth = viewport.minDepth;
        vp.MaxDepth = viewport.maxDepth;
        m_commandList->RSSetViewports(1, &vp);
    }

    void DX12CommandContext::SetViewports(std::span<const RHIViewport> viewports)
    {
        std::vector<D3D12_VIEWPORT> vps(viewports.size());
        for (size_t i = 0; i < viewports.size(); ++i)
        {
            vps[i].TopLeftX = viewports[i].x;
            vps[i].TopLeftY = viewports[i].y;
            vps[i].Width = viewports[i].width;
            vps[i].Height = viewports[i].height;
            vps[i].MinDepth = viewports[i].minDepth;
            vps[i].MaxDepth = viewports[i].maxDepth;
        }
        m_commandList->RSSetViewports(static_cast<UINT>(vps.size()), vps.data());
    }

    void DX12CommandContext::SetScissor(const RHIRect& scissor)
    {
        D3D12_RECT rect = {};
        rect.left = scissor.x;
        rect.top = scissor.y;
        rect.right = scissor.x + scissor.width;
        rect.bottom = scissor.y + scissor.height;
        m_commandList->RSSetScissorRects(1, &rect);
    }

    void DX12CommandContext::SetScissors(std::span<const RHIRect> scissors)
    {
        std::vector<D3D12_RECT> rects(scissors.size());
        for (size_t i = 0; i < scissors.size(); ++i)
        {
            rects[i].left = scissors[i].x;
            rects[i].top = scissors[i].y;
            rects[i].right = scissors[i].x + scissors[i].width;
            rects[i].bottom = scissors[i].y + scissors[i].height;
        }
        m_commandList->RSSetScissorRects(static_cast<UINT>(rects.size()), rects.data());
    }

    // =============================================================================
    // Draw Commands
    // =============================================================================
    void DX12CommandContext::Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex, uint32 firstInstance)
    {
        FlushBarriers();
        m_commandList->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
    }

    void DX12CommandContext::DrawIndexed(uint32 indexCount, uint32 instanceCount, uint32 firstIndex, int32 vertexOffset, uint32 firstInstance)
    {
        FlushBarriers();
        m_commandList->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }

    void DX12CommandContext::DrawIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride)
    {
        if (drawCount == 0)
            return;
        stride = NormalizeDX12IndirectCommandStride(
            RHIIndirectCommandSemantic::Draw, stride);
        const DX12IndirectValidationResult rangeValidation =
            ValidateDX12IndirectArgumentRange(buffer,
                                              offset,
                                              drawCount,
                                              stride,
                                              RHIIndirectCommandSemantic::Draw);
        if (!rangeValidation)
        {
            RVX_RHI_ERROR("DX12CommandContext: DrawIndirect rejected because {}",
                          rangeValidation.message);
            return;
        }
        auto* dx12Buffer = dynamic_cast<DX12Buffer*>(buffer);
        if (dx12Buffer == nullptr || dx12Buffer->GetResource() == nullptr)
        {
            RVX_RHI_ERROR("DX12CommandContext: DrawIndirect rejected because the argument buffer is not a live DX12 buffer");
            return;
        }
        constexpr RHIIndirectCommandLayout layout{
            RHIIndirectCommandSemantic::Draw,
            sizeof(D3D12_DRAW_ARGUMENTS),
            RHIIndirectCommandStateInvalidation::None};
        auto* signature = m_device->GetCommandSignature(layout);
        if (!signature)
            return;

        FlushBarriers();
        m_commandList->ExecuteIndirect(
            signature,
            drawCount,
            dx12Buffer->GetResource(),
            offset,
            nullptr,
            0);
    }

    void DX12CommandContext::DrawIndexedIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride)
    {
        if (drawCount == 0)
            return;
        stride = NormalizeDX12IndirectCommandStride(
            RHIIndirectCommandSemantic::DrawIndexed, stride);
        const RHICapabilities& capabilities = m_device->GetCapabilities();
        RHIIndexedIndirectExecutionDesc execution;
        execution.mode = RHIIndirectExecutionMode::FixedCount;
        execution.argumentBuffer = buffer;
        execution.argumentOffset = offset;
        execution.commandStride = stride;
        execution.maxDrawCount = drawCount;
        execution.argumentState =
            capabilities.indexedIndirectExecution.requiredArgumentState;
        const RHIIndexedIndirectExecutionValidationResult contractValidation =
            ValidateRHIIndexedIndirectExecutionDesc(capabilities, execution);
        if (!contractValidation)
        {
            RVX_RHI_ERROR("DX12CommandContext: DrawIndexedIndirect rejected by the RHI contract: {}",
                          contractValidation.message);
            return;
        }
        const DX12IndirectValidationResult rangeValidation =
            ValidateDX12IndirectArgumentRange(buffer,
                                              offset,
                                              drawCount,
                                              stride,
                                              RHIIndirectCommandSemantic::DrawIndexed);
        if (!rangeValidation)
        {
            RVX_RHI_ERROR("DX12CommandContext: DrawIndexedIndirect rejected because {}",
                          rangeValidation.message);
            return;
        }
        auto* dx12Buffer = dynamic_cast<DX12Buffer*>(buffer);
        if (dx12Buffer == nullptr || dx12Buffer->GetResource() == nullptr)
        {
            RVX_RHI_ERROR("DX12CommandContext: DrawIndexedIndirect rejected because the argument buffer is not a live DX12 buffer");
            return;
        }
        constexpr RHIIndirectCommandLayout layout{
            RHIIndirectCommandSemantic::DrawIndexed,
            sizeof(IndirectDrawIndexedCommand),
            RHIIndirectCommandStateInvalidation::None};
        auto* signature = m_device->GetCommandSignature(layout);
        if (!signature)
            return;

        FlushBarriers();
        m_commandList->ExecuteIndirect(
            signature,
            drawCount,
            dx12Buffer->GetResource(),
            offset,
            nullptr,
            0);
    }

    void DX12CommandContext::DrawIndexedIndirectCount(RHIBuffer* buffer,
                                                      uint64 offset,
                                                      RHIBuffer* countBuffer,
                                                      uint64 countOffset,
                                                      uint32 maxDrawCount,
                                                      uint32 stride)
    {
        if (maxDrawCount == 0)
            return;
        stride = NormalizeDX12IndirectCommandStride(
            RHIIndirectCommandSemantic::DrawIndexed, stride);
        const RHICapabilities& capabilities = m_device->GetCapabilities();
        RHIIndexedIndirectExecutionDesc execution;
        execution.mode = RHIIndirectExecutionMode::CountBuffer;
        execution.argumentBuffer = buffer;
        execution.argumentOffset = offset;
        execution.commandStride = stride;
        execution.maxDrawCount = maxDrawCount;
        execution.argumentState =
            capabilities.indexedIndirectExecution.requiredArgumentState;
        execution.countBuffer = countBuffer;
        execution.countOffset = countOffset;
        execution.countState =
            capabilities.indexedIndirectExecution.requiredCountState;
        const RHIIndexedIndirectExecutionValidationResult contractValidation =
            ValidateRHIIndexedIndirectExecutionDesc(capabilities, execution);
        if (!contractValidation)
        {
            RVX_RHI_ERROR("DX12CommandContext: DrawIndexedIndirectCount rejected by the RHI contract: {}",
                          contractValidation.message);
            return;
        }
        const DX12IndirectValidationResult rangeValidation =
            ValidateDX12IndirectArgumentRange(buffer,
                                              offset,
                                              maxDrawCount,
                                              stride,
                                              RHIIndirectCommandSemantic::DrawIndexed);
        const DX12IndirectValidationResult countValidation =
            ValidateDX12IndirectCountRange(countBuffer, countOffset);
        if (!rangeValidation || !countValidation)
        {
            const char* message = !rangeValidation
                ? rangeValidation.message
                : countValidation.message;
            RVX_RHI_ERROR("DX12CommandContext: DrawIndexedIndirectCount rejected because {}",
                          message);
            return;
        }
        auto* dx12Buffer = dynamic_cast<DX12Buffer*>(buffer);
        auto* dx12CountBuffer = dynamic_cast<DX12Buffer*>(countBuffer);
        if (dx12Buffer == nullptr || dx12Buffer->GetResource() == nullptr ||
            dx12CountBuffer == nullptr || dx12CountBuffer->GetResource() == nullptr)
        {
            RVX_RHI_ERROR("DX12CommandContext: DrawIndexedIndirectCount rejected because a buffer is not a live DX12 buffer");
            return;
        }
        constexpr RHIIndirectCommandLayout layout{
            RHIIndirectCommandSemantic::DrawIndexed,
            sizeof(IndirectDrawIndexedCommand),
            RHIIndirectCommandStateInvalidation::None};
        auto* signature = m_device->GetCommandSignature(layout);
        if (!signature)
            return;

        FlushBarriers();
        m_commandList->ExecuteIndirect(
            signature,
            maxDrawCount,
            dx12Buffer->GetResource(),
            offset,
            dx12CountBuffer->GetResource(),
            countOffset);
    }

    // =============================================================================
    // Compute Commands
    // =============================================================================
    void DX12CommandContext::Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ)
    {
        FlushBarriers();
        m_commandList->Dispatch(groupCountX, groupCountY, groupCountZ);
    }

    void DX12CommandContext::DispatchIndirect(RHIBuffer* buffer, uint64 offset)
    {
        const DX12IndirectValidationResult rangeValidation =
            ValidateDX12IndirectArgumentRange(buffer,
                                              offset,
                                              1,
                                              sizeof(D3D12_DISPATCH_ARGUMENTS),
                                              RHIIndirectCommandSemantic::Dispatch);
        if (!rangeValidation)
        {
            RVX_RHI_ERROR("DX12CommandContext: DispatchIndirect rejected because {}",
                          rangeValidation.message);
            return;
        }
        auto* dx12Buffer = dynamic_cast<DX12Buffer*>(buffer);
        if (dx12Buffer == nullptr || dx12Buffer->GetResource() == nullptr)
        {
            RVX_RHI_ERROR("DX12CommandContext: DispatchIndirect rejected because the argument buffer is not a live DX12 buffer");
            return;
        }
        constexpr RHIIndirectCommandLayout layout{
            RHIIndirectCommandSemantic::Dispatch,
            sizeof(D3D12_DISPATCH_ARGUMENTS),
            RHIIndirectCommandStateInvalidation::None};
        auto* signature = m_device->GetCommandSignature(layout);
        if (!signature)
            return;

        FlushBarriers();
        m_commandList->ExecuteIndirect(
            signature,
            1,
            dx12Buffer->GetResource(),
            offset,
            nullptr,
            0);
    }

    // =============================================================================
    // Ray Tracing Commands
    // =============================================================================
    void DX12CommandContext::BuildBottomLevelAccelerationStructure(
        RHIAccelerationStructure* dst,
        const RHIBottomLevelASDesc& desc,
        RHIBuffer* scratchBuffer,
        uint64 scratchOffset,
        RHIAccelerationStructure* src)
    {
        if (!m_device->GetCapabilities().supportsRaytracing)
        {
            RVX_RHI_WARN("DX12CommandContext: BLAS build skipped because ray tracing is unsupported");
            return;
        }

        if (m_queueType == RHICommandQueueType::Copy)
        {
            RVX_RHI_ERROR("DX12CommandContext: BLAS build cannot run on the copy queue");
            return;
        }

        if (!ValidateDX12RayTracingCommandState(m_isRecording, m_inRenderPass, "BLAS build"))
        {
            return;
        }

        auto validation = ValidateRHIBottomLevelASDesc(desc);
        if (!validation)
        {
            RVX_RHI_ERROR("DX12CommandContext: invalid BLAS build desc: {}", validation.message);
            return;
        }

        if (!ValidateDX12ASBuildFlags(m_device->GetCapabilities(), desc.buildFlags, "BLAS build"))
        {
            return;
        }

        auto* dstAS = static_cast<DX12AccelerationStructure*>(dst);
        auto* srcAS = static_cast<DX12AccelerationStructure*>(src);
        auto* scratch = static_cast<DX12Buffer*>(scratchBuffer);
        if (!dstAS || !dstAS->GetResource() || !scratch)
        {
            RVX_RHI_ERROR("DX12CommandContext: BLAS build requires destination AS and scratch buffer");
            return;
        }

        const bool update = srcAS != nullptr;
        if (dstAS->GetGPUVirtualAddress() == 0 || scratch->GetGPUVirtualAddress() == 0)
        {
            RVX_RHI_ERROR("DX12CommandContext: BLAS build requires GPU-addressable destination AS and scratch buffer");
            return;
        }

        if (update && (!srcAS->GetResource() || srcAS->GetGPUVirtualAddress() == 0))
        {
            RVX_RHI_ERROR("DX12CommandContext: BLAS update requires a GPU-addressable source AS");
            return;
        }

        if (update && !HasFlag(desc.buildFlags, RHIAccelerationStructureBuildFlags::AllowUpdate))
        {
            RVX_RHI_ERROR("DX12CommandContext: BLAS update requires AllowUpdate build flag");
            return;
        }

        if (!ValidateDX12BLASGeometryInputAddresses(desc))
        {
            return;
        }

        const RHIAccelerationStructureBuildSizes sizes = m_device->GetBottomLevelASBuildSizes(desc);
        if (!ValidateDX12ASBuildResources("BLAS build", RHIAccelerationStructureType::BottomLevel, sizes, update, dstAS, srcAS, scratch, scratchOffset))
        {
            return;
        }

        ComPtr<ID3D12GraphicsCommandList4> commandList4;
        if (FAILED(m_commandList.As(&commandList4)) || !commandList4)
        {
            RVX_RHI_ERROR("DX12CommandContext: BLAS build requires ID3D12GraphicsCommandList4");
            return;
        }

        const D3D12_GPU_VIRTUAL_ADDRESS scratchAddress = GetDX12BufferAddress(scratch, scratchOffset);
        if (scratchAddress == 0)
        {
            RVX_RHI_ERROR("DX12CommandContext: BLAS build scratch GPU address overflowed");
            return;
        }

        FlushBarriers();

        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries = BuildDX12GeometryDescs(desc);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
        buildDesc.DestAccelerationStructureData = dstAS->GetGPUVirtualAddress();
        buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        buildDesc.Inputs.Flags = ToD3D12ASBuildFlags(desc.buildFlags, update);
        buildDesc.Inputs.NumDescs = static_cast<UINT>(geometries.size());
        buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        buildDesc.Inputs.pGeometryDescs = geometries.data();
        buildDesc.SourceAccelerationStructureData = update ? srcAS->GetGPUVirtualAddress() : 0;
        buildDesc.ScratchAccelerationStructureData = scratchAddress;

        commandList4->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
        InsertAccelerationStructureBarrier(dstAS);
    }

    void DX12CommandContext::BuildTopLevelAccelerationStructure(
        RHIAccelerationStructure* dst,
        const RHITopLevelASDesc& desc,
        RHIBuffer* scratchBuffer,
        uint64 scratchOffset,
        RHIAccelerationStructure* src)
    {
        if (!m_device->GetCapabilities().supportsRaytracing)
        {
            RVX_RHI_WARN("DX12CommandContext: TLAS build skipped because ray tracing is unsupported");
            return;
        }

        if (m_queueType == RHICommandQueueType::Copy)
        {
            RVX_RHI_ERROR("DX12CommandContext: TLAS build cannot run on the copy queue");
            return;
        }

        if (!ValidateDX12RayTracingCommandState(m_isRecording, m_inRenderPass, "TLAS build"))
        {
            return;
        }

        auto validation = ValidateRHITopLevelASDesc(desc);
        if (!validation)
        {
            RVX_RHI_ERROR("DX12CommandContext: invalid TLAS build desc: {}", validation.message);
            return;
        }

        if (!ValidateDX12ASBuildFlags(m_device->GetCapabilities(), desc.buildFlags, "TLAS build"))
        {
            return;
        }

        if (!desc.instanceBuffer)
        {
            RVX_RHI_ERROR("DX12CommandContext: TLAS build requires a GPU instance buffer");
            return;
        }

        auto* dstAS = static_cast<DX12AccelerationStructure*>(dst);
        auto* srcAS = static_cast<DX12AccelerationStructure*>(src);
        auto* scratch = static_cast<DX12Buffer*>(scratchBuffer);
        auto* instanceBuffer = static_cast<DX12Buffer*>(desc.instanceBuffer);
        if (!dstAS || !dstAS->GetResource() || !scratch || !instanceBuffer)
        {
            RVX_RHI_ERROR("DX12CommandContext: TLAS build requires destination AS, instance buffer, and scratch buffer");
            return;
        }

        const bool update = srcAS != nullptr;
        if (dstAS->GetGPUVirtualAddress() == 0 ||
            scratch->GetGPUVirtualAddress() == 0 ||
            instanceBuffer->GetGPUVirtualAddress() == 0)
        {
            RVX_RHI_ERROR("DX12CommandContext: TLAS build requires GPU-addressable destination AS, instance buffer, and scratch buffer");
            return;
        }

        if (update && (!srcAS->GetResource() || srcAS->GetGPUVirtualAddress() == 0))
        {
            RVX_RHI_ERROR("DX12CommandContext: TLAS update requires a GPU-addressable source AS");
            return;
        }

        if (update && !HasFlag(desc.buildFlags, RHIAccelerationStructureBuildFlags::AllowUpdate))
        {
            RVX_RHI_ERROR("DX12CommandContext: TLAS update requires AllowUpdate build flag");
            return;
        }

        const RHIAccelerationStructureBuildSizes sizes = m_device->GetTopLevelASBuildSizes(desc);
        if (!ValidateDX12ASBuildResources("TLAS build", RHIAccelerationStructureType::TopLevel, sizes, update, dstAS, srcAS, scratch, scratchOffset))
        {
            return;
        }

        ComPtr<ID3D12GraphicsCommandList4> commandList4;
        if (FAILED(m_commandList.As(&commandList4)) || !commandList4)
        {
            RVX_RHI_ERROR("DX12CommandContext: TLAS build requires ID3D12GraphicsCommandList4");
            return;
        }

        const D3D12_GPU_VIRTUAL_ADDRESS scratchAddress = GetDX12BufferAddress(scratch, scratchOffset);
        if (scratchAddress == 0)
        {
            RVX_RHI_ERROR("DX12CommandContext: TLAS build scratch GPU address overflowed");
            return;
        }

        const D3D12_GPU_VIRTUAL_ADDRESS instanceAddress = GetDX12BufferAddress(instanceBuffer, desc.instanceOffset);
        if (instanceAddress == 0)
        {
            RVX_RHI_ERROR("DX12CommandContext: TLAS build instance GPU address overflowed");
            return;
        }

        FlushBarriers();

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
        buildDesc.DestAccelerationStructureData = dstAS->GetGPUVirtualAddress();
        buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        buildDesc.Inputs.Flags = ToD3D12ASBuildFlags(desc.buildFlags, update);
        buildDesc.Inputs.NumDescs = desc.instanceCount;
        buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        buildDesc.Inputs.InstanceDescs = instanceAddress;
        buildDesc.SourceAccelerationStructureData = update ? srcAS->GetGPUVirtualAddress() : 0;
        buildDesc.ScratchAccelerationStructureData = scratchAddress;

        commandList4->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
        InsertAccelerationStructureBarrier(dstAS);
    }

    void DX12CommandContext::DispatchRays(const RHIDispatchRaysDesc& desc)
    {
        if (!m_device->GetCapabilities().supportsRaytracingPipeline)
        {
            RVX_RHI_WARN("DX12CommandContext: DispatchRays skipped because DXR pipelines are unsupported");
            return;
        }

        if (m_queueType == RHICommandQueueType::Copy)
        {
            RVX_RHI_ERROR("DX12CommandContext: DispatchRays cannot run on the copy queue");
            return;
        }

        if (!ValidateDX12RayTracingCommandState(m_isRecording, m_inRenderPass, "DispatchRays"))
        {
            return;
        }

        if (!m_currentPipeline || !m_currentPipeline->IsRayTracing())
        {
            RVX_RHI_ERROR("DX12CommandContext: DispatchRays requires a bound ray tracing pipeline");
            return;
        }

        auto validation = ValidateRHIDispatchRaysDesc(desc, m_currentPipeline);
        if (!validation)
        {
            RVX_RHI_ERROR("DX12CommandContext: invalid DispatchRays desc: {}", validation.message);
            return;
        }

        auto* shaderTable = static_cast<DX12ShaderTable*>(desc.shaderTable);
        if (!shaderTable || !shaderTable->IsValid())
        {
            RVX_RHI_ERROR("DX12CommandContext: DispatchRays requires a valid DX12 shader table");
            return;
        }

        if (shaderTable->GetPipeline() != m_currentPipeline)
        {
            RVX_RHI_ERROR("DX12CommandContext: DispatchRays shader table does not match the bound ray tracing pipeline");
            return;
        }

        auto* pipelineLayout = m_currentPipeline->GetPipelineLayout();
        if (!pipelineLayout)
        {
            RVX_RHI_ERROR("DX12CommandContext: DispatchRays requires a ray tracing pipeline layout");
            return;
        }

        for (uint32 setIndex = 0; setIndex < pipelineLayout->GetSetLayoutCount(); ++setIndex)
        {
            DX12DescriptorSetLayout* expectedLayout = pipelineLayout->GetSetLayout(setIndex);
            if (!expectedLayout || expectedLayout->GetEntries().empty())
            {
                continue;
            }

            const bool descriptorSetBound =
                setIndex < m_boundRayTracingDescriptorSetLayouts.size() &&
                m_boundRayTracingDescriptorSetLayouts[setIndex] == expectedLayout;
            if (!descriptorSetBound)
            {
                RVX_RHI_ERROR("DX12CommandContext: DispatchRays requires descriptor set {} to be bound with the ray tracing pipeline layout",
                              setIndex);
                return;
            }
        }

        ComPtr<ID3D12GraphicsCommandList4> commandList4;
        if (FAILED(m_commandList.As(&commandList4)) || !commandList4)
        {
            RVX_RHI_ERROR("DX12CommandContext: DispatchRays requires ID3D12GraphicsCommandList4");
            return;
        }

        FlushBarriers();

        const D3D12_DISPATCH_RAYS_DESC d3dDesc =
            shaderTable->BuildDispatchRaysDesc(desc.width, desc.height, desc.depth);
        commandList4->DispatchRays(&d3dDesc);
    }

    // =============================================================================
    // Copy Commands
    // =============================================================================
    void DX12CommandContext::CopyBuffer(RHIBuffer* src, RHIBuffer* dst, uint64 srcOffset, uint64 dstOffset, uint64 size)
    {
        FlushBarriers();

        auto* dx12Src = static_cast<DX12Buffer*>(src);
        auto* dx12Dst = static_cast<DX12Buffer*>(dst);

        m_commandList->CopyBufferRegion(
            dx12Dst->GetResource(), dstOffset,
            dx12Src->GetResource(), srcOffset,
            size);
    }

    void DX12CommandContext::CopyTexture(RHITexture* src, RHITexture* dst, const RHITextureCopyDesc& desc)
    {
        FlushBarriers();

        auto* dx12Src = static_cast<DX12Texture*>(src);
        auto* dx12Dst = static_cast<DX12Texture*>(dst);

        D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
        srcLoc.pResource = dx12Src->GetResource();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLoc.SubresourceIndex = desc.srcSubresource;

        D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
        dstLoc.pResource = dx12Dst->GetResource();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = desc.dstSubresource;

        if (desc.width == 0 || desc.height == 0)
        {
            // Full copy
            m_commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
        }
        else
        {
            D3D12_BOX srcBox = {};
            srcBox.left = desc.srcX;
            srcBox.top = desc.srcY;
            srcBox.front = desc.srcZ;
            srcBox.right = desc.srcX + desc.width;
            srcBox.bottom = desc.srcY + desc.height;
            srcBox.back = desc.srcZ + desc.depth;

            m_commandList->CopyTextureRegion(&dstLoc, desc.dstX, desc.dstY, desc.dstZ, &srcLoc, &srcBox);
        }
    }

    void DX12CommandContext::CopyBufferToTexture(RHIBuffer* src, RHITexture* dst, const RHIBufferTextureCopyDesc& desc)
    {
        FlushBarriers();

        auto* dx12Src = static_cast<DX12Buffer*>(src);
        auto* dx12Dst = static_cast<DX12Texture*>(dst);

        D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
        srcLoc.pResource = dx12Src->GetResource();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint.Offset = desc.bufferOffset;
        srcLoc.PlacedFootprint.Footprint.Format = dx12Dst->GetDXGIFormat();
        const uint32 logicalWidth = desc.textureRegion.width > 0
            ? desc.textureRegion.width
            : dx12Dst->GetWidth();
        const uint32 logicalHeight = desc.textureRegion.height > 0
            ? desc.textureRegion.height
            : dx12Dst->GetHeight();
        const bool blockCompressed = IsCompressedFormat(dx12Dst->GetFormat());
        srcLoc.PlacedFootprint.Footprint.Width = blockCompressed
            ? (logicalWidth + 3u) & ~3u
            : logicalWidth;
        srcLoc.PlacedFootprint.Footprint.Height = blockCompressed
            ? (logicalHeight + 3u) & ~3u
            : logicalHeight;
        srcLoc.PlacedFootprint.Footprint.Depth = 1;
        const uint32 sourceRowBytes = blockCompressed
            ? (srcLoc.PlacedFootprint.Footprint.Width / 4u) *
                  GetFormatBytesPerPixel(dx12Dst->GetFormat())
            : srcLoc.PlacedFootprint.Footprint.Width *
                  GetFormatBytesPerPixel(dx12Dst->GetFormat());
        srcLoc.PlacedFootprint.Footprint.RowPitch = desc.bufferRowPitch > 0 ? desc.bufferRowPitch
            : ((sourceRowBytes + 255u) & ~255u);

        D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
        dstLoc.pResource = dx12Dst->GetResource();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = desc.textureSubresource;

        m_commandList->CopyTextureRegion(
            &dstLoc,
            desc.textureRegion.x, desc.textureRegion.y, desc.textureDepthSlice,
            &srcLoc,
            nullptr);
    }

    void DX12CommandContext::CopyTextureToBuffer(RHITexture* src, RHIBuffer* dst, const RHIBufferTextureCopyDesc& desc)
    {
        FlushBarriers();

        auto* dx12Src = static_cast<DX12Texture*>(src);
        auto* dx12Dst = static_cast<DX12Buffer*>(dst);

        D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
        srcLoc.pResource = dx12Src->GetResource();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLoc.SubresourceIndex = desc.textureSubresource;

        D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
        dstLoc.pResource = dx12Dst->GetResource();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dstLoc.PlacedFootprint.Offset = desc.bufferOffset;
        dstLoc.PlacedFootprint.Footprint.Format = dx12Src->GetDXGIFormat();
        const uint32 logicalWidth = desc.textureRegion.width > 0
            ? desc.textureRegion.width
            : dx12Src->GetWidth();
        const uint32 logicalHeight = desc.textureRegion.height > 0
            ? desc.textureRegion.height
            : dx12Src->GetHeight();
        const bool blockCompressed = IsCompressedFormat(dx12Src->GetFormat());
        dstLoc.PlacedFootprint.Footprint.Width = blockCompressed
            ? (logicalWidth + 3u) & ~3u
            : logicalWidth;
        dstLoc.PlacedFootprint.Footprint.Height = blockCompressed
            ? (logicalHeight + 3u) & ~3u
            : logicalHeight;
        dstLoc.PlacedFootprint.Footprint.Depth = 1;
        const uint32 destinationRowBytes = blockCompressed
            ? (dstLoc.PlacedFootprint.Footprint.Width / 4u) *
                  GetFormatBytesPerPixel(dx12Src->GetFormat())
            : dstLoc.PlacedFootprint.Footprint.Width *
                  GetFormatBytesPerPixel(dx12Src->GetFormat());
        dstLoc.PlacedFootprint.Footprint.RowPitch = desc.bufferRowPitch > 0 ? desc.bufferRowPitch
            : ((destinationRowBytes + 255u) & ~255u);

        D3D12_BOX srcBox = {};
        srcBox.left = desc.textureRegion.x;
        srcBox.top = desc.textureRegion.y;
        srcBox.front = desc.textureDepthSlice;
        srcBox.right = desc.textureRegion.x + (desc.textureRegion.width > 0 ? desc.textureRegion.width : dx12Src->GetWidth());
        srcBox.bottom = desc.textureRegion.y + (desc.textureRegion.height > 0 ? desc.textureRegion.height : dx12Src->GetHeight());
        srcBox.back = desc.textureDepthSlice + 1;

        m_commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &srcBox);
    }

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHICommandContextRef CreateDX12CommandContext(DX12Device* device, RHICommandQueueType type)
    {
        return Ref<DX12CommandContext>(new DX12CommandContext(device, type));
    }

    uint64 SubmitDX12CommandContext(DX12Device* device, RHICommandContext* context, RHIFence* signalFence)
    {
        if (!device || !context)
        {
            return 0;
        }

        auto* dx12Context = static_cast<DX12CommandContext*>(context);
        auto* queue = device->GetQueue(dx12Context->GetQueueType());
        if (!queue)
        {
            RVX_RHI_ERROR("SubmitDX12CommandContext: invalid command queue");
            return 0;
        }

        ID3D12CommandList* cmdLists[] = { dx12Context->GetCommandList() };
        queue->ExecuteCommandLists(1, cmdLists);

        uint64 submittedValue = 0;
        bool submissionFailed = false;
        if (signalFence)
        {
            auto* dx12Fence = static_cast<DX12Fence*>(signalFence);
            submittedValue = dx12Fence->AllocateSignalValue();
            const HRESULT signalResult =
                queue->Signal(dx12Fence->GetFence(), submittedValue);
            if (FAILED(signalResult))
            {
                device->HandleDeviceLost(
                    signalResult,
                    RHIDeviceFaultOperation::CommandSubmission);
                submissionFailed = true;
            }
        }

        const HRESULT deviceStatus = device->GetDeviceRemovedReason();
        if (FAILED(deviceStatus))
        {
            device->HandleDeviceLost(
                deviceStatus,
                RHIDeviceFaultOperation::CommandSubmission);
            submissionFailed = true;
        }
        if (submissionFailed)
        {
            static_cast<void>(dx12Context->DetachCommandAllocator());
            return 0;
        }

        device->GetAllocatorPool().Release(dx12Context->DetachCommandAllocator(),
                                           dx12Context->GetD3DListType(),
                                           queue);
        return submittedValue;
    }

    uint64 SubmitDX12CommandContexts(DX12Device* device, std::span<RHICommandContext* const> contexts, RHIFence* signalFence)
    {
        if (!device || contexts.empty())
            return 0;

        struct SubmissionEntry
        {
            DX12CommandContext* context = nullptr;
            ID3D12CommandQueue* queue = nullptr;
            ID3D12CommandList* commandList = nullptr;
        };
        std::vector<SubmissionEntry> entries;
        entries.reserve(contexts.size());
        for (auto* context : contexts)
        {
            if (!context)
            {
                RVX_RHI_ERROR("SubmitDX12CommandContexts: null command context");
                return 0;
            }

            auto* dx12Context = static_cast<DX12CommandContext*>(context);
            ID3D12CommandQueue* queue =
                device->GetQueue(dx12Context->GetQueueType());
            if (!queue)
            {
                RVX_RHI_ERROR("SubmitDX12CommandContexts: invalid command queue");
                return 0;
            }
            entries.push_back({dx12Context,
                               queue,
                               dx12Context->GetCommandList()});
        }
        const auto queueRank = [](const SubmissionEntry& entry)
        {
            switch (entry.context->GetQueueType())
            {
                case RHICommandQueueType::Copy: return 0;
                case RHICommandQueueType::Compute: return 1;
                case RHICommandQueueType::Graphics: return 2;
            }
            return 3;
        };
        std::stable_sort(entries.begin(), entries.end(),
                         [&queueRank](const SubmissionEntry& left,
                                      const SubmissionEntry& right)
                         {
                             return queueRank(left) < queueRank(right);
                         });

        uint64 submittedValue = 0;
        bool submissionFailed = false;
        for (size_t i = 0; i < entries.size(); ++i)
        {
            SubmissionEntry& entry = entries[i];
            if (i > 0 && entries[i - 1].queue != entry.queue)
            {
                const uint32 sourceQueueIndex = static_cast<uint32>(
                    entries[i - 1].context->GetQueueType());
                RVX_ASSERT(sourceQueueIndex <
                           device->m_queueTimelineFences.size());
                ID3D12Fence* queueChainFence =
                    device->m_queueTimelineFences[sourceQueueIndex].Get();
                const uint64 dependencyValue =
                    device->m_queueTimelineNextValues[sourceQueueIndex]++;
                if (!queueChainFence || dependencyValue == 0)
                {
                    RVX_RHI_ERROR(
                        "SubmitDX12CommandContexts exhausted its queue timeline");
                    submissionFailed = true;
                    break;
                }
                HRESULT result = entries[i - 1].queue->Signal(
                    queueChainFence, dependencyValue);
                if (SUCCEEDED(result))
                {
                    result = entry.queue->Wait(
                        queueChainFence, dependencyValue);
                }
                if (FAILED(result))
                {
                    device->HandleDeviceLost(
                        result,
                        RHIDeviceFaultOperation::CommandSubmission);
                    submissionFailed = true;
                    break;
                }
            }
            entry.queue->ExecuteCommandLists(1, &entry.commandList);
        }

        if (signalFence)
        {
            auto* dx12Fence = static_cast<DX12Fence*>(signalFence);
            submittedValue = dx12Fence->AllocateSignalValue();
            const HRESULT signalResult = submissionFailed
                ? E_FAIL
                : entries.back().queue->Signal(
                      dx12Fence->GetFence(), submittedValue);
            if (FAILED(signalResult))
            {
                device->HandleDeviceLost(
                    signalResult,
                    RHIDeviceFaultOperation::CommandSubmission);
                submissionFailed = true;
            }
        }

        const HRESULT deviceStatus = device->GetDeviceRemovedReason();
        if (FAILED(deviceStatus))
        {
            device->HandleDeviceLost(
                deviceStatus,
                RHIDeviceFaultOperation::CommandSubmission);
            submissionFailed = true;
        }
        if (submissionFailed)
        {
            for (SubmissionEntry& entry : entries)
            {
                static_cast<void>(
                    entry.context->DetachCommandAllocator());
            }
            return 0;
        }

        for (SubmissionEntry& entry : entries)
        {
            device->GetAllocatorPool().Release(
                entry.context->DetachCommandAllocator(),
                entry.context->GetD3DListType(),
                entry.queue);
        }
        return submittedValue;
    }

    uint64 SubmitDX12QueuePlan(DX12Device* device,
                               const RHIQueueSubmissionPlan& plan,
                               RHIFence* terminalFence)
    {
        if (!device)
        {
            return 0;
        }

        const RHIQueueSubmissionPlanValidationResult validation =
            ValidateRHIQueueSubmissionPlan(plan);
        if (!validation)
        {
            RVX_RHI_ERROR("SubmitDX12QueuePlan rejected invalid plan: {}",
                          validation.message);
            return 0;
        }

        struct BatchState
        {
            ID3D12CommandQueue* queue = nullptr;
            std::vector<DX12CommandContext*> contexts;
            std::vector<ID3D12CommandList*> commandLists;
            ID3D12Fence* completionFence = nullptr;
            uint64 completionValue = 0;
        };

        std::vector<BatchState> batches(plan.batches.size());
        std::vector<uint8> needsCrossQueueSignal(plan.batches.size(), 0);
        for (uint32 targetIndex = 0;
             targetIndex < static_cast<uint32>(plan.batches.size());
             ++targetIndex)
        {
            const RHIQueueSubmissionBatch& target = plan.batches[targetIndex];
            for (uint32 sourceIndex : target.prerequisiteBatchIndices)
            {
                if (plan.batches[sourceIndex].queueType != target.queueType)
                {
                    needsCrossQueueSignal[sourceIndex] = 1;
                }
            }
        }

        for (uint32 batchIndex = 0;
             batchIndex < static_cast<uint32>(plan.batches.size());
             ++batchIndex)
        {
            const RHIQueueSubmissionBatch& source = plan.batches[batchIndex];
            BatchState& batch = batches[batchIndex];
            batch.queue = device->GetQueue(source.queueType);
            if (!batch.queue)
            {
                RVX_RHI_ERROR("SubmitDX12QueuePlan encountered an unavailable queue");
                return 0;
            }
            batch.contexts.reserve(source.contexts.size());
            batch.commandLists.reserve(source.contexts.size());
            for (RHICommandContext* context : source.contexts)
            {
                auto* dx12Context = static_cast<DX12CommandContext*>(context);
                batch.contexts.push_back(dx12Context);
                batch.commandLists.push_back(dx12Context->GetCommandList());
            }

            if (needsCrossQueueSignal[batchIndex] != 0)
            {
                const uint32 queueIndex = static_cast<uint32>(source.queueType);
                RVX_ASSERT(queueIndex < device->m_queueTimelineFences.size());
                batch.completionFence =
                    device->m_queueTimelineFences[queueIndex].Get();
                batch.completionValue =
                    device->m_queueTimelineNextValues[queueIndex]++;
            }
        }

        const auto abandonAllocators = [&batches]()
        {
            for (BatchState& batch : batches)
            {
                for (DX12CommandContext* context : batch.contexts)
                {
                    static_cast<void>(context->DetachCommandAllocator());
                }
            }
        };

        uint64 submittedValue = 0;
        for (uint32 batchIndex = 0;
             batchIndex < static_cast<uint32>(plan.batches.size());
             ++batchIndex)
        {
            const RHIQueueSubmissionBatch& source = plan.batches[batchIndex];
            BatchState& batch = batches[batchIndex];
            for (uint32 prerequisiteIndex : source.prerequisiteBatchIndices)
            {
                BatchState& prerequisite = batches[prerequisiteIndex];
                if (prerequisite.queue == batch.queue)
                {
                    continue;
                }
                if (!prerequisite.completionFence ||
                    prerequisite.completionValue == 0)
                {
                    RVX_RHI_ERROR(
                        "SubmitDX12QueuePlan found a cross-queue dependency without a source signal");
                    abandonAllocators();
                    return 0;
                }
                const HRESULT waitResult = batch.queue->Wait(
                    prerequisite.completionFence,
                    prerequisite.completionValue);
                if (FAILED(waitResult))
                {
                    device->HandleDeviceLost(
                        waitResult,
                        RHIDeviceFaultOperation::CommandSubmission);
                    abandonAllocators();
                    return 0;
                }
            }

            batch.queue->ExecuteCommandLists(
                static_cast<UINT>(batch.commandLists.size()),
                batch.commandLists.data());

            if (batch.completionFence && batch.completionValue != 0)
            {
                const HRESULT signalResult = batch.queue->Signal(
                    batch.completionFence,
                    batch.completionValue);
                if (FAILED(signalResult))
                {
                    device->HandleDeviceLost(
                        signalResult,
                        RHIDeviceFaultOperation::CommandSubmission);
                    abandonAllocators();
                    return 0;
                }
            }

            if (batchIndex == plan.terminalGraphicsBatchIndex && terminalFence)
            {
                auto* dx12Fence = static_cast<DX12Fence*>(terminalFence);
                submittedValue = dx12Fence->AllocateSignalValue();
                const HRESULT signalResult = batch.queue->Signal(
                    dx12Fence->GetFence(), submittedValue);
                if (FAILED(signalResult))
                {
                    device->HandleDeviceLost(
                        signalResult,
                        RHIDeviceFaultOperation::CommandSubmission);
                    abandonAllocators();
                    return 0;
                }
            }
        }

        const HRESULT deviceStatus = device->GetDeviceRemovedReason();
        if (FAILED(deviceStatus))
        {
            device->HandleDeviceLost(
                deviceStatus,
                RHIDeviceFaultOperation::CommandSubmission);
            abandonAllocators();
            return 0;
        }

        for (BatchState& batch : batches)
        {
            for (DX12CommandContext* context : batch.contexts)
            {
                device->GetAllocatorPool().Release(
                    context->DetachCommandAllocator(),
                    context->GetD3DListType(),
                    batch.queue);
            }
        }
        return submittedValue;
    }

    // =============================================================================
    // Query Commands
    // =============================================================================
    void DX12CommandContext::BeginQuery(RHIQueryPool* pool, uint32 index)
    {
        auto* dx12Pool = ValidateDX12QueryPoolForContext(
            "BeginQuery",
            *this,
            true,
            m_isRecording,
            pool,
            pool != nullptr ? pool->GetType() : RHIQueryType::Timestamp);
        if (dx12Pool == nullptr ||
            !ValidateDX12QueryRange("BeginQuery", *dx12Pool, index, 1))
        {
            return;
        }

        D3D12_QUERY_TYPE queryType = dx12Pool->GetD3D12QueryType();

        // Timestamp queries don't have Begin/End, only WriteTimestamp
        if (queryType == D3D12_QUERY_TYPE_TIMESTAMP)
        {
            RVX_RHI_WARN("Timestamp queries don't support BeginQuery, use WriteTimestamp");
            return;
        }

        m_commandList->BeginQuery(dx12Pool->GetHeap(), queryType, index);
    }

    void DX12CommandContext::EndQuery(RHIQueryPool* pool, uint32 index)
    {
        auto* dx12Pool = ValidateDX12QueryPoolForContext(
            "EndQuery",
            *this,
            true,
            m_isRecording,
            pool,
            pool != nullptr ? pool->GetType() : RHIQueryType::Timestamp);
        if (dx12Pool == nullptr ||
            !ValidateDX12QueryRange("EndQuery", *dx12Pool, index, 1))
        {
            return;
        }

        D3D12_QUERY_TYPE queryType = dx12Pool->GetD3D12QueryType();

        // Timestamp queries don't have Begin/End, only WriteTimestamp
        if (queryType == D3D12_QUERY_TYPE_TIMESTAMP)
        {
            RVX_RHI_WARN("Timestamp queries don't support EndQuery, use WriteTimestamp");
            return;
        }

        m_commandList->EndQuery(dx12Pool->GetHeap(), queryType, index);
    }

    void DX12CommandContext::WriteTimestamp(RHIQueryPool* pool, uint32 index)
    {
        auto* dx12Pool = ValidateDX12QueryPoolForContext(
            "WriteTimestamp",
            *this,
            true,
            m_isRecording,
            pool,
            RHIQueryType::Timestamp);
        if (dx12Pool == nullptr ||
            !ValidateDX12QueryRange("WriteTimestamp", *dx12Pool, index, 1))
        {
            return;
        }

        // In DX12, timestamps are written using EndQuery with TIMESTAMP type
        m_commandList->EndQuery(dx12Pool->GetHeap(), D3D12_QUERY_TYPE_TIMESTAMP, index);
    }

    void DX12CommandContext::ResolveQueries(RHIQueryPool* pool, uint32 firstQuery, uint32 queryCount,
                                            RHIBuffer* destBuffer, uint64 destOffset)
    {
        auto* dx12Pool = ValidateDX12QueryPoolForContext(
            "ResolveQueries",
            *this,
            true,
            m_isRecording,
            pool,
            pool != nullptr ? pool->GetType() : RHIQueryType::Timestamp);
        if (dx12Pool == nullptr)
        {
            return;
        }

        if (destBuffer == nullptr)
        {
            RVX_RHI_ERROR("DX12CommandContext: ResolveQueries requires a destination buffer");
            return;
        }

        const RHIQueryValidationResult resolveValidation =
            ValidateRHIQueryResolveDestination(
                *dx12Pool,
                firstQuery,
                queryCount,
                *destBuffer,
                destOffset);
        if (!resolveValidation)
        {
            RVX_RHI_ERROR("DX12CommandContext: ResolveQueries rejected because {}",
                          resolveValidation.message);
            return;
        }

        auto* dx12Buffer = dynamic_cast<DX12Buffer*>(destBuffer);
        if (dx12Buffer == nullptr || dx12Buffer->GetResource() == nullptr)
        {
            RVX_RHI_ERROR("DX12CommandContext: ResolveQueries requires a live DX12 destination buffer");
            return;
        }

        m_commandList->ResolveQueryData(
            dx12Pool->GetHeap(),
            dx12Pool->GetD3D12QueryType(),
            firstQuery,
            queryCount,
            dx12Buffer->GetResource(),
            destOffset);
    }

    void DX12CommandContext::ResetQueries(RHIQueryPool* pool, uint32 firstQuery, uint32 queryCount)
    {
        auto* dx12Pool = ValidateDX12QueryPoolForContext(
            "ResetQueries",
            *this,
            false,
            m_isRecording,
            pool,
            pool != nullptr ? pool->GetType() : RHIQueryType::Timestamp);
        if (dx12Pool == nullptr ||
            !ValidateDX12QueryRange("ResetQueries", *dx12Pool, firstQuery, queryCount))
        {
            return;
        }

        // D3D12 queries don't need explicit reset like Vulkan
        // The ResolveQueryData operation handles this implicitly
        // This function is provided for API compatibility
        (void)dx12Pool;
    }

    // =============================================================================
    // Dynamic Render State
    // =============================================================================
    void DX12CommandContext::SetStencilReference(uint32 reference)
    {
        m_commandList->OMSetStencilRef(reference);
    }

    void DX12CommandContext::SetBlendConstants(const float constants[4])
    {
        m_commandList->OMSetBlendFactor(constants);
    }

    void DX12CommandContext::SetDepthBias(float constantFactor, float slopeFactor, float clamp)
    {
        // DX12 depth bias is baked into pipeline state (D3D12_RASTERIZER_DESC)
        // Dynamic depth bias is not directly supported in the same way as Vulkan
        // This is a no-op - depth bias should be set in pipeline state
        (void)constantFactor;
        (void)slopeFactor;
        (void)clamp;
    }

    void DX12CommandContext::SetDepthBounds(float minDepth, float maxDepth)
    {
        // OMSetDepthBounds requires ID3D12GraphicsCommandList1
        ComPtr<ID3D12GraphicsCommandList1> cmdList1;
        if (SUCCEEDED(m_commandList.As(&cmdList1)))
        {
            cmdList1->OMSetDepthBounds(minDepth, maxDepth);
        }
    }

    void DX12CommandContext::SetStencilReferenceSeparate(uint32 frontRef, uint32 backRef)
    {
        // DX12 doesn't support separate front/back stencil reference values
        // Use the front value for both (same as single reference)
        (void)backRef;
        m_commandList->OMSetStencilRef(frontRef);
    }

    void DX12CommandContext::SetLineWidth(float width)
    {
        // DX12 doesn't support line width (always 1.0)
        (void)width;
    }

    // =============================================================================
    // Synchronization
    // =============================================================================
    void DX12CommandContext::SignalFence(RHIFence* fence, uint64 value)
    {
        if (fence && m_device)
        {
            auto* dx12Fence = static_cast<DX12Fence*>(fence);
            auto* queue = m_device->GetQueue(m_queueType);
            if (queue)
            {
                dx12Fence->SignalOnQueue(value, m_queueType);
            }
            else
            {
                RVX_RHI_ERROR("DX12CommandContext::SignalFence: invalid command queue");
            }
        }
    }

    void DX12CommandContext::AliasingBarriers(
        std::span<const RHIResourceAliasingBarrier> barriers)
    {
        bool hasValidBarrier = false;
        for (const RHIResourceAliasingBarrier& barrier : barriers)
        {
            ID3D12Resource* before = GetDX12AliasingResource(
                barrier.resourceBefore);
            ID3D12Resource* after = GetDX12AliasingResource(
                barrier.resourceAfter);
            if (!before || !after || before == after)
            {
                RVX_RHI_ERROR("DX12CommandContext: aliasing barrier requires two distinct native buffer/texture resources");
                continue;
            }

            hasValidBarrier = true;
            if (UsesEnhancedBarriers())
            {
                continue;
            }

            D3D12_RESOURCE_BARRIER nativeBarrier = {};
            nativeBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
            nativeBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            nativeBarrier.Aliasing.pResourceBefore = before;
            nativeBarrier.Aliasing.pResourceAfter = after;
            m_pendingLegacyBarriers.push_back(nativeBarrier);
        }

        if (hasValidBarrier && UsesEnhancedBarriers())
        {
            // The current RHI aliasing contract identifies the overlapping
            // resources but not their final/initial access snapshots. Until
            // that contract is enriched, use the native conservative
            // equivalent of a NULL/NULL legacy aliasing barrier: finish all
            // preceding work and flush every write class. The first-use
            // resource barrier that follows activates the new resource and
            // establishes its layout/access.
            D3D12_GLOBAL_BARRIER nativeBarrier = {};
            nativeBarrier.SyncBefore = D3D12_BARRIER_SYNC_ALL;
            nativeBarrier.SyncAfter = D3D12_BARRIER_SYNC_ALL;
            nativeBarrier.AccessBefore =
                D3D12_BARRIER_ACCESS_RENDER_TARGET |
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS |
                D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE |
                D3D12_BARRIER_ACCESS_COPY_DEST |
                D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE;
            nativeBarrier.AccessAfter = D3D12_BARRIER_ACCESS_NO_ACCESS;
            QueueEnhancedBarrier(nativeBarrier);
        }
    }

    void DX12CommandContext::WaitFence(RHIFence* fence, uint64 value)
    {
        if (fence && m_device)
        {
            auto* dx12Fence = static_cast<DX12Fence*>(fence);
            auto* queue = m_device->GetQueue(m_queueType);
            if (queue)
            {
                queue->Wait(dx12Fence->GetFence(), value);
            }
            else
            {
                RVX_RHI_ERROR("DX12CommandContext::WaitFence: invalid command queue");
            }
        }
    }

    // =============================================================================
    // Split Barriers
    // =============================================================================
    void DX12CommandContext::BeginBarrier(const RHIBufferBarrier& barrier)
    {
        if (!barrier.buffer || barrier.stateBefore == barrier.stateAfter)
        {
            return;
        }

        auto* dx12Buffer = static_cast<DX12Buffer*>(barrier.buffer);
        if (!dx12Buffer || !dx12Buffer->GetResource())
            return;

        if (UsesEnhancedBarriers())
        {
            const RHIAccessSnapshot before = ResolveBarrierAccess(
                barrier.hasScopedAccess, barrier.accessBefore, barrier.stateBefore);
            const RHIAccessSnapshot after = ResolveBarrierAccess(
                barrier.hasScopedAccess, barrier.accessAfter, barrier.stateAfter);
            D3D12_BUFFER_BARRIER nativeBarrier = {};
            ResolveEnhancedBarrierAccess(
                before,
                after,
                barrier.discardIntent,
                nativeBarrier.SyncBefore,
                nativeBarrier.SyncAfter,
                nativeBarrier.AccessBefore,
                nativeBarrier.AccessAfter);
            nativeBarrier.SyncAfter = D3D12_BARRIER_SYNC_SPLIT;
            nativeBarrier.pResource = dx12Buffer->GetResource();
            nativeBarrier.Offset = 0;
            nativeBarrier.Size = UINT64_MAX;
            QueueEnhancedBarrier(nativeBarrier);
            return;
        }

        D3D12_RESOURCE_BARRIER d3dBarrier = {};
        d3dBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        d3dBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;  // Split barrier - begin
        d3dBarrier.Transition.pResource = dx12Buffer->GetResource();
        d3dBarrier.Transition.StateBefore = ToD3D12ResourceState(barrier.stateBefore);
        d3dBarrier.Transition.StateAfter = ToD3D12ResourceState(barrier.stateAfter);
        d3dBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        m_pendingLegacyBarriers.push_back(d3dBarrier);
    }

    void DX12CommandContext::BeginBarrier(const RHITextureBarrier& barrier)
    {
        if (!barrier.texture || barrier.stateBefore == barrier.stateAfter)
        {
            return;
        }

        auto* dx12Texture = static_cast<DX12Texture*>(barrier.texture);
        if (!dx12Texture || !dx12Texture->GetResource())
            return;

        if (UsesEnhancedBarriers())
        {
            const RHIAccessSnapshot before = ResolveBarrierAccess(
                barrier.hasScopedAccess, barrier.accessBefore, barrier.stateBefore);
            const RHIAccessSnapshot after = ResolveBarrierAccess(
                barrier.hasScopedAccess, barrier.accessAfter, barrier.stateAfter);
            D3D12_TEXTURE_BARRIER nativeBarrier = {};
            ResolveEnhancedBarrierAccess(
                before,
                after,
                barrier.discardIntent,
                nativeBarrier.SyncBefore,
                nativeBarrier.SyncAfter,
                nativeBarrier.AccessBefore,
                nativeBarrier.AccessAfter);
            nativeBarrier.SyncAfter = D3D12_BARRIER_SYNC_SPLIT;
            nativeBarrier.LayoutBefore =
                barrier.discardIntent == RHIDiscardIntent::Discard
                ? D3D12_BARRIER_LAYOUT_UNDEFINED
                : ToD3D12BarrierLayout(before);
            nativeBarrier.LayoutAfter = ToD3D12BarrierLayout(after);
            nativeBarrier.pResource = dx12Texture->GetResource();
            nativeBarrier.Flags =
                barrier.discardIntent == RHIDiscardIntent::Discard
                ? D3D12_TEXTURE_BARRIER_FLAG_DISCARD
                : D3D12_TEXTURE_BARRIER_FLAG_NONE;
            if (ResolveEnhancedSubresourceRange(
                    *dx12Texture,
                    barrier.subresourceRange,
                    nativeBarrier.Subresources))
            {
                QueueEnhancedBarrier(nativeBarrier);
            }
            return;
        }

        D3D12_RESOURCE_BARRIER d3dBarrier = {};
        d3dBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        d3dBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;  // Split barrier - begin
        d3dBarrier.Transition.pResource = dx12Texture->GetResource();
        d3dBarrier.Transition.StateBefore = ToD3D12ResourceState(barrier.stateBefore);
        d3dBarrier.Transition.StateAfter = ToD3D12ResourceState(barrier.stateAfter);
        d3dBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        m_pendingLegacyBarriers.push_back(d3dBarrier);
    }

    void DX12CommandContext::EndBarrier(const RHIBufferBarrier& barrier)
    {
        if (!barrier.buffer || barrier.stateBefore == barrier.stateAfter)
        {
            return;
        }

        auto* dx12Buffer = static_cast<DX12Buffer*>(barrier.buffer);
        if (!dx12Buffer || !dx12Buffer->GetResource())
            return;

        if (UsesEnhancedBarriers())
        {
            const RHIAccessSnapshot before = ResolveBarrierAccess(
                barrier.hasScopedAccess, barrier.accessBefore, barrier.stateBefore);
            const RHIAccessSnapshot after = ResolveBarrierAccess(
                barrier.hasScopedAccess, barrier.accessAfter, barrier.stateAfter);
            D3D12_BUFFER_BARRIER nativeBarrier = {};
            ResolveEnhancedBarrierAccess(
                before,
                after,
                barrier.discardIntent,
                nativeBarrier.SyncBefore,
                nativeBarrier.SyncAfter,
                nativeBarrier.AccessBefore,
                nativeBarrier.AccessAfter);
            nativeBarrier.SyncBefore = D3D12_BARRIER_SYNC_SPLIT;
            nativeBarrier.pResource = dx12Buffer->GetResource();
            nativeBarrier.Offset = 0;
            nativeBarrier.Size = UINT64_MAX;
            QueueEnhancedBarrier(nativeBarrier);
            return;
        }

        D3D12_RESOURCE_BARRIER d3dBarrier = {};
        d3dBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        d3dBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_END_ONLY;  // Split barrier - end
        d3dBarrier.Transition.pResource = dx12Buffer->GetResource();
        d3dBarrier.Transition.StateBefore = ToD3D12ResourceState(barrier.stateBefore);
        d3dBarrier.Transition.StateAfter = ToD3D12ResourceState(barrier.stateAfter);
        d3dBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        m_pendingLegacyBarriers.push_back(d3dBarrier);
    }

    void DX12CommandContext::EndBarrier(const RHITextureBarrier& barrier)
    {
        if (!barrier.texture || barrier.stateBefore == barrier.stateAfter)
        {
            return;
        }

        auto* dx12Texture = static_cast<DX12Texture*>(barrier.texture);
        if (!dx12Texture || !dx12Texture->GetResource())
            return;

        if (UsesEnhancedBarriers())
        {
            const RHIAccessSnapshot before = ResolveBarrierAccess(
                barrier.hasScopedAccess, barrier.accessBefore, barrier.stateBefore);
            const RHIAccessSnapshot after = ResolveBarrierAccess(
                barrier.hasScopedAccess, barrier.accessAfter, barrier.stateAfter);
            D3D12_TEXTURE_BARRIER nativeBarrier = {};
            ResolveEnhancedBarrierAccess(
                before,
                after,
                barrier.discardIntent,
                nativeBarrier.SyncBefore,
                nativeBarrier.SyncAfter,
                nativeBarrier.AccessBefore,
                nativeBarrier.AccessAfter);
            nativeBarrier.SyncBefore = D3D12_BARRIER_SYNC_SPLIT;
            nativeBarrier.LayoutBefore =
                barrier.discardIntent == RHIDiscardIntent::Discard
                ? D3D12_BARRIER_LAYOUT_UNDEFINED
                : ToD3D12BarrierLayout(before);
            nativeBarrier.LayoutAfter = ToD3D12BarrierLayout(after);
            nativeBarrier.pResource = dx12Texture->GetResource();
            nativeBarrier.Flags =
                barrier.discardIntent == RHIDiscardIntent::Discard
                ? D3D12_TEXTURE_BARRIER_FLAG_DISCARD
                : D3D12_TEXTURE_BARRIER_FLAG_NONE;
            if (ResolveEnhancedSubresourceRange(
                    *dx12Texture,
                    barrier.subresourceRange,
                    nativeBarrier.Subresources))
            {
                QueueEnhancedBarrier(nativeBarrier);
            }
            return;
        }

        D3D12_RESOURCE_BARRIER d3dBarrier = {};
        d3dBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        d3dBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_END_ONLY;  // Split barrier - end
        d3dBarrier.Transition.pResource = dx12Texture->GetResource();
        d3dBarrier.Transition.StateBefore = ToD3D12ResourceState(barrier.stateBefore);
        d3dBarrier.Transition.StateAfter = ToD3D12ResourceState(barrier.stateAfter);
        d3dBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        m_pendingLegacyBarriers.push_back(d3dBarrier);
    }

} // namespace RVX
