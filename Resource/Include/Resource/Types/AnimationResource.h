#pragma once

/** @file AnimationResource.h  @brief Immutable skeleton and clip resource. */

#include "Animation/Data/AnimationClip.h"
#include "Animation/Data/Skeleton.h"
#include "Resource/IResource.h"

#include <map>
#include <string>

namespace RVX::Resource
{
    class AnimationResource final : public IResource
    {
    public:
        using AnimationClipMap =
            std::map<std::string, Animation::AnimationClip::ConstPtr>;

        static constexpr ResourceType StaticResourceType = ResourceType::Animation;

        ResourceType GetType() const override { return StaticResourceType; }
        const char* GetTypeName() const override { return "Animation"; }
        size_t GetMemoryUsage() const override;

        /** @brief Atomically replace the immutable runtime payload after validation. */
        bool SetData(Animation::Skeleton::ConstPtr skeleton,
                     AnimationClipMap clips);

        [[nodiscard]] Animation::Skeleton::ConstPtr GetSkeleton() const
        {
            return m_skeleton;
        }
        [[nodiscard]] const AnimationClipMap& GetClips() const { return m_clips; }
        [[nodiscard]] Animation::AnimationClip::ConstPtr
            GetClip(const std::string& name) const;

        /**
         * @brief Deep-clone one clip for an exactly compatible skeleton.
         *
         * Compatibility requires identical bone count, order, names, parent
         * topology, local bind TRS, and inverse bind matrices. Floating-point
         * comparisons are exact after canonicalizing negative zero. Bounding
         * radii are deliberately excluded because they are culling metadata,
         * not animation or skinning semantics.
         *
         * @param name Clip name in this resource.
         * @param targetSkeleton Compatible skeleton to bind to the clone.
         * @param outClip Cleared on failure; receives an immutable deep clone on success.
         */
        bool CloneClipForSkeleton(
            const std::string& name,
            Animation::Skeleton::ConstPtr targetSkeleton,
            Animation::AnimationClip::ConstPtr& outClip) const;

    private:
        Animation::Skeleton::ConstPtr m_skeleton;
        AnimationClipMap m_clips;
    };
} // namespace RVX::Resource
