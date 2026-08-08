#pragma once

/**
 * @file ViewData.h
 * @brief View data for rendering - contains camera, viewport, and visible objects
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "RHI/RHI.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/Renderer/ShadowConstants.h"
#include "RenderContracts/RenderFramePacket.h"

#include <array>

namespace RVX
{
    class Camera;
    class ResourceViewCache;
    class RenderSubmissionResourceBatch;
    struct RenderFrameExecutionPlan;
    struct RenderFrameExecutionReport;
    struct RenderVisibilityResult;
    struct SceneMeshPassPreparation;
    struct SceneRenderInstanceBatchPlans;

    /**
     * @brief View data collected for rendering a single view/camera
     *
     * ViewData encapsulates all the information needed to render from a single
     * viewpoint, including camera matrices, viewport, and render targets.
     */
    struct ViewData
    {
        // =====================================================================
        // Camera Data
        // =====================================================================

        /// View matrix (world to camera)
        Mat4 viewMatrix = Mat4Identity();

        /// Projection matrix (camera to clip)
        Mat4 projectionMatrix = Mat4Identity();

        /// Combined view-projection matrix
        Mat4 viewProjectionMatrix = Mat4Identity();

        /// Previous rendered frame view-projection matrix, used for temporal reprojection
        Mat4 previousViewProjectionMatrix = Mat4Identity();

        /// Whether previousViewProjectionMatrix contains a valid rendered frame
        uint8 previousViewProjectionValid = 0;

        /// Inverse view matrix
        Mat4 inverseViewMatrix = Mat4Identity();

        /// Inverse projection matrix
        Mat4 inverseProjectionMatrix = Mat4Identity();

        /// Camera world position
        Vec3 cameraPosition{0.0f, 0.0f, 0.0f};

        /// Camera forward direction
        Vec3 cameraForward{0.0f, 0.0f, -1.0f};

        /// Near clip plane
        float nearPlane = 0.1f;

        /// Far clip plane
        float farPlane = 1000.0f;

        /// Field of view (radians, for perspective)
        float fieldOfView = 1.0472f;  // ~60 degrees

        // =====================================================================
        // Viewport
        // =====================================================================

        /// Viewport dimensions
        uint32_t viewportWidth = 0;
        uint32_t viewportHeight = 0;

        /// Viewport offset
        int32_t viewportX = 0;
        int32_t viewportY = 0;

        /// Aspect ratio
        float aspectRatio = 1.0f;

        // =====================================================================
        // Lighting Controls
        // =====================================================================

        /// Primary directional light direction; matches the previous hard-coded DefaultLit direction
        Vec3 directionalLightDirection{0.5f, -0.8f, 0.3f};

        /// Primary directional light intensity; matches the previous hard-coded DefaultLit radiance
        float directionalLightIntensity = 4.0f;

        /// Primary directional light RGB color; defaults to white for the existing visual baseline
        Vec3 directionalLightColor{1.0f, 1.0f, 1.0f};

        /// Directional shadow matrix for the selected primary light and cascade 0
        Mat4 directionalShadowViewProjection = Mat4Identity();

        /// Directional shadow matrices for active CSM cascades
        std::array<Mat4, RVX_MAX_DIRECTIONAL_SHADOW_CASCADES> directionalShadowViewProjections{};

        /// Absolute camera-forward split distances for active CSM cascades
        Vec4 directionalShadowCascadeSplits{0.0f, 0.0f, 0.0f, 0.0f};

        /// Absolute camera-forward fade widths before active CSM split boundaries
        Vec4 directionalShadowCascadeFadeDistances{0.0f, 0.0f, 0.0f, 0.0f};

        /// Number of active CSM cascades in directionalShadowViewProjections
        uint32 directionalShadowCascadeCount = 0;

        /// Directional shadow depth bias for manual shadow compare
        float directionalShadowDepthBias = 0.005f;

        /// Directional shadow darkening strength in [0, 1]
        float directionalShadowStrength = 1.0f;

        /// Reciprocal shadow map size used to derive the UV-space shadow filter step
        float directionalShadowInvMapSize = 0.0f;

        /// Shadow PCF radius in texels; multiplied by the inverse map size during upload
        float directionalShadowFilterRadiusTexels = 1.0f;

        /// Receiver normal offset in world units before projecting into the directional shadow map
        float directionalShadowNormalBias = 0.02f;

        /// Enables directional shadow sampling when the frame descriptor has a valid map
        uint8 directionalShadowEnabled = 0;

        /// Enables ray-traced directional shadow mask sampling when the frame descriptor has a valid mask
        uint8 rayTracedShadowEnabled = 0;

        /// Screen-space filter radius in pixels for ray-traced shadow mask softening
        float rayTracedShadowFilterRadiusPixels = 1.0f;

        /// Composition strategy used when a ray-traced directional shadow mask is available
        RayTracedShadowMode rayTracedShadowMode = RayTracedShadowMode::ComplementRaster;

        // =====================================================================
        // Render Targets
        // =====================================================================

        /// Main color target
        RGTextureHandle colorTarget;

        /// Depth target
        RGTextureHandle depthTarget;

        /// Optional motion-vector target for temporal reprojection
        RGTextureHandle velocityTarget;

        /// Source cubemap sampled by SkyboxPass when the frame selects a cubemap sky.
        RGTextureHandle environmentSkyTexture;

        /// Diffuse environment convolution sampled by DefaultLit.
        RGTextureHandle environmentIrradianceTexture;

        /// Specular prefiltered environment sampled by DefaultLit.
        RGTextureHandle environmentPrefilteredTexture;

        /// Split-sum BRDF integration lookup sampled by DefaultLit.
        RGTextureHandle environmentBRDFLUTTexture;

        // =====================================================================
        // Environment / IBL-Approximate Ambient
        // =====================================================================

        /// Diffuse ambient color multiplier for the minimum IBL-approx path
        Vec3 iblDiffuseColor{1.0f, 1.0f, 1.0f};

        /// Diffuse ambient intensity; defaults to the previous DefaultLit value
        float iblDiffuseIntensity = 0.12f;

        /// Specular ambient color multiplier for the minimum IBL-approx path
        Vec3 iblSpecularColor{1.0f, 1.0f, 1.0f};

        /// Specular ambient intensity; defaults to the previous DefaultLit value
        float iblSpecularIntensity = 0.04f;

        /// Allows callers to disable IBL-approx ambient without changing colors
        uint8 iblAmbientEnabled = 1;

        /// Enables real texture IBL sampling when irradiance/prefilter/BRDF resources are ready
        uint8 textureIBLEnabled = 0;

        /// Number of mips in the prefiltered environment map used by texture IBL
        uint32 textureIBLPrefilteredMipLevels = 1;

        /// Exposure/intensity multiplier applied to texture IBL samples
        float textureIBLIntensity = 1.0f;

        /// Legacy non-IBL ambient floor; set to zero when texture IBL is ready
        float ambientFloorIntensity = 0.08f;

        // =====================================================================
        // RenderGraph Reference
        // =====================================================================

        /// Pointer to the render graph (set during BuildRenderGraph)
        /// Allows passes to access actual RHI resources from handles during execution
        RenderGraph* renderGraph = nullptr;

        /// Pointer to the resource view cache (set during BuildRenderGraph)
        /// Allows passes to get cached texture/buffer views
        ResourceViewCache* viewCache = nullptr;

        /// Current recording batch for ephemeral GPU objects; null means no-submit standalone use.
        RenderSubmissionResourceBatch* submissionResourceBatch = nullptr;

        /// Borrowed frame-owned execution contract published by SceneRenderer.
        /// Passes must not retain these pointers beyond the current frame.
        const RenderFrameExecutionPlan* renderFrameExecutionPlan = nullptr;
        const SceneMeshPassPreparation* meshPassPreparation = nullptr;
        const SceneRenderInstanceBatchPlans* instanceBatchPlans = nullptr;
        RenderFrameExecutionReport* renderFrameExecutionReport = nullptr;
        /// Borrowed frame/view-owned visibility output. Passes must not retain it.
        const RenderVisibilityResult* renderVisibility = nullptr;

        // =====================================================================
        // Frame Info
        // =====================================================================

        /// Current frame number
        uint64_t frameNumber = 0;

        /// Backend-neutral Direct-lane instancing policy for this frame.
        RenderInstancingMode instancingMode = RenderInstancingMode::Disabled;

        /// Reset temporal histories for this view, e.g. after camera cuts or large scene jumps
        bool resetTemporalHistory = false;

        /// Time since start (seconds)
        float time = 0.0f;

        /// Delta time (seconds)
        float deltaTime = 0.0f;

        // =====================================================================
        // Methods
        // =====================================================================

        /**
         * @brief Setup view data from a camera
         * @param camera The camera to extract data from
         * @param width Viewport width
         * @param height Viewport height
         */
        void SetupFromCamera(const Camera& camera, uint32_t width, uint32_t height);

        /** @brief Setup numeric view state without retaining a Camera. */
        void SetupFromSnapshot(
            const RenderViewSnapshot& snapshot,
            const Mat4& previousRenderedViewProjection,
            bool previousRenderedViewValid,
            bool resetHistory);

        /**
         * @brief Create RHI viewport struct
         */
        RHIViewport GetRHIViewport() const
        {
            return RHIViewport{
                static_cast<float>(viewportX),
                static_cast<float>(viewportY),
                static_cast<float>(viewportWidth),
                static_cast<float>(viewportHeight),
                0.0f, 1.0f
            };
        }

        /**
         * @brief Create RHI scissor rect
         */
        RHIRect GetRHIScissor() const
        {
            return RHIRect{
                viewportX, viewportY,
                viewportWidth, viewportHeight
            };
        }

        /**
         * @brief Check whether a render-graph texture can be sampled through the view cache.
         */
        bool HasTextureShaderResourceView(RGTextureHandle handle) const;

        /**
         * @brief Resolve the default render-target view for a render-graph texture.
         */
        RHITextureView* GetTextureRenderTargetView(RGTextureHandle handle) const;

        /**
         * @brief Resolve the default shader-resource view for a render-graph texture.
         */
        RHITextureView* GetTextureShaderResourceView(RGTextureHandle handle) const;

        /**
         * @brief Resolve the default depth-stencil view for a render-graph texture.
         */
        RHITextureView* GetTextureDepthStencilView(RGTextureHandle handle) const;
    };

} // namespace RVX
