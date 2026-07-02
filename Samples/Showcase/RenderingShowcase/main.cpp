/**
 * @file main.cpp
 * @brief Shared implementation for RenderVerseX feature showcase samples
 */

#include "Core/Core.h"
#include "Core/MathTypes.h"
#include "Engine/Engine.h"
#include "HAL/Input/KeyCodes.h"
#include "Render/Passes/ShadowPass.h"
#include "Render/PostProcess/PostProcessStack.h"
#include "Render/PostProcess/ToneMappingTypes.h"
#include "Render/Renderer/SceneRenderer.h"
#include "Render/RenderSubsystem.h"
#include "Resource/ResourceManager.h"
#include "Resource/ResourceSubsystem.h"
#include "Resource/Types/ModelResource.h"
#include "Runtime/Camera/Camera.h"
#include "Runtime/Input/InputSubsystem.h"
#include "Runtime/Window/WindowSubsystem.h"
#include "Scene/Components/LightComponent.h"
#include "Scene/Components/SkyboxComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"
#include "World/World.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

using namespace RVX;

namespace
{
    enum class ShowcaseMode : uint8
    {
        PostProcess = 0,
        Lighting = 1,
        Material = 2,
        Rendering = 3,
        SceneInteraction = 4,
        TerrainWater = 5,
        ParticleFX = 6
    };

    struct ShowcaseOptions
    {
        ShowcaseMode mode = static_cast<ShowcaseMode>(RVX_FEATURE_SHOWCASE_DEFAULT_MODE);
        RHIBackendType backend = RHIBackendType::Auto;
        std::string modelPath;
        uint32 width = 1280;
        uint32 height = 720;
        uint32 frames = 0;
        bool enableValidation = true;
        bool showHelp = false;
    };

    struct OrbitCamera
    {
        Vec3 target{0.0f, 0.25f, 0.0f};
        float distance = 4.0f;
        float yaw = 0.0f;
        float pitch = 0.25f;
        float orbitSpeed = 0.005f;
        float zoomSpeed = 0.45f;
        float minDistance = 1.2f;
        float maxDistance = 25.0f;
        float minPitch = -1.35f;
        float maxPitch = 1.35f;

        Vec3 GetPosition() const
        {
            const float x = distance * std::cos(pitch) * std::sin(yaw);
            const float y = distance * std::sin(pitch);
            const float z = distance * std::cos(pitch) * std::cos(yaw);
            return target + Vec3(x, y, z);
        }
    };

    struct LightRig
    {
        SceneEntity* sun = nullptr;
        SceneEntity* warmPoint = nullptr;
        SceneEntity* coolPoint = nullptr;
        SceneEntity* spot = nullptr;
    };

    std::string ToLower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    const char* GetModeName(ShowcaseMode mode)
    {
        switch (mode)
        {
            case ShowcaseMode::PostProcess: return "RenderingShowcase-PostProcess";
            case ShowcaseMode::Lighting: return "RenderingShowcase-Lighting";
            case ShowcaseMode::Material: return "RenderingShowcase-Material";
            case ShowcaseMode::Rendering: return "RenderingShowcase";
            case ShowcaseMode::SceneInteraction: return "SceneInteractionShowcase";
            case ShowcaseMode::TerrainWater: return "TerrainWaterShowcase";
            case ShowcaseMode::ParticleFX: return "ParticleFXShowcase";
        }

        return "EngineShowcase";
    }

    const char* GetModeDescription(ShowcaseMode mode)
    {
        switch (mode)
        {
            case ShowcaseMode::PostProcess:
                return "post-processing presets, tone mapping, bloom, and grading";
            case ShowcaseMode::Lighting:
                return "directional, point, spot, colored, and shadowed lighting";
            case ShowcaseMode::Material:
                return "PBR material response under curated light and exposure presets";
            case ShowcaseMode::Rendering:
                return "integrated PBR, shadows, skybox, lighting, post-process, and camera showcase";
            case ShowcaseMode::SceneInteraction:
                return "scene entities, camera orbit, input controls, and picking-ready world setup";
            case ShowcaseMode::TerrainWater:
                return "terrain/water rendering staging with sky, lighting, and post-process presets";
            case ShowcaseMode::ParticleFX:
                return "particle FX staging with camera, lighting, and render pipeline integration";
        }

        return "render feature showcase";
    }

