#include "DX11CommandContext.h"
#include "DX11Device.h"
#include "DX11Resources.h"
#include "DX11Pipeline.h"
#include "DX11Debug.h"
#include "DX11Conversions.h"
#include "DX11BindingRemapper.h"

namespace RVX
{
    DX11CommandContext::DX11CommandContext(DX11Device* device, RHICommandQueueType queueType)
        : m_device(device)
        , m_queueType(queueType)
    {
        // Try to create deferred context if the device supports it and threading mode allows
        bool useDeferred = device->SupportsDeferredContext() && 
                          (device->GetThreadingMode() != DX11ThreadingMode::SingleThreaded);

        if (useDeferred)
        {
            m_context = device->CreateDeferredContext();
            m_isDeferred = (m_context != nullptr);

            if (!m_isDeferred)
            {
                RVX_RHI_WARN("DX11: Failed to create deferred context, falling back to immediate context");
            }
        }

        // Fall back to immediate context if deferred context creation failed or not supported
        if (!m_isDeferred)
        {
            m_context = device->GetImmediateContext();
        }

        RVX_RHI_DEBUG("DX11CommandContext created (deferred: {})", m_isDeferred);
    }

    DX11CommandContext::~DX11CommandContext()
    {
        if (m_isDeferred)
        {
            m_context.Reset();
        }
    }

    // =============================================================================
    // Lifecycle
    // =============================================================================
    void DX11CommandContext::Begin()
    {
        if (m_isOpen)
        {
            RVX_RHI_WARN("DX11CommandContext::Begin called on already open context");
            return;
        }

        // For deferred context, nothing special needed
        m_isOpen = true;
        m_descriptorSetsDirty = true;

        // Reset state
        m_currentGraphicsPipeline = nullptr;
        m_currentComputePipeline = nullptr;
        m_currentPipelineLayout = nullptr;
        m_descriptorSets.fill(nullptr);
        for (auto& offsets : m_dynamicOffsets)
        {
            offsets.clear();
        }
        m_rtvs.fill(nullptr);
        m_dsv = nullptr;
        m_rtvCount = 0;
    }

    void DX11CommandContext::End()
    {
        if (!m_isOpen)
        {
            RVX_RHI_WARN("DX11CommandContext::End called on closed context");
            return;
        }

        // For deferred context, finish command list
        if (m_isDeferred)
        {
            HRESULT hr = m_context->FinishCommandList(FALSE, &m_commandList);
            if (FAILED(hr))
            {
                RVX_RHI_ERROR("Failed to finish command list: {}", HRESULTToString(hr));
            }
        }

        m_isOpen = false;
    }

    void DX11CommandContext::Reset()
    {
        if (m_isOpen)
        {
            End();
        }

        m_commandList.Reset();

        if (m_isDeferred)
        {
            m_context.Reset();
            m_context = m_device->CreateDeferredContext();
        }
    }

    void DX11CommandContext::Submit()
    {
        if (m_isDeferred && m_commandList)
        {
            // Execute deferred command list on immediate context
            m_device->GetImmediateContext()->ExecuteCommandList(m_commandList.Get(), FALSE);
            m_commandList.Reset();
        }
        // For immediate context, commands are already executed
    }

