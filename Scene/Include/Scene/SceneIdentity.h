#pragma once

/**
 * @file SceneIdentity.h
 * @brief Runtime and persistent component identity contracts
 */

#include "Core/Handle.h"

namespace RVX
{
    struct ComponentHandleTag final
    {
    };

    using ComponentHandle = Handle<ComponentHandleTag, uint32>;
    using PersistentComponentId = uint64;

    inline constexpr ComponentHandle InvalidComponentHandle = ComponentHandle::Invalid();
    inline constexpr PersistentComponentId InvalidPersistentComponentId = 0;
} // namespace RVX
