#pragma once

#include "Camera/Camera.h"
#include "Core/Types.h"
#include <vector>

namespace RVX
{
    class CameraSystem
    {
    public:
        uint32 AddCamera(const Camera& camera);
        Camera* GetCamera(uint32 index);
        const Camera* GetCamera(uint32 index) const;
        uint32 GetCameraCount() const { return static_cast<uint32>(m_cameras.size()); }

    private:
        std::vector<Camera> m_cameras;
    };
} // namespace RVX
