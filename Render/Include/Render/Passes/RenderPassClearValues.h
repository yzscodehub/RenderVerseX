#pragma once

/** @file RenderPassClearValues.h @brief Stable render-pass clear-value policy */

#include "RHI/RHIResources.h"

namespace RVX
{
    inline constexpr RHIClearColor RVX_SCENE_COLOR_CLEAR_VALUE = {
        0.1f,
        0.1f,
        0.15f,
        1.0f};
} // namespace RVX
