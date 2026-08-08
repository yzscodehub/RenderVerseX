#pragma once

/**
 * @file EntityHandle.h
 * @brief Generation-safe identity shared by scene and spatial systems
 */

#include "Core/Handle.h"

namespace RVX::Spatial
{
    struct EntityHandleTag final
    {
    };

    using EntityHandle = Handle<EntityHandleTag, uint32>;

    inline constexpr EntityHandle InvalidEntityHandle = EntityHandle::Invalid();
    inline constexpr EntityHandle InvalidHandle = InvalidEntityHandle;
} // namespace RVX::Spatial
