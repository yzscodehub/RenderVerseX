#include "Scene/SceneEntity.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

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

SceneEntity::~SceneEntity()
{
    // Detach all components
    for (auto& [typeId, component] : m_components)
    {
        if (component)
        {
            component->OnDetach();
        }
    }
    m_components.clear();
}

// =============================================================================
// Hierarchy
// =============================================================================

void SceneEntity::AddChild(SceneEntity* child)
{
    if (!child || child == this)
        return;

    // Prevent circular hierarchy
    if (child->IsAncestorOf(this))
        return;

    // Remove from previous parent
    if (child->m_parent)
    {
        child->m_parent->RemoveChild(child);
    }

    // Add to this entity
    child->m_parent = this;
    m_children.push_back(child);

    // Child's world transform is now dirty
    child->MarkTransformDirty();
}

bool SceneEntity::RemoveChild(SceneEntity* child)
{
    if (!child)
        return false;

    auto it = std::find(m_children.begin(), m_children.end(), child);
    if (it != m_children.end())
    {
        child->m_parent = nullptr;
        m_children.erase(it);
        child->MarkTransformDirty();
        return true;
    }
    return false;
}

void SceneEntity::SetParent(SceneEntity* parent)
{
    if (parent == m_parent)
        return;

    if (parent)
    {
        parent->AddChild(this);
    }
    else
    {
        // Remove from current parent
        if (m_parent)
        {
            m_parent->RemoveChild(this);
        }
    }
}

bool SceneEntity::IsAncestorOf(const SceneEntity* entity) const
{
    if (!entity)
        return false;

    const SceneEntity* current = entity->m_parent;
    while (current)
    {
        if (current == this)
            return true;
        current = current->m_parent;
    }
    return false;
}

bool SceneEntity::IsDescendantOf(const SceneEntity* entity) const
{
    if (!entity)
        return false;
    return entity->IsAncestorOf(this);
}

SceneEntity* SceneEntity::GetRoot()
{
    SceneEntity* current = this;
    while (current->m_parent)
    {
        current = current->m_parent;
    }
    return current;
}

const SceneEntity* SceneEntity::GetRoot() const
{
    const SceneEntity* current = this;
    while (current->m_parent)
    {
        current = current->m_parent;
    }
    return current;
}

void SceneEntity::MarkTransformDirty()
{
    m_transformDirty = true;
    m_boundsDirty = true;  // World bounds depend on world transform
    MarkSpatialDirty();
    MarkChildrenTransformDirty();
}

void SceneEntity::MarkChildrenTransformDirty()
{
    for (auto* child : m_children)
    {
        child->m_transformDirty = true;
        child->m_boundsDirty = true;
        child->MarkSpatialDirty();
        child->MarkChildrenTransformDirty();
    }
}

// =============================================================================
// Bounds (Hybrid Mode)
// =============================================================================

AABB SceneEntity::GetWorldBounds() const
{
    if (m_boundsDirty)
    {
        m_cachedWorldBounds = ComputeBoundsFromComponents();
        m_boundsDirty = false;
    }
    return m_cachedWorldBounds;
}

void SceneEntity::MarkBoundsDirty()
{
    m_boundsDirty = true;
    MarkSpatialDirty();
    
    // Parent bounds may also be affected (if parent includes children bounds)
    if (m_parent)
    {
        m_parent->MarkBoundsDirty();
    }
}

AABB SceneEntity::ComputeBoundsFromComponents() const
{
    AABB combined;
    
    // Start with local bounds if set manually
    if (m_localBounds.IsValid())
    {
        combined = m_localBounds.Transformed(GetWorldMatrix());
    }
    
    // Iterate over all components that provide bounds
    for (const auto& [typeId, component] : m_components)
    {
        if (component && component->IsEnabled() && component->ProvidesBounds())
        {
            AABB compBounds = component->GetLocalBounds();
            if (compBounds.IsValid())
            {
                combined.Expand(compBounds.Transformed(GetWorldMatrix()));
            }
        }
    }
    
    // Expand to include children bounds
    for (const auto* child : m_children)
    {
        AABB childBounds = child->GetWorldBounds();
        if (childBounds.IsValid())
        {
            combined.Expand(childBounds);
        }
    }
    
    // If no bounds found, return a small default at entity position
    if (!combined.IsValid())
    {
        Vec3 worldPos = GetWorldPosition();
        const float defaultSize = 0.5f;
        combined = AABB(
            worldPos - Vec3(defaultSize),
            worldPos + Vec3(defaultSize)
        );
    }
    
    return combined;
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

Mat4 SceneEntity::GetLocalMatrix() const
{
    Mat4 localMatrix = Mat4(1.0f);
    localMatrix = glm::translate(localMatrix, m_position);
    localMatrix *= glm::mat4_cast(m_rotation);
    localMatrix = glm::scale(localMatrix, m_scale);
    return localMatrix;
}

Mat4 SceneEntity::GetWorldMatrix() const
{
    if (m_transformDirty)
    {
        Mat4 localMatrix = GetLocalMatrix();
        
        if (m_parent)
        {
            m_worldMatrix = m_parent->GetWorldMatrix() * localMatrix;
        }
        else
        {
            m_worldMatrix = localMatrix;
        }
        m_transformDirty = false;
    }
    return m_worldMatrix;
}

Vec3 SceneEntity::GetWorldPosition() const
{
    if (m_parent)
    {
        Mat4 worldMatrix = GetWorldMatrix();
        return Vec3(worldMatrix[3]);
    }
    return m_position;
}

Quat SceneEntity::GetWorldRotation() const
{
    if (m_parent)
    {
        return m_parent->GetWorldRotation() * m_rotation;
    }
    return m_rotation;
}

Vec3 SceneEntity::GetWorldScale() const
{
    if (m_parent)
    {
        return m_parent->GetWorldScale() * m_scale;
    }
    return m_scale;
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
    MarkBoundsDirty();
}

// =============================================================================
// Components
// =============================================================================

void SceneEntity::TickComponents(float deltaTime)
{
    for (auto& [typeId, component] : m_components)
    {
        if (component && component->IsEnabled())
        {
            component->Tick(deltaTime);
        }
    }
}

} // namespace RVX
