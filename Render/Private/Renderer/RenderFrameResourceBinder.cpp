#include "Renderer/RenderFrameResourceBinder.h"
#include "Render/Context/RenderContext.h"
#include "Render/Passes/DepthPrepass.h"
#include "Render/Passes/OpaquePass.h"
#include "Render/Passes/SkyboxPass.h"
#include "Render/Renderer/RenderScene.h"

namespace RVX
{
    void RenderFrameResourceBinder::BindScenePassResources(
        RenderContext& renderContext,
        RenderScene& renderScene,
        const std::vector<uint32_t>& visibleObjectIndices,
        RHITextureView* depthTargetView,
        DepthPrepass* depthPrepass,
        OpaquePass* opaquePass,
        SkyboxPass* skyboxPass)
    {
        RHITextureView* colorTargetView = renderContext.GetCurrentBackBufferView();

        if (depthPrepass)
        {
            depthPrepass->SetRenderScene(&renderScene, &visibleObjectIndices);
            depthPrepass->SetDepthTarget(depthTargetView);
        }

        if (opaquePass)
        {
            opaquePass->SetRenderScene(&renderScene, &visibleObjectIndices);
            opaquePass->SetRenderTargets(colorTargetView, depthTargetView);
        }

        if (skyboxPass)
        {
            skyboxPass->SetRenderTargets(colorTargetView, depthTargetView);
        }
    }

} // namespace RVX
