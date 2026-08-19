#pragma once

/**
 * @file EcsEnvironmentPresentationReceipt.h
 * @brief Backend-neutral proof that an exact ECS Skybox reached presentation.
 */

#include "Core/Types.h"
#include "ECS/Entity.h"

namespace RVX::ResourceSceneAdapters
{
    /** @brief Exact proof that one generation-qualified ECS Skybox reached accepted presentation. */
    struct EcsEnvironmentPresentationReceipt
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        ECS::EntityHandle skyboxEntity = ECS::EntityHandle::Invalid();
        uint64 skyboxWriteVersion = 0;
        uint64 frozenSourceSnapshotRevision = 0;
        uint64 renderSceneRevision = 0;
        uint64 carryingPresentedFrameSequence = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return sceneRuntimeId.IsValid() && skyboxEntity.IsValid() &&
                   frozenSourceSnapshotRevision != 0 &&
                   renderSceneRevision != 0 && carryingPresentedFrameSequence != 0;
        }
    };
} // namespace RVX::ResourceSceneAdapters
