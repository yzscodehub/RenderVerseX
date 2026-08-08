#include "Scene/TransformStore.h"

#include <algorithm>

namespace RVX
{
namespace
{
    const Vec3 kZeroLocation{0.0f};
    const Quat kIdentityRotation{1.0f, 0.0f, 0.0f, 0.0f};
    const Vec3 kUnitScale{1.0f};
    const Mat4 kIdentityTransform{1.0f};

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

void TransformStore::Reserve(size_t capacity)
{
    m_records.reserve(capacity);
    m_dirtyHandles.reserve(capacity);
    m_previousRollForwardHandles.reserve(capacity);
}

bool TransformStore::Register(ComponentHandle handle,
                              const Vec3& location,
                              const Quat& rotation,
                              const Vec3& scale)
{
    if (!handle.IsValid() || Contains(handle))
        return false;

    TransformStoreRecord record;
    record.localLocation = location;
    record.localRotation = rotation;
    record.localScale = scale;
    m_records.emplace(handle, std::move(record));
    m_dirtyHandles.push_back(handle);
    return true;
}

void TransformStore::Unregister(ComponentHandle handle)
{
    auto it = m_records.find(handle);
    if (it == m_records.end())
        return;

    const Mat4 parentlessWorld = GetWorldTransform(handle);
    const ComponentHandle parent = it->second.parent;
    if (auto* parentRecord = Find(parent))
    {
        auto& siblings = parentRecord->children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), handle), siblings.end());
    }

    const auto children = it->second.children;
    for (ComponentHandle child : children)
    {
        if (auto* childRecord = Find(child))
        {
            const Mat4 childWorld = GetWorldTransform(child);
            childRecord->parent = InvalidComponentHandle;
            SetLocalFromMatrix(*childRecord, childWorld);
            MarkDirty(child);
        }
    }

    (void)parentlessWorld;
    m_records.erase(it);
    m_dirtyHandles.erase(std::remove(m_dirtyHandles.begin(), m_dirtyHandles.end(), handle),
                         m_dirtyHandles.end());
    m_previousRollForwardHandles.erase(
        std::remove(m_previousRollForwardHandles.begin(),
                    m_previousRollForwardHandles.end(),
                    handle),
        m_previousRollForwardHandles.end());
}

bool TransformStore::Contains(ComponentHandle handle) const
{
    return handle.IsValid() && m_records.find(handle) != m_records.end();
}

const Vec3& TransformStore::GetLocalLocation(ComponentHandle handle) const
{
    const auto* record = Find(handle);
    return record ? record->localLocation : kZeroLocation;
}

const Quat& TransformStore::GetLocalRotation(ComponentHandle handle) const
{
    const auto* record = Find(handle);
    return record ? record->localRotation : kIdentityRotation;
}

const Vec3& TransformStore::GetLocalScale(ComponentHandle handle) const
{
    const auto* record = Find(handle);
    return record ? record->localScale : kUnitScale;
}

void TransformStore::SetLocalLocation(ComponentHandle handle, const Vec3& location)
{
    auto* record = Find(handle);
    if (record && record->localLocation != location)
    {
        record->localLocation = location;
        ++record->localRevision;
        MarkDirty(handle);
    }
}

void TransformStore::SetLocalRotation(ComponentHandle handle, const Quat& rotation)
{
    auto* record = Find(handle);
    if (record && record->localRotation != rotation)
    {
        record->localRotation = rotation;
        ++record->localRevision;
        MarkDirty(handle);
    }
}

void TransformStore::SetLocalScale(ComponentHandle handle, const Vec3& scale)
{
    auto* record = Find(handle);
    if (record && record->localScale != scale)
    {
        record->localScale = scale;
        ++record->localRevision;
        MarkDirty(handle);
    }
}

bool TransformStore::Attach(ComponentHandle child,
                            ComponentHandle parent,
                            AttachmentTransformRule rule)
{
    auto* childRecord = Find(child);
    auto* parentRecord = Find(parent);
    if (!childRecord || !parentRecord || child == parent || WouldCreateCycle(child, parent))
        return false;

    if (childRecord->parent == parent)
        return true;

    const Mat4 worldBefore = GetWorldTransform(child);
    if (auto* oldParent = Find(childRecord->parent))
    {
        auto& siblings = oldParent->children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), child), siblings.end());
    }

    childRecord->parent = parent;
    parentRecord->children.push_back(child);
    if (rule == AttachmentTransformRule::KeepWorld)
    {
        const Mat4 local = glm::inverse(GetWorldTransform(parent)) * worldBefore;
        SetLocalFromMatrix(*childRecord, local);
    }
    ++childRecord->localRevision;
    MarkDirty(child);
    return true;
}

