#pragma once

#include "RHI/RHIResources.h"

namespace RVX
{
    // =============================================================================
    // Render Target Attachment
    // =============================================================================
    struct RHIRenderTargetAttachment
    {
        RHITextureView* view = nullptr;
        RHILoadOp loadOp = RHILoadOp::Clear;
        RHIStoreOp storeOp = RHIStoreOp::Store;
        RHIClearColor clearColor = {0.0f, 0.0f, 0.0f, 1.0f};
    };

    // =============================================================================
    // Depth Stencil Attachment
    // =============================================================================
    struct RHIDepthStencilAttachment
    {
        RHITextureView* view = nullptr;
        RHILoadOp depthLoadOp = RHILoadOp::Clear;
        RHIStoreOp depthStoreOp = RHIStoreOp::Store;
        RHILoadOp stencilLoadOp = RHILoadOp::DontCare;
        RHIStoreOp stencilStoreOp = RHIStoreOp::DontCare;
        RHIClearDepthStencil clearValue = {1.0f, 0};
        bool readOnly = false;
    };

    // =============================================================================
    // Render Pass Description
    // =============================================================================
    struct RHIRenderPassDesc
    {
        RHIRenderTargetAttachment colorAttachments[RVX_MAX_RENDER_TARGETS];
        uint32 colorAttachmentCount = 0;
        RHIDepthStencilAttachment depthStencilAttachment;
        bool hasDepthStencil = false;

        // Render area (optional, default = full framebuffer)
        RHIRect renderArea = {0, 0, 0, 0};  // 0 = use attachment size

        RHIRenderPassDesc& AddColorAttachment(
            RHITextureView* view,
            RHILoadOp loadOp = RHILoadOp::Clear,
            RHIStoreOp storeOp = RHIStoreOp::Store,
            RHIClearColor clearColor = {0.0f, 0.0f, 0.0f, 1.0f})
        {
            if (colorAttachmentCount < RVX_MAX_RENDER_TARGETS)
            {
                colorAttachments[colorAttachmentCount++] = {view, loadOp, storeOp, clearColor};
            }
            return *this;
        }

        RHIRenderPassDesc& SetDepthStencil(
            RHITextureView* view,
            RHILoadOp depthLoadOp = RHILoadOp::Clear,
            RHIStoreOp depthStoreOp = RHIStoreOp::Store,
            float clearDepth = 1.0f,
            uint8 clearStencil = 0)
        {
            depthStencilAttachment.view = view;
            depthStencilAttachment.depthLoadOp = depthLoadOp;
            depthStencilAttachment.depthStoreOp = depthStoreOp;
            depthStencilAttachment.clearValue = {clearDepth, clearStencil};
            hasDepthStencil = true;
            return *this;
        }

        RHIRenderPassDesc& SetRenderArea(int32 x, int32 y, uint32 width, uint32 height)
        {
            renderArea = {x, y, width, height};
            return *this;
        }
    };

} // namespace RVX