    // =============================================================================
    // Debug Markers
    // =============================================================================
    void DX11CommandContext::BeginEvent(const char* name, uint32 color)
    {
        (void)color;
        #ifdef RVX_DX11_DEBUG
        wchar_t wname[256];
        MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 256);
        DX11Debug::Get().BeginEvent(wname);
        #else
        (void)name;
        #endif
    }

    void DX11CommandContext::EndEvent()
    {
        #ifdef RVX_DX11_DEBUG
        DX11Debug::Get().EndEvent();
        #endif
    }

    void DX11CommandContext::SetMarker(const char* name, uint32 color)
    {
        (void)color;
        #ifdef RVX_DX11_DEBUG
        wchar_t wname[256];
        MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 256);
        DX11Debug::Get().SetMarker(wname);
        #else
        (void)name;
        #endif
    }

    // =============================================================================
    // Resource Barriers (No-op for DX11)
    // =============================================================================
    void DX11CommandContext::BufferBarrier(const RHIBufferBarrier& barrier)
    {
        (void)barrier;
        // DX11 has implicit resource state management
    }

    void DX11CommandContext::TextureBarrier(const RHITextureBarrier& barrier)
    {
        (void)barrier;
        // DX11 has implicit resource state management
    }

    void DX11CommandContext::Barriers(std::span<const RHIBufferBarrier> bufferBarriers,
                                       std::span<const RHITextureBarrier> textureBarriers)
    {
        (void)bufferBarriers;
        (void)textureBarriers;
        // DX11 has implicit resource state management
    }

    // =============================================================================
    // Render Pass
    // =============================================================================
    void DX11CommandContext::BeginRenderPass(const RHIRenderPassDesc& desc)
    {
        DX11_DEBUG_EVENT_BEGIN("RenderPass");

        m_rtvCount = 0;
        m_dsv = nullptr;

        // Set render targets
        for (uint32 i = 0; i < desc.colorAttachmentCount; ++i)
        {
            const auto& attachment = desc.colorAttachments[i];
            if (attachment.view)
            {
                auto* dx11View = static_cast<DX11TextureView*>(attachment.view);
                m_rtvs[i] = dx11View->GetRTV();

                if (!m_rtvs[i])
                {
                    RVX_RHI_ERROR("DX11: RTV is null for color attachment {}", i);
                    continue;
                }

                // Clear if requested
                if (attachment.loadOp == RHILoadOp::Clear)
                {
                    float clearColor[4] = {
                        attachment.clearColor.r,
                        attachment.clearColor.g,
                        attachment.clearColor.b,
                        attachment.clearColor.a
                    };
                    m_context->ClearRenderTargetView(m_rtvs[i], clearColor);
                }

                m_rtvCount = i + 1;
            }
        }

        // Set depth stencil
        if (desc.hasDepthStencil && desc.depthStencilAttachment.view)
        {
            auto* dx11View = static_cast<DX11TextureView*>(desc.depthStencilAttachment.view);
            m_dsv = dx11View->GetDSV();

            if (desc.depthStencilAttachment.depthLoadOp == RHILoadOp::Clear)
            {
                UINT clearFlags = D3D11_CLEAR_DEPTH;
                if (desc.depthStencilAttachment.stencilLoadOp == RHILoadOp::Clear)
                {
                    clearFlags |= D3D11_CLEAR_STENCIL;
                }
                m_context->ClearDepthStencilView(
                    m_dsv,
                    clearFlags,
                    desc.depthStencilAttachment.clearValue.depth,
                    static_cast<UINT8>(desc.depthStencilAttachment.clearValue.stencil)
                );
            }
        }

        // Bind render targets
        m_context->OMSetRenderTargets(m_rtvCount, m_rtvs.data(), m_dsv);
    }

    void DX11CommandContext::EndRenderPass()
    {
        // Unbind render targets
        ID3D11RenderTargetView* nullRTVs[DX11_MAX_RENDER_TARGETS] = {};
        m_context->OMSetRenderTargets(DX11_MAX_RENDER_TARGETS, nullRTVs, nullptr);

        m_rtvs.fill(nullptr);
        m_dsv = nullptr;
        m_rtvCount = 0;

        DX11_DEBUG_EVENT_END();
    }

    // =============================================================================
    // Pipeline
    // =============================================================================
    void DX11CommandContext::SetPipeline(RHIPipeline* pipeline)
    {
        if (!pipeline) return;

        if (pipeline->IsCompute())
        {
            m_currentComputePipeline = static_cast<DX11ComputePipeline*>(pipeline);
            m_currentComputePipeline->Apply(m_context.Get());
            m_currentPipelineLayout = m_currentComputePipeline->GetLayout();
        }
        else
        {
            m_currentGraphicsPipeline = static_cast<DX11GraphicsPipeline*>(pipeline);
            m_currentGraphicsPipeline->Apply(m_context.Get());
            m_currentPipelineLayout = m_currentGraphicsPipeline->GetLayout();
        }

        DX11_DEBUG_STAT_INC(stateChanges);
    }

    void DX11CommandContext::SetDescriptorSet(uint32 slot, RHIDescriptorSet* set, std::span<const uint32> dynamicOffsets)
    {
        if (slot < m_descriptorSets.size())
        {
            m_descriptorSets[slot] = static_cast<DX11DescriptorSet*>(set);
            
            // Store dynamic offsets for this set
            m_dynamicOffsets[slot].assign(dynamicOffsets.begin(), dynamicOffsets.end());
            
            m_descriptorSetsDirty = true;
        }
    }

    void DX11CommandContext::SetPushConstants(const void* data, uint32 size, uint32 offset)
    {
        if (!data || size == 0 || !m_currentPipelineLayout)
        {
            return;
        }

        ID3D11Buffer* pushConstantBuffer = m_currentPipelineLayout->GetPushConstantBuffer();
        if (!pushConstantBuffer)
        {
            RVX_RHI_WARN("DX11: SetPushConstants called but pipeline layout has no push constant buffer");
            return;
        }

        // Map and update the push constant buffer
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = m_context->Map(pushConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("DX11: Failed to map push constant buffer: {}", HRESULTToString(hr));
            return;
        }

        // Copy data at the specified offset
        uint8* dst = static_cast<uint8*>(mapped.pData) + offset;
        memcpy(dst, data, size);

        m_context->Unmap(pushConstantBuffer, 0);

        // Bind the push constant buffer to the reserved slot for all shader stages
        constexpr UINT pushConstantSlot = DX11BindingRemapper::PUSH_CONSTANT_SLOT;

        // Bind to all active shader stages
        if (m_currentGraphicsPipeline)
        {
            m_context->VSSetConstantBuffers(pushConstantSlot, 1, &pushConstantBuffer);
            m_context->PSSetConstantBuffers(pushConstantSlot, 1, &pushConstantBuffer);
            m_context->GSSetConstantBuffers(pushConstantSlot, 1, &pushConstantBuffer);
            m_context->HSSetConstantBuffers(pushConstantSlot, 1, &pushConstantBuffer);
            m_context->DSSetConstantBuffers(pushConstantSlot, 1, &pushConstantBuffer);
        }
        else if (m_currentComputePipeline)
        {
            m_context->CSSetConstantBuffers(pushConstantSlot, 1, &pushConstantBuffer);
        }
    }

    void DX11CommandContext::FlushBindings()
    {
        if (m_descriptorSetsDirty)
        {
            ApplyDescriptorSets();
            m_descriptorSetsDirty = false;
        }
    }

    void DX11CommandContext::ApplyDescriptorSets()
    {
        for (uint32 i = 0; i < m_descriptorSets.size(); ++i)
        {
            if (m_descriptorSets[i])
            {
                // Pass set index for proper slot remapping and dynamic offsets
                m_descriptorSets[i]->Apply(m_context.Get(), RHIShaderStage::All, i, m_dynamicOffsets[i]);
            }
        }
    }

    // =============================================================================
    // Vertex/Index Buffers
    // =============================================================================
    void DX11CommandContext::SetVertexBuffer(uint32 slot, RHIBuffer* buffer, uint64 offset)
    {
        if (!buffer) return;

        auto* dx11Buffer = static_cast<DX11Buffer*>(buffer);
        ID3D11Buffer* vb = dx11Buffer->GetBuffer();
        UINT stride = dx11Buffer->GetStride();
        UINT off = static_cast<UINT>(offset);

        m_context->IASetVertexBuffers(slot, 1, &vb, &stride, &off);

        DX11_DEBUG_STAT_INC(bufferBinds);
    }

    void DX11CommandContext::SetVertexBuffers(uint32 startSlot, std::span<RHIBuffer* const> buffers, std::span<const uint64> offsets)
    {
        if (buffers.empty()) return;

        // Use static arrays for batch binding (DX11 max is 16 vertex buffers)
        std::array<ID3D11Buffer*, DX11_MAX_VERTEX_BUFFERS> d3dBuffers = {};
        std::array<UINT, DX11_MAX_VERTEX_BUFFERS> strides = {};
        std::array<UINT, DX11_MAX_VERTEX_BUFFERS> offs = {};

        size_t count = std::min(buffers.size(), static_cast<size_t>(DX11_MAX_VERTEX_BUFFERS - startSlot));

        for (size_t i = 0; i < count; ++i)
        {
            if (buffers[i])
            {
                auto* dx11Buffer = static_cast<DX11Buffer*>(buffers[i]);
                d3dBuffers[i] = dx11Buffer->GetBuffer();
                strides[i] = dx11Buffer->GetStride();
            }
            else
            {
                d3dBuffers[i] = nullptr;
                strides[i] = 0;
            }
            offs[i] = (i < offsets.size()) ? static_cast<UINT>(offsets[i]) : 0;
        }

        m_context->IASetVertexBuffers(startSlot, static_cast<UINT>(count),
                                       d3dBuffers.data(), strides.data(), offs.data());

        DX11_DEBUG_STAT_INC(bufferBinds);
    }

    void DX11CommandContext::SetIndexBuffer(RHIBuffer* buffer, RHIFormat format, uint64 offset)
    {
        if (!buffer) return;

        auto* dx11Buffer = static_cast<DX11Buffer*>(buffer);
        DXGI_FORMAT dxgiFormat = format == RHIFormat::R16_UINT ?
            DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;

        m_context->IASetIndexBuffer(dx11Buffer->GetBuffer(), dxgiFormat, static_cast<UINT>(offset));

        DX11_DEBUG_STAT_INC(bufferBinds);
    }

    // =============================================================================
    // Viewport/Scissor
    // =============================================================================
    void DX11CommandContext::SetViewport(const RHIViewport& viewport)
    {
        D3D11_VIEWPORT vp = {};
        vp.TopLeftX = viewport.x;
        vp.TopLeftY = viewport.y;
        vp.Width = viewport.width;
        vp.Height = viewport.height;
        vp.MinDepth = viewport.minDepth;
        vp.MaxDepth = viewport.maxDepth;

        m_context->RSSetViewports(1, &vp);
    }

    void DX11CommandContext::SetViewports(std::span<const RHIViewport> viewports)
    {
        std::vector<D3D11_VIEWPORT> d3dViewports(viewports.size());
        for (size_t i = 0; i < viewports.size(); ++i)
        {
            d3dViewports[i].TopLeftX = viewports[i].x;
            d3dViewports[i].TopLeftY = viewports[i].y;
            d3dViewports[i].Width = viewports[i].width;
            d3dViewports[i].Height = viewports[i].height;
            d3dViewports[i].MinDepth = viewports[i].minDepth;
            d3dViewports[i].MaxDepth = viewports[i].maxDepth;
        }
        m_context->RSSetViewports(static_cast<UINT>(d3dViewports.size()), d3dViewports.data());
    }

    void DX11CommandContext::SetScissor(const RHIRect& scissor)
    {
        D3D11_RECT rect = {};
        rect.left = scissor.x;
        rect.top = scissor.y;
        rect.right = scissor.x + static_cast<LONG>(scissor.width);
        rect.bottom = scissor.y + static_cast<LONG>(scissor.height);

        m_context->RSSetScissorRects(1, &rect);
    }

    void DX11CommandContext::SetScissors(std::span<const RHIRect> scissors)
    {
        std::vector<D3D11_RECT> d3dRects(scissors.size());
        for (size_t i = 0; i < scissors.size(); ++i)
        {
            d3dRects[i].left = scissors[i].x;
            d3dRects[i].top = scissors[i].y;
            d3dRects[i].right = scissors[i].x + static_cast<LONG>(scissors[i].width);
            d3dRects[i].bottom = scissors[i].y + static_cast<LONG>(scissors[i].height);
        }
        m_context->RSSetScissorRects(static_cast<UINT>(d3dRects.size()), d3dRects.data());
    }

    // =============================================================================
    // Draw
    // =============================================================================
    void DX11CommandContext::Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex, uint32 firstInstance)
    {
        FlushBindings();

        if (instanceCount > 1 || firstInstance > 0)
        {
            m_context->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
        }
        else
        {
            m_context->Draw(vertexCount, firstVertex);
        }

        DX11_DEBUG_STAT_INC(drawCalls);
    }

    void DX11CommandContext::DrawIndexed(uint32 indexCount, uint32 instanceCount, uint32 firstIndex, int32 vertexOffset, uint32 firstInstance)
    {
        FlushBindings();

        if (instanceCount > 1 || firstInstance > 0)
        {
            m_context->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
        }
        else
        {
            m_context->DrawIndexed(indexCount, firstIndex, vertexOffset);
        }

        DX11_DEBUG_STAT_INC(drawCalls);
    }

    void DX11CommandContext::DrawIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride)
    {
        if (!buffer || drawCount == 0)
        {
            return;
        }

        FlushBindings();

        auto* dx11Buffer = static_cast<DX11Buffer*>(buffer);
        ID3D11Buffer* argsBuffer = dx11Buffer->GetBuffer();

        // DX11 does not support multi-draw indirect, so we loop
        // Default stride for DrawInstancedIndirect is sizeof(D3D11_DRAW_INSTANCED_INDIRECT_ARGS) = 16 bytes
        uint32 effectiveStride = (stride > 0) ? stride : 16;

        for (uint32 i = 0; i < drawCount; ++i)
        {
            m_context->DrawInstancedIndirect(argsBuffer, static_cast<UINT>(offset + i * effectiveStride));
        }

        DX11_DEBUG_STAT_INC(drawCalls);
    }

    void DX11CommandContext::DrawIndexedIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride)
    {
        if (!buffer || drawCount == 0)
        {
            return;
        }

        FlushBindings();

        auto* dx11Buffer = static_cast<DX11Buffer*>(buffer);
        ID3D11Buffer* argsBuffer = dx11Buffer->GetBuffer();

        // DX11 does not support multi-draw indirect, so we loop
        // Default stride for DrawIndexedInstancedIndirect is sizeof(D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS) = 20 bytes
        uint32 effectiveStride = (stride > 0) ? stride : 20;

        for (uint32 i = 0; i < drawCount; ++i)
        {
            m_context->DrawIndexedInstancedIndirect(argsBuffer, static_cast<UINT>(offset + i * effectiveStride));
        }

        DX11_DEBUG_STAT_INC(drawCalls);
    }

    // =============================================================================
    // Dispatch
    // =============================================================================
    void DX11CommandContext::Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ)
    {
        FlushBindings();
        m_context->Dispatch(groupCountX, groupCountY, groupCountZ);

        DX11_DEBUG_STAT_INC(dispatchCalls);
    }

    void DX11CommandContext::DispatchIndirect(RHIBuffer* buffer, uint64 offset)
    {
        if (!buffer)
        {
            return;
        }

        FlushBindings();

        auto* dx11Buffer = static_cast<DX11Buffer*>(buffer);
        m_context->DispatchIndirect(dx11Buffer->GetBuffer(), static_cast<UINT>(offset));

        DX11_DEBUG_STAT_INC(dispatchCalls);
    }

    // =============================================================================
    // Copy Operations
    // =============================================================================
    void DX11CommandContext::CopyBuffer(RHIBuffer* src, RHIBuffer* dst, uint64 srcOffset, uint64 dstOffset, uint64 size)
    {
        auto* srcBuffer = static_cast<DX11Buffer*>(src);
        auto* dstBuffer = static_cast<DX11Buffer*>(dst);

        D3D11_BOX box = {};
        box.left = static_cast<UINT>(srcOffset);
        box.right = static_cast<UINT>(srcOffset + size);
        box.top = 0;
        box.bottom = 1;
        box.front = 0;
        box.back = 1;

        m_context->CopySubresourceRegion(
            dstBuffer->GetBuffer(), 0, static_cast<UINT>(dstOffset), 0, 0,
            srcBuffer->GetBuffer(), 0, &box
        );
    }

    void DX11CommandContext::CopyTexture(RHITexture* src, RHITexture* dst, const RHITextureCopyDesc& desc)
    {
        auto* srcTexture = static_cast<DX11Texture*>(src);
        auto* dstTexture = static_cast<DX11Texture*>(dst);

        if (desc.width == 0 && desc.height == 0 && desc.depth == 0)
        {
            // Full copy
            m_context->CopyResource(dstTexture->GetResource(), srcTexture->GetResource());
        }
        else
        {
            // Partial copy
            D3D11_BOX box = {};
            box.left = desc.srcX;
            box.top = desc.srcY;
            box.front = desc.srcZ;
            box.right = desc.srcX + desc.width;
            box.bottom = desc.srcY + desc.height;
            box.back = desc.srcZ + desc.depth;

            m_context->CopySubresourceRegion(
                dstTexture->GetResource(), desc.dstSubresource,
                desc.dstX, desc.dstY, desc.dstZ,
                srcTexture->GetResource(), desc.srcSubresource,
                &box
            );
        }
    }

    void DX11CommandContext::CopyBufferToTexture(RHIBuffer* src, RHITexture* dst, const RHIBufferTextureCopyDesc& desc)
    {
        if (!src || !dst)
        {
            RVX_RHI_ERROR("DX11: CopyBufferToTexture - null source or destination");
            return;
        }

        auto* srcBuffer = static_cast<DX11Buffer*>(src);
        auto* dstTexture = static_cast<DX11Texture*>(dst);

        // Determine copy region
        uint32 copyWidth = desc.textureRegion.width > 0 ? desc.textureRegion.width : dst->GetWidth();
        uint32 copyHeight = desc.textureRegion.height > 0 ? desc.textureRegion.height : dst->GetHeight();

        // Calculate row pitch
        uint32 bytesPerPixel = GetFormatBytesPerPixel(dst->GetFormat());
        uint32 rowPitch = desc.bufferRowPitch > 0 ? desc.bufferRowPitch : (copyWidth * bytesPerPixel);
        uint32 depthPitch = rowPitch * (desc.bufferImageHeight > 0 ? desc.bufferImageHeight : copyHeight);

        // Map source buffer to get data pointer
        void* srcData = srcBuffer->Map();
        if (!srcData)
        {
            // Buffer might be GPU-only, use UpdateSubresource with staging
            RVX_RHI_WARN("DX11: CopyBufferToTexture - cannot map source buffer, using UpdateSubresource");
            
            // For GPU-only buffers, we need to use a staging approach or UpdateSubresource
            // Since the buffer might have data already, we create a temp staging buffer
            D3D11_BOX srcBox = {};
            srcBox.left = static_cast<UINT>(desc.bufferOffset);
            srcBox.right = static_cast<UINT>(desc.bufferOffset + depthPitch);
            srcBox.top = 0;
            srcBox.bottom = 1;
            srcBox.front = 0;
            srcBox.back = 1;

            // Note: This path requires staging buffer for GPU->GPU copy
            // For now, this is a limitation - buffer should be CPU-accessible
            RVX_RHI_ERROR("DX11: CopyBufferToTexture requires CPU-accessible source buffer");
            return;
        }

        // Offset into the buffer
        const uint8* bufferData = static_cast<const uint8*>(srcData) + desc.bufferOffset;

        // Use UpdateSubresource to copy data to texture
        D3D11_BOX destBox = {};
        destBox.left = desc.textureRegion.x;
        destBox.top = desc.textureRegion.y;
        destBox.front = desc.textureDepthSlice;
        destBox.right = desc.textureRegion.x + copyWidth;
        destBox.bottom = desc.textureRegion.y + copyHeight;
        destBox.back = desc.textureDepthSlice + 1;

        m_context->UpdateSubresource(
            dstTexture->GetResource(),
            desc.textureSubresource,
            &destBox,
            bufferData,
            rowPitch,
            depthPitch
        );

        srcBuffer->Unmap();
    }

    void DX11CommandContext::CopyTextureToBuffer(RHITexture* src, RHIBuffer* dst, const RHIBufferTextureCopyDesc& desc)
    {
        if (!src || !dst)
        {
            RVX_RHI_ERROR("DX11: CopyTextureToBuffer - null source or destination");
            return;
        }

        auto* srcTexture = static_cast<DX11Texture*>(src);
        auto* dstBuffer = static_cast<DX11Buffer*>(dst);

        // Determine copy region
        uint32 copyWidth = desc.textureRegion.width > 0 ? desc.textureRegion.width : src->GetWidth();
        uint32 copyHeight = desc.textureRegion.height > 0 ? desc.textureRegion.height : src->GetHeight();

        // Calculate row pitch
        uint32 bytesPerPixel = GetFormatBytesPerPixel(src->GetFormat());
        uint32 dstRowPitch = desc.bufferRowPitch > 0 ? desc.bufferRowPitch : (copyWidth * bytesPerPixel);

        // Create a staging texture to read from
        D3D11_TEXTURE2D_DESC stagingDesc = {};
        stagingDesc.Width = copyWidth;
        stagingDesc.Height = copyHeight;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = srcTexture->GetDXGIFormat();
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.SampleDesc.Quality = 0;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;

        ComPtr<ID3D11Texture2D> stagingTexture;
        HRESULT hr = m_device->GetD3DDevice()->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("DX11: Failed to create staging texture for CopyTextureToBuffer: {}", HRESULTToString(hr));
            return;
        }

        // Copy the region from source to staging
        D3D11_BOX srcBox = {};
        srcBox.left = desc.textureRegion.x;
        srcBox.top = desc.textureRegion.y;
        srcBox.front = 0;
        srcBox.right = desc.textureRegion.x + copyWidth;
        srcBox.bottom = desc.textureRegion.y + copyHeight;
        srcBox.back = 1;

        m_context->CopySubresourceRegion(
            stagingTexture.Get(), 0,
            0, 0, 0,
            srcTexture->GetResource(), desc.textureSubresource,
            &srcBox
        );

        // Map the staging texture
        D3D11_MAPPED_SUBRESOURCE mappedTexture = {};
        hr = m_context->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mappedTexture);
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("DX11: Failed to map staging texture: {}", HRESULTToString(hr));
            return;
        }

        // Map the destination buffer
        void* dstData = dstBuffer->Map();
        if (!dstData)
        {
            m_context->Unmap(stagingTexture.Get(), 0);
            RVX_RHI_ERROR("DX11: CopyTextureToBuffer - cannot map destination buffer");
            return;
        }

        // Copy row by row (texture row pitch might differ from buffer row pitch)
        uint8* dstPtr = static_cast<uint8*>(dstData) + desc.bufferOffset;
        const uint8* srcPtr = static_cast<const uint8*>(mappedTexture.pData);
        uint32 copyRowBytes = copyWidth * bytesPerPixel;

        for (uint32 row = 0; row < copyHeight; ++row)
        {
            memcpy(dstPtr + row * dstRowPitch, srcPtr + row * mappedTexture.RowPitch, copyRowBytes);
        }

        dstBuffer->Unmap();
        m_context->Unmap(stagingTexture.Get(), 0);
    }

    // =============================================================================
    // Query Operations
    // =============================================================================
    void DX11CommandContext::BeginQuery(RHIQueryPool* pool, uint32 index)
    {
        if (!pool) return;

        auto* dx11Pool = static_cast<DX11QueryPool*>(pool);
        ID3D11Query* query = dx11Pool->GetQuery(index);
        if (query)
        {
            m_context->Begin(query);
        }
    }

    void DX11CommandContext::EndQuery(RHIQueryPool* pool, uint32 index)
    {
        if (!pool) return;

        auto* dx11Pool = static_cast<DX11QueryPool*>(pool);
        ID3D11Query* query = dx11Pool->GetQuery(index);
        if (query)
        {
            m_context->End(query);
        }
    }

    void DX11CommandContext::WriteTimestamp(RHIQueryPool* pool, uint32 index)
    {
        if (!pool) return;

        auto* dx11Pool = static_cast<DX11QueryPool*>(pool);
        if (dx11Pool->GetType() != RHIQueryType::Timestamp)
        {
            RVX_RHI_WARN("DX11: WriteTimestamp called on non-timestamp query pool");
            return;
        }

        ID3D11Query* query = dx11Pool->GetQuery(index);
        if (query)
        {
            // Timestamp queries use End() to write the timestamp
            m_context->End(query);
        }
    }

    void DX11CommandContext::ResolveQueries(RHIQueryPool* pool, uint32 firstQuery, uint32 queryCount,
                                            RHIBuffer* destBuffer, uint64 destOffset)
    {
        if (!pool || !destBuffer) return;

        auto* dx11Pool = static_cast<DX11QueryPool*>(pool);
        auto* dx11Buffer = static_cast<DX11Buffer*>(destBuffer);

        // Map the destination buffer
        void* mapped = dx11Buffer->Map();
        if (!mapped)
        {
            RVX_RHI_ERROR("DX11: ResolveQueries - cannot map destination buffer");
            return;
        }

        uint8* dst = static_cast<uint8*>(mapped) + destOffset;
        RHIQueryType type = dx11Pool->GetType();

        for (uint32 i = 0; i < queryCount; ++i)
        {
            ID3D11Query* query = dx11Pool->GetQuery(firstQuery + i);
            if (!query) continue;

            // Get query data - this will block until data is available
            switch (type)
            {
                case RHIQueryType::Occlusion:
                {
                    UINT64 samples = 0;
                    while (m_context->GetData(query, &samples, sizeof(samples), 0) == S_FALSE)
                    {
                        // Wait for data
                    }
                    memcpy(dst + i * sizeof(UINT64), &samples, sizeof(UINT64));
                    break;
                }
                case RHIQueryType::BinaryOcclusion:
                {
                    BOOL passed = FALSE;
                    while (m_context->GetData(query, &passed, sizeof(passed), 0) == S_FALSE)
                    {
                        // Wait for data
                    }
                    UINT64 result = passed ? 1 : 0;
                    memcpy(dst + i * sizeof(UINT64), &result, sizeof(UINT64));
                    break;
                }
                case RHIQueryType::Timestamp:
                {
                    UINT64 timestamp = 0;
                    while (m_context->GetData(query, &timestamp, sizeof(timestamp), 0) == S_FALSE)
                    {
                        // Wait for data
                    }
                    memcpy(dst + i * sizeof(UINT64), &timestamp, sizeof(UINT64));
                    break;
                }
                case RHIQueryType::PipelineStatistics:
                {
                    D3D11_QUERY_DATA_PIPELINE_STATISTICS stats = {};
                    while (m_context->GetData(query, &stats, sizeof(stats), 0) == S_FALSE)
                    {
                        // Wait for data
                    }
                    memcpy(dst + i * sizeof(stats), &stats, sizeof(stats));
                    break;
                }
                default:
                    break;
            }
        }

        dx11Buffer->Unmap();
    }

    void DX11CommandContext::ResetQueries(RHIQueryPool* pool, uint32 firstQuery, uint32 queryCount)
    {
        // DX11 queries are automatically reset when Begin() is called
        // This is a no-op for DX11
        (void)pool;
        (void)firstQuery;
        (void)queryCount;
    }

} // namespace RVX
