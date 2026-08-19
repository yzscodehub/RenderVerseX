/**
 * @file World.cpp
 * @brief Pure ECS World implementation.
 */

#include "World/World.h"

#include <utility>

namespace RVX
{
World::World(WorldConfig config)
    : m_config(std::move(config))
    , m_sceneRuntime()
    , m_cameraService(m_sceneRuntime)
{
}
} // namespace RVX