    bool ParseMode(const std::string& text, ShowcaseMode& outMode)
    {
        const std::string value = ToLower(text);
        if (value == "post" || value == "postprocess" || value == "post-process")
        {
            outMode = ShowcaseMode::PostProcess;
            return true;
        }
        if (value == "lighting" || value == "lights")
        {
            outMode = ShowcaseMode::Lighting;
            return true;
        }
        if (value == "material" || value == "materials")
        {
            outMode = ShowcaseMode::Material;
            return true;
        }
        if (value == "rendering" || value == "renderer" || value == "showcase")
        {
            outMode = ShowcaseMode::Rendering;
            return true;
        }
        if (value == "scene" || value == "interaction" || value == "scene-interaction")
        {
            outMode = ShowcaseMode::SceneInteraction;
            return true;
        }
        if (value == "terrain" || value == "water" || value == "terrain-water")
        {
            outMode = ShowcaseMode::TerrainWater;
            return true;
        }
        if (value == "particle" || value == "particles" || value == "particle-fx")
        {
            outMode = ShowcaseMode::ParticleFX;
            return true;
        }

        return false;
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

    void PrintUsage()
    {
        std::cout
            << "RenderVerseX feature showcase samples\n"
            << "Usage:\n"
            << "  <ShowcaseTarget> [options]\n"
            << "Options:\n"
            << "  --mode <postprocess|lighting|material|rendering|scene-interaction|terrain-water|particle-fx>\n"
            << "  --model <path.gltf>       Override the default fixture model\n"
            << "  --backend <auto|dx11|dx12|vulkan|metal|opengl>\n"
            << "  --width <pixels>\n"
            << "  --height <pixels>\n"
            << "  --frames <count>       Run a bounded smoke loop, 0 means interactive\n"
            << "  --smoke                Alias for --frames 8\n"
            << "  --validation\n"
            << "  --no-validation\n"
            << "  --help\n";
    }

    bool ParseOptions(int argc, char* argv[], ShowcaseOptions& options)
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
            else if (arg == "--mode")
            {
                const char* value = requireValue("--mode");
                if (!value || !ParseMode(value, options.mode))
                {
                    RVX_CORE_ERROR("Invalid --mode value: {}", value ? value : "");
                    return false;
                }
            }
            else if (arg == "--model")
            {
                const char* value = requireValue("--model");
                if (!value)
                {
                    return false;
                }
                options.modelPath = value;
            }
            else if (arg == "--backend")
            {
                const char* value = requireValue("--backend");
                if (!value || !ParseBackend(value, options.backend))
                {
                    RVX_CORE_ERROR("Invalid --backend value: {}", value ? value : "");
                    return false;
                }
            }
            else if (arg == "--width")
            {
                const char* value = requireValue("--width");
                if (!ParseUInt(value, options.width))
                {
                    RVX_CORE_ERROR("Invalid --width value: {}", value ? value : "");
                    return false;
                }
            }
            else if (arg == "--height")
            {
                const char* value = requireValue("--height");
                if (!ParseUInt(value, options.height))
                {
                    RVX_CORE_ERROR("Invalid --height value: {}", value ? value : "");
                    return false;
                }
            }
            else if (arg == "--frames")
            {
                const char* value = requireValue("--frames");
                if (!ParseUInt(value, options.frames))
                {
                    RVX_CORE_ERROR("Invalid --frames value: {}", value ? value : "");
                    return false;
                }
            }
            else if (arg == "--smoke")
            {
                options.frames = 8;
            }
            else if (arg == "--validation")
            {
                options.enableValidation = true;
            }
            else if (arg == "--no-validation")
            {
                options.enableValidation = false;
            }
            else
            {
                RVX_CORE_ERROR("Unknown option: {}", arg);
                return false;
            }
        }

