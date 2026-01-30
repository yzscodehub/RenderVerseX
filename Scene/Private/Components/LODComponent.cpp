#include "Scene/Components/LODComponent.h"
#include "Scene/SceneEntity.h"
#include "Resource/Types/MeshResource.h"
#include <cmath>
#include <algorithm>

namespace RVX
{

void LODComponent::OnAttach()
{
    NotifyBoundsChanged();
}

void LODComponent::OnDetach()
{
    // Nothing special needed
}

void LODComponent::Tick(float deltaTime)
{
    // Update transition if in progress
    if (m_isTransitioning)
    {
        UpdateTransition(deltaTime);
    }
}

AABB LODComponent::GetLocalBounds() const
{
    // Return bounds of current LOD mesh
    if (m_currentLOD >= 0 && m_currentLOD < static_cast<int>(m_levels.size()))
    {
        return GetLODBounds(m_currentLOD);
    }

    // Fallback: return first LOD bounds or empty
    if (!m_levels.empty())
    {
        return GetLODBounds(0);
    }

    return AABB();
}

void LODComponent::AddLODLevel(const LODLevel& level)
{
    m_levels.push_back(level);
    NotifyBoundsChanged();
}

void LODComponent::InsertLODLevel(size_t index, const LODLevel& level)
{
    if (index <= m_levels.size())
    {
        m_levels.insert(m_levels.begin() + index, level);
        NotifyBoundsChanged();
    }
}

void LODComponent::RemoveLODLevel(size_t index)
{
    if (index < m_levels.size())
    {
        m_levels.erase(m_levels.begin() + index);
        
        // Adjust current LOD if needed
        if (m_currentLOD >= static_cast<int>(m_levels.size()))
        {
            m_currentLOD = static_cast<int>(m_levels.size()) - 1;
        }
        
        NotifyBoundsChanged();
    }
}

void LODComponent::ClearLODLevels()
{
    m_levels.clear();
    m_currentLOD = 0;
    NotifyBoundsChanged();
}

const LODLevel* LODComponent::GetLODLevel(size_t index) const
{
    if (index < m_levels.size())
    {
        return &m_levels[index];
    }
    return nullptr;
}

LODLevel* LODComponent::GetLODLevel(size_t index)
{
    if (index < m_levels.size())
    {
        return &m_levels[index];
    }
    return nullptr;
}

void LODComponent::ForceLOD(int lodIndex)
{
    if (lodIndex >= 0 && lodIndex < static_cast<int>(m_levels.size()))
    {
        m_forcedLOD = lodIndex;
        
        if (m_currentLOD != lodIndex)
        {
            m_previousLOD = m_currentLOD;
            m_currentLOD = lodIndex;
            
            if (m_fadeMode != LODFadeMode::None)
            {
                m_isTransitioning = true;
                m_fadeProgress = 0.0f;
            }
        }
    }
}

float LODComponent::CalculateScreenSize(const Vec3& cameraPosition, float fov, float screenHeight) const
{
    SceneEntity* owner = GetOwner();
    if (!owner)
    {
        return 0.0f;
    }

    // Get object bounds and world position
    AABB bounds = GetLocalBounds();
    Vec3 worldPos = owner->GetWorldPosition();
    Vec3 worldScale = owner->GetWorldScale();

    // Calculate bounding sphere radius (scaled)
    Vec3 halfExtents = (bounds.GetMax() - bounds.GetMin()) * 0.5f * worldScale;
    float boundingRadius = length(halfExtents);

    // Calculate distance to camera
    float distance = length(worldPos - cameraPosition);
    if (distance < 0.001f)
    {
        return 1.0f;  // Object at camera position
    }

    // Calculate projected screen size
    // screenSize = (objectRadius / distance) * screenHeight / (2 * tan(fov/2))
    float tanHalfFov = std::tan(fov * 0.5f);
    float projectedSize = (boundingRadius / distance) / tanHalfFov;

    return projectedSize;
}

int LODComponent::CalculateLODForScreenSize(float screenSize) const
{
    // Apply LOD bias
    float adjustedSize = screenSize * std::pow(2.0f, -m_lodBias);

    // Find appropriate LOD level
    for (size_t i = 0; i < m_levels.size(); ++i)
    {
        if (adjustedSize >= m_levels[i].screenSizeThreshold)
        {
            return static_cast<int>(i);
        }
    }

    // Return lowest LOD if screen size is below all thresholds
    return static_cast<int>(m_levels.size()) - 1;
}

int LODComponent::CalculateLODForDistance(float distance) const
{
    // Find appropriate LOD level based on distance
    for (size_t i = 0; i < m_levels.size(); ++i)
    {
        if (m_levels[i].distanceThreshold > 0.0f && distance <= m_levels[i].distanceThreshold)
        {
            return static_cast<int>(i);
        }
    }

    // Return lowest LOD if distance exceeds all thresholds
    return static_cast<int>(m_levels.size()) - 1;
}

void LODComponent::UpdateLOD(const Vec3& cameraPosition, float fov, float screenHeight)
{
    if (m_levels.empty())
    {
        return;
    }

    // If LOD is forced, don't update automatically
    if (m_forcedLOD >= 0)
    {
        return;
    }

    int newLOD;

    if (m_useDistanceLOD)
    {
        SceneEntity* owner = GetOwner();
        float distance = owner ? length(owner->GetWorldPosition() - cameraPosition) : 0.0f;
        newLOD = CalculateLODForDistance(distance);
    }
    else
    {
        float screenSize = CalculateScreenSize(cameraPosition, fov, screenHeight);
        m_lastScreenSize = screenSize;

        // Check for culling
        if (m_autoCull && screenSize < m_cullScreenSize)
        {
            m_isCulled = true;
            return;
        }
        m_isCulled = false;

        newLOD = CalculateLODForScreenSize(screenSize);
    }

    // Clamp to valid range
    newLOD = std::clamp(newLOD, 0, static_cast<int>(m_levels.size()) - 1);

    // Check if LOD changed
    if (newLOD != m_currentLOD)
    {
        m_previousLOD = m_currentLOD;
        m_currentLOD = newLOD;

        if (m_fadeMode != LODFadeMode::None)
        {
            m_isTransitioning = true;
            m_fadeProgress = 0.0f;
        }

        NotifyBoundsChanged();
    }
}

void LODComponent::UpdateTransition(float deltaTime)
{
    if (!m_isTransitioning || m_crossFadeDuration <= 0.0f)
    {
        m_isTransitioning = false;
        m_fadeProgress = 1.0f;
        return;
    }

    m_fadeProgress += deltaTime / m_crossFadeDuration;

    if (m_fadeProgress >= 1.0f)
    {
        m_fadeProgress = 1.0f;
        m_isTransitioning = false;
    }
}

AABB LODComponent::GetLODBounds(size_t lodIndex) const
{
    if (lodIndex >= m_levels.size())
    {
        return AABB();
    }

    const auto& level = m_levels[lodIndex];
    if (!level.mesh.IsValid() || !level.mesh.IsLoaded())
    {
        return AABB();
    }

    return level.mesh->GetBounds();
}

} // namespace RVX