bool TransformStore::Detach(ComponentHandle child, AttachmentTransformRule rule)
{
    auto* childRecord = Find(child);
    if (!childRecord || !childRecord->parent.IsValid())
        return false;

    const Mat4 worldBefore = GetWorldTransform(child);
    if (auto* parentRecord = Find(childRecord->parent))
    {
        auto& siblings = parentRecord->children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), child), siblings.end());
    }
    childRecord->parent = InvalidComponentHandle;
    if (rule == AttachmentTransformRule::KeepWorld)
        SetLocalFromMatrix(*childRecord, worldBefore);
    ++childRecord->localRevision;
    MarkDirty(child);
    return true;
}

ComponentHandle TransformStore::GetParent(ComponentHandle handle) const
{
    const auto* record = Find(handle);
    return record ? record->parent : InvalidComponentHandle;
}

const Mat4& TransformStore::GetWorldTransform(ComponentHandle handle)
{
    std::vector<ComponentHandle> stack;
    ResolveOne(handle, stack);
    auto* record = Find(handle);
    return record ? record->worldTransform : kIdentityTransform;
}

const Mat4& TransformStore::GetPreviousWorldTransform(ComponentHandle handle) const
{
    const auto* record = Find(handle);
    return record ? record->previousWorldTransform : kIdentityTransform;
}

Quat TransformStore::GetWorldRotation(ComponentHandle handle)
{
    Vec3 scale;
    Quat rotation;
    Vec3 translation;
    DecomposeTransform(GetWorldTransform(handle), translation, rotation, scale);
    return rotation;
}

Vec3 TransformStore::GetWorldScale(ComponentHandle handle)
{
    Vec3 scale;
    Quat rotation;
    Vec3 translation;
    DecomposeTransform(GetWorldTransform(handle), translation, rotation, scale);
    return scale;
}

uint64 TransformStore::GetLocalRevision(ComponentHandle handle) const
{
    const auto* record = Find(handle);
    return record ? record->localRevision : 0;
}

uint64 TransformStore::GetWorldRevision(ComponentHandle handle) const
{
    const auto* record = Find(handle);
    return record ? record->worldRevision : 0;
}

void TransformStore::BeginFrame()
{
    Resolve();
    for (ComponentHandle handle : m_previousRollForwardHandles)
    {
        if (TransformStoreRecord* record = Find(handle))
        {
            record->previousWorldTransform = record->worldTransform;
            record->previousRollForwardPending = false;
        }
    }
    m_previousRollForwardHandles.clear();
}

void TransformStore::Resolve()
{
    m_lastResolveStats = {
        m_records.size(),
        m_dirtyHandles.size(),
        0,
    };
    m_collectResolveStats = true;
    const auto dirty = m_dirtyHandles;
    for (ComponentHandle handle : dirty)
    {
        std::vector<ComponentHandle> stack;
        ResolveOne(handle, stack);
    }
    m_collectResolveStats = false;
    m_dirtyHandles.clear();
}

TransformStoreRecord* TransformStore::Find(ComponentHandle handle)
{
    auto it = m_records.find(handle);
    return it != m_records.end() ? &it->second : nullptr;
}

const TransformStoreRecord* TransformStore::Find(ComponentHandle handle) const
{
    auto it = m_records.find(handle);
    return it != m_records.end() ? &it->second : nullptr;
}

void TransformStore::MarkDirty(ComponentHandle handle)
{
    auto* record = Find(handle);
    if (!record)
        return;

    if (!record->dirty)
    {
        record->dirty = true;
        m_dirtyHandles.push_back(handle);
    }
    for (ComponentHandle child : record->children)
        MarkDirty(child);
}

bool TransformStore::WouldCreateCycle(ComponentHandle child, ComponentHandle parent) const
{
    ComponentHandle current = parent;
    while (current.IsValid())
    {
        if (current == child)
            return true;
        const auto* record = Find(current);
        if (!record)
            return false;
        current = record->parent;
    }
    return false;
}

void TransformStore::ResolveOne(ComponentHandle handle, std::vector<ComponentHandle>& stack)
{
    auto* record = Find(handle);
    if (!record || !record->dirty)
        return;

    if (std::find(stack.begin(), stack.end(), handle) != stack.end())
        return;
    stack.push_back(handle);

    Mat4 local(1.0f);
    local = glm::translate(local, record->localLocation);
    local *= glm::mat4_cast(record->localRotation);
    local = glm::scale(local, record->localScale);

    if (record->parent.IsValid())
    {
        ResolveOne(record->parent, stack);
        const auto* parent = Find(record->parent);
        record->worldTransform = parent ? parent->worldTransform * local : local;
    }
    else
    {
        record->worldTransform = local;
    }

    record->worldRevision = m_nextWorldRevision++;
    record->dirty = false;
    if (!record->previousRollForwardPending)
    {
        record->previousRollForwardPending = true;
        m_previousRollForwardHandles.push_back(handle);
    }
    if (m_collectResolveStats)
        ++m_lastResolveStats.resolvedCount;
    stack.pop_back();
}

void TransformStore::SetLocalFromMatrix(TransformStoreRecord& record, const Mat4& localMatrix)
{
    DecomposeTransform(localMatrix,
                       record.localLocation,
                       record.localRotation,
                       record.localScale);
}

} // namespace RVX
