#pragma once

/**
 * @file SceneComponent.h
 * @brief Transform component with parent-child attachment support
 */

#include "Core/MathTypes.h"
#include "Scene/ActorComponent.h"
#include "Scene/TransformStore.h"

#include <vector>

namespace RVX
{
    /**
     * @brief Actor component with a relative transform and attachment hierarchy.
     */
    class SceneComponent : public ActorComponent
    {
    public:
        // =====================================================================
        // Construction
        // =====================================================================

        SceneComponent() = default;
        ~SceneComponent() override;

        // =====================================================================
        // Type Information
        // =====================================================================

        const char* GetClassName() const override { return "SceneComponent"; }

        // =====================================================================
        // Relative Transform
        // =====================================================================

        const Vec3& GetRelativeLocation() const;
        void SetRelativeLocation(const Vec3& location);

        const Quat& GetRelativeRotation() const;
        void SetRelativeRotation(const Quat& rotation);

        const Vec3& GetRelativeScale() const;
        void SetRelativeScale(const Vec3& scale);
        const Vec3& GetRelativeScale3D() const { return GetRelativeScale(); }
        void SetRelativeScale3D(const Vec3& scale) { SetRelativeScale(scale); }

        Mat4 GetRelativeTransform() const;
        const Mat4& GetWorldTransform() const;
        const Mat4& GetPreviousWorldTransform() const;
        Vec3 GetWorldLocation() const;
        Quat GetWorldRotation() const;
        Vec3 GetWorldScale() const;
        uint64 GetLocalTransformRevision() const;
        uint64 GetWorldTransformRevision() const;

        // =====================================================================
        // Attachment
        // =====================================================================

        bool AttachToComponent(
            SceneComponent* parent,
            AttachmentTransformRule rule = AttachmentTransformRule::KeepLocal);
        void DetachFromComponent(
            AttachmentTransformRule rule = AttachmentTransformRule::KeepLocal);

        SceneComponent* GetAttachParent() const { return m_attachParent; }
        const std::vector<SceneComponent*>& GetAttachChildren() const { return m_attachChildren; }

    protected:
        virtual void OnTransformChanged() {}
        void MarkTransformDirty();

    private:
        bool WouldCreateCycle(const SceneComponent* parent) const;
        TransformStore* GetRuntimeTransformStore() const;

        Vec3 m_relativeLocation{0.0f};
        Quat m_relativeRotation{1.0f, 0.0f, 0.0f, 0.0f};
        Vec3 m_relativeScale{1.0f};
        mutable Mat4 m_worldTransform{1.0f};
        mutable bool m_transformDirty = true;
        SceneComponent* m_attachParent = nullptr;
        std::vector<SceneComponent*> m_attachChildren;
    };

} // namespace RVX