        return true;
    }

    std::filesystem::path GetExecutableDirectory(char* argv0)
    {
#ifdef _WIN32
        char path[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (length > 0 && length < MAX_PATH)
        {
            return std::filesystem::path(path).parent_path();
        }
#endif

        if (argv0 && argv0[0] != '\0')
        {
            return std::filesystem::absolute(argv0).parent_path();
        }

        return std::filesystem::current_path();
    }

    void AddSearchRoots(std::vector<std::filesystem::path>& roots, const std::filesystem::path& start)
    {
        if (start.empty())
        {
            return;
        }

        std::filesystem::path current = start;
        for (uint32 i = 0; i < 8 && !current.empty(); ++i)
        {
            roots.push_back(current);
            const std::filesystem::path parent = current.parent_path();
            if (parent == current)
            {
                break;
            }
            current = parent;
        }
    }

    std::filesystem::path ResolvePathFromRoots(const std::filesystem::path& path,
                                               const std::vector<std::filesystem::path>& roots)
    {
        if (path.empty())
        {
            return {};
        }

        if (path.is_absolute() && std::filesystem::exists(path))
        {
            return path;
        }

        if (std::filesystem::exists(path))
        {
            return std::filesystem::absolute(path);
        }

        for (const auto& root : roots)
        {
            const std::filesystem::path candidate = root / path;
            if (std::filesystem::exists(candidate))
            {
                return std::filesystem::absolute(candidate);
            }
        }

        return {};
    }

    std::filesystem::path GetDefaultModelRelativePath(ShowcaseMode mode)
    {
        switch (mode)
        {
            case ShowcaseMode::Lighting:
            case ShowcaseMode::Rendering:
            case ShowcaseMode::SceneInteraction:
            case ShowcaseMode::TerrainWater:
            case ShowcaseMode::ParticleFX:
                return "Tests/Fixtures/ModelViewer/ShadowPlaneCaster.gltf";
            case ShowcaseMode::PostProcess:
            case ShowcaseMode::Material:
                return "Tests/Fixtures/ModelViewer/PBRMaterialSwatch.gltf";
        }

        return "Tests/Fixtures/ModelViewer/R7Triangle.gltf";
    }

    std::filesystem::path GetDefaultMaterialSwatchRelativePath()
    {
        return "Tests/Fixtures/ModelViewer/PBRMaterialSwatch.gltf";
    }

    Quat MakeLookRotation(const Vec3& direction, const Vec3& up)
    {
        Vec3 forward = direction;
        if (length(forward) < 0.001f)
        {
            forward = Vec3(0.0f, -1.0f, 0.0f);
        }
        forward = normalize(forward);

        Vec3 right = cross(up, forward);
        if (length(right) < 0.001f)
        {
            right = Vec3(1.0f, 0.0f, 0.0f);
        }
        else
        {
            right = normalize(right);
        }

        const Vec3 correctedUp = cross(forward, right);

        Mat4 lookMatrix(1.0f);
        lookMatrix[0] = Vec4(right, 0.0f);
        lookMatrix[1] = Vec4(correctedUp, 0.0f);
        lookMatrix[2] = Vec4(-forward, 0.0f);
        lookMatrix[3] = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
        return Mat4ToQuat(lookMatrix);
    }

    SceneEntity* SpawnLight(SceneManager* sceneManager,
                            const char* name,
                            LightType type,
                            const Vec3& position,
                            const Vec3& direction,
                            const Vec3& color,
                            float intensity,
                            float range,
                            bool castsShadow)
    {
        ActorSpawnParams params;
        params.name = name ? name : "ShowcaseLight";
        SceneEntity* entity = sceneManager ? sceneManager->SpawnActor(params) : nullptr;
        LightComponent* light = entity ? entity->AddComponent<LightComponent>() : nullptr;
        if (!entity || !light)
        {
            RVX_CORE_ERROR("Failed to create showcase light '{}'", params.name);
            return nullptr;
        }

        entity->SetPosition(position);
        entity->SetRotation(MakeLookRotation(direction, Vec3(0.0f, 1.0f, 0.0f)));

        light->SetLightType(type);
        light->SetColor(color);
        light->SetIntensity(intensity);
        light->SetRange(range);
        light->SetCastsShadow(castsShadow);
        if (type == LightType::Spot)
        {
            light->SetInnerConeAngle(radians(18.0f));
            light->SetOuterConeAngle(radians(38.0f));
        }

        return entity;
    }

    LightRig CreateLightRig(SceneManager* sceneManager)
    {
        LightRig rig;
        rig.sun = SpawnLight(sceneManager,
                             "ShowcaseSun",
                             LightType::Directional,
                             Vec3(0.0f, 4.0f, 2.0f),
                             normalize(Vec3(-0.25f, -0.45f, -0.85f)),
                             Vec3(1.0f, 0.96f, 0.88f),
                             4.0f,
                             32.0f,
                             false);

        rig.warmPoint = SpawnLight(sceneManager,
                                   "ShowcaseWarmPoint",
                                   LightType::Point,
                                   Vec3(-1.6f, 1.2f, 1.0f),
                                   Vec3(0.0f, -1.0f, 0.0f),
                                   Vec3(1.0f, 0.42f, 0.18f),
                                   18.0f,
                                   5.5f,
                                   false);

        rig.coolPoint = SpawnLight(sceneManager,
                                   "ShowcaseCoolPoint",
                                   LightType::Point,
                                   Vec3(1.7f, 1.1f, 0.6f),
                                   Vec3(0.0f, -1.0f, 0.0f),
                                   Vec3(0.22f, 0.58f, 1.0f),
                                   14.0f,
                                   5.0f,
                                   false);

        rig.spot = SpawnLight(sceneManager,
                              "ShowcaseSpot",
                              LightType::Spot,
                              Vec3(0.0f, 2.2f, 2.2f),
                              normalize(Vec3(0.0f, -0.55f, -1.0f)),
                              Vec3(0.85f, 1.0f, 0.74f),
                              30.0f,
                              8.0f,
                              false);

        return rig;
    }

    void SetLightActive(SceneEntity* entity, bool active)
    {
        if (entity)
        {
            entity->SetActive(active);
        }
    }

    void ApplyLightPreset(LightRig& rig, int presetIndex)
    {
        SetLightActive(rig.sun, false);
        SetLightActive(rig.warmPoint, false);
        SetLightActive(rig.coolPoint, false);
        SetLightActive(rig.spot, false);

        switch (presetIndex)
        {
            case 1:
                SetLightActive(rig.sun, true);
                SetLightActive(rig.warmPoint, true);
                SetLightActive(rig.coolPoint, true);
                SetLightActive(rig.spot, false);
                if (auto* light = rig.sun ? rig.sun->GetComponent<LightComponent>() : nullptr)
                {
                    light->SetIntensity(2.2f);
                    light->SetCastsShadow(false);
                }
                RVX_CORE_INFO("Lighting preset 2: colored point lights");
                break;
            case 2:
                SetLightActive(rig.sun, true);
                if (auto* light = rig.sun ? rig.sun->GetComponent<LightComponent>() : nullptr)
                {
                    light->SetIntensity(5.0f);
                    light->SetCastsShadow(true);
                    light->SetShadowBias(0.0008f);
                }
                RVX_CORE_INFO("Lighting preset 3: directional shadow");
                break;
            case 0:
            default:
                SetLightActive(rig.sun, true);
                SetLightActive(rig.spot, true);
                if (auto* light = rig.sun ? rig.sun->GetComponent<LightComponent>() : nullptr)
                {
                    light->SetIntensity(4.0f);
                    light->SetCastsShadow(false);
                }
                RVX_CORE_INFO("Lighting preset 1: studio key plus soft spot");
                break;
        }
    }

    void ApplyPostProcessPreset(SceneRenderer* sceneRenderer, int presetIndex)
    {
        if (!sceneRenderer)
        {
            return;
        }

        PostProcessSettings settings = sceneRenderer->GetPostProcessSettings();
        settings.enableToneMapping = true;
        settings.toneMappingOperator = ToneMappingOperator::ACES;
        settings.exposureMode = ToneMappingExposureMode::ManualMultiplier;
        settings.exposure = 1.0f;
        settings.gamma = 2.2f;
        settings.enableBloom = false;
        settings.bloomIntensity = 0.0f;
        settings.bloomThreshold = 1.0f;
        settings.bloomRadius = 0.5f;
        settings.enableFXAA = true;
        settings.enableColorGrading = true;
        settings.contrast = 1.0f;
        settings.saturation = 1.0f;
        settings.brightness = 0.0f;
        settings.enableVignette = false;
        settings.vignetteIntensity = 0.25f;
        settings.enableChromaticAberration = false;
        settings.chromaticAberrationIntensity = 0.05f;

        switch (presetIndex)
        {
            case 1:
                settings.enableBloom = true;
                settings.bloomIntensity = 1.5f;
                settings.bloomThreshold = 0.75f;
                settings.bloomRadius = 1.25f;
                settings.exposure = 1.1f;
                RVX_CORE_INFO("Post-process preset 2: bloom");
                break;
            case 2:
                settings.contrast = 1.18f;
                settings.saturation = 1.12f;
                settings.brightness = 0.02f;
                settings.enableVignette = true;
                settings.vignetteIntensity = 0.32f;
                settings.enableChromaticAberration = true;
                settings.chromaticAberrationIntensity = 0.035f;
                RVX_CORE_INFO("Post-process preset 3: filmic grade");
                break;
            case 3:
                settings.toneMappingOperator = ToneMappingOperator::Neutral;
                settings.exposure = 0.75f;
                settings.enableColorGrading = false;
                settings.enableFXAA = false;
                RVX_CORE_INFO("Post-process preset 4: neutral debug");
                break;
            case 0:
            default:
                RVX_CORE_INFO("Post-process preset 1: clean ACES");
                break;
        }

        sceneRenderer->ApplyPostProcessSettings(settings);
    }

    ShadowPassConfig MakeShowcaseShadowConfig()
    {
        ShadowPassConfig config;
        config.shadowMapSize = 2048;
        config.numCascades = 3;
        config.cascadeSplitLambda = 0.90f;
        config.filterRadiusTexels = 1.25f;
        config.shadowBias = 0.0035f;
        config.normalBias = 0.02f;
        config.cascadeBlendRatio = 0.05f;
        return config;
    }

    bool CreateSkybox(SceneManager* sceneManager)
    {
        ActorSpawnParams params;
        params.name = "ShowcaseProceduralSky";
        SceneEntity* entity = sceneManager ? sceneManager->SpawnActor(params) : nullptr;
        SkyboxComponent* skybox = entity ? entity->AddComponent<SkyboxComponent>() : nullptr;
        if (!skybox)
        {
            RVX_CORE_WARN("Could not create showcase skybox component");
            return false;
        }

        skybox->SetSkyboxType(SkyboxType::Procedural);
        skybox->SetSunDirection(Vec3{0.35f, 0.65f, 0.45f});
        skybox->SetSunColor(Vec3{1.0f, 0.94f, 0.82f});
        skybox->SetZenithColor(Vec3{0.12f, 0.28f, 0.58f});
        skybox->SetHorizonColor(Vec3{0.55f, 0.68f, 0.82f});
        skybox->SetGroundColor(Vec3{0.08f, 0.09f, 0.11f});
        skybox->SetScatteringIntensity(0.65f);
        skybox->SetExposure(1.0f);
        skybox->SetContributesToLighting(false);
        return true;
    }

    SceneEntity* LoadModel(SceneManager* sceneManager,
                           const std::filesystem::path& modelPath,
                           ShowcaseMode mode,
                           const Vec3& position,
                           const Vec3& scale,
                           Resource::ResourceHandle<Resource::ModelResource>& outHandle)
    {
        if (!sceneManager || modelPath.empty())
        {
            return nullptr;
        }

        RVX_CORE_INFO("Loading model: {}", modelPath.string());
        outHandle = Resource::ResourceManager::Get().Load<Resource::ModelResource>(modelPath.string());
        if (!outHandle.IsValid() || !outHandle.IsLoaded())
        {
            RVX_CORE_ERROR("Failed to load model: {}", modelPath.string());
            return nullptr;
        }

        SceneEntity* modelEntity = outHandle->Instantiate(sceneManager);
        if (!modelEntity)
        {
            RVX_CORE_ERROR("Failed to instantiate model: {}", modelPath.string());
            return nullptr;
        }

        (void)mode;
        modelEntity->SetPosition(position);
        modelEntity->SetScale(scale);

        RVX_CORE_INFO("Model ready: meshes={}, materials={}, nodes={}",
                      outHandle->GetMeshCount(),
                      outHandle->GetMaterialCount(),
                      outHandle->GetNodeCount());
        return modelEntity;
    }

    void ConfigureCameraForMode(Camera* camera, ShowcaseMode mode, uint32 width, uint32 height, OrbitCamera& orbit)
    {
        if (!camera)
        {
            return;
        }

        const float aspect = static_cast<float>(width) / static_cast<float>(height);
        camera->SetPerspective(radians(45.0f), aspect, 0.1f, 1000.0f);

        switch (mode)
        {
            case ShowcaseMode::Lighting:
                orbit.target = Vec3(0.0f, 0.35f, 0.0f);
                orbit.distance = 4.0f;
                orbit.pitch = 0.28f;
                break;
            case ShowcaseMode::Material:
                orbit.target = Vec3(0.0f, 0.18f, 0.0f);
                orbit.distance = 2.7f;
                orbit.pitch = 0.18f;
                break;
            case ShowcaseMode::Rendering:
                orbit.target = Vec3(0.0f, 0.45f, -0.15f);
                orbit.distance = 5.0f;
                orbit.pitch = 0.24f;
                break;
            case ShowcaseMode::SceneInteraction:
                orbit.target = Vec3(0.0f, 0.4f, -0.1f);
                orbit.distance = 4.6f;
                orbit.pitch = 0.2f;
                break;
            case ShowcaseMode::TerrainWater:
                orbit.target = Vec3(0.0f, 0.2f, -0.45f);
                orbit.distance = 6.0f;
                orbit.pitch = 0.16f;
                break;
            case ShowcaseMode::ParticleFX:
                orbit.target = Vec3(0.0f, 0.5f, -0.2f);
                orbit.distance = 4.2f;
                orbit.pitch = 0.3f;
                break;
            case ShowcaseMode::PostProcess:
            default:
                orbit.target = Vec3(0.0f, 0.2f, 0.0f);
                orbit.distance = 3.2f;
                orbit.pitch = 0.24f;
                break;
        }

        camera->SetPosition(orbit.GetPosition());
        camera->LookAt(orbit.target);
    }

    void PrintControls(ShowcaseMode mode)
    {
        RVX_CORE_INFO("Controls:");
        RVX_CORE_INFO("  Left mouse drag: orbit camera");
        RVX_CORE_INFO("  Mouse wheel: zoom");
        RVX_CORE_INFO("  R: reset camera");
        RVX_CORE_INFO("  1-4: post-process presets");
        RVX_CORE_INFO("  F1-F3: lighting presets");
        RVX_CORE_INFO("  ESC: exit");
        RVX_CORE_INFO("Current sample '{}': {}", GetModeName(mode), GetModeDescription(mode));
    }
} // namespace

