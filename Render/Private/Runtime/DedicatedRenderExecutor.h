#pragma once

/**
 * @file DedicatedRenderExecutor.h
 * @brief Production factory for the dedicated render-thread executor.
 */

#include "Runtime/IRenderExecutor.h"

#include <memory>

namespace RVX
{
    /** @brief Create the only executor implementation linked into production. */
    std::unique_ptr<IRenderExecutor> CreateDedicatedRenderExecutor();
} // namespace RVX
