#pragma once

/**
 * @file CameraRegistry.h
 * @brief Multi-camera management
 */

#include "Runtime/Camera/Camera.h"
#include "Core/Types.h"
#include <vector>

namespace RVX
{
    /**
     * @brief Registry for managing multiple cameras
     * 
     * Useful for multi-viewport rendering, split-screen, picture-in-picture, etc.
     */
    class CameraRegistry
    {
    public:
        /// Add a camera and return its index
        uint32 AddCamera(const Camera& camera);
        
        /// Get camera by index
        Camera* GetCamera(uint32 index);
        const Camera* GetCamera(uint32 index) const;
        
        /// Get number of registered cameras
        uint32 GetCameraCount() const { return static_cast<uint32>(m_cameras.size()); }

        /// Remove camera by index
        void RemoveCamera(uint32 index);
        
        /// Clear all cameras
        void Clear() { m_cameras.clear(); }

    private:
        std::vector<Camera> m_cameras;
    };
} // namespace RVX
