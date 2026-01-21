#include "Runtime/Camera/CameraRegistry.h"

namespace RVX
{
    uint32 CameraRegistry::AddCamera(const Camera& camera)
    {
        m_cameras.push_back(camera);
        return static_cast<uint32>(m_cameras.size() - 1);
    }

    Camera* CameraRegistry::GetCamera(uint32 index)
    {
        if (index >= m_cameras.size())
            return nullptr;
        return &m_cameras[index];
    }

    const Camera* CameraRegistry::GetCamera(uint32 index) const
    {
        if (index >= m_cameras.size())
            return nullptr;
        return &m_cameras[index];
    }

    void CameraRegistry::RemoveCamera(uint32 index)
    {
        if (index < m_cameras.size())
        {
            m_cameras.erase(m_cameras.begin() + index);
        }
    }
} // namespace RVX
