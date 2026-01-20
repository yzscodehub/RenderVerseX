#include "DX11CommandContext.h"
#include "DX11Device.h"
#include "DX11Resources.h"
#include "DX11Pipeline.h"
#include "DX11Debug.h"
#include "DX11Conversions.h"

namespace RVX
{
    DX11CommandContext::DX11CommandContext(DX11Device* device, RHICommandQueueType queueType)
        : m_device(device)
        , m_queueType(queueType)
    {
        // TODO: Re-enable deferred context after debugging
        // For now, use immediate context to simplify debugging
        m_context = device->GetImmediateContext();
        m_isDeferred = false;

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
        m_descriptorSets.fill(nullptr);
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
        }
        else
        {
            m_currentGraphicsPipeline = static_cast<DX11GraphicsPipeline*>(pipeline);
            m_currentGraphicsPipeline->Apply(m_context.Get());
        }

        DX11_DEBUG_STAT_INC(stateChanges);
    }

    void DX11CommandContext::SetDescriptorSet(uint32 slot, RHIDescriptorSet* set, std::span<const uint32> dynamicOffsets)
    {
        (void)dynamicOffsets;  // TODO: handle dynamic offsets
        if (slot < m_descriptorSets.size())
        {
            m_descriptorSets[slot] = static_cast<DX11DescriptorSet*>(set);
            m_descriptorSetsDirty = true;
        }
    }

    void DX11CommandContext::SetPushConstants(const void* data, uint32 size, uint32 offset)
    {
        (void)data;
        (void)size;
        (void)offset;
        // TODO: Phase 3 - map to constant buffer
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
                m_descriptorSets[i]->Apply(m_context.Get(), RHIShaderStage::All);
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
        // TODO: Phase 4 - batch vertex buffer binding
        for (size_t i = 0; i < buffers.size(); ++i)
        {
            uint64 offset = (i < offsets.size()) ? offsets[i] : 0;
            SetVertexBuffer(startSlot + static_cast<uint32>(i), buffers[i], offset);
        }
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
        (void)buffer;
        (void)offset;
        (void)drawCount;
        (void)stride;
        // TODO: Phase 7 - indirect drawing
    }

    void DX11CommandContext::DrawIndexedIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride)
    {
        (void)buffer;
        (void)offset;
        (void)drawCount;
        (void)stride;
        // TODO: Phase 7 - indirect drawing
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
        (void)buffer;
        (void)offset;
        // TODO: Phase 7 - indirect dispatch
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
        (void)src;
        (void)dst;
        (void)desc;
        // TODO: Phase 4 implementation
    }

    void DX11CommandContext::CopyTextureToBuffer(RHITexture* src, RHIBuffer* dst, const RHIBufferTextureCopyDesc& desc)
    {
        (void)src;
        (void)dst;
        (void)desc;
        // TODO: Phase 4 implementation
    }

} // namespace RVX
