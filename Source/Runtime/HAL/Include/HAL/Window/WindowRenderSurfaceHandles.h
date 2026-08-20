/**
 * @file WindowRenderSurfaceHandles.h
 * @brief HAL-owned native window handles captured for rendering
 */

#pragma once

#include "Core/Types.h"

#include <cstdint>

namespace RVX::HAL
{
    /** @brief Non-owning platform handles captured on the main/update thread. */
    struct WindowRenderSurfaceHandles
    {
        uintptr_t nativeWindow = 0;
        uintptr_t nativeDisplay = 0;
        uintptr_t nativeLayer = 0;
        uintptr_t backendWindow = 0;
        uint32 width = 0;
        uint32 height = 0;
        float32 contentScale = 1.0f;
    };

} // namespace RVX::HAL
