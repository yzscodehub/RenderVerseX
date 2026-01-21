#include "Scene/SceneEntity.h"
#include <glm/gtc/matrix_transform.hpp>

namespace RVX
{

std::atomic<SceneEntity::Handle> SceneEntity::s_nextHandle{1};

SceneEntity::Handle SceneEntity::GenerateHandle()
{
    return s_nextHandle.fetch_add(1, std::memory_order_relaxed);
}

SceneEntity::SceneEntity(const std::string& name)
    : m_handle(GenerateHandle())
    , m_name(name)
{
}

AABB SceneEntity::GetWorldBounds() const
{
    if (!m_localBounds.IsValid())
    {
        // Return a small default bounds at entity position
        const float defaultSize = 0.5f;
        return AABB(
            m_position - Vec3(defaultSize),
            m_position + Vec3(defaultSize)
        );
    }

    // Transform local bounds to world space
    return m_localBounds.Transformed(GetWorldMatrix());
}

void SceneEntity::SetPosition(const Vec3& position)
{
    if (m_position != position)
    {
        m_position = position;
        MarkTransformDirty();
    }
}

void SceneEntity::SetRotation(const Quat& rotation)
{
    if (m_rotation != rotation)
    {
        m_rotation = rotation;
        MarkTransformDirty();
    }
}

void SceneEntity::SetScale(const Vec3& scale)
{
    if (m_scale != scale)
    {
        m_scale = scale;
        MarkTransformDirty();
    }
}

Mat4 SceneEntity::GetWorldMatrix() const
{
    if (m_transformDirty)
    {
        m_worldMatrix = Mat4(1.0f);
        m_worldMatrix = glm::translate(m_worldMatrix, m_position);
        m_worldMatrix *= glm::mat4_cast(m_rotation);
        m_worldMatrix = glm::scale(m_worldMatrix, m_scale);
        m_transformDirty = false;
    }
    return m_worldMatrix;
}

void SceneEntity::Translate(const Vec3& delta)
{
    SetPosition(m_position + delta);
}

void SceneEntity::Rotate(const Quat& delta)
{
    SetRotation(delta * m_rotation);
}

void SceneEntity::RotateAround(const Vec3& axis, float angle)
{
    Quat rotation = glm::angleAxis(angle, glm::normalize(axis));
    Rotate(rotation);
}

void SceneEntity::SetLocalBounds(const AABB& bounds)
{
    m_localBounds = bounds;
    MarkSpatialDirty();
}

} // namespace RVX
