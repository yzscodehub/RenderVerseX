/**
 * @file main.cpp
 * @brief ModelViewer Sample - Demonstrates the integrated Resource/Scene/Render pipeline
 * 
 * This sample shows how to:
 * 1. Load a glTF/GLB model using ResourceManager
 * 2. Instantiate it into the scene as SceneEntity with StaticMeshComponent
 * 3. Let the engine handle GPU upload and rendering automatically
 * 
 * Usage:
 *   ModelViewer.exe [path_to_model.gltf]
 *   ModelViewer.exe --smoke --model Tests/Fixtures/ModelViewer/R7Triangle.gltf --screenshot out.ppm
 * 
 * If no model path is provided, it will look for a default model.
 * 
 * Controls:
 *   Left mouse drag: Orbit camera around model
 *   Mouse wheel: Zoom in/out
 *   R key: Reset camera to default position
 */

#include "Engine/Engine.h"
#include "Render/Context/RenderContext.h"
#include "Render/RenderSubsystem.h"
#include "Runtime/Window/WindowSubsystem.h"
#include "Runtime/Input/InputSubsystem.h"
#include "World/World.h"
#include "Runtime/Camera/Camera.h"
#include "Scene/SceneManager.h"
#include "Scene/SceneEntity.h"
#include "Scene/ComponentFactory.h"
#include "Resource/ResourceSubsystem.h"
#include "Resource/ResourceManager.h"
#include "Resource/Types/ModelResource.h"
#include "Resource/Types/MeshResource.h"
#include "Core/Log.h"
#include "Core/MathTypes.h"
#include "HAL/Input/KeyCodes.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

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

struct ModelViewerOptions
{
    std::string modelPath;
    std::string screenshotPath;
    RHIBackendType backend = RHIBackendType::Auto;
    uint32 width = 1280;
    uint32 height = 720;
    uint32 frames = 0;
    bool smoke = false;
    bool enableValidation = true;
    bool showHelp = false;
    bool backendSet = false;
    bool widthSet = false;
    bool heightSet = false;
    bool framesSet = false;
};

struct PendingScreenshot
{
    RHIBufferRef readbackBuffer;
    uint32 width = 0;
    uint32 height = 0;
    uint32 rowPitch = 0;
    uint32 bytesPerPixel = 0;
};

namespace
{
    constexpr uint32 kSmokeDefaultWidth = 320;
    constexpr uint32 kSmokeDefaultHeight = 180;
    constexpr uint32 kSmokeDefaultFrames = 8;
    constexpr float kSmokeDeltaSeconds = 1.0f / 60.0f;
    constexpr uint32 kDX12TextureCopyPitchAlignment = 256;

    void PrintUsage()
    {
        std::cout
            << "ModelViewer - RenderVerseX\n"
            << "Usage:\n"
            << "  ModelViewer.exe [path_to_model.gltf]\n"
            << "  ModelViewer.exe --smoke --model <path> --screenshot <actual.ppm>\n"
            << "Options:\n"
            << "  --smoke              Run bounded deterministic visual gate mode\n"
            << "  --model <path>       Model path to load\n"
            << "  --frames <count>     Number of frames in smoke mode\n"
            << "  --width <pixels>     Window width\n"
            << "  --height <pixels>    Window height\n"
            << "  --backend <name>     auto, dx11, dx12, vulkan, metal, opengl\n"
            << "  --screenshot <path>  Write final smoke frame as binary PPM\n"
            << "  --validation         Enable backend validation\n"
            << "  --no-validation      Disable backend validation\n"
            << "  --help               Show this help\n";
    }

