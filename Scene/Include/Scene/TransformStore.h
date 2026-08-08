#pragma once

/**
 * @file TransformStore.h
 * @brief Data-oriented authoritative transform storage for runtime components
 */

#include "Core/MathTypes.h"
#include "Scene/SceneIdentity.h"

#include <unordered_map>
#include <vector>

namespace RVX
{
    enum class AttachmentTransformRule : uint8
    {
        KeepLocal = 0,
        KeepWorld
    };

    struct TransformStoreRecord
    {
        ComponentHandle parent = InvalidComponentHandle;
        std::vector<ComponentHandle> children;
        Vec3 localLocation{0.0f};
        Quat localRotation{1.0f, 0.0f, 0.0f, 0.0f};
        Vec3 localScale{1.0f};
        Mat4 worldTransform{1.0f};
        Mat4 previousWorldTransform{1.0f};
        uint64 localRevision = 1;
        uint64 worldRevision = 0;
        bool dirty = true;
        bool previousRollForwardPending = false;
    };

    struct TransformResolveStats
    {
        size_t registeredCount = 0;
        size_t dirtyInputCount = 0;
        size_t resolvedCount = 0;
    };

    /** @brief Compact runtime store backing all scene-owned SceneComponents. */
    class TransformStore
    {
    public:
        void Reserve(size_t capacity);
        bool Register(ComponentHandle handle,
                      const Vec3& location,
                      const Quat& rotation,
                      const Vec3& scale);
        void Unregister(ComponentHandle handle);
        bool Contains(ComponentHandle handle) const;

        const Vec3& GetLocalLocation(ComponentHandle handle) const;
        const Quat& GetLocalRotation(ComponentHandle handle) const;
        const Vec3& GetLocalScale(ComponentHandle handle) const;
        void SetLocalLocation(ComponentHandle handle, const Vec3& location);
        void SetLocalRotation(ComponentHandle handle, const Quat& rotation);
        void SetLocalScale(ComponentHandle handle, const Vec3& scale);

        bool Attach(ComponentHandle child,
                    ComponentHandle parent,
                    AttachmentTransformRule rule);
        bool Detach(ComponentHandle child, AttachmentTransformRule rule);
        ComponentHandle GetParent(ComponentHandle handle) const;

        const Mat4& GetWorldTransform(ComponentHandle handle);
        const Mat4& GetPreviousWorldTransform(ComponentHandle handle) const;
        Quat GetWorldRotation(ComponentHandle handle);
        Vec3 GetWorldScale(ComponentHandle handle);
        uint64 GetLocalRevision(ComponentHandle handle) const;
        uint64 GetWorldRevision(ComponentHandle handle) const;

        void BeginFrame();
        void Resolve();
        size_t GetDirtyCount() const { return m_dirtyHandles.size(); }
        size_t GetCount() const { return m_records.size(); }
        const TransformResolveStats& GetLastResolveStats() const
        {
            return m_lastResolveStats;
        }

    private:
        TransformStoreRecord* Find(ComponentHandle handle);
        const TransformStoreRecord* Find(ComponentHandle handle) const;
        void MarkDirty(ComponentHandle handle);
        bool WouldCreateCycle(ComponentHandle child, ComponentHandle parent) const;
        void ResolveOne(ComponentHandle handle, std::vector<ComponentHandle>& stack);
        void SetLocalFromMatrix(TransformStoreRecord& record, const Mat4& localMatrix);

        std::unordered_map<ComponentHandle, TransformStoreRecord> m_records;
        std::vector<ComponentHandle> m_dirtyHandles;
        std::vector<ComponentHandle> m_previousRollForwardHandles;
        TransformResolveStats m_lastResolveStats;
        uint64 m_nextWorldRevision = 1;
        bool m_collectResolveStats = false;
    };
} // namespace RVX
