#include "Camera/CameraSystem.h"

namespace RVX
{
    uint32 CameraSystem::AddCamera(const Camera& camera)
    {
        m_cameras.push_back(camera);
        return static_cast<uint32>(m_cameras.size() - 1);
    }

    Camera* CameraSystem::GetCamera(uint32 index)
    {
        if (index >= m_cameras.size())
            return nullptr;
        return &m_cameras[index];
    }

    const Camera* CameraSystem::GetCamera(uint32 index) const
    {
        if (index >= m_cameras.size())
            return nullptr;
        return &m_cameras[index];
    }
} // namespace RVX
