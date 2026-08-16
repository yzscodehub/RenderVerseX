#pragma once

/**
 * @file RetirementFragments.h
 * @brief Data-only ownership markers for Scene ECS retirement consumers.
 */

#include "ECS/Fragment.h"

namespace RVX::SceneECS
{
    /**
     * @brief Marks an entity whose Render retirement is owned by a specialized coordinator.
     *
     * The generic Engine Render retirement coordinator deliberately ignores this
     * marker.  It is a tag rather than an ownership-bearing object so an asset
     * instance can publish the marker atomically with every member it owns.
     */
    struct SpecializedRenderRetirement final
    {
    };

    static_assert(ECS::Fragment<SpecializedRenderRetirement>);
} // namespace RVX::SceneECS
