#pragma once

#include "Camera/Camera.h"
#include "Core/Types.h"
#include <vector>

namespace RVX
{
    /// @brief Registry for managing multiple cameras (renamed from CameraSystem to avoid
    ///        confusion with Engine/Systems/CameraSystem which is an ISystem implementation)
    class CameraRegistry
    {
    public:
        uint32 AddCamera(const Camera& camera);
        Camera* GetCamera(uint32 index);
        const Camera* GetCamera(uint32 index) const;
        uint32 GetCameraCount() const { return static_cast<uint32>(m_cameras.size()); }

        void RemoveCamera(uint32 index);
        void Clear() { m_cameras.clear(); }

    private:
        std::vector<Camera> m_cameras;
    };
} // namespace RVX