    std::string ToLower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }

    bool ParseBackend(const std::string& text, RHIBackendType& outBackend)
    {
        const std::string value = ToLower(text);
        if (value == "auto")
        {
            outBackend = RHIBackendType::Auto;
        }
        else if (value == "dx11" || value == "d3d11")
        {
            outBackend = RHIBackendType::DX11;
        }
        else if (value == "dx12" || value == "d3d12")
        {
            outBackend = RHIBackendType::DX12;
        }
        else if (value == "vulkan" || value == "vk")
        {
            outBackend = RHIBackendType::Vulkan;
        }
        else if (value == "metal")
        {
            outBackend = RHIBackendType::Metal;
        }
        else if (value == "opengl" || value == "gl")
        {
            outBackend = RHIBackendType::OpenGL;
        }
        else
        {
            return false;
        }

        return true;
    }

    bool ParseUInt(const char* text, uint32& outValue)
    {
        if (!text)
        {
            return false;
        }

        try
        {
            const unsigned long parsed = std::stoul(text);
            if (parsed == 0 || parsed > std::numeric_limits<uint32>::max())
            {
                return false;
            }
            outValue = static_cast<uint32>(parsed);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ParseOptions(int argc, char* argv[], ModelViewerOptions& options)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            const auto requireValue = [&](const char* name) -> const char*
            {
                if (i + 1 >= argc)
                {
                    RVX_CORE_ERROR("Missing value for {}", name);
                    return nullptr;
                }
                return argv[++i];
            };

            if (arg == "--help" || arg == "-h")
            {
                options.showHelp = true;
            }
            else if (arg == "--smoke")
            {
                options.smoke = true;
            }
            else if (arg == "--model")
            {
                const char* value = requireValue("--model");
                if (!value) return false;
                options.modelPath = value;
            }
            else if (arg == "--frames")
            {
                const char* value = requireValue("--frames");
                if (!ParseUInt(value, options.frames))
                {
                    RVX_CORE_ERROR("Invalid --frames value: {}", value ? value : "");
                    return false;
                }
                options.framesSet = true;
            }
            else if (arg == "--width")
            {
                const char* value = requireValue("--width");
                if (!ParseUInt(value, options.width))
                {
                    RVX_CORE_ERROR("Invalid --width value: {}", value ? value : "");
                    return false;
                }
                options.widthSet = true;
            }
            else if (arg == "--height")
            {
                const char* value = requireValue("--height");
                if (!ParseUInt(value, options.height))
                {
                    RVX_CORE_ERROR("Invalid --height value: {}", value ? value : "");
                    return false;
                }
                options.heightSet = true;
            }
            else if (arg == "--backend")
            {
                const char* value = requireValue("--backend");
                if (!value || !ParseBackend(value, options.backend))
                {
                    RVX_CORE_ERROR("Invalid --backend value: {}", value ? value : "");
                    return false;
                }
                options.backendSet = true;
            }
            else if (arg == "--screenshot")
            {
                const char* value = requireValue("--screenshot");
                if (!value) return false;
                options.screenshotPath = value;
            }
            else if (arg == "--validation")
            {
                options.enableValidation = true;
            }
            else if (arg == "--no-validation")
            {
                options.enableValidation = false;
            }
            else if (!arg.empty() && arg[0] != '-' && options.modelPath.empty())
            {
                options.modelPath = arg;
            }
            else
            {
                RVX_CORE_ERROR("Unknown argument: {}", arg);
                return false;
            }
        }

        if (options.smoke)
        {
            if (!options.backendSet)
            {
                options.backend = RHIBackendType::DX11;
            }
            if (!options.widthSet)
            {
                options.width = kSmokeDefaultWidth;
            }
            if (!options.heightSet)
            {
                options.height = kSmokeDefaultHeight;
            }
            if (!options.framesSet)
            {
                options.frames = kSmokeDefaultFrames;
            }
        }

        return true;
    }

    std::string GetEnvironmentVariableValue(const char* name)
    {
#if defined(_MSC_VER)
        char* value = nullptr;
        size_t size = 0;
        if (_dupenv_s(&value, &size, name) != 0 || !value)
        {
            return {};
        }

        std::string result(value);
        std::free(value);
        return result;
#else
        const char* value = std::getenv(name);
        return value ? std::string(value) : std::string();
#endif
    }

    std::string ResolveModelPath(const ModelViewerOptions& options)
    {
        if (!options.modelPath.empty())
        {
            return options.modelPath;
        }

        std::vector<std::string> defaultPaths;
        auto appendDesktopHelmetPath = [&defaultPaths](const char* envName)
        {
            const std::string envValue = GetEnvironmentVariableValue(envName);
            if (envValue.empty())
            {
                return;
            }

            const std::filesystem::path desktopHelmetPath =
                std::filesystem::path(envValue) / "Desktop" / "DamagedHelmet.glb";
            defaultPaths.push_back(desktopHelmetPath.string());
        };

        appendDesktopHelmetPath("USERPROFILE");
        appendDesktopHelmetPath("HOME");
        defaultPaths.push_back("models/DamagedHelmet.glb");
        defaultPaths.push_back("models/helmet.gltf");
        defaultPaths.push_back("assets/models/DamagedHelmet.glb");
        defaultPaths.push_back("../assets/models/DamagedHelmet.glb");
        defaultPaths.push_back("../../assets/models/DamagedHelmet.glb");
        defaultPaths.push_back("Tests/Fixtures/ModelViewer/R7Triangle.gltf");
        defaultPaths.push_back("../Tests/Fixtures/ModelViewer/R7Triangle.gltf");
        defaultPaths.push_back("../../Tests/Fixtures/ModelViewer/R7Triangle.gltf");

        for (const auto& path : defaultPaths)
        {
            if (std::filesystem::exists(path))
            {
                return path;
            }
        }

        return {};
    }

    bool QueueBackBufferScreenshot(RenderSubsystem* renderSubsystem, PendingScreenshot& outScreenshot)
    {
        if (!renderSubsystem || !renderSubsystem->GetRenderContext())
        {
            RVX_CORE_ERROR("ModelViewer smoke capture: render subsystem is not ready");
            return false;
        }

        RenderContext* renderContext = renderSubsystem->GetRenderContext();
        IRHIDevice* device = renderSubsystem->GetDevice();
        if (!device)
        {
            RVX_CORE_ERROR("ModelViewer smoke capture: RHI device is not ready");
            return false;
        }

        const RHIBackendType backendType = device->GetBackendType();
        if (backendType != RHIBackendType::DX11 && backendType != RHIBackendType::DX12)
        {
            RVX_CORE_ERROR("ModelViewer smoke capture currently supports DX11/DX12; active backend is {}",
                           ToString(backendType));
            return false;
        }

        RHITexture* backBuffer = renderContext->GetCurrentBackBuffer();
        RHICommandContext* commandContext = renderContext->GetGraphicsContext();
        if (!backBuffer || !commandContext)
        {
            RVX_CORE_ERROR("ModelViewer smoke capture: back buffer or command context is not ready");
            return false;
        }

        const uint32 bytesPerPixel = GetFormatBytesPerPixel(backBuffer->GetFormat());
        if (bytesPerPixel < 4)
        {
            RVX_CORE_ERROR("ModelViewer smoke capture: unsupported back buffer format {}", static_cast<int>(backBuffer->GetFormat()));
            return false;
        }

        const uint32 width = backBuffer->GetWidth();
        const uint32 height = backBuffer->GetHeight();
        uint32 rowPitch = width * bytesPerPixel;
        if (backendType == RHIBackendType::DX12)
        {
            rowPitch = (rowPitch + kDX12TextureCopyPitchAlignment - 1u) &
                       ~(kDX12TextureCopyPitchAlignment - 1u);
        }

        RHIBufferDesc readbackDesc;
        readbackDesc.size = static_cast<uint64>(rowPitch) * height;
        readbackDesc.usage = RHIBufferUsage::CopyDst;
        readbackDesc.memoryType = RHIMemoryType::Readback;
        readbackDesc.debugName = "ModelViewerSmokeReadback";

        outScreenshot.readbackBuffer = device->CreateBuffer(readbackDesc);
        if (!outScreenshot.readbackBuffer)
        {
            RVX_CORE_ERROR("ModelViewer smoke capture: failed to create readback buffer");
            return false;
        }

        RHIBufferTextureCopyDesc copyDesc;
        copyDesc.bufferRowPitch = rowPitch;
        copyDesc.textureRegion = {0, 0, width, height};

        commandContext->TextureBarrier(backBuffer, RHIResourceState::Present, RHIResourceState::CopySource);
        commandContext->CopyTextureToBuffer(backBuffer, outScreenshot.readbackBuffer.Get(), copyDesc);
        commandContext->TextureBarrier(backBuffer, RHIResourceState::CopySource, RHIResourceState::Present);

        outScreenshot.width = width;
        outScreenshot.height = height;
        outScreenshot.rowPitch = rowPitch;
        outScreenshot.bytesPerPixel = bytesPerPixel;
        return true;
    }

    bool WriteScreenshotPPM(const PendingScreenshot& screenshot, const std::filesystem::path& path)
    {
        if (!screenshot.readbackBuffer || screenshot.width == 0 || screenshot.height == 0)
        {
            RVX_CORE_ERROR("ModelViewer smoke capture: invalid screenshot data");
            return false;
        }

        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent);
        }

        void* mapped = screenshot.readbackBuffer->Map();
        if (!mapped)
        {
            RVX_CORE_ERROR("ModelViewer smoke capture: failed to map readback buffer");
            return false;
        }

        std::ofstream stream(path, std::ios::binary);
        if (!stream)
        {
            screenshot.readbackBuffer->Unmap();
            RVX_CORE_ERROR("ModelViewer smoke capture: failed to create screenshot {}", path.string());
            return false;
        }

        stream << "P6\n" << screenshot.width << " " << screenshot.height << "\n255\n";

        const uint8* data = static_cast<const uint8*>(mapped);
        for (uint32 y = 0; y < screenshot.height; ++y)
        {
            const uint8* row = data + static_cast<uint64>(y) * screenshot.rowPitch;
            for (uint32 x = 0; x < screenshot.width; ++x)
            {
                const uint8* bgra = row + static_cast<uint64>(x) * screenshot.bytesPerPixel;
                const uint8 rgb[3] = {bgra[2], bgra[1], bgra[0]};
                stream.write(reinterpret_cast<const char*>(rgb), sizeof(rgb));
            }
        }

        screenshot.readbackBuffer->Unmap();

        if (!stream)
        {
            RVX_CORE_ERROR("ModelViewer smoke capture: failed while writing screenshot {}", path.string());
            return false;
        }

        RVX_CORE_INFO("ModelViewer smoke capture wrote {}", path.string());
        return true;
    }
} // namespace

