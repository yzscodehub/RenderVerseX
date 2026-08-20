#pragma once

/**
 * @file RenderThreadPlatform.h
 * @brief Narrow platform policy and autorelease scopes for the render thread.
 */

#include "Core/Types.h"

namespace RVX
{
    /** @brief Applies the platform thread name, policy, and lifetime scope. */
    class RenderThreadPlatformBootstrap final : public NonMovable
    {
    public:
        RenderThreadPlatformBootstrap() noexcept;
        ~RenderThreadPlatformBootstrap() noexcept;

    private:
        void* m_state = nullptr;
    };

    /** @brief Owns per-pump platform state such as an Apple autorelease pool. */
    class RenderThreadPlatformIterationScope final : public NonMovable
    {
    public:
        RenderThreadPlatformIterationScope() noexcept;
        ~RenderThreadPlatformIterationScope() noexcept;

    private:
        void* m_state = nullptr;
    };
} // namespace RVX
