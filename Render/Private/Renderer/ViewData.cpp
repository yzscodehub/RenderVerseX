/**
 * @file ViewData.cpp
 * @brief ViewData implementation
 */

#include "Render/Renderer/ViewData.h"
#include "Core/Camera/Camera.h"
#include "Render/Graph/ResourceViewCache.h"

namespace RVX
{
namespace
{
    RHITexture* ResolveViewTexture(const ViewData& view, RGTextureHandle handle)
    {
        if (!view.renderGraph || !handle.IsValid())
            return nullptr;

        return view.renderGraph->GetTexture(handle);
    }
} // namespace

void ViewData::SetupFromCamera(const Camera& camera, uint32_t width, uint32_t height)
{
    // Matrices
    viewMatrix = camera.GetView();
    projectionMatrix = camera.GetProjection();
    viewProjectionMatrix = camera.GetViewProjection();
    
    // Inverse matrices
    inverseViewMatrix = inverse(viewMatrix);
    inverseProjectionMatrix = inverse(projectionMatrix);
    
    // Camera transform
    cameraPosition = camera.GetPosition();
    // Extract forward from view matrix (inverted Z column)
    cameraForward = -Vec3(inverseViewMatrix[2]);
    
    // Viewport
    viewportWidth = width;
    viewportHeight = height;
    viewportX = 0;
    viewportY = 0;
    aspectRatio = (height > 0) ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
}

bool ViewData::HasTextureShaderResourceView(RGTextureHandle handle) const
{
    RHITexture* texture = ResolveViewTexture(*this, handle);
    return texture &&
           HasFlag(texture->GetUsage(), RHITextureUsage::ShaderResource) &&
           viewCache &&
           viewCache->GetDefaultSRV(texture);
}

RHITextureView* ViewData::GetTextureRenderTargetView(RGTextureHandle handle) const
{
    RHITexture* texture = ResolveViewTexture(*this, handle);
    return texture && viewCache ? viewCache->GetDefaultRTV(texture) : nullptr;
}

RHITextureView* ViewData::GetTextureShaderResourceView(RGTextureHandle handle) const
{
    RHITexture* texture = ResolveViewTexture(*this, handle);
    return texture && viewCache ? viewCache->GetDefaultSRV(texture) : nullptr;
}

RHITextureView* ViewData::GetTextureDepthStencilView(RGTextureHandle handle) const
{
    RHITexture* texture = ResolveViewTexture(*this, handle);
    return texture && viewCache ? viewCache->GetDefaultDSV(texture) : nullptr;
}

} // namespace RVX
