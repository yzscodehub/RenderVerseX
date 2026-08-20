/**
 * @file ViewData.cpp
 * @brief ViewData implementation
 */

#include "Render/Renderer/ViewData.h"
#include "Core/Camera/Camera.h"

namespace RVX
{
void ViewData::SetupFromCamera(const Camera& camera, uint32_t width, uint32_t height)
{
    // Matrices
    viewMatrix = camera.GetView();
    projectionMatrix = camera.GetProjection();
    viewProjectionMatrix = camera.GetViewProjection();
    
    // Inverse matrices
    inverseViewMatrix = inverse(viewMatrix);
    inverseProjectionMatrix = inverse(projectionMatrix);
    
    // Camera transform
    cameraPosition = camera.GetPosition();
    // Extract forward from view matrix (inverted Z column)
    cameraForward = -Vec3(inverseViewMatrix[2]);
    // Core::Camera has no layer-selection state. Do not retain a mask from a
    // previous snapshot-backed setup.
    cullingMask = ~0U;
    clearPolicy = RenderViewClearPolicy::Skybox;
    clearColor = {0.1f, 0.1f, 0.15f, 1.0f};
    
    // Viewport
    viewportWidth = width;
    viewportHeight = height;
    viewportX = 0;
    viewportY = 0;
    aspectRatio = (height > 0) ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
}

void ViewData::SetupFromSnapshot(
    const RenderViewSnapshot& snapshot,
    const Mat4& previousRenderedViewProjection,
    bool previousRenderedViewValid,
    bool resetHistory)
{
    viewMatrix = snapshot.viewMatrix;
    projectionMatrix = snapshot.projectionMatrix;
    viewProjectionMatrix = snapshot.viewProjectionMatrix;
    inverseViewMatrix = inverse(viewMatrix);
    inverseProjectionMatrix = inverse(projectionMatrix);
    cameraPosition = snapshot.cameraPosition;
    cameraForward = snapshot.cameraDirection;
    cullingMask = snapshot.cullingMask;
    clearPolicy = snapshot.clearPolicy;
    clearColor = snapshot.clearColor;
    nearPlane = snapshot.nearPlane;
    farPlane = snapshot.farPlane;
    viewportX = static_cast<int32>(snapshot.viewportX);
    viewportY = static_cast<int32>(snapshot.viewportY);
    viewportWidth = snapshot.viewportWidth;
    viewportHeight = snapshot.viewportHeight;
    aspectRatio = viewportHeight == 0
                      ? 1.0f
                      : static_cast<float>(viewportWidth) /
                            static_cast<float>(viewportHeight);
    previousViewProjectionMatrix = resetHistory
                                       ? snapshot.viewProjectionMatrix
                                       : previousRenderedViewProjection;
    previousViewProjectionValid =
        static_cast<uint8>(previousRenderedViewValid && !resetHistory);
    resetTemporalHistory = resetHistory;
    time = snapshot.absoluteTime;
    deltaTime = snapshot.deltaTime;
}

} // namespace RVX