int main(int argc, char* argv[])
{
    Log::Initialize();

    ShowcaseOptions options;
    if (!ParseOptions(argc, argv, options))
    {
        PrintUsage();
        Log::Shutdown();
        return -1;
    }

    if (options.showHelp)
    {
        PrintUsage();
        Log::Shutdown();
        return 0;
    }

    std::vector<std::filesystem::path> searchRoots;
    AddSearchRoots(searchRoots, std::filesystem::current_path());
    AddSearchRoots(searchRoots, GetExecutableDirectory(argv[0]));

    const std::filesystem::path requestedModel =
        options.modelPath.empty() ? GetDefaultModelRelativePath(options.mode) : std::filesystem::path(options.modelPath);
    const std::filesystem::path modelPath = ResolvePathFromRoots(requestedModel, searchRoots);
    const std::filesystem::path materialSwatchPath =
        ResolvePathFromRoots(GetDefaultMaterialSwatchRelativePath(), searchRoots);

    RVX_CORE_INFO("=== {} ===", GetModeName(options.mode));
    RVX_CORE_INFO("Showcase: {}", GetModeDescription(options.mode));
    RVX_CORE_INFO("Backend: {}", ToString(options.backend));

    if (modelPath.empty())
    {
        RVX_CORE_WARN("Model path not found: {}", requestedModel.string());
    }
    else
    {
        RVX_CORE_INFO("Model path: {}", modelPath.string());
    }

    Engine engine;
    EngineConfig engineConfig;
    engineConfig.appName = GetModeName(options.mode);
    engineConfig.windowWidth = options.width;
    engineConfig.windowHeight = options.height;
    engineConfig.vsync = true;
    engineConfig.enableJobSystem = false;
    engine.SetConfig(engineConfig);

    auto* windowSubsystem = engine.AddSubsystem<WindowSubsystem>();
    const std::string windowTitle = std::string("RenderVerseX - ") + GetModeName(options.mode);
    WindowConfig windowConfig;
    windowConfig.title = windowTitle.c_str();
    windowConfig.width = options.width;
    windowConfig.height = options.height;
    windowConfig.resizable = options.frames == 0;
    windowConfig.vsync = options.frames == 0;
    windowConfig.graphicsApi = options.backend == RHIBackendType::OpenGL ?
        WindowGraphicsApi::OpenGL :
        WindowGraphicsApi::None;
    windowSubsystem->SetConfig(windowConfig);

    engine.AddSubsystem<Resource::ResourceSubsystem>();
    engine.AddSubsystem<InputSubsystem>();

    auto* renderSubsystem = engine.AddSubsystem<RenderSubsystem>();
    RenderConfig renderConfig;
    renderConfig.backendType = options.backend;
    renderConfig.enableValidation = options.enableValidation;
    renderConfig.vsync = options.frames == 0;
    renderConfig.autoBindWindow = true;
    renderConfig.autoRender = true;
    renderSubsystem->SetConfig(renderConfig);

    engine.Initialize();
    if (!engine.IsInitialized())
    {
        RVX_CORE_ERROR("Failed to initialize engine");
        Log::Shutdown();
        return -1;
    }

    auto* input = engine.GetSubsystem<InputSubsystem>();
    if (input && windowSubsystem->GetWindow())
    {
        input->SetWindow(windowSubsystem->GetWindow());
    }

    SceneRenderer* sceneRenderer = renderSubsystem->GetSceneRenderer();
    if (sceneRenderer)
    {
        sceneRenderer->ApplyShadowPassConfig(MakeShowcaseShadowConfig());
        const int startupPostPreset =
            (options.mode == ShowcaseMode::Material ||
             options.mode == ShowcaseMode::Rendering ||
             options.mode == ShowcaseMode::TerrainWater ||
             options.mode == ShowcaseMode::ParticleFX) ? 2 : 0;
        ApplyPostProcessPreset(sceneRenderer, startupPostPreset);
    }

    World* world = engine.CreateWorld("ShowcaseWorld");
    if (!world)
    {
        RVX_CORE_ERROR("Failed to create showcase world");
        engine.Shutdown();
        Log::Shutdown();
        return -1;
    }

    Camera* camera = world->CreateCamera("MainCamera");
    OrbitCamera orbitCamera;
    ConfigureCameraForMode(camera, options.mode, options.width, options.height, orbitCamera);
    world->SetActiveCamera(camera);
    engine.SetActiveWorld(world);

    SceneManager* sceneManager = world->GetSceneManager();
    if (!sceneManager)
    {
        RVX_CORE_ERROR("Showcase world has no scene manager");
        engine.Shutdown();
        Log::Shutdown();
        return -1;
    }

    CreateSkybox(sceneManager);

    std::vector<Resource::ResourceHandle<Resource::ModelResource>> modelHandles;
    if (!modelPath.empty())
    {
        modelHandles.emplace_back();
        LoadModel(sceneManager,
                  modelPath,
                  options.mode,
                  Vec3(0.0f, 0.0f, 0.0f),
                  Vec3(1.0f),
                  modelHandles.back());
    }

    if ((options.mode == ShowcaseMode::Rendering || options.mode == ShowcaseMode::SceneInteraction) &&
        !materialSwatchPath.empty())
    {
        const std::array<Vec3, 3> swatchPositions = {
            Vec3(-1.35f, 0.55f, -0.95f),
            Vec3(0.0f, 0.55f, -0.95f),
            Vec3(1.35f, 0.55f, -0.95f)
        };

        for (const Vec3& position : swatchPositions)
        {
            modelHandles.emplace_back();
            SceneEntity* swatch = LoadModel(sceneManager,
                                            materialSwatchPath,
                                            options.mode,
                                            position,
                                            Vec3(0.65f),
                                            modelHandles.back());
            if (swatch)
            {
                swatch->SetName("RenderingShowcaseMaterialSwatch");
            }
        }
    }

    LightRig lightRig = CreateLightRig(sceneManager);
    const int initialLightPreset =
        (options.mode == ShowcaseMode::Lighting ||
         options.mode == ShowcaseMode::Rendering ||
         options.mode == ShowcaseMode::SceneInteraction ||
         options.mode == ShowcaseMode::TerrainWater ||
         options.mode == ShowcaseMode::ParticleFX) ? 2 : 0;
    ApplyLightPreset(lightRig, initialLightPreset);

    bool isVulkan = false;
    if (auto* device = renderSubsystem->GetDevice())
    {
        isVulkan = device->GetBackendType() == RHIBackendType::Vulkan;
        RVX_CORE_INFO("Adapter: {}", device->GetCapabilities().adapterName);
    }
    const float pitchDirection = isVulkan ? -1.0f : 1.0f;

    float lastMouseX = 0.0f;
    float lastMouseY = 0.0f;
    if (input)
    {
        input->GetMousePosition(lastMouseX, lastMouseY);
    }

    int postPreset = (options.mode == ShowcaseMode::Material ||
                      options.mode == ShowcaseMode::Rendering ||
                      options.mode == ShowcaseMode::TerrainWater ||
                      options.mode == ShowcaseMode::ParticleFX) ? 2 : 0;
    int lightPreset = initialLightPreset;
    uint32 renderedFrames = 0;
    PrintControls(options.mode);

    while (!engine.ShouldShutdown())
    {
        if (input)
        {
            if (input->IsKeyPressed(Key::Escape))
            {
                engine.RequestShutdown();
            }

            if (input->IsKeyPressed(Key::Num1))
            {
                postPreset = 0;
                ApplyPostProcessPreset(sceneRenderer, postPreset);
            }
            if (input->IsKeyPressed(Key::Num2))
            {
                postPreset = 1;
                ApplyPostProcessPreset(sceneRenderer, postPreset);
            }
            if (input->IsKeyPressed(Key::Num3))
            {
                postPreset = 2;
                ApplyPostProcessPreset(sceneRenderer, postPreset);
            }
            if (input->IsKeyPressed(Key::Num4))
            {
                postPreset = 3;
                ApplyPostProcessPreset(sceneRenderer, postPreset);
            }

            if (input->IsKeyPressed(Key::F1))
            {
                lightPreset = 0;
                ApplyLightPreset(lightRig, lightPreset);
            }
            if (input->IsKeyPressed(Key::F2))
            {
                lightPreset = 1;
                ApplyLightPreset(lightRig, lightPreset);
            }
            if (input->IsKeyPressed(Key::F3))
            {
                lightPreset = 2;
                ApplyLightPreset(lightRig, lightPreset);
            }

            float mouseX = 0.0f;
            float mouseY = 0.0f;
            input->GetMousePosition(mouseX, mouseY);

            if (input->IsMouseButtonDown(MouseButton::Left))
            {
                const float deltaX = mouseX - lastMouseX;
                const float deltaY = mouseY - lastMouseY;
                orbitCamera.yaw -= deltaX * orbitCamera.orbitSpeed;
                orbitCamera.pitch += deltaY * orbitCamera.orbitSpeed * pitchDirection;
                orbitCamera.pitch = std::clamp(orbitCamera.pitch,
                                               orbitCamera.minPitch,
                                               orbitCamera.maxPitch);
            }

            float scrollX = 0.0f;
            float scrollY = 0.0f;
            input->GetScrollDelta(scrollX, scrollY);
            if (scrollY != 0.0f)
            {
                orbitCamera.distance -= scrollY * orbitCamera.zoomSpeed;
                orbitCamera.distance = std::clamp(orbitCamera.distance,
                                                  orbitCamera.minDistance,
                                                  orbitCamera.maxDistance);
            }

            if (input->IsKeyPressed(Key::R))
            {
                ConfigureCameraForMode(camera, options.mode, options.width, options.height, orbitCamera);
                RVX_CORE_INFO("Camera reset");
            }

            lastMouseX = mouseX;
            lastMouseY = mouseY;
        }

        if (camera)
        {
            camera->SetPosition(orbitCamera.GetPosition());
            camera->LookAt(orbitCamera.target);
        }

        (void)postPreset;
        (void)lightPreset;
        engine.Tick();

        if (options.frames > 0 && ++renderedFrames >= options.frames)
        {
            engine.RequestShutdown();
        }
    }

    engine.Shutdown();
    RVX_CORE_INFO("=== {} Complete ===", GetModeName(options.mode));
    Log::Shutdown();
    return 0;
}
