#pragma once

/**
 * @file RenderRuntimeTestSupport.h
 * @brief Test-only factories for render runtime conformance tests.
 */

#include "Runtime/IRenderExecutor.h"

#include <memory>

namespace RVX
{
    /** @brief Create the synchronous executor used only by validation tests. */
    std::unique_ptr<IRenderExecutor> CreateInlineRenderExecutor();
} // namespace RVX
