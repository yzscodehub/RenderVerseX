#pragma once

/**
 * @file VelocityPassBindings.h
 * @brief Non-owning motion-vector pass bindings shared by raster recipes.
 */

#include "Render/Passes/IRenderPass.h"

namespace RVX
{
    /**
     * @brief Exact registry-owned producers of the canonical velocity field.
     *
     * Camera initializes the full target and Object overlays rigid/skinned
     * motion.  Both pointers are borrowed from SceneRenderer's pass registry.
     */
    struct VelocityPassBindings
    {
        IRenderPass* camera = nullptr;
        IRenderPass* object = nullptr;

        [[nodiscard]] bool IsComplete() const noexcept
        {
            return camera != nullptr && object != nullptr && camera != object;
        }

        [[nodiscard]] bool Contains(const IRenderPass* pass) const noexcept
        {
            return pass != nullptr && (pass == camera || pass == object);
        }
    };
} // namespace RVX
