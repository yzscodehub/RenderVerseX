#include "DX12CommandContext.h"
#include "DX12Device.h"
#include "DX12Resources.h"
#include "DX12Pipeline.h"

namespace RVX
{
    // =============================================================================
    // Constructor / Destructor
    // =============================================================================
    DX12CommandContext::DX12CommandContext(DX12Device* device, RHICommandQueueType type)
        : m_device(device)
        , m_queueType(type)
    {
        auto d3dDevice = device->GetD3DDevice();

        D3D12_COMMAND_LIST_TYPE listType;
        switch (type)
        {
            case RHICommandQueueType::Graphics: listType = D3D12_COMMAND_LIST_TYPE_DIRECT; break;
            case RHICommandQueueType::Compute:  listType = D3D12_COMMAND_LIST_TYPE_COMPUTE; break;
            case RHICommandQueueType::Copy:     listType = D3D12_COMMAND_LIST_TYPE_COPY; break;
            default: listType = D3D12_COMMAND_LIST_TYPE_DIRECT; break;
        }

        DX12_CHECK(d3dDevice->CreateCommandAllocator(listType, IID_PPV_ARGS(&m_commandAllocator)));
        DX12_CHECK(d3dDevice->CreateCommandList(0, listType, m_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList)));

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

        DX12_CHECK(m_commandAllocator->Reset());
        DX12_CHECK(m_commandList->Reset(m_commandAllocator.Get(), nullptr));

        m_isRecording = true;
        m_currentPipeline = nullptr;

