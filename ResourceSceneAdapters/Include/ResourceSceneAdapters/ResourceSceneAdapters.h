#pragma once

/**
 * @file ResourceSceneAdapters.h
 * @brief Scene integration adapters for Resource model instantiation.
 */

#include "ResourceSceneAdapters/SceneAssetInstantiation.h"
#include "ResourceSceneAdapters/SceneAssetLoadCoordinator.h"

namespace RVX::ResourceSceneAdapters
{
    /** @brief Register Scene component factories used by Resource model instantiation. */
    void RegisterDefaults();
}
