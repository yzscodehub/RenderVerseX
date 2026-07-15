/**
 * @file RHINativeSurface.h
 * @brief Tagged non-owning native presentation-surface contract
 */

#pragma once

#include "RHI/RHIDefinitions.h"

#include <cmath>
#include <cstdint>

namespace RVX
{
    /** @brief Platform identity for the non-owning native surface handles. */
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

    /** @brief Immutable value captured by HAL and consumed by RHI/Render. */
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
        RHIFormat preferredFormat = RHIFormat::BGRA8_UNORM;
        bool vsync = true;
        uint64 generation = 0;

        /** @brief Validate common fields and backend-required tagged handles. */
        [[nodiscard]] bool IsValidFor(RHIBackendType backend) const noexcept
        {
            if (platform == NativeSurfacePlatform::None ||
                width == 0 || height == 0 || generation == 0 ||
                !std::isfinite(contentScale) || contentScale <= 0.0f ||
                preferredFormat == RHIFormat::Unknown)
            {
                return false;
            }

            switch (backend)
            {
                case RHIBackendType::DX11:
                case RHIBackendType::DX12:
                    return platform == NativeSurfacePlatform::Win32 &&
                           nativeWindow != 0;

                case RHIBackendType::Vulkan:
                    return (platform == NativeSurfacePlatform::Win32 ||
                            platform == NativeSurfacePlatform::X11 ||
                            platform == NativeSurfacePlatform::Wayland ||
                            platform == NativeSurfacePlatform::GLFW) &&
                           backendWindow != 0;

                case RHIBackendType::Metal:
                    return (platform == NativeSurfacePlatform::Cocoa ||
                            platform == NativeSurfacePlatform::UIKit) &&
                           nativeLayer != 0;

                case RHIBackendType::OpenGL:
                    return backendWindow != 0;

                case RHIBackendType::None:
                case RHIBackendType::Auto:
                    return false;
            }

            return false;
        }
    };

} // namespace RVX
