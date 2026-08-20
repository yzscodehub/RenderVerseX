/**
 * @file NativeSurface.h
 * @brief Backend-neutral non-owning native presentation surface contract
 */

#pragma once

#include "Core/Types.h"

#include <cmath>
#include <cstdint>

namespace RVX::HAL
{
    /** @brief Platform identity for non-owning native surface handles. */
    enum class NativeSurfacePlatform : uint8
    {
        None = 0,
        Win32,
        X11,
        Wayland,
        Cocoa,
        UIKit,
        GLFW
    };

    /**
     * @brief Complete HAL-owned native surface snapshot.
     *
     * This value deliberately contains no RHI backend, pixel format, present
     * mode, or synchronization policy. Those belong to swap-chain
     * configuration in the RHI layer.
     */
    struct NativeSurfaceDesc
    {
        NativeSurfacePlatform platform = NativeSurfacePlatform::None;
        uintptr_t nativeWindow = 0;
        uintptr_t nativeDisplay = 0;
        uintptr_t nativeLayer = 0;
        uintptr_t backendWindow = 0;
        uint32 width = 0;
        uint32 height = 0;
        float32 contentScale = 1.0f;
        uint64 generation = 0;

        /** @brief Validate fields common to every presentation backend. */
        [[nodiscard]] bool IsValid() const noexcept
        {
            return platform != NativeSurfacePlatform::None &&
                   width != 0 && height != 0 && generation != 0 &&
                   std::isfinite(contentScale) && contentScale > 0.0f;
        }
    };

    /** @brief Required action for a complete native-surface snapshot update. */
    enum class NativeSurfaceUpdateKind : uint8
    {
        Reject = 0,
        Resize,
        Replace
    };

    /** @brief Classify a newer complete surface snapshot for lifecycle routing. */
    [[nodiscard]] inline NativeSurfaceUpdateKind ClassifyNativeSurfaceUpdate(
        const NativeSurfaceDesc& current,
        const NativeSurfaceDesc& update) noexcept
    {
        if (update.generation <= current.generation)
        {
            return NativeSurfaceUpdateKind::Reject;
        }

        const bool sameBinding =
            update.platform == current.platform &&
            update.nativeWindow == current.nativeWindow &&
            update.nativeDisplay == current.nativeDisplay &&
            update.nativeLayer == current.nativeLayer &&
            update.backendWindow == current.backendWindow &&
            update.contentScale == current.contentScale;
        const bool extentChanged =
            update.width != current.width || update.height != current.height;

        if (sameBinding && extentChanged)
        {
            return NativeSurfaceUpdateKind::Resize;
        }
        return NativeSurfaceUpdateKind::Replace;
    }

} // namespace RVX::HAL