int main(int argc, char* argv[])
{
    // Initialize logging
    Log::Initialize();
    RVX_CORE_INFO("=== ModelViewer Sample ===");

    ModelViewerOptions options;
    if (!ParseOptions(argc, argv, options))
    {
        PrintUsage();
        return -1;
    }

    if (options.showHelp)
    {
        PrintUsage();
        return 0;
    }

    const std::string modelPath = ResolveModelPath(options);
    if (modelPath.empty())
    {
        RVX_CORE_WARN("No model file specified. Usage: ModelViewer.exe [path_to_model.gltf]");
        if (options.smoke)
        {
            RVX_CORE_ERROR("Smoke mode requires a deterministic model fixture");
            return -1;
        }
        RVX_CORE_WARN("Will create an empty scene for demonstration.");
    }
    else
    {
        RVX_CORE_INFO("Model path: {}", modelPath);
    }

    if (options.smoke)
    {
        RVX_CORE_INFO("Smoke mode: backend={}, frames={}, resolution={}x{}, validation={}",
                      ToString(options.backend),
                      options.frames,
                      options.width,
                      options.height,
                      options.enableValidation);
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
    windowConfig.width = options.width;
    windowConfig.height = options.height;
    windowConfig.resizable = !options.smoke;
    windowConfig.vsync = !options.smoke;
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
    renderConfig.backendType = options.backend;
    renderConfig.enableValidation = options.enableValidation;
    renderConfig.vsync = !options.smoke;
    renderConfig.autoBindWindow = true;
    renderConfig.autoRender = !options.smoke;  // Smoke mode renders manually for capture timing.
    renderSubsystem->SetConfig(renderConfig);

    // Initialize engine
    engine.Initialize();

    if (!engine.IsInitialized())
    {
        RVX_CORE_ERROR("Failed to initialize engine");
        return -1;
    }

    if (options.smoke)
    {
        if (!renderSubsystem->IsReady())
        {
            RVX_CORE_ERROR("Smoke mode requires a ready render subsystem and swap chain");
            engine.Shutdown();
            return -1;
        }

        if (!options.screenshotPath.empty())
        {
            IRHIDevice* device = renderSubsystem->GetDevice();
            const RHIBackendType backendType = device ? device->GetBackendType() : RHIBackendType::None;
            if (!device || (backendType != RHIBackendType::DX11 && backendType != RHIBackendType::DX12))
            {
                RVX_CORE_ERROR("Smoke screenshot gate currently supports DX11/DX12");
                engine.Shutdown();
                return -1;
            }
        }
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
    Vec3 cameraPos = options.smoke ? Vec3(0.0f, 1.5f, 4.0f) : Vec3(0.0f, 2.0f, 5.0f);
    Vec3 target(0.0f, 0.0f, 0.0f);
    camera->SetPosition(cameraPos);
    camera->LookAt(target);
    camera->SetPerspective(glm::radians(45.0f), static_cast<float>(options.width) / static_cast<float>(options.height), 0.1f, 1000.0f);
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
            // This creates a SceneEntity tree with StaticMeshComponents attached.
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

    // If no model was loaded, create a fallback entity for interactive demonstration.
    if (!modelEntity && !options.smoke)
    {
        RVX_CORE_INFO("Creating fallback entity (no model loaded)");
        
        ActorSpawnParams fallbackParams;
        fallbackParams.name = "FallbackEntity";
        modelEntity = sceneManager->SpawnActor(fallbackParams);
        
        if (modelEntity)
        {
            modelEntity->SetPosition(Vec3(0.0f, 0.0f, 0.0f));
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

    if (options.smoke)
    {
        RVX_CORE_INFO("Running deterministic smoke loop");
        bool smokeSucceeded = true;

        for (uint32 frameIndex = 0; frameIndex < options.frames; ++frameIndex)
        {
            engine.TickWithoutRender(kSmokeDeltaSeconds);

            cameraPos = Vec3(0.0f, 1.5f, 4.0f);
            camera->SetPosition(cameraPos);
            camera->LookAt(target);

            PendingScreenshot pendingScreenshot;
            const bool captureFrame = !options.screenshotPath.empty() && (frameIndex + 1 == options.frames);

            renderSubsystem->BeginFrame();
            renderSubsystem->Render(world, camera);

            if (captureFrame && !QueueBackBufferScreenshot(renderSubsystem, pendingScreenshot))
            {
                smokeSucceeded = false;
            }

            renderSubsystem->EndFrame();

            if (captureFrame && smokeSucceeded)
            {
                renderSubsystem->GetRenderContext()->WaitIdle();
                if (!WriteScreenshotPPM(pendingScreenshot, options.screenshotPath))
                {
                    smokeSucceeded = false;
                }
            }

            renderSubsystem->Present();

            if (engine.ShouldShutdown())
            {
                RVX_CORE_ERROR("Smoke mode ended early because shutdown was requested");
                smokeSucceeded = false;
                break;
            }
        }

        engine.Shutdown();
        RVX_CORE_INFO("=== ModelViewer Smoke {} ===", smokeSucceeded ? "PASS" : "FAIL");
        return smokeSucceeded ? 0 : -1;
    }

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
