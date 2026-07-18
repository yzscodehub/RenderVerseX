/**
 * @file WorldCameraBridge.cpp
 * @brief WorldCameraBridge implementation.
 */

#include "RenderExtraction/WorldCameraBridge.h"

#include "Core/Camera/Camera.h"
#include "World/World.h"

#include <glm/gtc/matrix_inverse.hpp>

namespace RVX
{
WorldCameraBridgeCode WorldCameraBridge::Extract(
    World* world,
    const RenderViewExtractionParameters& parameters,
    RenderViewSnapshot& outView) const
{
    outView = {};
    if (world == nullptr)
    {
        return WorldCameraBridgeCode::NullWorld;
    }
    Camera* camera = world->GetActiveCamera();
    if (camera == nullptr)
    {
        return WorldCameraBridgeCode::MissingActiveCamera;
    }

    outView.viewMatrix = camera->GetView();
    outView.projectionMatrix = camera->GetProjection();
    outView.viewProjectionMatrix = camera->GetViewProjection();
    outView.inverseViewProjectionMatrix =
        glm::inverse(outView.viewProjectionMatrix);
    outView.cameraPosition = camera->GetPosition();
    const Mat4 inverseView = glm::inverse(outView.viewMatrix);
    outView.cameraDirection = -Vec3(inverseView[2]);
    outView.cameraUp = Vec3(inverseView[1]);
    outView.viewportX = parameters.viewportX;
    outView.viewportY = parameters.viewportY;
    outView.viewportWidth = parameters.viewportWidth;
    outView.viewportHeight = parameters.viewportHeight;
    outView.nearPlane = parameters.nearPlane;
    outView.farPlane = parameters.farPlane;
    outView.absoluteTime = parameters.absoluteTime;
    outView.deltaTime = parameters.deltaTime;
    outView.exposure = parameters.exposure;
    return WorldCameraBridgeCode::Complete;
}

Camera* WorldCameraBridge::GetActiveCamera(World* world) const
{
    return world ? world->GetActiveCamera() : nullptr;
}

} // namespace RVX
