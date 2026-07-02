#pragma once

#include "Render/Renderer/RenderDrawItem.h"
#include "RHI/RHITexture.h"

#include <vector>

namespace RVX
{
    class DepthPrepass;
    class OpaquePass;
    class RenderContext;
    class RenderScene;
    class ShadowPass;
    class SkyboxPass;
    class TransparentPass;

    class RenderFrameResourceBinder
    {
    public:
        static void BindScenePassResources(
            RenderContext& renderContext,
            RenderScene& renderScene,
            const std::vector<RenderDrawItem>& opaqueDrawItems,
            const std::vector<RenderDrawItem>& maskedDrawItems,
            const std::vector<RenderDrawItem>& transparentDrawItems,
            RHITextureView* colorTargetViewOverride,
            RHITextureView* depthTargetView,
            DepthPrepass* depthPrepass,
            OpaquePass* opaquePass,
            ShadowPass* shadowPass,
            TransparentPass* transparentPass,
            SkyboxPass* skyboxPass);
    };

} // namespace RVX
