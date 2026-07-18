#pragma once

/**
 * @file WorldCameraBridge.h
 * @brief Extract active camera state into an owned render-view value.
 */

#include "Core/Types.h"
#include "RenderContracts/RenderFramePacket.h"

namespace RVX
{
    class Camera;
    class World;

    struct RenderViewExtractionParameters
    {
        uint32 viewportX = 0;
        uint32 viewportY = 0;
        uint32 viewportWidth = 0;
        uint32 viewportHeight = 0;
        float32 nearPlane = 0.1f;
        float32 farPlane = 1000.0f;
        float32 absoluteTime = 0.0f;
        float32 deltaTime = 0.0f;
        float32 exposure = 1.0f;
    };

    enum class WorldCameraBridgeCode : uint8
    {
        Complete = 0,
        NullWorld,
        MissingActiveCamera
    };

    /**
     * @brief Extracts camera-facing data from World without exposing World internals to Render.
     */
    class WorldCameraBridge
    {
    public:
        [[nodiscard]] WorldCameraBridgeCode Extract(
            World* world,
            const RenderViewExtractionParameters& parameters,
            RenderViewSnapshot& outView) const;

        // Task-13 compatibility seam for the legacy synchronous renderer.
        Camera* GetActiveCamera(World* world) const;
    };

} // namespace RVX
