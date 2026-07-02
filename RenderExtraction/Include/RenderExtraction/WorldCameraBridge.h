#pragma once

/**
 * @file WorldCameraBridge.h
 * @brief World camera queries for render entry points.
 */

namespace RVX
{
    class Camera;
    class World;

    /**
     * @brief Extracts camera-facing data from World without exposing World internals to Render.
     */
    class WorldCameraBridge
    {
    public:
        Camera* GetActiveCamera(World* world) const;
    };

} // namespace RVX
