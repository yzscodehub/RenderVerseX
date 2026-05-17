#include "Renderer/RenderFrameResourceBinder.h"
#include "Render/Context/RenderContext.h"
#include "Render/Passes/DepthPrepass.h"
#include "Render/Passes/OpaquePass.h"
#include "Render/Passes/SkyboxPass.h"
#include "Render/Passes/TransparentPass.h"
#include "Render/Renderer/RenderScene.h"

namespace RVX
{
    void RenderFrameResourceBinder::BindScenePassResources(
        RenderContext& renderContext,
        RenderScene& renderScene,
        const std::vector<RenderDrawItem>& opaqueDrawItems,
        const std::vector<RenderDrawItem>& maskedDrawItems,
        const std::vector<RenderDrawItem>& transparentDrawItems,
        RHITextureView* depthTargetView,
        DepthPrepass* depthPrepass,
        OpaquePass* opaquePass,
        TransparentPass* transparentPass,
        SkyboxPass* skyboxPass)
    {
        RHITextureView* colorTargetView = renderContext.GetCurrentBackBufferView();

        if (depthPrepass)
        {
            depthPrepass->SetRenderScene(&renderScene, &opaqueDrawItems, &maskedDrawItems);
            depthPrepass->SetDepthTarget(depthTargetView);
        }

        if (opaquePass)
        {
            opaquePass->SetRenderScene(&renderScene, &opaqueDrawItems, &maskedDrawItems);
            opaquePass->SetRenderTargets(colorTargetView, depthTargetView);
        }

        if (transparentPass)
        {
            transparentPass->SetRenderScene(&renderScene, &transparentDrawItems);
            transparentPass->SetRenderTargets(colorTargetView, depthTargetView);
        }

        if (skyboxPass)
        {
            skyboxPass->SetRenderTargets(colorTargetView, depthTargetView);
        }
    }

} // namespace RVX
