#include "Scene/SceneComponent.h"

#include <algorithm>

namespace RVX
{

SceneComponent::~SceneComponent()
{
    DetachFromComponent();

    for (SceneComponent* child : m_attachChildren)
    {
        if (child)
        {
            child->m_attachParent = nullptr;
            child->MarkTransformDirty();
        }
    }
    m_attachChildren.clear();
}

void SceneComponent::SetRelativeLocation(const Vec3& location)
{
    if (m_relativeLocation != location)
    {
        m_relativeLocation = location;
        MarkTransformDirty();
    }
}

void SceneComponent::SetRelativeRotation(const Quat& rotation)
{
    if (m_relativeRotation != rotation)
    {
        m_relativeRotation = rotation;
        MarkTransformDirty();
    }
}

void SceneComponent::SetRelativeScale(const Vec3& scale)
{
    if (m_relativeScale != scale)
    {
        m_relativeScale = scale;
        MarkTransformDirty();
    }
}

Mat4 SceneComponent::GetRelativeTransform() const
{
    Mat4 transform(1.0f);
    transform = glm::translate(transform, m_relativeLocation);
    transform *= glm::mat4_cast(m_relativeRotation);
    transform = glm::scale(transform, m_relativeScale);
    return transform;
}

const Mat4& SceneComponent::GetWorldTransform() const
{
    if (m_transformDirty)
    {
        if (m_attachParent)
        {
            m_worldTransform = m_attachParent->GetWorldTransform() * GetRelativeTransform();
        }
        else
        {
            m_worldTransform = GetRelativeTransform();
        }
        m_transformDirty = false;
    }
    return m_worldTransform;
}

Vec3 SceneComponent::GetWorldLocation() const
{
    return Vec3(GetWorldTransform()[3]);
}

bool SceneComponent::AttachToComponent(SceneComponent* parent)
{
    if (!parent || parent == this || WouldCreateCycle(parent))
        return false;

    if (m_attachParent == parent)
        return true;

    DetachFromComponent();
    m_attachParent = parent;
    parent->m_attachChildren.push_back(this);
    MarkTransformDirty();
    return true;
}

void SceneComponent::DetachFromComponent()
{
    if (!m_attachParent)
        return;

    auto& siblings = m_attachParent->m_attachChildren;
    siblings.erase(std::remove(siblings.begin(), siblings.end(), this), siblings.end());
    m_attachParent = nullptr;
    MarkTransformDirty();
}

void SceneComponent::MarkTransformDirty()
{
    m_transformDirty = true;
    OnTransformChanged();

    for (SceneComponent* child : m_attachChildren)
    {
        if (child)
        {
            child->MarkTransformDirty();
        }
    }
}

bool SceneComponent::WouldCreateCycle(const SceneComponent* parent) const
{
    const SceneComponent* current = parent;
    while (current)
    {
        if (current == this)
            return true;

        current = current->m_attachParent;
    }
    return false;
}

} // namespace RVX
