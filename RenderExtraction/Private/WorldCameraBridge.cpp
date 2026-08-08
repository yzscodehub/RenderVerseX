/**
 * @file WorldCameraBridge.cpp
 * @brief WorldCameraBridge implementation.
 */

#include "RenderExtraction/WorldCameraBridge.h"

#include "Scene/Components/CameraComponent.h"
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
    CameraComponent* cameraComponent = world->GetActiveCameraComponent();
    if (cameraComponent == nullptr)
    {
        return WorldCameraBridgeCode::MissingActiveCamera;
    }

    outView.viewMatrix = cameraComponent->GetViewMatrix();
    outView.projectionMatrix = cameraComponent->GetProjectionMatrix();
    outView.viewProjectionMatrix =
        cameraComponent->GetViewProjectionMatrix();
    outView.nearPlane = cameraComponent->GetNearPlane();
    outView.farPlane = cameraComponent->GetFarPlane();
    outView.inverseViewProjectionMatrix =
        glm::inverse(outView.viewProjectionMatrix);
    const Mat4 inverseView = glm::inverse(outView.viewMatrix);
    outView.cameraPosition = Vec3(inverseView[3]);
    outView.cameraDirection = -Vec3(inverseView[2]);
    outView.cameraUp = Vec3(inverseView[1]);
    outView.viewportX = parameters.viewportX;
    outView.viewportY = parameters.viewportY;
    outView.viewportWidth = parameters.viewportWidth;
    outView.viewportHeight = parameters.viewportHeight;
    outView.absoluteTime = parameters.absoluteTime;
    outView.deltaTime = parameters.deltaTime;
    outView.exposure = parameters.exposure;
    return WorldCameraBridgeCode::Complete;
}

} // namespace RVX
