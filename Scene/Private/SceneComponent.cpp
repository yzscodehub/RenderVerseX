#include "Scene/SceneComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneRuntime.h"
#include "Scene/TransformStore.h"

#include <algorithm>

namespace RVX
{
namespace
{
    void DecomposeTransform(const Mat4& transform,
                            Vec3& location,
                            Quat& rotation,
                            Vec3& scale)
    {
        location = Vec3(transform[3]);
        Vec3 xAxis(transform[0]);
        Vec3 yAxis(transform[1]);
        Vec3 zAxis(transform[2]);
        scale = Vec3(glm::length(xAxis), glm::length(yAxis), glm::length(zAxis));
        if (scale.x > 0.0f) xAxis /= scale.x;
        if (scale.y > 0.0f) yAxis /= scale.y;
        if (scale.z > 0.0f) zAxis /= scale.z;
        Mat3 rotationMatrix(xAxis, yAxis, zAxis);
        if (glm::determinant(rotationMatrix) < 0.0f)
        {
            scale.x = -scale.x;
            rotationMatrix[0] = -rotationMatrix[0];
        }
        rotation = glm::normalize(glm::quat_cast(rotationMatrix));
    }
}

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
    if (GetRelativeLocation() != location)
    {
        m_relativeLocation = location;
        if (auto* store = GetRuntimeTransformStore())
            store->SetLocalLocation(GetComponentHandle(), location);
        MarkTransformDirty();
    }
}

void SceneComponent::SetRelativeRotation(const Quat& rotation)
{
    if (GetRelativeRotation() != rotation)
    {
        m_relativeRotation = rotation;
        if (auto* store = GetRuntimeTransformStore())
            store->SetLocalRotation(GetComponentHandle(), rotation);
        MarkTransformDirty();
    }
}

void SceneComponent::SetRelativeScale(const Vec3& scale)
{
    if (GetRelativeScale() != scale)
    {
        m_relativeScale = scale;
        if (auto* store = GetRuntimeTransformStore())
            store->SetLocalScale(GetComponentHandle(), scale);
        MarkTransformDirty();
    }
}

const Vec3& SceneComponent::GetRelativeLocation() const
{
    if (auto* store = GetRuntimeTransformStore())
        return store->GetLocalLocation(GetComponentHandle());
    return m_relativeLocation;
}

const Quat& SceneComponent::GetRelativeRotation() const
{
    if (auto* store = GetRuntimeTransformStore())
        return store->GetLocalRotation(GetComponentHandle());
    return m_relativeRotation;
}

const Vec3& SceneComponent::GetRelativeScale() const
{
    if (auto* store = GetRuntimeTransformStore())
        return store->GetLocalScale(GetComponentHandle());
    return m_relativeScale;
}

Mat4 SceneComponent::GetRelativeTransform() const
{
    Mat4 transform(1.0f);
    transform = glm::translate(transform, GetRelativeLocation());
    transform *= glm::mat4_cast(GetRelativeRotation());
    transform = glm::scale(transform, GetRelativeScale());
    return transform;
}

const Mat4& SceneComponent::GetWorldTransform() const
{
    if (auto* store = GetRuntimeTransformStore())
        return store->GetWorldTransform(GetComponentHandle());

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

const Mat4& SceneComponent::GetPreviousWorldTransform() const
{
    if (auto* store = GetRuntimeTransformStore())
        return store->GetPreviousWorldTransform(GetComponentHandle());
    return GetWorldTransform();
}

Vec3 SceneComponent::GetWorldLocation() const
{
    return Vec3(GetWorldTransform()[3]);
}

Quat SceneComponent::GetWorldRotation() const
{
    if (auto* store = GetRuntimeTransformStore())
        return store->GetWorldRotation(GetComponentHandle());

    if (m_attachParent)
    {
        return m_attachParent->GetWorldRotation() * m_relativeRotation;
    }
    return m_relativeRotation;
}

Vec3 SceneComponent::GetWorldScale() const
{
    if (auto* store = GetRuntimeTransformStore())
        return store->GetWorldScale(GetComponentHandle());

    if (m_attachParent)
    {
        return m_attachParent->GetWorldScale() * m_relativeScale;
    }
    return m_relativeScale;
}

uint64 SceneComponent::GetLocalTransformRevision() const
{
    if (auto* store = GetRuntimeTransformStore())
        return store->GetLocalRevision(GetComponentHandle());
    return 0;
}

uint64 SceneComponent::GetWorldTransformRevision() const
{
    if (auto* store = GetRuntimeTransformStore())
        return store->GetWorldRevision(GetComponentHandle());
    return 0;
}

bool SceneComponent::AttachToComponent(SceneComponent* parent,
                                       AttachmentTransformRule rule)
{
    if (!parent || parent == this || WouldCreateCycle(parent))
        return false;

    if (m_attachParent == parent)
        return true;

    TransformStore* childStore = GetRuntimeTransformStore();
    TransformStore* parentStore = parent->GetRuntimeTransformStore();
    if (childStore || parentStore)
    {
        if (!childStore || childStore != parentStore ||
            !childStore->Attach(GetComponentHandle(), parent->GetComponentHandle(), rule))
        {
            return false;
        }
    }

    if (m_attachParent)
    {
        auto& siblings = m_attachParent->m_attachChildren;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), this), siblings.end());
    }
    m_attachParent = parent;
    parent->m_attachChildren.push_back(this);
    MarkTransformDirty();
    return true;
}

void SceneComponent::DetachFromComponent(AttachmentTransformRule rule)
{
    if (!m_attachParent)
        return;

    Mat4 worldBefore(1.0f);
    const bool keepWorld = rule == AttachmentTransformRule::KeepWorld;
    if (keepWorld && !GetRuntimeTransformStore())
        worldBefore = GetWorldTransform();

    if (auto* store = GetRuntimeTransformStore())
        store->Detach(GetComponentHandle(), rule);

    auto& siblings = m_attachParent->m_attachChildren;
    siblings.erase(std::remove(siblings.begin(), siblings.end(), this), siblings.end());
    m_attachParent = nullptr;

    if (keepWorld && !GetRuntimeTransformStore())
    {
        DecomposeTransform(worldBefore,
                           m_relativeLocation,
                           m_relativeRotation,
                           m_relativeScale);
    }
    MarkTransformDirty();
}

void SceneComponent::MarkTransformDirty()
{
    m_transformDirty = true;
    OnTransformChanged();

    if (auto* entity = dynamic_cast<SceneEntity*>(GetOwner()))
    {
        entity->NotifySceneComponentTransformChanged(this);
    }

    if (Actor* owner = GetOwner())
    {
        if (Scene* scene = owner->GetScene())
            scene->NotifyTransformChanged(GetComponentHandle());
    }

    for (SceneComponent* child : m_attachChildren)
    {
        if (child)
        {
            child->MarkTransformDirty();
        }
    }
}

TransformStore* SceneComponent::GetRuntimeTransformStore() const
{
    Actor* owner = GetOwner();
    Scene* scene = owner ? owner->GetScene() : nullptr;
    if (!scene || !GetComponentHandle().IsValid() ||
        !scene->GetTransformStore().Contains(GetComponentHandle()))
    {
        return nullptr;
    }
    return &scene->GetTransformStore();
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
