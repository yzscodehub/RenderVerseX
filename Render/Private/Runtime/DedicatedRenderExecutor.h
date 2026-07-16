#pragma once

/**
 * @file DedicatedRenderExecutor.h
 * @brief Production factory for the dedicated render-thread executor.
 */

#include "Runtime/IRenderExecutor.h"

#include <memory>

namespace RVX
{
    /** @brief Private observation seam immediately before platform bootstrap. */
    class IDedicatedRenderExecutorBootstrapHook
    {
    public:
        virtual ~IDedicatedRenderExecutorBootstrapHook() = default;

        virtual void BeforePlatformBootstrap() noexcept = 0;
    };

    /** @brief Create the only executor implementation linked into production. */
    std::unique_ptr<IRenderExecutor> CreateDedicatedRenderExecutor();
    std::unique_ptr<IRenderExecutor> CreateDedicatedRenderExecutor(
        std::shared_ptr<IDedicatedRenderExecutorBootstrapHook> bootstrapHook);
} // namespace RVX
