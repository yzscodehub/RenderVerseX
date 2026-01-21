/**
 * @file main.cpp
 * @brief ModelViewer Sample - Demonstrates the integrated Resource/Scene/Render pipeline
 * 
 * This sample shows how to:
 * 1. Load a glTF/GLB model using ResourceManager
 * 2. Instantiate it into the scene as SceneEntity with MeshRendererComponent
 * 3. Let the engine handle GPU upload and rendering automatically
 * 
 * Usage:
 *   ModelViewer.exe [path_to_model.gltf]
 * 
 * If no model path is provided, it will look for a default model.
 * 
 * Controls:
 *   Left mouse drag: Orbit camera around model
 *   Mouse wheel: Zoom in/out
 *   R key: Reset camera to default position
 */

#include "Engine/Engine.h"
#include "Render/RenderSubsystem.h"
#include "Runtime/Window/WindowSubsystem.h"
#include "Runtime/Input/InputSubsystem.h"
#include "World/World.h"
#include "Runtime/Camera/Camera.h"
#include "Scene/SceneManager.h"
#include "Scene/SceneEntity.h"
#include "Scene/ComponentFactory.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Resource/ResourceSubsystem.h"
#include "Resource/ResourceManager.h"
#include "Resource/Types/ModelResource.h"
#include "Resource/Types/MeshResource.h"
#include "Core/Log.h"
#include "Core/MathTypes.h"
#include "HAL/Input/KeyCodes.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <filesystem>
#include <algorithm>

using namespace RVX;

// Orbit camera controller state
struct OrbitCamera
{
    Vec3 target{0.0f, 0.0f, 0.0f};  // Look-at target
    float distance = 5.0f;          // Distance from target
    float yaw = 0.0f;               // Horizontal angle (radians)
    float pitch = 0.3f;             // Vertical angle (radians)
    
    float orbitSpeed = 0.005f;      // Mouse sensitivity for orbit
    float zoomSpeed = 0.5f;         // Scroll wheel sensitivity
    float minDistance = 1.0f;
    float maxDistance = 50.0f;
    float minPitch = -1.5f;         // ~-85 degrees
    float maxPitch = 1.5f;          // ~85 degrees
    
    Vec3 GetCameraPosition() const
    {
        float x = distance * std::cos(pitch) * std::sin(yaw);
        float y = distance * std::sin(pitch);
        float z = distance * std::cos(pitch) * std::cos(yaw);
        return target + Vec3(x, y, z);
    }
};

