#pragma once

/**
 * @file ViewData.h
 * @brief View data for rendering - contains camera, viewport, and visible objects
 */

#include "Core/MathTypes.h"
#include "RHI/RHI.h"
#include "Render/Graph/RenderGraph.h"

namespace RVX
{
    class Camera;

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
        // Render Targets
        // =====================================================================
        
        /// Main color target
        RGTextureHandle colorTarget;
        
        /// Depth target
        RGTextureHandle depthTarget;

        // =====================================================================
        // Frame Info
        // =====================================================================
        
        /// Current frame number
        uint64_t frameNumber = 0;
        
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
    };

} // namespace RVX
