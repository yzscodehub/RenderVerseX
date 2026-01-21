/**
 * @file ViewData.cpp
 * @brief ViewData implementation
 */

#include "Render/Renderer/ViewData.h"
#include "Runtime/Camera/Camera.h"

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
    
    // Viewport
    viewportWidth = width;
    viewportHeight = height;
    viewportX = 0;
    viewportY = 0;
    aspectRatio = (height > 0) ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
}

} // namespace RVX