        // Set descriptor heaps
        ID3D12DescriptorHeap* heaps[] = {
            m_device->GetDescriptorHeapManager().GetCbvSrvUavHeap(),
            m_device->GetDescriptorHeapManager().GetSamplerHeap()
        };
        m_commandList->SetDescriptorHeaps(2, heaps);
    }

    void DX12CommandContext::End()
    {
        if (!m_isRecording)
        {
            RVX_RHI_WARN("CommandContext::End called while not recording");
            return;
        }

        FlushBarriers();

        DX12_CHECK(m_commandList->Close());
        m_isRecording = false;
    }

    void DX12CommandContext::Reset()
    {
        if (m_isRecording)
        {
            End();
        }
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
        auto* dx12Buffer = static_cast<DX12Buffer*>(barrier.buffer);

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
        auto* dx12Texture = static_cast<DX12Texture*>(barrier.texture);

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
        auto* dx12Pipeline = static_cast<DX12Pipeline*>(pipeline);
        m_currentPipeline = dx12Pipeline;

        m_commandList->SetPipelineState(dx12Pipeline->GetPipelineState());

        if (dx12Pipeline->IsCompute())
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

        auto* dx12Set = static_cast<DX12DescriptorSet*>(set);
        auto* pipelineLayout = m_currentPipeline->GetPipelineLayout();
        const auto& bindings = dx12Set->GetBindings();

        if (pipelineLayout)
        {
            // Bind descriptor tables (SRV/UAV + Sampler)
            uint32 srvUavTableIndex = pipelineLayout->GetSrvUavTableIndex(slot);
            if (srvUavTableIndex != UINT32_MAX && dx12Set->HasCbvSrvUavTable())
            {
                if (m_currentPipeline->IsCompute())
                {
                    m_commandList->SetComputeRootDescriptorTable(srvUavTableIndex, dx12Set->GetCbvSrvUavGpuHandle());
                }
                else
                {
                    m_commandList->SetGraphicsRootDescriptorTable(srvUavTableIndex, dx12Set->GetCbvSrvUavGpuHandle());
                }
            }

            uint32 samplerTableIndex = pipelineLayout->GetSamplerTableIndex(slot);
            if (samplerTableIndex != UINT32_MAX && dx12Set->HasSamplerTable())
            {
                if (m_currentPipeline->IsCompute())
                {
                    m_commandList->SetComputeRootDescriptorTable(samplerTableIndex, dx12Set->GetSamplerGpuHandle());
                }
                else
                {
                    m_commandList->SetGraphicsRootDescriptorTable(samplerTableIndex, dx12Set->GetSamplerGpuHandle());
                }
            }

            // Bind root CBVs for uniform buffers
            auto* layout = dx12Set->GetLayout();
            for (const auto& binding : bindings)
            {
                if (!binding.buffer || !layout)
                    continue;

                const auto* entry = layout->FindEntry(binding.binding);
                if (!entry)
                    continue;

                if (entry->type != RHIBindingType::UniformBuffer &&
                    entry->type != RHIBindingType::DynamicUniformBuffer)
                {
                    continue;
                }

                uint32 rootIndex = pipelineLayout->GetRootCBVIndex(slot, binding.binding);
                if (rootIndex == UINT32_MAX)
                    continue;

                uint64 dynamicOffset = 0;
                if (entry->isDynamic)
                {
                    uint32 dynamicIndex = layout->GetDynamicBindingIndex(binding.binding);
                    if (dynamicIndex != UINT32_MAX && dynamicIndex < dynamicOffsets.size())
                    {
                        dynamicOffset = dynamicOffsets[dynamicIndex];
                    }
                }

                auto* dx12Buffer = static_cast<DX12Buffer*>(binding.buffer);
                D3D12_GPU_VIRTUAL_ADDRESS gpuAddr = dx12Buffer->GetGPUVirtualAddress() + binding.offset + dynamicOffset;

                if (m_currentPipeline->IsCompute())
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

                if (m_currentPipeline->IsCompute())
                {
                    m_commandList->SetComputeRootConstantBufferView(rootIndex, gpuAddr);
                }
                else
                {
                    m_commandList->SetGraphicsRootConstantBufferView(rootIndex, gpuAddr);
                }
            }
        }
    }

    void DX12CommandContext::SetPushConstants(const void* data, uint32 size, uint32 offset)
    {
        if (!m_currentPipeline || !data || size == 0) return;

        // Get push constant root index from pipeline layout
        auto* pipelineLayout = m_currentPipeline->GetPipelineLayout();
        uint32 rootIndex = pipelineLayout ? pipelineLayout->GetPushConstantRootIndex() : UINT32_MAX;
        if (rootIndex == UINT32_MAX)
            return;
        
        if (m_currentPipeline->IsCompute())
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

    void SubmitDX12CommandContext(DX12Device* device, RHICommandContext* context, RHIFence* signalFence)
    {
        auto* dx12Context = static_cast<DX12CommandContext*>(context);
        auto* queue = device->GetQueue(dx12Context->GetQueueType());

        ID3D12CommandList* cmdLists[] = { dx12Context->GetCommandList() };
        queue->ExecuteCommandLists(1, cmdLists);

        if (signalFence)
        {
            auto* dx12Fence = static_cast<DX12Fence*>(signalFence);
            uint64 value = dx12Fence->GetCompletedValue() + 1;
            queue->Signal(dx12Fence->GetFence(), value);
        }
    }

    void SubmitDX12CommandContexts(DX12Device* device, std::span<RHICommandContext* const> contexts, RHIFence* signalFence)
    {
        if (contexts.empty())
            return;

        std::vector<ID3D12CommandList*> cmdLists;
        cmdLists.reserve(contexts.size());

        RHICommandQueueType queueType = RHICommandQueueType::Graphics;
        for (auto* context : contexts)
        {
            auto* dx12Context = static_cast<DX12CommandContext*>(context);
            cmdLists.push_back(dx12Context->GetCommandList());
            queueType = dx12Context->GetQueueType();
        }

        auto* queue = device->GetQueue(queueType);
        queue->ExecuteCommandLists(static_cast<UINT>(cmdLists.size()), cmdLists.data());

        if (signalFence)
        {
            auto* dx12Fence = static_cast<DX12Fence*>(signalFence);
            uint64 value = dx12Fence->GetCompletedValue() + 1;
            queue->Signal(dx12Fence->GetFence(), value);
        }
    }

} // namespace RVX
