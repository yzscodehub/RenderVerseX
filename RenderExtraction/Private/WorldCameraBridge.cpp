/**
 * @file WorldCameraBridge.cpp
 * @brief WorldCameraBridge implementation.
 */

#include "RenderExtraction/WorldCameraBridge.h"

#include "World/World.h"

namespace RVX
{

Camera* WorldCameraBridge::GetActiveCamera(World* world) const
{
    return world ? world->GetActiveCamera() : nullptr;
}

} // namespace RVX