int main(int argc, char* argv[])
{
    // Initialize logging
    Log::Initialize();
    RVX_CORE_INFO("=== ModelViewer Sample ===");

    // Get model path from command line or use default
    std::string modelPath;
    if (argc > 1)
    {
        modelPath = argv[1];
    }
    else
    {
        // Default model paths to try
        std::vector<std::string> defaultPaths = {
            "models/DamagedHelmet.glb",
            "models/helmet.gltf",
            "assets/models/DamagedHelmet.glb",
            "../assets/models/DamagedHelmet.glb",
            "../../assets/models/DamagedHelmet.glb",
            "C:/Users/yinzs/Desktop/DamagedHelmet.glb"
        };
        
        for (const auto& path : defaultPaths)
        {
            if (std::filesystem::exists(path))
            {
                modelPath = path;
                break;
            }
        }
    }

    if (modelPath.empty())
    {
        RVX_CORE_WARN("No model file specified. Usage: ModelViewer.exe [path_to_model.gltf]");
        RVX_CORE_WARN("Will create an empty scene for demonstration.");
    }
    else
    {
        RVX_CORE_INFO("Model path: {}", modelPath);
    }

    // Create and configure engine
    Engine engine;
    EngineConfig engineConfig;
    engineConfig.enableJobSystem = false;
    engine.SetConfig(engineConfig);

    // Add window subsystem
    auto* windowSubsystem = engine.AddSubsystem<WindowSubsystem>();
    
    // Configure window
    WindowConfig windowConfig;
    windowConfig.title = "ModelViewer - RenderVerseX";
    windowConfig.width = 1280;
    windowConfig.height = 720;
    windowConfig.resizable = true;
    windowSubsystem->SetConfig(windowConfig);

    // Add resource subsystem (must be added before loading any resources)
    auto* resourceSubsystem = engine.AddSubsystem<Resource::ResourceSubsystem>();
    (void)resourceSubsystem;  // Will be used via ResourceManager::Get()

    // Add input subsystem for mouse control
    auto* inputSubsystem = engine.AddSubsystem<InputSubsystem>();
    (void)inputSubsystem;  // Will be retrieved after init

    // Add render subsystem
    auto* renderSubsystem = engine.AddSubsystem<RenderSubsystem>();
    
    // Configure render
    RenderConfig renderConfig;
    renderConfig.backendType = RHIBackendType::Auto;
    renderConfig.enableValidation = true;
    renderConfig.vsync = true;
    renderConfig.autoBindWindow = true;
    renderConfig.autoRender = true;  // Engine will render automatically
    renderSubsystem->SetConfig(renderConfig);

    // Initialize engine
    engine.Initialize();

    if (!engine.IsInitialized())
    {
        RVX_CORE_ERROR("Failed to initialize engine");
        return -1;
    }

    // Connect InputSubsystem to window (must be done after engine init)
    auto* input = engine.GetSubsystem<InputSubsystem>();
    if (input && windowSubsystem->GetWindow())
    {
        input->SetWindow(windowSubsystem->GetWindow());
        RVX_CORE_INFO("InputSubsystem connected to window");
    }

    // Create world
    World* world = engine.CreateWorld("Main");
    if (!world)
    {
        RVX_CORE_ERROR("Failed to create world");
        engine.Shutdown();
        return -1;
    }

    // Create camera
    Camera* camera = world->CreateCamera("MainCamera");
    Vec3 cameraPos(0.0f, 2.0f, 5.0f);
    Vec3 target(0.0f, 0.0f, 0.0f);
    camera->SetPosition(cameraPos);
    camera->LookAt(target);
    camera->SetPerspective(glm::radians(45.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
    world->SetActiveCamera(camera);

    // Get scene manager
    SceneManager* sceneManager = world->GetSceneManager();
    if (!sceneManager)
    {
        RVX_CORE_ERROR("World has no scene manager");
        engine.Shutdown();
        return -1;
    }

    // The root entity of the loaded model (if loaded successfully)
    SceneEntity* modelEntity = nullptr;
    Resource::ResourceHandle<Resource::ModelResource> modelHandle;

    // Load model using ResourceManager
    if (!modelPath.empty())
    {
        RVX_CORE_INFO("Loading model: {}", modelPath);
        
        auto& resourceManager = Resource::ResourceManager::Get();
        modelHandle = resourceManager.Load<Resource::ModelResource>(modelPath);
        
        if (modelHandle.IsValid() && modelHandle.IsLoaded())
        {
            RVX_CORE_INFO("Model loaded successfully!");
            RVX_CORE_INFO("  - Meshes: {}", modelHandle->GetMeshCount());
            RVX_CORE_INFO("  - Materials: {}", modelHandle->GetMaterialCount());
            RVX_CORE_INFO("  - Nodes: {}", modelHandle->GetNodeCount());
            
            // Instantiate model into the scene
            // This creates SceneEntity tree with MeshRendererComponents attached
            modelEntity = modelHandle->Instantiate(sceneManager);
            
            if (modelEntity)
            {
                RVX_CORE_INFO("Model instantiated as SceneEntity: {}", modelEntity->GetName());
                
                // Center the model at origin
                modelEntity->SetPosition(Vec3(0.0f, 0.0f, 0.0f));
            }
            else
            {
                RVX_CORE_ERROR("Failed to instantiate model into scene");
            }
        }
        else
        {
            RVX_CORE_ERROR("Failed to load model: {}", modelPath);
        }
    }

    // If no model was loaded, create a fallback entity
    if (!modelEntity)
    {
        RVX_CORE_INFO("Creating fallback entity (no model loaded)");
        
        SceneEntity::Handle entityHandle = sceneManager->CreateEntity("FallbackEntity");
        modelEntity = sceneManager->GetEntity(entityHandle);
        
        if (modelEntity)
        {
            modelEntity->SetPosition(Vec3(0.0f, 0.0f, 0.0f));
            
            // Add empty MeshRendererComponent for demonstration
            auto* renderer = modelEntity->AddComponent<MeshRendererComponent>();
            renderer->SetVisible(true);
        }
    }

    if (!modelEntity)
    {
        RVX_CORE_ERROR("Failed to create any scene entity");
        engine.Shutdown();
        return -1;
    }

    RVX_CORE_INFO("Scene entity ready: {}", modelEntity->GetName());
    RVX_CORE_INFO("Entity position: ({}, {}, {})", 
                  modelEntity->GetPosition().x, 
                  modelEntity->GetPosition().y, 
                  modelEntity->GetPosition().z);

    // Setup orbit camera
    OrbitCamera orbitCamera;
    orbitCamera.distance = 5.0f;
    orbitCamera.pitch = 0.4f;  // Slightly above
    orbitCamera.yaw = 0.0f;
    
    // Adjust camera distance based on model bounds (if available)
    if (modelHandle.IsValid())
    {
        // Could compute bounds and adjust camera here
        // For now, keep the default distance
    }
    
    // Detect if using Vulkan (need to invert pitch direction due to Y-flip)
    bool isVulkan = false;
    if (auto* device = renderSubsystem->GetDevice())
    {
        isVulkan = (device->GetBackendType() == RHIBackendType::Vulkan);
    }
    float pitchDirection = isVulkan ? -1.0f : 1.0f;
    
    // Mouse state tracking (input subsystem already retrieved above)
    float lastMouseX = 0.0f, lastMouseY = 0.0f;
    if (input)
    {
        input->GetMousePosition(lastMouseX, lastMouseY);
    }

    // Main loop
    RVX_CORE_INFO("Entering main loop...");
    RVX_CORE_INFO("Controls:");
    RVX_CORE_INFO("  Left mouse drag: Orbit camera");
    RVX_CORE_INFO("  Mouse wheel: Zoom in/out");
    RVX_CORE_INFO("  R key: Reset camera");
    
    while (!engine.ShouldShutdown())
    {
        // Get current mouse position
        float mouseX = 0.0f, mouseY = 0.0f;
        if (input)
        {
            input->GetMousePosition(mouseX, mouseY);
            
            // Left mouse button: Orbit camera
            if (input->IsMouseButtonDown(MouseButton::Left))
            {
                float deltaX = mouseX - lastMouseX;
                float deltaY = mouseY - lastMouseY;
                
                // Update yaw and pitch
                // pitchDirection compensates for Vulkan's Y-flip in view-projection
                orbitCamera.yaw -= deltaX * orbitCamera.orbitSpeed;
                orbitCamera.pitch += deltaY * orbitCamera.orbitSpeed * pitchDirection;
                
                // Clamp pitch to avoid gimbal lock
                orbitCamera.pitch = std::clamp(orbitCamera.pitch, 
                                               orbitCamera.minPitch, 
                                               orbitCamera.maxPitch);
            }
            
            // Mouse wheel: Zoom
            float scrollX, scrollY;
            input->GetScrollDelta(scrollX, scrollY);
            if (scrollY != 0.0f)
            {
                orbitCamera.distance -= scrollY * orbitCamera.zoomSpeed;
                orbitCamera.distance = std::clamp(orbitCamera.distance,
                                                   orbitCamera.minDistance,
                                                   orbitCamera.maxDistance);
            }
            
            // R key: Reset camera
            if (input->IsKeyPressed(Key::R))
            {
                orbitCamera.yaw = 0.0f;
                orbitCamera.pitch = 0.4f;
                orbitCamera.distance = 5.0f;
                RVX_CORE_INFO("Camera reset");
            }
            
            lastMouseX = mouseX;
            lastMouseY = mouseY;
        }
        
        // Update camera from orbit parameters
        cameraPos = orbitCamera.GetCameraPosition();
        camera->SetPosition(cameraPos);
        camera->LookAt(orbitCamera.target);

        // Engine tick handles:
        // 1. Window events
        // 2. World updates
        // 3. GPU resource uploads (via GPUResourceManager)
        // 4. Rendering (auto-render is enabled)
        engine.Tick();
    }

    RVX_CORE_INFO("Exiting main loop");

    // Model handle will be released automatically when it goes out of scope
    // ResourceManager handles cleanup of resources
    
    // Cleanup
    engine.Shutdown();
    
    RVX_CORE_INFO("=== ModelViewer Sample Complete ===");
    return 0;
}
