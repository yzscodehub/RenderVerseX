#include "DX12CommandContext.h"
#include "DX12Device.h"
#include "DX12Resources.h"
#include "DX12Pipeline.h"
#include "DX12Query.h"

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

        void InsertAccelerationStructureUAVBarrier(
            ID3D12GraphicsCommandList* commandList,
            DX12AccelerationStructure* accelerationStructure)
        {
            if (!commandList || !accelerationStructure || !accelerationStructure->GetResource())
                return;

            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.UAV.pResource = accelerationStructure->GetResource();
            commandList->ResourceBarrier(1, &barrier);
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
    void DX12CommandContext::BufferBarrier(const RHIBufferBarrier& barrier)
    {
        if (!barrier.buffer || barrier.stateBefore == barrier.stateAfter)
        {
            return;
        }

        auto* dx12Buffer = static_cast<DX12Buffer*>(barrier.buffer);
        if (!dx12Buffer || !dx12Buffer->GetResource())
        {
            return;
        }

        D3D12_RESOURCE_BARRIER d3dBarrier = {};
        d3dBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        d3dBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        d3dBarrier.Transition.pResource = dx12Buffer->GetResource();
        d3dBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        d3dBarrier.Transition.StateBefore = ToD3D12ResourceState(barrier.stateBefore);
        d3dBarrier.Transition.StateAfter = ToD3D12ResourceState(barrier.stateAfter);

        m_pendingBarriers.push_back(d3dBarrier);
    }

    void DX12CommandContext::TextureBarrier(const RHITextureBarrier& barrier)
    {
        if (!barrier.texture || barrier.stateBefore == barrier.stateAfter)
        {
            return;
        }

        auto* dx12Texture = static_cast<DX12Texture*>(barrier.texture);
        if (!dx12Texture || !dx12Texture->GetResource())
        {
            return;
        }

        D3D12_RESOURCE_BARRIER d3dBarrier = {};
        d3dBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        d3dBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        d3dBarrier.Transition.pResource = dx12Texture->GetResource();
        d3dBarrier.Transition.StateBefore = ToD3D12ResourceState(barrier.stateBefore);
        d3dBarrier.Transition.StateAfter = ToD3D12ResourceState(barrier.stateAfter);

        // Handle subresource range
        const auto& range = barrier.subresourceRange;
        if (range.mipLevelCount == RVX_ALL_MIPS && range.arrayLayerCount == RVX_ALL_LAYERS)
        {
            d3dBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            m_pendingBarriers.push_back(d3dBarrier);
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
                    m_pendingBarriers.push_back(d3dBarrier);
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
        if (!m_pendingBarriers.empty())
        {
            m_commandList->ResourceBarrier(
                static_cast<UINT>(m_pendingBarriers.size()),
                m_pendingBarriers.data());
            m_pendingBarriers.clear();
        }
    }

    // =============================================================================
    // Render Pass
    // =============================================================================
    void DX12CommandContext::BeginRenderPass(const RHIRenderPassDesc& desc)
    {
        FlushBarriers();

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
        FlushBarriers();
        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);
        if (!dx12Buffer)
            return;

        if (stride == 0)
            stride = sizeof(D3D12_DRAW_ARGUMENTS);

        if (stride != sizeof(D3D12_DRAW_ARGUMENTS))
        {
            RVX_RHI_WARN("DrawIndirect stride {} does not match D3D12_DRAW_ARGUMENTS size {}", stride, sizeof(D3D12_DRAW_ARGUMENTS));
        }

        auto* signature = m_device->GetDrawCommandSignature();
        if (!signature)
            return;

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
        FlushBarriers();
        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);
        if (!dx12Buffer)
            return;

        if (stride == 0)
            stride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);

        if (stride != sizeof(D3D12_DRAW_INDEXED_ARGUMENTS))
        {
            RVX_RHI_WARN("DrawIndexedIndirect stride {} does not match D3D12_DRAW_INDEXED_ARGUMENTS size {}", stride, sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
        }

        auto* signature = m_device->GetDrawIndexedCommandSignature();
        if (!signature)
            return;

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
        FlushBarriers();
        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);
        auto* dx12CountBuffer = static_cast<DX12Buffer*>(countBuffer);
        if (!dx12Buffer || !dx12CountBuffer)
            return;

        if (stride == 0)
            stride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);

        if (stride != sizeof(D3D12_DRAW_INDEXED_ARGUMENTS))
        {
            RVX_RHI_WARN("DrawIndexedIndirectCount stride {} does not match D3D12_DRAW_INDEXED_ARGUMENTS size {}",
                         stride,
                         sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
        }

        auto* signature = m_device->GetDrawIndexedCommandSignature();
        if (!signature)
            return;

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
        FlushBarriers();
        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);
        if (!dx12Buffer)
            return;

        auto* signature = m_device->GetDispatchCommandSignature();
        if (!signature)
            return;

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
        InsertAccelerationStructureUAVBarrier(m_commandList.Get(), dstAS);
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
        InsertAccelerationStructureUAVBarrier(m_commandList.Get(), dstAS);
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
        srcLoc.PlacedFootprint.Footprint.Width = desc.textureRegion.width > 0 ? desc.textureRegion.width : dx12Dst->GetWidth();
        srcLoc.PlacedFootprint.Footprint.Height = desc.textureRegion.height > 0 ? desc.textureRegion.height : dx12Dst->GetHeight();
        srcLoc.PlacedFootprint.Footprint.Depth = 1;
        srcLoc.PlacedFootprint.Footprint.RowPitch = desc.bufferRowPitch > 0 ? desc.bufferRowPitch
            : ((srcLoc.PlacedFootprint.Footprint.Width * GetFormatBytesPerPixel(dx12Dst->GetFormat()) + 255) & ~255);

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
        dstLoc.PlacedFootprint.Footprint.Width = desc.textureRegion.width > 0 ? desc.textureRegion.width : dx12Src->GetWidth();
        dstLoc.PlacedFootprint.Footprint.Height = desc.textureRegion.height > 0 ? desc.textureRegion.height : dx12Src->GetHeight();
        dstLoc.PlacedFootprint.Footprint.Depth = 1;
        dstLoc.PlacedFootprint.Footprint.RowPitch = desc.bufferRowPitch > 0 ? desc.bufferRowPitch
            : ((dstLoc.PlacedFootprint.Footprint.Width * GetFormatBytesPerPixel(dx12Src->GetFormat()) + 255) & ~255);

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

        std::vector<ID3D12CommandList*> cmdLists;
        cmdLists.reserve(contexts.size());

        if (!contexts.front())
        {
            RVX_RHI_ERROR("SubmitDX12CommandContexts: null first command context");
            return 0;
        }

        auto* firstContext = static_cast<DX12CommandContext*>(contexts.front());
        RHICommandQueueType queueType = firstContext->GetQueueType();
        for (auto* context : contexts)
        {
            if (!context)
            {
                RVX_RHI_ERROR("SubmitDX12CommandContexts: null command context");
                return 0;
            }

            auto* dx12Context = static_cast<DX12CommandContext*>(context);
            if (dx12Context->GetQueueType() != queueType)
            {
                RVX_RHI_ERROR("SubmitDX12CommandContexts requires all contexts to use the same command queue type");
                return 0;
            }

            cmdLists.push_back(dx12Context->GetCommandList());
        }

        auto* queue = device->GetQueue(queueType);
        if (!queue)
        {
            RVX_RHI_ERROR("SubmitDX12CommandContexts: invalid command queue");
            return 0;
        }

        queue->ExecuteCommandLists(static_cast<UINT>(cmdLists.size()), cmdLists.data());

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
            for (auto* context : contexts)
            {
                auto* dx12Context =
                    static_cast<DX12CommandContext*>(context);
                static_cast<void>(
                    dx12Context->DetachCommandAllocator());
            }
            return 0;
        }

        for (auto* context : contexts)
        {
            auto* dx12Context = static_cast<DX12CommandContext*>(context);
            device->GetAllocatorPool().Release(dx12Context->DetachCommandAllocator(),
                                               dx12Context->GetD3DListType(),
                                               queue);
        }
        return submittedValue;
    }

    // =============================================================================
    // Query Commands
    // =============================================================================
    void DX12CommandContext::BeginQuery(RHIQueryPool* pool, uint32 index)
    {
        if (!pool)
            return;

        auto* dx12Pool = static_cast<DX12QueryPool*>(pool);
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
        if (!pool)
            return;

        auto* dx12Pool = static_cast<DX12QueryPool*>(pool);
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
        if (!pool)
            return;

        auto* dx12Pool = static_cast<DX12QueryPool*>(pool);

        // In DX12, timestamps are written using EndQuery with TIMESTAMP type
        m_commandList->EndQuery(dx12Pool->GetHeap(), D3D12_QUERY_TYPE_TIMESTAMP, index);
    }

    void DX12CommandContext::ResolveQueries(RHIQueryPool* pool, uint32 firstQuery, uint32 queryCount,
                                            RHIBuffer* destBuffer, uint64 destOffset)
    {
        if (!pool || !destBuffer)
            return;

        auto* dx12Pool = static_cast<DX12QueryPool*>(pool);
        auto* dx12Buffer = static_cast<DX12Buffer*>(destBuffer);

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
        // D3D12 queries don't need explicit reset like Vulkan
        // The ResolveQueryData operation handles this implicitly
        // This function is provided for API compatibility
        (void)pool;
        (void)firstQuery;
        (void)queryCount;
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

        D3D12_RESOURCE_BARRIER d3dBarrier = {};
        d3dBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        d3dBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;  // Split barrier - begin
        d3dBarrier.Transition.pResource = dx12Buffer->GetResource();
        d3dBarrier.Transition.StateBefore = ToD3D12ResourceState(barrier.stateBefore);
        d3dBarrier.Transition.StateAfter = ToD3D12ResourceState(barrier.stateAfter);
        d3dBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        m_pendingBarriers.push_back(d3dBarrier);
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

        D3D12_RESOURCE_BARRIER d3dBarrier = {};
        d3dBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        d3dBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;  // Split barrier - begin
        d3dBarrier.Transition.pResource = dx12Texture->GetResource();
        d3dBarrier.Transition.StateBefore = ToD3D12ResourceState(barrier.stateBefore);
        d3dBarrier.Transition.StateAfter = ToD3D12ResourceState(barrier.stateAfter);
        d3dBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        m_pendingBarriers.push_back(d3dBarrier);
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

        D3D12_RESOURCE_BARRIER d3dBarrier = {};
        d3dBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        d3dBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_END_ONLY;  // Split barrier - end
        d3dBarrier.Transition.pResource = dx12Buffer->GetResource();
        d3dBarrier.Transition.StateBefore = ToD3D12ResourceState(barrier.stateBefore);
        d3dBarrier.Transition.StateAfter = ToD3D12ResourceState(barrier.stateAfter);
        d3dBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        m_pendingBarriers.push_back(d3dBarrier);
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

        D3D12_RESOURCE_BARRIER d3dBarrier = {};
        d3dBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        d3dBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_END_ONLY;  // Split barrier - end
        d3dBarrier.Transition.pResource = dx12Texture->GetResource();
        d3dBarrier.Transition.StateBefore = ToD3D12ResourceState(barrier.stateBefore);
        d3dBarrier.Transition.StateAfter = ToD3D12ResourceState(barrier.stateAfter);
        d3dBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        m_pendingBarriers.push_back(d3dBarrier);
    }

} // namespace RVX
