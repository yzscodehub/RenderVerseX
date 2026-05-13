#pragma once

#include "RHI/RHITexture.h"

#include <vector>

namespace RVX
{
    class DepthPrepass;
    class OpaquePass;
    class RenderContext;
    class RenderScene;
    class SkyboxPass;

    class RenderFrameResourceBinder
    {
    public:
        static void BindScenePassResources(
            RenderContext& renderContext,
            RenderScene& renderScene,
            const std::vector<uint32_t>& visibleObjectIndices,
            RHITextureView* depthTargetView,
            DepthPrepass* depthPrepass,
            OpaquePass* opaquePass,
            SkyboxPass* skyboxPass);
    };

} // namespace RVX
