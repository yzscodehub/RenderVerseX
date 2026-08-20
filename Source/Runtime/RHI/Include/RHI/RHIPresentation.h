/**
 * @file RHIPresentation.h
 * @brief RHI-owned presentation policy and native-surface validation
 */

#pragma once

#include "HAL/Window/NativeSurface.h"
#include "RHI/RHIDefinitions.h"

namespace RVX
{
    /** @brief Backend-neutral swap-chain present-mode request. */
    enum class RHIPresentMode : uint8
    {
        Auto = 0,
        Immediate,
        Mailbox,
        Fifo
    };

    /**
     * @brief RHI-owned swap-chain presentation policy.
     *
     * Auto resolves from vsync. Explicit modes must agree with vsync so the
     * request has one unambiguous synchronization contract on every backend.
     */
    struct RHISwapChainConfig
    {
        RHIFormat format = RHIFormat::BGRA8_UNORM;
        RHIPresentMode presentMode = RHIPresentMode::Auto;
        bool vsync = true;
    };

    /** @brief Validate a swap-chain policy before backend creation. */
    [[nodiscard]] constexpr bool IsSwapChainConfigValid(
        const RHISwapChainConfig& config) noexcept
    {
        if (config.format == RHIFormat::Unknown)
        {
            return false;
        }
        switch (config.presentMode)
        {
            case RHIPresentMode::Auto:
                return true;
            case RHIPresentMode::Immediate:
            case RHIPresentMode::Mailbox:
                return !config.vsync;
            case RHIPresentMode::Fifo:
                return config.vsync;
        }
        return false;
    }

    /** @brief Validate HAL handles required by a selected RHI backend. */
    [[nodiscard]] inline bool IsNativeSurfaceValidFor(
        const HAL::NativeSurfaceDesc& surface,
        RHIBackendType backend) noexcept
    {
        if (!surface.IsValid())
        {
            return false;
        }

        switch (backend)
        {
            case RHIBackendType::DX11:
            case RHIBackendType::DX12:
                return surface.platform == HAL::NativeSurfacePlatform::Win32 &&
                       surface.nativeWindow != 0;

            case RHIBackendType::Vulkan:
                return (surface.platform == HAL::NativeSurfacePlatform::Win32 ||
                        surface.platform == HAL::NativeSurfacePlatform::X11 ||
                        surface.platform == HAL::NativeSurfacePlatform::Wayland ||
                        surface.platform == HAL::NativeSurfacePlatform::GLFW) &&
                       surface.backendWindow != 0;

            case RHIBackendType::Metal:
                return (surface.platform == HAL::NativeSurfacePlatform::Cocoa ||
                        surface.platform == HAL::NativeSurfacePlatform::UIKit) &&
                       surface.nativeLayer != 0;

            case RHIBackendType::OpenGL:
                return surface.backendWindow != 0;

            case RHIBackendType::None:
            case RHIBackendType::Auto:
                return false;
        }
        return false;
    }

} // namespace RVX
