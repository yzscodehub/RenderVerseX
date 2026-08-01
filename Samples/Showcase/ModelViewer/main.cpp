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
#include "Particle/ParticleComponent.h"
#include "Particle/ParticleSubsystem.h"
#include "Render/RenderSubsystem.h"
#include "Runtime/Window/WindowSubsystem.h"
#include "Runtime/Input/InputSubsystem.h"
#include "World/World.h"
#include "Runtime/Camera/Camera.h"
#include "Scene/SceneManager.h"
#include "Scene/SceneEntity.h"
#include "Scene/ComponentFactory.h"
#include "Scene/Components/LightComponent.h"
#include "Scene/Components/SkyboxComponent.h"
#include "Resource/ResourceSubsystem.h"
#include "Resource/ResourceManager.h"
#include "Resource/Loader/HDRTextureLoader.h"
#include "Resource/Types/ModelResource.h"
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/TextureResource.h"
#include "ResourceSceneAdapters/ResourceSceneAdapters.h"
#include "Samples/RuntimeFrameDriver.h"
#include "Core/Log.h"
#include "Core/MathTypes.h"
#include "HAL/Input/KeyCodes.h"
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
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

enum class ShadowQualityPreset
{
    Default = 0,
    Low,
    Medium,
    High,
    Ultra
};

enum class ToneMapSelection
{
    Default = 0,
    None,
    Reinhard,
    ReinhardExtended,
    ACES,
    Uncharted2,
    Neutral
};

struct ModelViewerOptions
{
    std::string modelPath;
    std::string screenshotPath;
    std::string hdriPath;
    RHIBackendType backend = RHIBackendType::Auto;
    uint32 width = 1280;
    uint32 height = 720;
    uint32 frames = 0;
    bool smoke = false;
    bool enableProceduralIBL = true;
    bool expectIBLReady = false;
    bool expectSkyboxReady = false;
    bool expectShadowReady = false;
    bool expectRayTracedShadowReady = false;
    bool expectRayTracedShadowAlphaReady = false;
    bool expectRayTracedShadowHistoryReady = false;
    bool expectRayTracedShadowHistoryResetReady = false;
    bool expectRayTracedShadowHistoryResizeReady = false;
    bool expectRayTracedReflectionReady = false;
    bool expectRayTracedReflectionAssetReady = false;
    bool expectRayTracedReflectionHistoryReady = false;
    bool expectRayTracedReflectionHistoryResetReady = false;
    bool expectRayTracedReflectionHistoryResizeReady = false;
    bool expectRayTracingBudgetApplied = false;
    bool expectRayTracingRayBudgetRespected = false;
    bool expectRayTracingResourceBudgetExceeded = false;
    bool expectRayTracingGpuTimingReady = false;
    bool expectRayTracingGpuBudgetApplied = false;
    bool expectRayTracingGpuBudgetRecovered = false;
    bool expectMaterialReady = false;
    bool expectProceduralIBLQuality = false;
    bool expectGPUDrivenCullingReady = false;
    bool expectParticlesReady = false;
    bool gpuDrivenCullingTestScene = false;
    bool disableGPUDrivenCulling = false;
    bool particleTestScene = false;
    bool materialTestScene = false;
    bool shadowTestScene = false;
    bool enableRayTracedReflections = false;
    bool enableRayTracedReflectionDenoise = true;
    uint64 rayTracingMaxRayCount = 0;
    uint64 rayTracingMaxDenoiseTapCount = 0;
    uint64 rayTracingMaxResourceBytes = 0;
    float rayTracingMaxGpuMs = 0.0f;
    float rayTracingMaxShadowGpuMs = 0.0f;
    float rayTracingMaxReflectionGpuMs = 0.0f;
    float rayTracingRecoveryMaxGpuMs = 0.0f;
    float rayTracingRecoveryMaxShadowGpuMs = 0.0f;
    float rayTracingRecoveryMaxReflectionGpuMs = 0.0f;
    uint32 rayTracingGpuAdjustmentFrameCount = 0;
    uint32 rayTracingHistoryResetFrame = 0;
    uint32 rayTracingResizeFrame = 0;
    uint32 rayTracingResizeWidth = 0;
    uint32 rayTracingResizeHeight = 0;
    bool enableValidation = true;
    bool enableGPUValidation = false;
    bool showHelp = false;
    bool backendSet = false;
    bool widthSet = false;
    bool heightSet = false;
    bool framesSet = false;
    ShadowQualityPreset shadowQualityPreset = ShadowQualityPreset::Default;
    ToneMapSelection tonemapSelection = ToneMapSelection::Default;
    bool postExposureSet = false;
    bool displayGammaSet = false;
    bool cameraEV100Set = false;
    bool exposureCompensationSet = false;
    bool bloomIntensitySet = false;
    bool bloomThresholdSet = false;
    bool bloomRadiusSet = false;
    float postExposure = 1.0f;
    float displayGamma = 2.2f;
    float cameraEV100 = 0.0f;
    float exposureCompensationEV = 0.0f;
    float bloomIntensity = 0.0f;
    float bloomThreshold = 1.0f;
    float bloomRadius = 0.5f;
};

struct ProceduralIBLResources
{
    Resource::ResourceHandle<Resource::TextureResource> irradiance;
    Resource::ResourceHandle<Resource::TextureResource> prefiltered;
    Resource::ResourceHandle<Resource::TextureResource> brdfLUT;
    uint32 equirectWidth = 0;
    uint32 equirectHeight = 0;
    uint32 environmentResolution = 0;
    uint32 irradianceResolution = 0;
    uint32 prefilteredResolution = 0;
    uint32 prefilteredMipLevels = 0;
    uint32 brdfLUTResolution = 0;
    uint32 convolutionSamples = 0;
};

struct HDRIEnvironmentResources
{
    Resource::ResourceHandle<Resource::TextureResource> environment;
    Resource::ResourceHandle<Resource::TextureResource> irradiance;
    Resource::ResourceHandle<Resource::TextureResource> prefiltered;
    Resource::ResourceHandle<Resource::TextureResource> brdfLUT;
    uint32 prefilteredMipLevels = 0;

    bool IsValid() const
    {
        return environment && irradiance && prefiltered && brdfLUT && prefilteredMipLevels > 0;
    }
};

namespace
{
    constexpr uint32 kSmokeDefaultWidth = 320;
    constexpr uint32 kSmokeDefaultHeight = 180;
    constexpr uint32 kSmokeDefaultFrames = 8;
    constexpr float kSmokeDeltaSeconds = 1.0f / 60.0f;
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
            << "  --hdri <path>        Use an HDR/EXR environment for skybox and texture IBL\n"
            << "  --no-ibl             Disable procedural ModelViewer IBL wiring\n"
            << "  --material-test-scene Add deterministic lighting for PBR material visual gates\n"
            << "  --shadow-test-scene  Add a deterministic shadow-casting directional light\n"
            << "  --shadow-quality <default|low|medium|high|ultra>\n"
            << "                       Select directional shadow quality preset\n"
            << "  --ray-traced-reflections\n"
            << "                       Enable the ray-traced reflection pass\n"
            << "  --no-rt-reflection-denoise\n"
            << "                       Composite raw RT reflections without the denoise pass\n"
            << "  --expect-ray-traced-shadow-history-ready\n"
            << "                       Require stable RT shadow history reuse by the final smoke frame\n"
            << "  --expect-ray-traced-shadow-history-reset-ready\n"
            << "                       Require RT shadow history reset on --rt-reset-history-frame and recovery by final frame\n"
            << "  --expect-ray-traced-shadow-history-resize-ready\n"
            << "                       Require RT shadow history resize/recreation on --rt-resize-frame and recovery by final frame\n"
            << "  --expect-ray-traced-reflection-history-ready\n"
            << "                       Require stable RT reflection history reuse by the final smoke frame\n"
            << "  --expect-ray-traced-reflection-history-reset-ready\n"
            << "                       Require RT reflection history reset on --rt-reset-history-frame and recovery by final frame\n"
            << "  --expect-ray-traced-reflection-history-resize-ready\n"
            << "                       Require RT reflection history resize/recreation on --rt-resize-frame and recovery by final frame\n"
            << "  --rt-max-rays <count>\n"
            << "                       Enable RT budget mode and cap estimated rays per frame\n"
            << "  --rt-max-denoise-taps <count>\n"
            << "                       Enable RT budget mode and cap estimated reflection denoise taps\n"
            << "  --rt-max-resource-bytes <bytes>\n"
            << "                       Enable RT budget mode and report tracked RT resource pressure\n"
            << "  --rt-max-gpu-ms <ms>\n"
            << "                       Enable RT budget mode and cap total measured RT GPU time\n"
            << "  --rt-max-shadow-gpu-ms <ms>\n"
            << "                       Enable RT budget mode and cap measured RT shadow GPU time\n"
            << "  --rt-max-reflection-gpu-ms <ms>\n"
            << "                       Enable RT budget mode and cap measured RT reflection GPU time\n"
            << "  --rt-recovery-gpu-ms <ms>\n"
            << "                       Relax total measured RT GPU budget after smoke observes GPU budget degradation\n"
            << "  --rt-recovery-shadow-gpu-ms <ms>\n"
            << "                       Relax measured RT shadow GPU budget after smoke observes GPU budget degradation\n"
            << "  --rt-recovery-reflection-gpu-ms <ms>\n"
            << "                       Relax measured RT reflection GPU budget after smoke observes GPU budget degradation\n"
            << "  --rt-gpu-adjust-frames <count>\n"
            << "                       Consecutive measured frames before RT GPU budget adjusts quality\n"
            << "  --rt-reset-history-frame <frame>\n"
            << "                       Request temporal history reset on the given smoke frame\n"
            << "  --rt-resize-frame <frame>\n"
            << "                       Resize the smoke swap chain on the given frame\n"
            << "  --rt-resize-width <pixels>\n"
            << "                       Width used by --rt-resize-frame\n"
            << "  --rt-resize-height <pixels>\n"
            << "                       Height used by --rt-resize-frame\n"
            << "  --tonemap <default|none|reinhard|reinhard-extended|aces|uncharted2|neutral>\n"
            << "                       Select tone mapping operator without changing default smoke/golden behavior\n"
            << "  --post-exposure <linear>\n"
            << "                       Set tone mapping exposure multiplier, range [0.0, 64.0]\n"
            << "  --camera-ev100 <value>\n"
            << "                       Set tone mapping camera EV100, range [-16.0, 32.0]\n"
            << "  --exposure-compensation <ev>\n"
            << "                       Set camera exposure compensation, range [-16.0, 16.0]\n"
            << "  --display-gamma <value>\n"
            << "                       Set tone mapping display gamma, range [0.1, 10.0]\n"
            << "  --bloom-intensity <value>\n"
            << "                       Set Bloom intensity, range [0.0, 16.0]\n"
            << "  --bloom-threshold <value>\n"
            << "                       Set HDR Bloom threshold, range [0.0, 64.0]\n"
            << "  --bloom-radius <texels>\n"
            << "                       Set Bloom sample radius in texels, range [0.0, 16.0]\n"
            << "  --expect-ibl-ready   Smoke mode fails unless texture IBL becomes ready\n"
            << "  --expect-skybox-ready Smoke mode fails unless SkyboxPass becomes ready\n"
            << "  --expect-shadow-ready Smoke mode fails unless directional shadow sampling is ready\n"
            << "  --expect-ray-traced-shadow-ready Smoke mode fails unless RT shadow dispatch records\n"
            << "  --expect-ray-traced-shadow-alpha-ready Smoke mode fails unless RT shadow alpha resources bind\n"
            << "  --expect-ray-traced-reflection-ready Smoke mode fails unless RT reflection, denoise, and composite record\n"
            << "  --expect-ray-traced-reflection-asset-ready Smoke mode fails unless RT reflection asset resources bind\n"
            << "  --expect-ray-tracing-budget-applied Smoke mode fails unless RT budget changes quality\n"
            << "  --expect-ray-tracing-ray-budget-respected Smoke mode fails unless final estimated RT rays fit the budget\n"
            << "  --expect-ray-tracing-resource-budget-exceeded Smoke mode fails unless tracked RT resources exceed budget\n"
            << "  --expect-ray-tracing-gpu-timing-ready Smoke mode fails unless RT timestamp queries record and resolve\n"
            << "  --expect-ray-tracing-gpu-budget-applied Smoke mode fails unless measured RT GPU budget reduces quality\n"
            << "  --expect-ray-tracing-gpu-budget-recovered Smoke mode fails unless relaxed measured RT GPU budget recovers quality\n"
            << "  --expect-material-ready Smoke mode fails unless the PBR material swatch binds all texture maps\n"
            << "  --expect-procedural-ibl-quality Smoke mode fails unless default procedural IBL uses the CPU HDR pipeline\n"
            << "  --expect-gpu-driven-culling-ready Smoke mode fails unless GPU-driven culling feeds indirect draws\n"
            << "  --expect-particles-ready Smoke mode fails unless CPU billboard particles simulate and render\n"
            << "  --gpu-driven-culling-test-scene Add a deterministic distance-culled GPU-driven draw\n"
            << "  --disable-gpu-driven-culling Disable SceneRenderer GPU-driven culling for comparison gates\n"
            << "  --particle-test-scene Add a deterministic CPU billboard particle system\n"
            << "  --validation         Enable backend validation\n"
            << "  --gpu-validation     Enable DX12 GPU-based validation for dedicated bounded runs\n"
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

    const char* GetShadowQualityPresetName(ShadowQualityPreset preset)
    {
        switch (preset)
        {
            case ShadowQualityPreset::Default: return "default";
            case ShadowQualityPreset::Low: return "low";
            case ShadowQualityPreset::Medium: return "medium";
            case ShadowQualityPreset::High: return "high";
            case ShadowQualityPreset::Ultra: return "ultra";
        }

        return "default";
    }

    bool ParseShadowQualityPreset(const std::string& text, ShadowQualityPreset& outPreset)
    {
        const std::string value = ToLower(text);
        if (value == "default")
        {
            outPreset = ShadowQualityPreset::Default;
        }
        else if (value == "low")
        {
            outPreset = ShadowQualityPreset::Low;
        }
        else if (value == "medium")
        {
            outPreset = ShadowQualityPreset::Medium;
        }
        else if (value == "high")
        {
            outPreset = ShadowQualityPreset::High;
        }
        else if (value == "ultra")
        {
            outPreset = ShadowQualityPreset::Ultra;
        }
        else
        {
            return false;
        }

        return true;
    }

    const char* GetToneMapSelectionName(ToneMapSelection selection)
    {
        switch (selection)
        {
            case ToneMapSelection::Default: return "default";
            case ToneMapSelection::None: return "none";
            case ToneMapSelection::Reinhard: return "reinhard";
            case ToneMapSelection::ReinhardExtended: return "reinhard-extended";
            case ToneMapSelection::ACES: return "aces";
            case ToneMapSelection::Uncharted2: return "uncharted2";
            case ToneMapSelection::Neutral: return "neutral";
        }

        return "default";
    }

    bool ParseToneMapSelection(const std::string& text, ToneMapSelection& outSelection)
    {
        const std::string value = ToLower(text);
        if (value == "default")
        {
            outSelection = ToneMapSelection::Default;
        }
        else if (value == "none")
        {
            outSelection = ToneMapSelection::None;
        }
        else if (value == "reinhard")
        {
            outSelection = ToneMapSelection::Reinhard;
        }
        else if (value == "reinhard-extended")
        {
            outSelection = ToneMapSelection::ReinhardExtended;
        }
        else if (value == "aces")
        {
            outSelection = ToneMapSelection::ACES;
        }
        else if (value == "uncharted2")
        {
            outSelection = ToneMapSelection::Uncharted2;
        }
        else if (value == "neutral")
        {
            outSelection = ToneMapSelection::Neutral;
        }
        else
        {
            return false;
        }

        return true;
    }

    bool TryGetToneMappingOperator(
        ToneMapSelection selection,
        RenderToneMappingOperator& outOperator)
    {
        switch (selection)
        {
            case ToneMapSelection::None:
                outOperator = RenderToneMappingOperator::None;
                return true;
            case ToneMapSelection::Reinhard:
                outOperator = RenderToneMappingOperator::Reinhard;
                return true;
            case ToneMapSelection::ReinhardExtended:
                outOperator = RenderToneMappingOperator::ReinhardExtended;
                return true;
            case ToneMapSelection::ACES:
                outOperator = RenderToneMappingOperator::ACES;
                return true;
            case ToneMapSelection::Uncharted2:
                outOperator = RenderToneMappingOperator::Uncharted2;
                return true;
            case ToneMapSelection::Neutral:
                outOperator = RenderToneMappingOperator::Neutral;
                return true;
            case ToneMapSelection::Default:
                break;
        }

        return false;
    }

    RenderShadowSettings MakeShadowQualityConfig(ShadowQualityPreset preset)
    {
        if (preset == ShadowQualityPreset::Default)
        {
            return RenderShadowSettings{};
        }

        RenderShadowSettings config{};
        switch (preset)
        {
            case ShadowQualityPreset::Low:
                config.atlasResolution = 1024;
                config.cascadeCount = 2;
                config.cascadeSplitLambda = 0.85f;
                config.filterRadiusTexels = 0.75f;
                config.shadowBias = 0.0050f;
                config.normalBias = 0.0200f;
                config.cascadeBlendRatio = 0.04f;
                break;
            case ShadowQualityPreset::Medium:
                config.atlasResolution = 2048;
                config.cascadeCount = 3;
                config.cascadeSplitLambda = 0.90f;
                config.filterRadiusTexels = 1.00f;
                config.shadowBias = 0.0040f;
                config.normalBias = 0.0200f;
                config.cascadeBlendRatio = 0.05f;
                break;
            case ShadowQualityPreset::High:
                config.atlasResolution = 4096;
                config.cascadeCount = 4;
                config.cascadeSplitLambda = 0.95f;
                config.filterRadiusTexels = 1.50f;
                config.shadowBias = 0.0030f;
                config.normalBias = 0.0250f;
                config.cascadeBlendRatio = 0.06f;
                break;
            case ShadowQualityPreset::Ultra:
                config.atlasResolution = 4096;
                config.cascadeCount = 4;
                config.cascadeSplitLambda = 0.98f;
                config.filterRadiusTexels = 2.00f;
                config.shadowBias = 0.0025f;
                config.normalBias = 0.0300f;
                config.cascadeBlendRatio = 0.08f;
                break;
            case ShadowQualityPreset::Default:
                break;
        }

        return config;
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

    bool ParseUInt64(const char* text, uint64& outValue)
    {
        if (!text)
        {
            return false;
        }

        try
        {
            size_t parsedChars = 0;
            const unsigned long long parsed = std::stoull(text, &parsedChars);
            if (parsedChars != std::strlen(text) || parsed == 0)
            {
                return false;
            }
            outValue = static_cast<uint64>(parsed);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ParseFloat(const char* text, float& outValue)
    {
        if (!text)
        {
            return false;
        }

        try
        {
            size_t parsedChars = 0;
            const float parsed = std::stof(text, &parsedChars);
            if (parsedChars != std::strlen(text) || !std::isfinite(parsed))
            {
                return false;
            }

            outValue = parsed;
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
            else if (arg == "--hdri")
            {
                const char* value = requireValue("--hdri");
                if (!value) return false;
                options.hdriPath = value;
            }
            else if (arg == "--no-ibl")
            {
                options.enableProceduralIBL = false;
            }
            else if (arg == "--material-test-scene")
            {
                options.materialTestScene = true;
            }
            else if (arg == "--shadow-test-scene")
            {
                options.shadowTestScene = true;
            }
            else if (arg == "--ray-traced-reflections")
            {
                options.enableRayTracedReflections = true;
            }
            else if (arg == "--no-rt-reflection-denoise")
            {
                options.enableRayTracedReflectionDenoise = false;
            }
            else if (arg == "--rt-max-rays")
            {
                const char* value = requireValue("--rt-max-rays");
                uint64 parsed = 0;
                if (!ParseUInt64(value, parsed))
                {
                    RVX_CORE_ERROR("Invalid --rt-max-rays value: {} (expected positive integer)", value ? value : "");
                    return false;
                }
                options.rayTracingMaxRayCount = parsed;
            }
            else if (arg == "--rt-max-denoise-taps")
            {
                const char* value = requireValue("--rt-max-denoise-taps");
                uint64 parsed = 0;
                if (!ParseUInt64(value, parsed))
                {
                    RVX_CORE_ERROR("Invalid --rt-max-denoise-taps value: {} (expected positive integer)", value ? value : "");
                    return false;
                }
                options.rayTracingMaxDenoiseTapCount = parsed;
            }
            else if (arg == "--rt-max-resource-bytes")
            {
                const char* value = requireValue("--rt-max-resource-bytes");
                uint64 parsed = 0;
                if (!ParseUInt64(value, parsed))
                {
                    RVX_CORE_ERROR("Invalid --rt-max-resource-bytes value: {} (expected positive integer)", value ? value : "");
                    return false;
                }
                options.rayTracingMaxResourceBytes = parsed;
            }
            else if (arg == "--rt-max-gpu-ms")
            {
                const char* value = requireValue("--rt-max-gpu-ms");
                float parsed = 0.0f;
                if (!ParseFloat(value, parsed) || parsed <= 0.0f || parsed > 1000.0f)
                {
                    RVX_CORE_ERROR("Invalid --rt-max-gpu-ms value: {} (expected finite range (0.0, 1000.0])",
                                   value ? value : "");
                    return false;
                }
                options.rayTracingMaxGpuMs = parsed;
            }
            else if (arg == "--rt-max-shadow-gpu-ms")
            {
                const char* value = requireValue("--rt-max-shadow-gpu-ms");
                float parsed = 0.0f;
                if (!ParseFloat(value, parsed) || parsed <= 0.0f || parsed > 1000.0f)
                {
                    RVX_CORE_ERROR("Invalid --rt-max-shadow-gpu-ms value: {} (expected finite range (0.0, 1000.0])",
                                   value ? value : "");
                    return false;
                }
                options.rayTracingMaxShadowGpuMs = parsed;
            }
            else if (arg == "--rt-max-reflection-gpu-ms")
            {
                const char* value = requireValue("--rt-max-reflection-gpu-ms");
                float parsed = 0.0f;
                if (!ParseFloat(value, parsed) || parsed <= 0.0f || parsed > 1000.0f)
                {
                    RVX_CORE_ERROR("Invalid --rt-max-reflection-gpu-ms value: {} (expected finite range (0.0, 1000.0])",
                                   value ? value : "");
                    return false;
                }
                options.rayTracingMaxReflectionGpuMs = parsed;
            }
            else if (arg == "--rt-recovery-gpu-ms")
            {
                const char* value = requireValue("--rt-recovery-gpu-ms");
                float parsed = 0.0f;
                if (!ParseFloat(value, parsed) || parsed <= 0.0f || parsed > 1000.0f)
                {
                    RVX_CORE_ERROR("Invalid --rt-recovery-gpu-ms value: {} (expected finite range (0.0, 1000.0])",
                                   value ? value : "");
                    return false;
                }
                options.rayTracingRecoveryMaxGpuMs = parsed;
            }
            else if (arg == "--rt-recovery-shadow-gpu-ms")
            {
                const char* value = requireValue("--rt-recovery-shadow-gpu-ms");
                float parsed = 0.0f;
                if (!ParseFloat(value, parsed) || parsed <= 0.0f || parsed > 1000.0f)
                {
                    RVX_CORE_ERROR("Invalid --rt-recovery-shadow-gpu-ms value: {} (expected finite range (0.0, 1000.0])",
                                   value ? value : "");
                    return false;
                }
                options.rayTracingRecoveryMaxShadowGpuMs = parsed;
            }
            else if (arg == "--rt-recovery-reflection-gpu-ms")
            {
                const char* value = requireValue("--rt-recovery-reflection-gpu-ms");
                float parsed = 0.0f;
                if (!ParseFloat(value, parsed) || parsed <= 0.0f || parsed > 1000.0f)
                {
                    RVX_CORE_ERROR("Invalid --rt-recovery-reflection-gpu-ms value: {} (expected finite range (0.0, 1000.0])",
                                   value ? value : "");
                    return false;
                }
                options.rayTracingRecoveryMaxReflectionGpuMs = parsed;
            }
            else if (arg == "--rt-gpu-adjust-frames")
            {
                const char* value = requireValue("--rt-gpu-adjust-frames");
                uint32 parsed = 0;
                if (!ParseUInt(value, parsed) || parsed > 120u)
                {
                    RVX_CORE_ERROR("Invalid --rt-gpu-adjust-frames value: {} (expected integer range [1, 120])",
                                   value ? value : "");
                    return false;
                }
                options.rayTracingGpuAdjustmentFrameCount = parsed;
            }
            else if (arg == "--rt-reset-history-frame")
            {
                const char* value = requireValue("--rt-reset-history-frame");
                uint32 parsed = 0;
                if (!ParseUInt(value, parsed) || parsed > 120u)
                {
                    RVX_CORE_ERROR("Invalid --rt-reset-history-frame value: {} (expected integer range [1, 120])",
                                   value ? value : "");
                    return false;
                }
                options.rayTracingHistoryResetFrame = parsed;
            }
            else if (arg == "--rt-resize-frame")
            {
                const char* value = requireValue("--rt-resize-frame");
                uint32 parsed = 0;
                if (!ParseUInt(value, parsed) || parsed > 120u)
                {
                    RVX_CORE_ERROR("Invalid --rt-resize-frame value: {} (expected integer range [1, 120])",
                                   value ? value : "");
                    return false;
                }
                options.rayTracingResizeFrame = parsed;
            }
            else if (arg == "--rt-resize-width")
            {
                const char* value = requireValue("--rt-resize-width");
                uint32 parsed = 0;
                if (!ParseUInt(value, parsed) || parsed == 0u || parsed > 8192u)
                {
                    RVX_CORE_ERROR("Invalid --rt-resize-width value: {} (expected integer range [1, 8192])",
                                   value ? value : "");
                    return false;
                }
                options.rayTracingResizeWidth = parsed;
            }
            else if (arg == "--rt-resize-height")
            {
                const char* value = requireValue("--rt-resize-height");
                uint32 parsed = 0;
                if (!ParseUInt(value, parsed) || parsed == 0u || parsed > 8192u)
                {
                    RVX_CORE_ERROR("Invalid --rt-resize-height value: {} (expected integer range [1, 8192])",
                                   value ? value : "");
                    return false;
                }
                options.rayTracingResizeHeight = parsed;
            }
            else if (arg == "--shadow-quality")
            {
                const char* value = requireValue("--shadow-quality");
                if (!value || !ParseShadowQualityPreset(value, options.shadowQualityPreset))
                {
                    RVX_CORE_ERROR("Invalid --shadow-quality value: {}", value ? value : "");
                    return false;
                }
            }
            else if (arg == "--tonemap")
            {
                const char* value = requireValue("--tonemap");
                if (!value || !ParseToneMapSelection(value, options.tonemapSelection))
                {
                    RVX_CORE_ERROR("Invalid --tonemap value: {}", value ? value : "");
                    return false;
                }
            }
            else if (arg == "--post-exposure")
            {
                const char* value = requireValue("--post-exposure");
                float parsed = 0.0f;
                if (!ParseFloat(value, parsed) || parsed < 0.0f || parsed > 64.0f)
                {
                    RVX_CORE_ERROR("Invalid --post-exposure value: {} (expected finite range [0.0, 64.0])",
                                   value ? value : "");
                    return false;
                }
                options.postExposure = parsed;
                options.postExposureSet = true;
            }
            else if (arg == "--camera-ev100")
            {
                const char* value = requireValue("--camera-ev100");
                float parsed = 0.0f;
                if (!ParseFloat(value, parsed) || parsed < -16.0f || parsed > 32.0f)
                {
                    RVX_CORE_ERROR("Invalid --camera-ev100 value: {} (expected finite range [-16.0, 32.0])",
                                   value ? value : "");
                    return false;
                }
                options.cameraEV100 = parsed;
                options.cameraEV100Set = true;
            }
            else if (arg == "--exposure-compensation")
            {
                const char* value = requireValue("--exposure-compensation");
                float parsed = 0.0f;
                if (!ParseFloat(value, parsed) || parsed < -16.0f || parsed > 16.0f)
                {
                    RVX_CORE_ERROR("Invalid --exposure-compensation value: {} (expected finite range [-16.0, 16.0])",
                                   value ? value : "");
                    return false;
                }
                options.exposureCompensationEV = parsed;
                options.exposureCompensationSet = true;
            }
            else if (arg == "--display-gamma")
            {
                const char* value = requireValue("--display-gamma");
                float parsed = 0.0f;
                if (!ParseFloat(value, parsed) || parsed < 0.1f || parsed > 10.0f)
                {
                    RVX_CORE_ERROR("Invalid --display-gamma value: {} (expected finite range [0.1, 10.0])",
                                   value ? value : "");
                    return false;
                }
                options.displayGamma = parsed;
                options.displayGammaSet = true;
            }
            else if (arg == "--bloom-intensity")
            {
                const char* value = requireValue("--bloom-intensity");
                float parsed = 0.0f;
                if (!ParseFloat(value, parsed) || parsed < 0.0f || parsed > 16.0f)
                {
                    RVX_CORE_ERROR("Invalid --bloom-intensity value: {} (expected finite range [0.0, 16.0])",
                                   value ? value : "");
                    return false;
                }
                options.bloomIntensity = parsed;
                options.bloomIntensitySet = true;
            }
            else if (arg == "--bloom-threshold")
            {
                const char* value = requireValue("--bloom-threshold");
                float parsed = 0.0f;
                if (!ParseFloat(value, parsed) || parsed < 0.0f || parsed > 64.0f)
                {
                    RVX_CORE_ERROR("Invalid --bloom-threshold value: {} (expected finite range [0.0, 64.0])",
                                   value ? value : "");
                    return false;
                }
                options.bloomThreshold = parsed;
                options.bloomThresholdSet = true;
            }
            else if (arg == "--bloom-radius")
            {
                const char* value = requireValue("--bloom-radius");
                float parsed = 0.0f;
                if (!ParseFloat(value, parsed) || parsed < 0.0f || parsed > 16.0f)
                {
                    RVX_CORE_ERROR("Invalid --bloom-radius value: {} (expected finite range [0.0, 16.0])",
                                   value ? value : "");
                    return false;
                }
                options.bloomRadius = parsed;
                options.bloomRadiusSet = true;
            }
            else if (arg == "--expect-ibl-ready")
            {
                options.expectIBLReady = true;
            }
            else if (arg == "--expect-skybox-ready")
            {
                options.expectSkyboxReady = true;
            }
            else if (arg == "--expect-shadow-ready")
            {
                options.expectShadowReady = true;
            }
            else if (arg == "--expect-ray-traced-shadow-ready")
            {
                options.expectRayTracedShadowReady = true;
            }
            else if (arg == "--expect-ray-traced-shadow-alpha-ready")
            {
                options.expectRayTracedShadowAlphaReady = true;
            }
            else if (arg == "--expect-ray-traced-shadow-history-ready")
            {
                options.expectRayTracedShadowHistoryReady = true;
            }
            else if (arg == "--expect-ray-traced-shadow-history-reset-ready")
            {
                options.expectRayTracedShadowHistoryResetReady = true;
            }
            else if (arg == "--expect-ray-traced-shadow-history-resize-ready")
            {
                options.expectRayTracedShadowHistoryResizeReady = true;
            }
            else if (arg == "--expect-ray-traced-reflection-ready")
            {
                options.expectRayTracedReflectionReady = true;
            }
            else if (arg == "--expect-ray-traced-reflection-asset-ready")
            {
                options.expectRayTracedReflectionAssetReady = true;
            }
            else if (arg == "--expect-ray-traced-reflection-history-ready")
            {
                options.expectRayTracedReflectionHistoryReady = true;
            }
            else if (arg == "--expect-ray-traced-reflection-history-reset-ready")
            {
                options.expectRayTracedReflectionHistoryResetReady = true;
            }
            else if (arg == "--expect-ray-traced-reflection-history-resize-ready")
            {
                options.expectRayTracedReflectionHistoryResizeReady = true;
            }
            else if (arg == "--expect-ray-tracing-budget-applied")
            {
                options.expectRayTracingBudgetApplied = true;
            }
            else if (arg == "--expect-ray-tracing-ray-budget-respected")
            {
                options.expectRayTracingRayBudgetRespected = true;
            }
            else if (arg == "--expect-ray-tracing-resource-budget-exceeded")
            {
                options.expectRayTracingResourceBudgetExceeded = true;
            }
            else if (arg == "--expect-ray-tracing-gpu-timing-ready")
            {
                options.expectRayTracingGpuTimingReady = true;
            }
            else if (arg == "--expect-ray-tracing-gpu-budget-applied")
            {
                options.expectRayTracingGpuBudgetApplied = true;
            }
            else if (arg == "--expect-ray-tracing-gpu-budget-recovered")
            {
                options.expectRayTracingGpuBudgetRecovered = true;
            }
            else if (arg == "--expect-material-ready")
            {
                options.expectMaterialReady = true;
            }
            else if (arg == "--expect-procedural-ibl-quality")
            {
                options.expectProceduralIBLQuality = true;
            }
            else if (arg == "--expect-gpu-driven-culling-ready")
            {
                options.expectGPUDrivenCullingReady = true;
            }
            else if (arg == "--expect-particles-ready")
            {
                options.expectParticlesReady = true;
            }
            else if (arg == "--gpu-driven-culling-test-scene")
            {
                options.gpuDrivenCullingTestScene = true;
            }
            else if (arg == "--disable-gpu-driven-culling")
            {
                options.disableGPUDrivenCulling = true;
            }
            else if (arg == "--particle-test-scene")
            {
                options.particleTestScene = true;
            }
            else if (arg == "--validation")
            {
                options.enableValidation = true;
            }
            else if (arg == "--gpu-validation")
            {
                options.enableGPUValidation = true;
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

        if (!options.hdriPath.empty() && !options.enableProceduralIBL)
        {
            RVX_CORE_ERROR("--hdri cannot be combined with --no-ibl");
            return false;
        }

        if (options.expectProceduralIBLQuality && !options.smoke)
        {
            RVX_CORE_ERROR("--expect-procedural-ibl-quality requires --smoke");
            return false;
        }

        if (options.expectGPUDrivenCullingReady && !options.smoke)
        {
            RVX_CORE_ERROR("--expect-gpu-driven-culling-ready requires --smoke");
            return false;
        }

        if (options.expectParticlesReady && !options.smoke)
        {
            RVX_CORE_ERROR("--expect-particles-ready requires --smoke");
            return false;
        }

        if (options.expectParticlesReady && !options.particleTestScene)
        {
            RVX_CORE_ERROR("--expect-particles-ready requires --particle-test-scene");
            return false;
        }

        if (options.gpuDrivenCullingTestScene && !options.smoke)
        {
            RVX_CORE_ERROR("--gpu-driven-culling-test-scene requires --smoke");
            return false;
        }

        if (options.particleTestScene && !options.smoke)
        {
            RVX_CORE_ERROR("--particle-test-scene requires --smoke");
            return false;
        }

        if (options.disableGPUDrivenCulling && !options.smoke)
        {
            RVX_CORE_ERROR("--disable-gpu-driven-culling requires --smoke");
            return false;
        }

        if (options.enableGPUValidation && options.backend != RHIBackendType::DX12)
        {
            RVX_CORE_ERROR("--gpu-validation currently requires --backend dx12");
            return false;
        }

        if (options.enableGPUValidation && !options.enableValidation)
        {
            RVX_CORE_ERROR("--gpu-validation requires --validation");
            return false;
        }

        if (options.expectGPUDrivenCullingReady && options.disableGPUDrivenCulling)
        {
            RVX_CORE_ERROR("--expect-gpu-driven-culling-ready cannot be combined with --disable-gpu-driven-culling");
            return false;
        }

        if (options.expectShadowReady && !options.smoke)
        {
            RVX_CORE_ERROR("--expect-shadow-ready requires --smoke");
            return false;
        }

        if (options.expectShadowReady && !options.shadowTestScene)
        {
            RVX_CORE_ERROR("--expect-shadow-ready requires --shadow-test-scene");
            return false;
        }

        if (options.expectRayTracedShadowReady && !options.smoke)
        {
            RVX_CORE_ERROR("--expect-ray-traced-shadow-ready requires --smoke");
            return false;
        }

        if (options.expectRayTracedShadowReady && !options.shadowTestScene)
        {
            RVX_CORE_ERROR("--expect-ray-traced-shadow-ready requires --shadow-test-scene");
            return false;
        }

        if (options.expectRayTracedShadowAlphaReady && !options.smoke)
        {
            RVX_CORE_ERROR("--expect-ray-traced-shadow-alpha-ready requires --smoke");
            return false;
        }

        if (options.expectRayTracedShadowAlphaReady && !options.shadowTestScene)
        {
            RVX_CORE_ERROR("--expect-ray-traced-shadow-alpha-ready requires --shadow-test-scene");
            return false;
        }

        if (options.expectRayTracedShadowHistoryReady && !options.smoke)
        {
            RVX_CORE_ERROR("--expect-ray-traced-shadow-history-ready requires --smoke");
            return false;
        }

        if (options.expectRayTracedShadowHistoryReady && !options.shadowTestScene)
        {
            RVX_CORE_ERROR("--expect-ray-traced-shadow-history-ready requires --shadow-test-scene");
            return false;
        }

        if (options.expectRayTracedShadowHistoryResetReady && !options.expectRayTracedShadowHistoryReady)
        {
            RVX_CORE_ERROR("--expect-ray-traced-shadow-history-reset-ready requires "
                           "--expect-ray-traced-shadow-history-ready");
            return false;
        }

        if (options.expectRayTracedShadowHistoryResizeReady && !options.expectRayTracedShadowHistoryReady)
        {
            RVX_CORE_ERROR("--expect-ray-traced-shadow-history-resize-ready requires "
                           "--expect-ray-traced-shadow-history-ready");
            return false;
        }

        if (options.expectRayTracedReflectionReady && !options.smoke)
        {
            RVX_CORE_ERROR("--expect-ray-traced-reflection-ready requires --smoke");
            return false;
        }

        if (options.expectRayTracedReflectionReady && !options.enableRayTracedReflections)
        {
            RVX_CORE_ERROR("--expect-ray-traced-reflection-ready requires --ray-traced-reflections");
            return false;
        }

        if (options.expectRayTracedReflectionAssetReady && !options.smoke)
        {
            RVX_CORE_ERROR("--expect-ray-traced-reflection-asset-ready requires --smoke");
            return false;
        }

        if (options.expectRayTracedReflectionAssetReady && !options.enableRayTracedReflections)
        {
            RVX_CORE_ERROR("--expect-ray-traced-reflection-asset-ready requires --ray-traced-reflections");
            return false;
        }

        if (options.expectRayTracedReflectionHistoryReady && !options.smoke)
        {
            RVX_CORE_ERROR("--expect-ray-traced-reflection-history-ready requires --smoke");
            return false;
        }

        if (options.expectRayTracedReflectionHistoryReady && !options.enableRayTracedReflections)
        {
            RVX_CORE_ERROR("--expect-ray-traced-reflection-history-ready requires --ray-traced-reflections");
            return false;
        }

        if (options.expectRayTracedReflectionHistoryResetReady && !options.expectRayTracedReflectionHistoryReady)
        {
            RVX_CORE_ERROR("--expect-ray-traced-reflection-history-reset-ready requires "
                           "--expect-ray-traced-reflection-history-ready");
            return false;
        }

        if (options.expectRayTracedReflectionHistoryResizeReady && !options.expectRayTracedReflectionHistoryReady)
        {
            RVX_CORE_ERROR("--expect-ray-traced-reflection-history-resize-ready requires "
                           "--expect-ray-traced-reflection-history-ready");
            return false;
        }

        const bool rayTracingHistoryResetRequested = options.expectRayTracedShadowHistoryResetReady ||
                                                     options.expectRayTracedReflectionHistoryResetReady;
        const bool rayTracingHistoryResizeRequested = options.expectRayTracedShadowHistoryResizeReady ||
                                                      options.expectRayTracedReflectionHistoryResizeReady;
        if (rayTracingHistoryResetRequested && options.rayTracingHistoryResetFrame == 0u)
        {
            RVX_CORE_ERROR("RT history reset readiness gates require --rt-reset-history-frame");
            return false;
        }

        if (rayTracingHistoryResetRequested && options.rayTracingHistoryResetFrame < 4u)
        {
            RVX_CORE_ERROR("RT history reset readiness gates require --rt-reset-history-frame >= 4");
            return false;
        }

        if (rayTracingHistoryResizeRequested && options.rayTracingResizeFrame == 0u)
        {
            RVX_CORE_ERROR("RT history resize readiness gates require --rt-resize-frame");
            return false;
        }

        if (rayTracingHistoryResizeRequested &&
            (options.rayTracingResizeWidth == 0u || options.rayTracingResizeHeight == 0u))
        {
            RVX_CORE_ERROR("RT history resize readiness gates require --rt-resize-width and --rt-resize-height");
            return false;
        }

        if (rayTracingHistoryResizeRequested && options.rayTracingResizeFrame < 4u)
        {
            RVX_CORE_ERROR("RT history resize readiness gates require --rt-resize-frame >= 4");
            return false;
        }

        const bool rayTracingGpuBudgetConfigured = options.rayTracingMaxGpuMs > 0.0f ||
                                                   options.rayTracingMaxShadowGpuMs > 0.0f ||
                                                   options.rayTracingMaxReflectionGpuMs > 0.0f;
        const bool rayTracingGpuRecoveryBudgetConfigured = options.rayTracingRecoveryMaxGpuMs > 0.0f ||
                                                           options.rayTracingRecoveryMaxShadowGpuMs > 0.0f ||
                                                           options.rayTracingRecoveryMaxReflectionGpuMs > 0.0f;
        const bool rayTracingQualityBudgetConfigured = options.rayTracingMaxRayCount > 0 ||
                                                       options.rayTracingMaxDenoiseTapCount > 0 ||
                                                       rayTracingGpuBudgetConfigured;
        if (options.expectRayTracingBudgetApplied && !options.smoke)
        {
            RVX_CORE_ERROR("--expect-ray-tracing-budget-applied requires --smoke");
            return false;
        }

        if (options.expectRayTracingBudgetApplied && !rayTracingQualityBudgetConfigured)
        {
            RVX_CORE_ERROR("--expect-ray-tracing-budget-applied requires --rt-max-rays, "
                           "--rt-max-denoise-taps, or an RT GPU budget");
            return false;
        }

        if (options.expectRayTracingBudgetApplied && !options.enableRayTracedReflections)
        {
            RVX_CORE_ERROR("--expect-ray-tracing-budget-applied requires --ray-traced-reflections");
            return false;
        }

        if (options.expectRayTracingRayBudgetRespected && !options.smoke)
        {
            RVX_CORE_ERROR("--expect-ray-tracing-ray-budget-respected requires --smoke");
            return false;
        }

        if (options.expectRayTracingRayBudgetRespected && options.rayTracingMaxRayCount == 0)
        {
            RVX_CORE_ERROR("--expect-ray-tracing-ray-budget-respected requires --rt-max-rays");
            return false;
        }

        if (options.expectRayTracingRayBudgetRespected && !options.enableRayTracedReflections)
        {
            RVX_CORE_ERROR("--expect-ray-tracing-ray-budget-respected requires --ray-traced-reflections");
            return false;
        }

        if (options.expectRayTracingResourceBudgetExceeded && !options.smoke)
        {
            RVX_CORE_ERROR("--expect-ray-tracing-resource-budget-exceeded requires --smoke");
            return false;
        }

        if (options.expectRayTracingResourceBudgetExceeded && options.rayTracingMaxResourceBytes == 0)
        {
            RVX_CORE_ERROR("--expect-ray-tracing-resource-budget-exceeded requires --rt-max-resource-bytes");
            return false;
        }

        if (options.expectRayTracingResourceBudgetExceeded &&
            !options.expectRayTracedShadowReady &&
            !options.enableRayTracedReflections)
        {
            RVX_CORE_ERROR("--expect-ray-tracing-resource-budget-exceeded requires an RT shadow or reflection smoke path");
            return false;
        }

        if (options.expectRayTracingGpuTimingReady && !options.smoke)
        {
            RVX_CORE_ERROR("--expect-ray-tracing-gpu-timing-ready requires --smoke");
            return false;
        }

        if (options.expectRayTracingGpuTimingReady &&
            !options.expectRayTracedShadowReady &&
            !options.enableRayTracedReflections)
        {
            RVX_CORE_ERROR("--expect-ray-tracing-gpu-timing-ready requires an RT shadow or reflection smoke path");
            return false;
        }

        if (options.expectRayTracingGpuBudgetApplied && !options.smoke)
        {
            RVX_CORE_ERROR("--expect-ray-tracing-gpu-budget-applied requires --smoke");
            return false;
        }

        if (options.expectRayTracingGpuBudgetApplied && !rayTracingGpuBudgetConfigured)
        {
            RVX_CORE_ERROR("--expect-ray-tracing-gpu-budget-applied requires --rt-max-gpu-ms, "
                           "--rt-max-shadow-gpu-ms, or --rt-max-reflection-gpu-ms");
            return false;
        }

        if (options.expectRayTracingGpuBudgetApplied &&
            !options.expectRayTracedShadowReady &&
            !options.enableRayTracedReflections)
        {
            RVX_CORE_ERROR("--expect-ray-tracing-gpu-budget-applied requires an RT shadow or reflection smoke path");
            return false;
        }

        if (options.expectRayTracingGpuBudgetRecovered && !options.smoke)
        {
            RVX_CORE_ERROR("--expect-ray-tracing-gpu-budget-recovered requires --smoke");
            return false;
        }

        if (options.expectRayTracingGpuBudgetRecovered && !rayTracingGpuBudgetConfigured)
        {
            RVX_CORE_ERROR("--expect-ray-tracing-gpu-budget-recovered requires an initial measured RT GPU budget");
            return false;
        }

        if (options.expectRayTracingGpuBudgetRecovered && !rayTracingGpuRecoveryBudgetConfigured)
        {
            RVX_CORE_ERROR("--expect-ray-tracing-gpu-budget-recovered requires --rt-recovery-gpu-ms, "
                           "--rt-recovery-shadow-gpu-ms, or --rt-recovery-reflection-gpu-ms");
            return false;
        }

        if (options.expectRayTracingGpuBudgetRecovered && !options.enableRayTracedReflections)
        {
            RVX_CORE_ERROR("--expect-ray-tracing-gpu-budget-recovered currently requires --ray-traced-reflections");
            return false;
        }

        if (options.expectMaterialReady && !options.smoke)
        {
            RVX_CORE_ERROR("--expect-material-ready requires --smoke");
            return false;
        }

        if (options.expectMaterialReady && !options.materialTestScene)
        {
            RVX_CORE_ERROR("--expect-material-ready requires --material-test-scene");
            return false;
        }

        if (options.materialTestScene && options.shadowTestScene)
        {
            RVX_CORE_ERROR("--material-test-scene cannot be combined with --shadow-test-scene");
            return false;
        }

        if (options.gpuDrivenCullingTestScene && (options.materialTestScene || options.shadowTestScene ||
                                                  options.particleTestScene))
        {
            RVX_CORE_ERROR("--gpu-driven-culling-test-scene cannot be combined with material, shadow, or particle test scenes");
            return false;
        }

        if (options.particleTestScene && (options.materialTestScene || options.shadowTestScene))
        {
            RVX_CORE_ERROR("--particle-test-scene cannot be combined with material or shadow test scenes");
            return false;
        }

        if (options.cameraEV100Set && options.postExposureSet)
        {
            RVX_CORE_ERROR("--camera-ev100 cannot be combined with --post-exposure");
            return false;
        }

        if (options.exposureCompensationSet && !options.cameraEV100Set)
        {
            RVX_CORE_ERROR("--exposure-compensation requires --camera-ev100");
            return false;
        }

        if (options.expectProceduralIBLQuality && (!options.enableProceduralIBL || !options.hdriPath.empty()))
        {
            RVX_CORE_ERROR("--expect-procedural-ibl-quality requires the default procedural IBL path");
            return false;
        }

        if (options.smoke)
        {
            if (!options.backendSet)
            {
                const bool rayTracingSmokeRequested = options.enableRayTracedReflections ||
                                                      options.expectRayTracedShadowReady ||
                                                      options.expectRayTracedShadowAlphaReady ||
                                                      options.expectRayTracedShadowHistoryReady ||
                                                      options.expectRayTracedShadowHistoryResetReady ||
                                                      options.expectRayTracedShadowHistoryResizeReady ||
                                                      options.expectRayTracedReflectionReady ||
                                                      options.expectRayTracedReflectionAssetReady ||
                                                      options.expectRayTracedReflectionHistoryReady ||
                                                      options.expectRayTracedReflectionHistoryResetReady ||
                                                      options.expectRayTracedReflectionHistoryResizeReady ||
                                                      options.expectRayTracingResourceBudgetExceeded ||
                                                      options.expectRayTracingGpuTimingReady ||
                                                      options.expectRayTracingGpuBudgetApplied ||
                                                      options.expectRayTracingGpuBudgetRecovered;
                const bool dx12SmokeRequested = rayTracingSmokeRequested ||
                                                options.expectGPUDrivenCullingReady ||
                                                options.gpuDrivenCullingTestScene ||
                                                options.disableGPUDrivenCulling;
                options.backend = dx12SmokeRequested ? RHIBackendType::DX12 : RHIBackendType::DX11;
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

        if ((options.expectRayTracedShadowHistoryReady || options.expectRayTracedReflectionHistoryReady) &&
            options.frames < 3u)
        {
            RVX_CORE_ERROR("RT history readiness gates require at least 3 smoke frames");
            return false;
        }

        if (rayTracingHistoryResetRequested && options.frames <= options.rayTracingHistoryResetFrame)
        {
            RVX_CORE_ERROR("RT history reset readiness gates require at least one frame after "
                           "--rt-reset-history-frame");
            return false;
        }

        if (rayTracingHistoryResizeRequested && options.frames <= options.rayTracingResizeFrame)
        {
            RVX_CORE_ERROR("RT history resize readiness gates require at least one frame after --rt-resize-frame");
            return false;
        }

        if (rayTracingHistoryResizeRequested &&
            options.rayTracingResizeWidth == options.width &&
            options.rayTracingResizeHeight == options.height)
        {
            RVX_CORE_ERROR("RT history resize readiness gates require a resize target different from --width/--height");
            return false;
        }

        if (options.expectRayTracingGpuBudgetApplied && options.frames < 6u)
        {
            RVX_CORE_ERROR("--expect-ray-tracing-gpu-budget-applied requires at least 6 smoke frames");
            return false;
        }

        if (options.expectRayTracingGpuBudgetRecovered && options.frames < 8u)
        {
            RVX_CORE_ERROR("--expect-ray-tracing-gpu-budget-recovered requires at least 8 smoke frames");
            return false;
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

    Resource::ResourceHandle<Resource::TextureResource> CreateTextureResource(
        Resource::ResourceId id,
        const std::string& name,
        std::vector<uint8> pixels,
        const Resource::TextureMetadata& metadata)
    {
        auto* texture = new Resource::TextureResource();
        texture->SetId(id);
        texture->SetName(name);
        texture->SetData(std::move(pixels), metadata);
        return Resource::ResourceHandle<Resource::TextureResource>(texture);
    }

    constexpr Resource::ResourceId kProceduralIrradianceId = 0x4D5649424C495201ull;
    constexpr Resource::ResourceId kProceduralPrefilteredId = 0x4D5649424C505201ull;
    constexpr Resource::ResourceId kProceduralBRDFLUTId = 0x4D56494252444601ull;

    struct ProceduralIBLQualityPreset
    {
        uint32 equirectWidth = 16;
        uint32 equirectHeight = 8;
        uint32 environmentResolution = 8;
        uint32 irradianceResolution = 4;
        uint32 prefilteredResolution = 8;
        uint32 prefilteredMipLevels = 4;
        uint32 brdfLUTResolution = 16;
        uint32 convolutionSamples = 16;
    };

    ProceduralIBLQualityPreset GetProceduralIBLPreset(bool smoke)
    {
        if (smoke)
        {
            return {};
        }

        ProceduralIBLQualityPreset preset;
        preset.equirectWidth = 64;
        preset.equirectHeight = 32;
        preset.environmentResolution = 32;
        preset.irradianceResolution = 16;
        preset.prefilteredResolution = 32;
        preset.prefilteredMipLevels = 5;
        preset.brdfLUTResolution = 64;
        preset.convolutionSamples = 64;
        return preset;
    }

    std::vector<float> MakeProceduralEquirectangularHDR(const ProceduralIBLQualityPreset& preset)
    {
        const Vec3 sunDirection = glm::normalize(Vec3{0.35f, 0.65f, 0.45f});
        const Vec3 sunColor{1.0f, 0.94f, 0.82f};
        const Vec3 zenithColor{0.12f, 0.28f, 0.58f};
        const Vec3 horizonColor{0.55f, 0.68f, 0.82f};
        const Vec3 groundColor{0.08f, 0.09f, 0.11f};

        std::vector<float> pixels(static_cast<size_t>(preset.equirectWidth) *
                                  static_cast<size_t>(preset.equirectHeight) * 4u);
        for (uint32 y = 0; y < preset.equirectHeight; ++y)
        {
            const float v = static_cast<float>(y) / static_cast<float>(preset.equirectHeight - 1u);
            const float theta = v * glm::pi<float>();
            for (uint32 x = 0; x < preset.equirectWidth; ++x)
            {
                const float u = static_cast<float>(x) / static_cast<float>(preset.equirectWidth);
                const float phi = u * glm::two_pi<float>();
                const Vec3 direction = glm::normalize(Vec3{
                    std::sin(theta) * std::cos(phi),
                    std::cos(theta),
                    std::sin(theta) * std::sin(phi)});

                const float up = std::clamp(direction.y * 0.5f + 0.5f, 0.0f, 1.0f);
                const float horizon = std::pow(1.0f - std::abs(direction.y), 2.0f);
                Vec3 skyColor = glm::mix(groundColor, zenithColor, up);
                skyColor = glm::mix(skyColor, horizonColor, horizon * 0.65f);

                const float sunDot = std::max(glm::dot(direction, sunDirection), 0.0f);
                const Vec3 sunDisc = sunColor * std::pow(sunDot, 512.0f) * 9.0f;
                const Vec3 sunGlow = sunColor * std::pow(sunDot, 32.0f) * 0.75f;
                const Vec3 color = skyColor + sunDisc + sunGlow;

                const size_t offset = (static_cast<size_t>(y) * preset.equirectWidth + x) * 4u;
                pixels[offset + 0] = color.r;
                pixels[offset + 1] = color.g;
                pixels[offset + 2] = color.b;
                pixels[offset + 3] = 1.0f;
            }
        }

        return pixels;
    }

    std::vector<uint8> PackCubemapFacesRGBA32F(const Resource::CubemapFaces& faces)
    {
        const size_t faceDataSize = static_cast<size_t>(faces.faceSize) * faces.faceSize * 4u * sizeof(float);
        std::vector<uint8> packed(faceDataSize * Resource::CubemapFaces::FACE_COUNT);
        for (int face = 0; face < Resource::CubemapFaces::FACE_COUNT; ++face)
        {
            std::memcpy(packed.data() + static_cast<size_t>(face) * faceDataSize,
                        faces.faces[face].data(),
                        faceDataSize);
        }
        return packed;
    }

    std::vector<uint8> PackCubemapMipChainRGBA32F(const std::vector<Resource::CubemapFaces>& mipChain)
    {
        size_t totalSize = 0;
        for (const Resource::CubemapFaces& mip : mipChain)
        {
            totalSize += static_cast<size_t>(mip.faceSize) * mip.faceSize * 4u * sizeof(float) *
                         Resource::CubemapFaces::FACE_COUNT;
        }

        std::vector<uint8> packed(totalSize);
        size_t offset = 0;
        for (const Resource::CubemapFaces& mip : mipChain)
        {
            const size_t faceDataSize = static_cast<size_t>(mip.faceSize) * mip.faceSize * 4u * sizeof(float);
            for (int face = 0; face < Resource::CubemapFaces::FACE_COUNT; ++face)
            {
                std::memcpy(packed.data() + offset, mip.faces[face].data(), faceDataSize);
                offset += faceDataSize;
            }
        }

        return packed;
    }

    Resource::ResourceHandle<Resource::TextureResource> CreateCubemapResourceRGBA32F(
        Resource::ResourceId id,
        const std::string& name,
        const std::string& path,
        std::vector<uint8> pixels,
        uint32 faceSize,
        uint32 mipLevels)
    {
        Resource::TextureMetadata metadata;
        metadata.width = faceSize;
        metadata.height = faceSize;
        metadata.depth = 1;
        metadata.mipLevels = std::max(1u, mipLevels);
        metadata.arrayLayers = Resource::CubemapFaces::FACE_COUNT;
        metadata.format = Resource::TextureFormat::RGBA32F;
        metadata.isCubemap = true;
        metadata.isArray = false;
        metadata.isSRGB = false;
        metadata.usage = Resource::TextureUsage::Color;

        auto texture = CreateTextureResource(id, name, std::move(pixels), metadata);
        if (texture)
        {
            texture->SetPath(path);
        }
        return texture;
    }

    void AssignTextureIdentity(Resource::TextureResource* texture,
                               Resource::ResourceId id,
                               const std::string& name,
                               const std::string& path)
    {
        if (!texture)
            return;

        texture->SetId(id);
        texture->SetName(name);
        texture->SetPath(path);
    }

    ProceduralIBLResources CreateProceduralIBLResources(bool smoke)
    {
        const ProceduralIBLQualityPreset preset = GetProceduralIBLPreset(smoke);

        Resource::HDRTextureLoader loader(nullptr);
        const std::vector<float> equirect = MakeProceduralEquirectangularHDR(preset);
        const Resource::CubemapFaces environment = loader.EquirectangularToCubemap(
            equirect.data(),
            preset.equirectWidth,
            preset.equirectHeight,
            preset.environmentResolution);
        const Resource::CubemapFaces irradiance = loader.GenerateIrradianceMap(
            environment,
            preset.irradianceResolution,
            preset.convolutionSamples);
        const std::vector<Resource::CubemapFaces> prefiltered = loader.GeneratePrefilteredMap(
            environment,
            preset.prefilteredResolution,
            preset.prefilteredMipLevels,
            preset.convolutionSamples);
        Resource::TextureResource* brdfLUT = loader.GenerateBRDFLUT(
            preset.brdfLUTResolution,
            preset.convolutionSamples);
        AssignTextureIdentity(brdfLUT,
                              kProceduralBRDFLUTId,
                              "ModelViewerProceduralBRDFLUT",
                              "__modelviewer_procedural_brdf_lut__");

        ProceduralIBLResources resources;
        resources.equirectWidth = preset.equirectWidth;
        resources.equirectHeight = preset.equirectHeight;
        resources.environmentResolution = preset.environmentResolution;
        resources.irradianceResolution = preset.irradianceResolution;
        resources.prefilteredResolution = preset.prefilteredResolution;
        resources.prefilteredMipLevels = static_cast<uint32>(prefiltered.size());
        resources.brdfLUTResolution = preset.brdfLUTResolution;
        resources.convolutionSamples = preset.convolutionSamples;
        resources.irradiance = CreateCubemapResourceRGBA32F(kProceduralIrradianceId,
                                                            "ModelViewerProceduralIrradiance",
                                                            "__modelviewer_procedural_irradiance__",
                                                            PackCubemapFacesRGBA32F(irradiance),
                                                            preset.irradianceResolution,
                                                            1);
        resources.prefiltered = CreateCubemapResourceRGBA32F(kProceduralPrefilteredId,
                                                             "ModelViewerProceduralPrefiltered",
                                                             "__modelviewer_procedural_prefiltered__",
                                                             PackCubemapMipChainRGBA32F(prefiltered),
                                                             preset.prefilteredResolution,
                                                             resources.prefilteredMipLevels);
        resources.brdfLUT = Resource::ResourceHandle<Resource::TextureResource>(brdfLUT);

        RVX_CORE_INFO("ModelViewer procedural CPU IBL generated: equirect={}x{}, envCube={}, irradiance={}, "
                      "prefiltered={} mips={}, brdf={}, samples={}",
                      resources.equirectWidth,
                      resources.equirectHeight,
                      resources.environmentResolution,
                      resources.irradianceResolution,
                      resources.prefilteredResolution,
                      resources.prefilteredMipLevels,
                      resources.brdfLUTResolution,
                      resources.convolutionSamples);

        return resources;
    }

    bool PublishProceduralIBL(Resource::ResourceSubsystem* resourceSubsystem,
                              const ProceduralIBLResources& resources)
    {
        if (!resourceSubsystem)
            return false;

        auto publish = [resourceSubsystem](
                           const Resource::ResourceHandle<
                               Resource::TextureResource>& texture) -> bool
        {
            if (!texture)
                return false;
            return resourceSubsystem->PublishRenderResource(
                Resource::ResourceHandle<Resource::IResource>(texture));
        };

        return publish(resources.irradiance) &&
               publish(resources.prefiltered) &&
               publish(resources.brdfLUT);
    }

    bool ValidateProceduralIBLQuality(const ProceduralIBLResources& resources,
                                      bool smoke,
                                      Resource::ResourceSubsystem* resourceSubsystem)
    {
        const ProceduralIBLQualityPreset expected = GetProceduralIBLPreset(smoke);
        bool valid = true;

        auto requireTexture = [&valid](const Resource::TextureHandle& texture,
                                       const char* label) -> Resource::TextureResource*
        {
            if (!texture)
            {
                RVX_CORE_ERROR("ModelViewer procedural IBL quality check failed: {} texture is missing", label);
                valid = false;
                return nullptr;
            }
            return texture.Get();
        };

        auto checkCubemap = [&valid, resourceSubsystem](const Resource::TextureHandle& texture,
                                                   const char* label,
                                                   Resource::ResourceId expectedId,
                                                   const char* expectedName,
                                                   const char* expectedPath,
                                                   uint32 expectedSize,
                                                   uint32 expectedMipLevels)
        {
            Resource::TextureResource* resource = texture.Get();
            if (!resource)
            {
                RVX_CORE_ERROR("ModelViewer procedural IBL quality check failed: {} texture is missing", label);
                valid = false;
                return;
            }

            const Resource::TextureMetadata& metadata = resource->GetMetadata();
            const bool metadataOk =
                resource->GetId() == expectedId &&
                resource->GetName() == expectedName &&
                resource->GetPath() == expectedPath &&
                metadata.format == Resource::TextureFormat::RGBA32F &&
                metadata.width == expectedSize &&
                metadata.height == expectedSize &&
                metadata.mipLevels == expectedMipLevels &&
                metadata.arrayLayers == Resource::CubemapFaces::FACE_COUNT &&
                metadata.isCubemap &&
                !metadata.isArray &&
                !metadata.isSRGB &&
                metadata.usage == Resource::TextureUsage::Color;

            if (!metadataOk)
            {
                RVX_CORE_ERROR("ModelViewer procedural IBL quality check failed: {} metadata mismatch "
                               "(id={}, name='{}', path='{}', format={}, size={}x{}, mips={}, layers={}, "
                               "cubemap={}, sRGB={}, usage={})",
                               label,
                               resource->GetId(),
                               resource->GetName(),
                               resource->GetPath(),
                               static_cast<int>(metadata.format),
                               metadata.width,
                               metadata.height,
                               metadata.mipLevels,
                               metadata.arrayLayers,
                               metadata.isCubemap ? "true" : "false",
                               metadata.isSRGB ? "true" : "false",
                               static_cast<int>(metadata.usage));
                valid = false;
            }

            const RenderResourceResolveResult resolved = resourceSubsystem
                ? resourceSubsystem->ResolveRenderResource(
                      AssetId{resource->GetId()},
                      RenderResourceKind::Texture)
                : RenderResourceResolveResult{};
            if (resolved.code != RenderResourceResolveCode::Resolved ||
                resolved.status.state != RenderResourcePublicState::GPUReady)
            {
                RVX_CORE_ERROR("ModelViewer procedural IBL quality check failed: {} texture is not GPU-ready", label);
                valid = false;
            }
        };

        Resource::TextureResource* brdf = requireTexture(resources.brdfLUT, "BRDF LUT");
        checkCubemap(resources.irradiance,
                     "irradiance",
                     kProceduralIrradianceId,
                     "ModelViewerProceduralIrradiance",
                     "__modelviewer_procedural_irradiance__",
                     expected.irradianceResolution,
                     1);
        checkCubemap(resources.prefiltered,
                     "prefiltered",
                     kProceduralPrefilteredId,
                     "ModelViewerProceduralPrefiltered",
                     "__modelviewer_procedural_prefiltered__",
                     expected.prefilteredResolution,
                     expected.prefilteredMipLevels);

        if (brdf)
        {
            const Resource::TextureMetadata& metadata = brdf->GetMetadata();
            const bool metadataOk =
                brdf->GetId() == kProceduralBRDFLUTId &&
                brdf->GetName() == "ModelViewerProceduralBRDFLUT" &&
                brdf->GetPath() == "__modelviewer_procedural_brdf_lut__" &&
                metadata.format == Resource::TextureFormat::RGBA16F &&
                metadata.width == expected.brdfLUTResolution &&
                metadata.height == expected.brdfLUTResolution &&
                metadata.mipLevels == 1 &&
                metadata.arrayLayers == 1 &&
                !metadata.isCubemap &&
                !metadata.isArray &&
                !metadata.isSRGB &&
                metadata.usage == Resource::TextureUsage::Data;

            if (!metadataOk)
            {
                RVX_CORE_ERROR("ModelViewer procedural IBL quality check failed: BRDF LUT metadata mismatch "
                               "(id={}, name='{}', path='{}', format={}, size={}x{}, mips={}, layers={}, "
                               "cubemap={}, sRGB={}, usage={})",
                               brdf->GetId(),
                               brdf->GetName(),
                               brdf->GetPath(),
                               static_cast<int>(metadata.format),
                               metadata.width,
                               metadata.height,
                               metadata.mipLevels,
                               metadata.arrayLayers,
                               metadata.isCubemap ? "true" : "false",
                               metadata.isSRGB ? "true" : "false",
                               static_cast<int>(metadata.usage));
                valid = false;
            }

            const RenderResourceResolveResult resolved = resourceSubsystem
                ? resourceSubsystem->ResolveRenderResource(
                      AssetId{brdf->GetId()},
                      RenderResourceKind::Texture)
                : RenderResourceResolveResult{};
            if (resolved.code != RenderResourceResolveCode::Resolved ||
                resolved.status.state != RenderResourcePublicState::GPUReady)
            {
                RVX_CORE_ERROR("ModelViewer procedural IBL quality check failed: BRDF LUT texture is not GPU-ready");
                valid = false;
            }
        }

        const bool presetOk =
            resources.equirectWidth == expected.equirectWidth &&
            resources.equirectHeight == expected.equirectHeight &&
            resources.environmentResolution == expected.environmentResolution &&
            resources.irradianceResolution == expected.irradianceResolution &&
            resources.prefilteredResolution == expected.prefilteredResolution &&
            resources.prefilteredMipLevels == expected.prefilteredMipLevels &&
            resources.brdfLUTResolution == expected.brdfLUTResolution &&
            resources.convolutionSamples == expected.convolutionSamples;
        if (!presetOk)
        {
            RVX_CORE_ERROR("ModelViewer procedural IBL quality check failed: preset mismatch "
                           "(equirect={}x{}, env={}, irradiance={}, prefiltered={} mips={}, brdf={}, samples={})",
                           resources.equirectWidth,
                           resources.equirectHeight,
                           resources.environmentResolution,
                           resources.irradianceResolution,
                           resources.prefilteredResolution,
                           resources.prefilteredMipLevels,
                           resources.brdfLUTResolution,
                           resources.convolutionSamples);
            valid = false;
        }

        if (valid)
        {
            RVX_CORE_INFO("ModelViewer procedural IBL quality check passed");
        }
        return valid;
    }

    Resource::HDRLoadOptions MakeHDRILoadOptions(bool smoke)
    {
        Resource::HDRLoadOptions options;
        options.generateCubemap = true;
        options.generateIBL = true;
        options.applyGamma = false;
        options.exposure = 1.0f;

        if (smoke)
        {
            options.cubemapResolution = 4;
            options.irradianceResolution = 1;
            options.prefilteredResolution = 4;
            options.prefilteredMipLevels = 3;
            options.brdfLUTResolution = 4;
            options.convolutionSamples = 8;
        }
        else
        {
            options.cubemapResolution = 64;
            options.irradianceResolution = 8;
            options.prefilteredResolution = 64;
            options.prefilteredMipLevels = 5;
            options.brdfLUTResolution = 64;
            options.convolutionSamples = 64;
        }

        return options;
    }

    HDRIEnvironmentResources LoadHDRIEnvironment(const std::string& path, bool smoke)
    {
        HDRIEnvironmentResources resources;
        if (path.empty())
        {
            return resources;
        }

        if (!std::filesystem::exists(path))
        {
            RVX_CORE_ERROR("ModelViewer HDRI path does not exist: {}", path);
            return resources;
        }

        auto& resourceManager = Resource::ResourceManager::Get();
        Resource::HDRTextureLoader loader(&resourceManager);
        const Resource::HDRLoadOptions loadOptions = MakeHDRILoadOptions(smoke);
        Resource::IBLData ibl = loader.LoadIBL(path, loadOptions);
        if (!ibl.IsValid())
        {
            RVX_CORE_ERROR("ModelViewer failed to generate HDRI environment from {}", path);
            return resources;
        }

        resources.environment = Resource::TextureHandle(ibl.environmentMap);
        resources.irradiance = Resource::TextureHandle(ibl.irradianceMap);
        resources.prefiltered = Resource::TextureHandle(ibl.prefilteredMap);
        resources.brdfLUT = Resource::TextureHandle(ibl.brdfLUT);
        resources.prefilteredMipLevels = ibl.prefilteredMipLevels;

        RVX_CORE_INFO("ModelViewer HDRI generated: env={} irradiance={} prefiltered={} brdf={} prefilteredMips={}",
                      resources.environment ? resources.environment->GetName() : "<missing>",
                      resources.irradiance ? resources.irradiance->GetName() : "<missing>",
                      resources.prefiltered ? resources.prefiltered->GetName() : "<missing>",
                      resources.brdfLUT ? resources.brdfLUT->GetName() : "<missing>",
                      resources.prefilteredMipLevels);

        return resources;
    }

    bool PublishHDRIEnvironment(Resource::ResourceSubsystem* resourceSubsystem,
                                const HDRIEnvironmentResources& resources)
    {
        if (!resourceSubsystem || !resources.IsValid())
            return false;

        auto publish = [resourceSubsystem](const Resource::TextureHandle& texture,
                                           const char* label) -> bool
        {
            if (!texture)
            {
                RVX_CORE_ERROR("ModelViewer HDRI {} resource is missing", label);
                return false;
            }

            if (!resourceSubsystem->PublishRenderResource(
                    Resource::ResourceHandle<Resource::IResource>(texture)))
            {
                RVX_CORE_ERROR("ModelViewer HDRI {} publication was rejected", label);
                return false;
            }
            return true;
        };

        return publish(resources.environment, "environment") &&
               publish(resources.irradiance, "irradiance") &&
               publish(resources.prefiltered, "prefiltered") &&
               publish(resources.brdfLUT, "BRDF LUT");
    }

    bool IsSkyboxPassReady(const RenderFrameFeatureDiagnostics* sceneRenderer,
                           std::string& outReason)
    {
        if (!sceneRenderer)
        {
            outReason = "NoSceneRenderer";
            return false;
        }

        const RenderPassFeatureDiagnostics& status = sceneRenderer->skybox;
        if (status.supported && status.enabled)
        {
            outReason.clear();
            return true;
        }
        outReason = status.reason.empty() ? "SkyboxPassNotReady"
                                          : status.reason;
        return false;
    }

    bool IsDirectionalShadowReady(
        const RenderFrameFeatureDiagnostics* sceneRenderer,
        std::string& outReason)
    {
        if (!sceneRenderer)
        {
            outReason = "NoSceneRenderer";
            return false;
        }

        if (sceneRenderer->directionalShadow.samplingEnabled)
        {
            outReason.clear();
            return true;
        }
        outReason = sceneRenderer->directionalShadow.reason.empty()
                        ? "ShadowSamplingDisabled"
                        : sceneRenderer->directionalShadow.reason;
        return false;
    }

    const char* BoolText(bool value)
    {
        return value ? "true" : "false";
    }

    std::string DescribeGPUDrivenCullingReadiness(const RenderGPUDrivenCullingDiagnostics& stats)
    {
        return std::string("enabled=") + BoolText(stats.enabled) +
               ", graphPassAdded=" + BoolText(stats.graphPassAdded) +
               ", graphPassRecorded=" + BoolText(stats.graphPassRecorded) +
               ", gpuExecutionRecorded=" + BoolText(stats.gpuExecutionRecorded) +
               ", graphInputDrawItemCount=" + std::to_string(stats.graphInputDrawItemCount) +
               ", opaqueIndirectRequested=" + BoolText(stats.opaqueIndirectRequested) +
               ", opaqueCullingReady=" + BoolText(stats.opaqueCullingReady) +
               ", opaquePipelineReady=" + BoolText(stats.opaquePipelineReady) +
               ", opaqueIndirectEligible=" + BoolText(stats.opaqueIndirectEligible) +
               ", opaqueIndirectSubmitted=" + BoolText(stats.opaqueIndirectSubmitted) +
               ", opaqueDirectDraws=" + std::to_string(stats.opaqueDirectDrawCount) +
               ", opaqueIndirectBatches=" + std::to_string(stats.opaqueGpuDrivenIndirectBatchCount) +
               ", opaqueIndirectDraws=" + std::to_string(stats.opaqueGpuDrivenIndirectDrawCount) +
               ", opaqueFallbackReason=" +
                   GetGPUDrivenDrawFallbackReasonName(stats.opaqueFallbackReason) +
               ", visibleCullableDrawItemCount=" + std::to_string(stats.visibleCullableDrawItemCount) +
               ", frustumCulledDrawItemCount=" + std::to_string(stats.frustumCulledDrawItemCount) +
               ", distanceCulledDrawItemCount=" + std::to_string(stats.distanceCulledDrawItemCount) +
               ", skippedMissingGpuDataCount=" + std::to_string(stats.skippedMissingGpuDataCount);
    }

    bool IsGPUDrivenCullingReady(const RenderFrameFeatureDiagnostics* sceneRenderer,
                                 bool requireCullAffectsDrawCount,
                                 std::string& outReason)
    {
        if (!sceneRenderer)
        {
            outReason = "NoSceneRenderer";
            return false;
        }

        const RenderGPUDrivenCullingDiagnostics& stats =
            sceneRenderer->gpuDrivenCulling;
        const bool cullAffectsDrawCount =
            stats.graphInputDrawItemCount > stats.visibleCullableDrawItemCount &&
            (stats.frustumCulledDrawItemCount > 0 || stats.distanceCulledDrawItemCount > 0);
        const bool ready = stats.enabled &&
                           stats.graphInputDrawItemCount > 0 &&
                           stats.graphPassAdded &&
                           stats.graphPassRecorded &&
                           stats.gpuExecutionRecorded &&
                           stats.opaqueIndirectRequested &&
                           stats.opaqueCullingReady &&
                           stats.opaquePipelineReady &&
                           stats.opaqueIndirectEligible &&
                           stats.opaqueIndirectSubmitted &&
                           stats.opaqueGpuDrivenIndirectBatchCount > 0 &&
                           stats.opaqueGpuDrivenIndirectDrawCount > 0 &&
                           (!requireCullAffectsDrawCount || cullAffectsDrawCount);
        if (ready)
        {
            outReason.clear();
            return true;
        }

        outReason = DescribeGPUDrivenCullingReadiness(stats);
        return false;
    }

    bool IsGPUDrivenDirectFallbackReady(
        const RenderFrameFeatureDiagnostics* sceneRenderer,
        std::string& outReason)
    {
        if (!sceneRenderer)
        {
            outReason = "NoSceneRenderer";
            return false;
        }

        const RenderGPUDrivenCullingDiagnostics& stats =
            sceneRenderer->gpuDrivenCulling;
        const bool ready = !stats.enabled &&
                           !stats.opaqueIndirectRequested &&
                           !stats.opaqueIndirectEligible &&
                           !stats.opaqueIndirectSubmitted &&
                           stats.opaqueGpuDrivenIndirectDrawCount == 0 &&
                           stats.opaqueDirectDrawCount > 0 &&
                           stats.opaqueFallbackReason ==
                               GPUDrivenDrawFallbackReason::Disabled;
        if (ready)
        {
            outReason.clear();
            return true;
        }

        outReason = DescribeGPUDrivenCullingReadiness(stats);
        return false;
    }

    std::string DescribeParticleReadiness(
        const RenderFrameFeatureDiagnostics* renderFeatures)
    {
        if (!renderFeatures)
        {
            return "NoRenderFeatureDiagnostics";
        }

        const RenderParticleFeatureDiagnostics& stats =
            renderFeatures->particles;
        return std::string("requested=") + BoolText(stats.requested) +
               ", supported=" + BoolText(stats.supported) +
               ", enabled=" + BoolText(stats.enabled) +
               ", graphPassScheduled=" +
               BoolText(stats.graphPassScheduled) +
               ", drawSubmitted=" + BoolText(stats.drawSubmitted) +
               ", itemCount=" + std::to_string(stats.itemCount) +
               ", payloadReadyItems=" +
               std::to_string(stats.renderPayloadReadyItemCount) +
               ", totalAliveParticles=" +
               std::to_string(stats.totalAliveParticles) +
               ", reason=" + stats.reason;
    }

    bool IsParticleRuntimeReady(
        const RenderFrameFeatureDiagnostics* renderFeatures,
        std::string& outReason)
    {
        if (!renderFeatures)
        {
            outReason = "NoRenderFeatureDiagnostics";
            return false;
        }

        const RenderParticleFeatureDiagnostics& stats =
            renderFeatures->particles;
        const bool ready = stats.requested && stats.supported &&
                           stats.enabled && stats.graphPassScheduled &&
                           stats.drawSubmitted && stats.itemCount > 0 &&
                           stats.renderPayloadReadyItemCount > 0 &&
                           stats.totalAliveParticles > 0;
        if (ready)
        {
            outReason.clear();
            return true;
        }

        outReason = DescribeParticleReadiness(renderFeatures);
        return false;
    }

    std::string DescribeRayTracingShadowReadiness(const RenderRayTracingDiagnostics& stats)
    {
        return std::string("scenePrepared=") + BoolText(stats.scenePrepared) +
               ", tlasAvailable=" + BoolText(stats.tlasAvailable) +
               ", requested=" + BoolText(stats.shadowRequested) +
               ", supported=" + BoolText(stats.shadowSupported) +
               ", recorded=" + BoolText(stats.shadowRecorded) +
               ", size=" + std::to_string(stats.shadowWidth) + "x" + std::to_string(stats.shadowHeight);
    }

    std::string DescribeRayTracingShadowAlphaReadiness(const RenderRayTracingDiagnostics& stats)
    {
        return DescribeRayTracingShadowReadiness(stats) +
               ", materialTextureTableAvailable=" + BoolText(stats.shadowMaterialTextureTableAvailable) +
               ", alphaMetadataAvailable=" + BoolText(stats.shadowAlphaMetadataAvailable) +
               ", alphaTextureTableAvailable=" + BoolText(stats.shadowAlphaTextureTableAvailable) +
               ", alphaGeometryTableAvailable=" + BoolText(stats.shadowAlphaGeometryTableAvailable) +
               ", materialTextures=" + std::to_string(stats.shadowMaterialTexturesBound) +
               "/" + std::to_string(stats.shadowMaterialTextureCount) +
               ", alphaTextures=" + std::to_string(stats.shadowAlphaTexturesBound) +
               "/" + std::to_string(stats.shadowAlphaTextureCount) +
               ", alphaIndexBuffers=" + std::to_string(stats.shadowAlphaIndexBufferCount) +
               ", alphaUVBuffers=" + std::to_string(stats.shadowAlphaUVBufferCount);
    }

    std::string DescribeRayTracingReflectionReadiness(const RenderRayTracingDiagnostics& stats)
    {
        return std::string("scenePrepared=") + BoolText(stats.scenePrepared) +
               ", tlasAvailable=" + BoolText(stats.tlasAvailable) +
               ", requested=" + BoolText(stats.reflectionRequested) +
               ", supported=" + BoolText(stats.reflectionSupported) +
               ", recorded=" + BoolText(stats.reflectionRecorded) +
               ", denoiseRequested=" + BoolText(stats.reflectionDenoiseRequested) +
               ", denoiseSupported=" + BoolText(stats.reflectionDenoiseSupported) +
               ", denoiseRecorded=" + BoolText(stats.reflectionDenoiseRecorded) +
               ", compositeRequested=" + BoolText(stats.reflectionCompositeRequested) +
               ", compositeSupported=" + BoolText(stats.reflectionCompositeSupported) +
               ", compositeRecorded=" + BoolText(stats.reflectionCompositeRecorded) +
               ", denoiseFallbackToRaw=" + BoolText(stats.denoiseFallbackToRaw) +
               ", size=" + std::to_string(stats.reflectionWidth) + "x" + std::to_string(stats.reflectionHeight);
    }

    std::string DescribeRayTracingReflectionAssetReadiness(const RenderRayTracingDiagnostics& stats)
    {
        return DescribeRayTracingReflectionReadiness(stats) +
               ", materialTextureTableAvailable=" + BoolText(stats.reflectionMaterialTextureTableAvailable) +
               ", geometryMetadataAvailable=" + BoolText(stats.reflectionGeometryMetadataAvailable) +
               ", geometryTableAvailable=" + BoolText(stats.reflectionGeometryTableAvailable) +
               ", materialTextures=" + std::to_string(stats.reflectionMaterialTexturesBound) +
               "/" + std::to_string(stats.reflectionMaterialTextureCount) +
               ", geometryIndexBuffers=" + std::to_string(stats.reflectionGeometryIndexBufferCount) +
               ", geometryUVBuffers=" + std::to_string(stats.reflectionGeometryUVBufferCount) +
               ", geometryNormalBuffers=" + std::to_string(stats.reflectionGeometryNormalBufferCount) +
               ", geometryTangentBuffers=" + std::to_string(stats.reflectionGeometryTangentBufferCount);
    }

    std::string DescribeRayTracingShadowHistoryReadiness(const RenderRayTracingDiagnostics& stats)
    {
        return DescribeRayTracingShadowReadiness(stats) +
               ", historyAvailable=" + BoolText(stats.shadowHistoryAvailable) +
               ", depthHistoryAvailable=" + BoolText(stats.shadowDepthHistoryAvailable) +
               ", normalHistoryAvailable=" + BoolText(stats.shadowNormalHistoryAvailable) +
               ", historyReset=" + BoolText(stats.shadowHistoryReset) +
               ", historyRecreated=" + BoolText(stats.shadowHistoryRecreated) +
               ", historyResolutionChanged=" + BoolText(stats.shadowHistoryResolutionChanged) +
               ", historyConfigChanged=" + BoolText(stats.shadowHistoryConfigChanged) +
               ", temporalAccumulated=" + BoolText(stats.shadowTemporalAccumulated);
    }

    std::string DescribeRayTracingReflectionHistoryReadiness(const RenderRayTracingDiagnostics& stats)
    {
        return DescribeRayTracingReflectionReadiness(stats) +
               ", historyAvailable=" + BoolText(stats.reflectionHistoryAvailable) +
               ", depthHistoryAvailable=" + BoolText(stats.reflectionDepthHistoryAvailable) +
               ", normalHistoryAvailable=" + BoolText(stats.reflectionNormalHistoryAvailable) +
               ", historyReset=" + BoolText(stats.reflectionHistoryReset) +
               ", historyRecreated=" + BoolText(stats.reflectionHistoryRecreated) +
               ", historyResolutionChanged=" + BoolText(stats.reflectionHistoryResolutionChanged) +
               ", historyConfigChanged=" + BoolText(stats.reflectionHistoryConfigChanged) +
               ", temporalAccumulated=" + BoolText(stats.reflectionTemporalAccumulated);
    }

    bool IsRayTracedShadowHistoryReady(
        const RenderFrameFeatureDiagnostics* sceneRenderer,
        std::string& outReason)
    {
        if (!sceneRenderer)
        {
            outReason = "NoSceneRenderer";
            return false;
        }

        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
        const bool ready = stats.scenePrepared && stats.tlasAvailable &&
                           stats.shadowRequested && stats.shadowSupported && stats.shadowRecorded &&
                           stats.shadowHistoryAvailable &&
                           stats.shadowDepthHistoryAvailable && stats.shadowNormalHistoryAvailable &&
                           !stats.shadowHistoryReset && !stats.shadowHistoryRecreated &&
                           !stats.shadowHistoryResolutionChanged && !stats.shadowHistoryConfigChanged &&
                           stats.shadowTemporalAccumulated;
        if (ready)
        {
            outReason.clear();
            return true;
        }

        outReason = DescribeRayTracingShadowHistoryReadiness(stats);
        return false;
    }

    bool IsRayTracedReflectionHistoryReady(
        const RenderFrameFeatureDiagnostics* sceneRenderer,
        std::string& outReason)
    {
        if (!sceneRenderer)
        {
            outReason = "NoSceneRenderer";
            return false;
        }

        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
        const bool ready = stats.scenePrepared && stats.tlasAvailable &&
                           stats.reflectionRequested && stats.reflectionSupported && stats.reflectionRecorded &&
                           stats.reflectionHistoryAvailable &&
                           stats.reflectionDepthHistoryAvailable && stats.reflectionNormalHistoryAvailable &&
                           !stats.reflectionHistoryReset && !stats.reflectionHistoryRecreated &&
                           !stats.reflectionHistoryResolutionChanged && !stats.reflectionHistoryConfigChanged &&
                           stats.reflectionTemporalAccumulated;
        if (ready)
        {
            outReason.clear();
            return true;
        }

        outReason = DescribeRayTracingReflectionHistoryReadiness(stats);
        return false;
    }

    bool IsRayTracedShadowHistoryResetReady(
        const RenderFrameFeatureDiagnostics* sceneRenderer,
        std::string& outReason)
    {
        if (!sceneRenderer)
        {
            outReason = "NoSceneRenderer";
            return false;
        }

        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
        const bool ready = stats.scenePrepared && stats.tlasAvailable &&
                           stats.shadowRequested && stats.shadowSupported && stats.shadowRecorded &&
                           stats.shadowHistoryReset &&
                           stats.shadowHistoryAvailable &&
                           stats.shadowDepthHistoryAvailable && stats.shadowNormalHistoryAvailable &&
                           !stats.shadowTemporalAccumulated;
        if (ready)
        {
            outReason.clear();
            return true;
        }

        outReason = DescribeRayTracingShadowHistoryReadiness(stats);
        return false;
    }

    bool IsRayTracedReflectionHistoryResetReady(
        const RenderFrameFeatureDiagnostics* sceneRenderer,
        std::string& outReason)
    {
        if (!sceneRenderer)
        {
            outReason = "NoSceneRenderer";
            return false;
        }

        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
        const bool ready = stats.scenePrepared && stats.tlasAvailable &&
                           stats.reflectionRequested && stats.reflectionSupported && stats.reflectionRecorded &&
                           stats.reflectionHistoryReset &&
                           stats.reflectionHistoryAvailable &&
                           stats.reflectionDepthHistoryAvailable && stats.reflectionNormalHistoryAvailable &&
                           !stats.reflectionTemporalAccumulated;
        if (ready)
        {
            outReason.clear();
            return true;
        }

        outReason = DescribeRayTracingReflectionHistoryReadiness(stats);
        return false;
    }

    bool IsRayTracedShadowHistoryResizeReady(
                                             const RenderFrameFeatureDiagnostics* sceneRenderer,
                                             uint32 expectedWidth,
                                             uint32 expectedHeight,
                                             std::string& outReason)
    {
        if (!sceneRenderer)
        {
            outReason = "NoSceneRenderer";
            return false;
        }

        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
        const bool sizeMatches = stats.shadowWidth == expectedWidth && stats.shadowHeight == expectedHeight;
        const bool ready = stats.scenePrepared && stats.tlasAvailable &&
                           stats.shadowRequested && stats.shadowSupported && stats.shadowRecorded &&
                           stats.shadowHistoryReset && stats.shadowHistoryRecreated &&
                           stats.shadowHistoryResolutionChanged && !stats.shadowHistoryConfigChanged &&
                           stats.shadowHistoryAvailable &&
                           stats.shadowDepthHistoryAvailable && stats.shadowNormalHistoryAvailable &&
                           !stats.shadowTemporalAccumulated && sizeMatches;
        if (ready)
        {
            outReason.clear();
            return true;
        }

        outReason = DescribeRayTracingShadowHistoryReadiness(stats) +
                    ", expectedSize=" + std::to_string(expectedWidth) + "x" +
                    std::to_string(expectedHeight);
        return false;
    }

    bool IsRayTracedReflectionHistoryResizeReady(
                                                 const RenderFrameFeatureDiagnostics* sceneRenderer,
                                                 uint32 expectedWidth,
                                                 uint32 expectedHeight,
                                                 std::string& outReason)
    {
        if (!sceneRenderer)
        {
            outReason = "NoSceneRenderer";
            return false;
        }

        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
        const bool sizeMatches = stats.reflectionWidth == expectedWidth && stats.reflectionHeight == expectedHeight;
        const bool ready = stats.scenePrepared && stats.tlasAvailable &&
                           stats.reflectionRequested && stats.reflectionSupported && stats.reflectionRecorded &&
                           stats.reflectionHistoryReset && stats.reflectionHistoryRecreated &&
                           stats.reflectionHistoryResolutionChanged && !stats.reflectionHistoryConfigChanged &&
                           stats.reflectionHistoryAvailable &&
                           stats.reflectionDepthHistoryAvailable && stats.reflectionNormalHistoryAvailable &&
                           !stats.reflectionTemporalAccumulated && sizeMatches;
        if (ready)
        {
            outReason.clear();
            return true;
        }

        outReason = DescribeRayTracingReflectionHistoryReadiness(stats) +
                    ", expectedSize=" + std::to_string(expectedWidth) + "x" +
                    std::to_string(expectedHeight);
        return false;
    }

    std::string DescribeRayTracingBudgetReadiness(const RenderRayTracingDiagnostics& stats)
    {
        return std::string("budgetEnabled=") + BoolText(stats.budgetEnabled) +
               ", budgetApplied=" + BoolText(stats.budgetApplied) +
               ", rayBudget=" + std::to_string(stats.rayBudget) +
               ", estimatedTotalRayCount=" + std::to_string(stats.estimatedTotalRayCount) +
               ", denoiseTapBudget=" + std::to_string(stats.denoiseTapBudget) +
               ", estimatedDenoiseTapCount=" + std::to_string(stats.estimatedReflectionDenoiseTapCount) +
               ", trackedResourceBudget=" + std::to_string(stats.trackedResourceBudget) +
               ", resourceBudgetExceeded=" + BoolText(stats.resourceBudgetExceeded) +
               ", blasCacheEvictionFrameThreshold=" +
               std::to_string(stats.blasCacheEvictionFrameThreshold) +
               ", cachedBLASCount=" + std::to_string(stats.cachedBLASCount) +
               ", evictedBLASCount=" + std::to_string(stats.evictedBLASCount) +
               ", resourceBudgetEvictedBLASCount=" +
               std::to_string(stats.resourceBudgetEvictedBLASCount) +
               ", releasedBLASScratchCount=" + std::to_string(stats.releasedBLASScratchCount) +
               ", pendingBLASScratchReleaseCount=" +
               std::to_string(stats.pendingBLASScratchReleaseCount) +
               ", releasedBLASScratchBytes=" + std::to_string(stats.releasedBLASScratchBytes) +
               ", resourceBudgetEvictionAttempted=" +
               BoolText(stats.resourceBudgetEvictionAttempted) +
               ", resourceByteAccountingOverflowed=" +
               BoolText(stats.resourceByteAccountingOverflowed) +
               ", rtTrackedResourceBytes=" + std::to_string(stats.totalTrackedResourceBytes) +
               ", cachedBLASAccelerationStructureBytes=" +
               std::to_string(stats.cachedBLASAccelerationStructureBytes) +
               ", cachedBLASScratchBytes=" + std::to_string(stats.cachedBLASScratchBytes) +
               ", topLevelAccelerationStructureBytes=" +
               std::to_string(stats.topLevelAccelerationStructureBytes) +
               ", topLevelScratchBytes=" + std::to_string(stats.topLevelScratchBytes) +
               ", instanceBufferBytes=" + std::to_string(stats.instanceBufferBytes) +
               ", materialMetadataBufferBytes=" + std::to_string(stats.materialMetadataBufferBytes) +
               ", alphaMetadataBufferBytes=" + std::to_string(stats.alphaMetadataBufferBytes) +
               ", reflectionScale=" + std::to_string(stats.reflectionResolutionScale) +
               ", reflectionSize=" + std::to_string(stats.reflectionWidth) + "x" + std::to_string(stats.reflectionHeight) +
               ", rayBudgetExceeded=" + BoolText(stats.rayBudgetExceeded) +
               ", denoiseTapBudgetExceeded=" + BoolText(stats.denoiseTapBudgetExceeded) +
               ", gpuTimeBudget=" + std::to_string(stats.gpuTimeBudget) +
               ", shadowGpuTimeBudget=" + std::to_string(stats.shadowGpuTimeBudget) +
               ", reflectionGpuTimeBudget=" + std::to_string(stats.reflectionGpuTimeBudget) +
               ", measuredGpuTimeAvailable=" + BoolText(stats.measuredGpuTimeAvailable) +
               ", measuredGpuMs=" + std::to_string(stats.measuredGpuTimeForBudgetMs) +
               ", measuredShadowGpuMs=" + std::to_string(stats.measuredShadowGpuTimeForBudgetMs) +
               ", measuredReflectionGpuMs=" + std::to_string(stats.measuredReflectionGpuTimeForBudgetMs) +
               ", gpuTimeBudgetApplied=" + BoolText(stats.gpuTimeBudgetApplied) +
               ", shadowGpuTimeBudgetApplied=" + BoolText(stats.shadowGpuTimeBudgetApplied) +
               ", reflectionGpuTimeBudgetApplied=" + BoolText(stats.reflectionGpuTimeBudgetApplied) +
               ", gpuQualityScale=" + std::to_string(stats.gpuTimeBudgetQualityScale) +
               ", shadowGpuQualityScale=" + std::to_string(stats.shadowGpuTimeBudgetQualityScale) +
               ", reflectionGpuQualityScale=" + std::to_string(stats.reflectionGpuTimeBudgetQualityScale);
    }

    std::string DescribeRayTracingGpuTimingReadiness(const RenderRayTracingDiagnostics& stats)
    {
        return std::string("shadowRequested=") + BoolText(stats.shadowRequested) +
               ", shadowSupported=" + BoolText(stats.shadowGpuTimingSupported) +
               ", shadowQueriesRecorded=" + BoolText(stats.shadowGpuTimingQueriesRecorded) +
               ", shadowResolveRecorded=" + BoolText(stats.shadowGpuTimingResolveRecorded) +
               ", shadowReadbackBuffer=" + BoolText(stats.shadowGpuTimingReadbackBufferAvailable) +
               ", shadowResultAvailable=" + BoolText(stats.shadowGpuTimingResultAvailable) +
               ", shadowFrequency=" + std::to_string(stats.shadowGpuTimestampFrequency) +
               ", reflectionRequested=" + BoolText(stats.reflectionRequested) +
               ", reflectionSupported=" + BoolText(stats.reflectionGpuTimingSupported) +
               ", reflectionQueriesRecorded=" + BoolText(stats.reflectionGpuTimingQueriesRecorded) +
               ", reflectionResolveRecorded=" + BoolText(stats.reflectionGpuTimingResolveRecorded) +
               ", reflectionReadbackBuffer=" + BoolText(stats.reflectionGpuTimingReadbackBufferAvailable) +
               ", reflectionResultAvailable=" + BoolText(stats.reflectionGpuTimingResultAvailable) +
               ", reflectionFrequency=" + std::to_string(stats.reflectionGpuTimestampFrequency);
    }

    bool IsRayTracingGpuTimingReady(
        const RenderFrameFeatureDiagnostics* sceneRenderer,
        std::string& outReason)
    {
        if (!sceneRenderer)
        {
            outReason = "NoSceneRenderer";
            return false;
        }

        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
        const bool shadowRequired = stats.shadowRequested;
        const bool reflectionRequired = stats.reflectionRequested;
        if (!shadowRequired && !reflectionRequired)
        {
            outReason = "NoRayTracingPassRequested";
            return false;
        }

        const bool shadowReady = !shadowRequired ||
                                 (stats.shadowGpuTimingSupported &&
                                  stats.shadowGpuTimingQueriesRecorded &&
                                  stats.shadowGpuTimingResolveRecorded &&
                                  stats.shadowGpuTimingReadbackBufferAvailable &&
                                  stats.shadowGpuTimestampFrequency > 0);
        const bool reflectionReady = !reflectionRequired ||
                                     (stats.reflectionGpuTimingSupported &&
                                      stats.reflectionGpuTimingQueriesRecorded &&
                                      stats.reflectionGpuTimingResolveRecorded &&
                                      stats.reflectionGpuTimingReadbackBufferAvailable &&
                                      stats.reflectionGpuTimestampFrequency > 0);
        if (shadowReady && reflectionReady)
        {
            outReason.clear();
            return true;
        }

        outReason = DescribeRayTracingGpuTimingReadiness(stats);
        return false;
    }

    std::string DescribeRayTracingGpuBudgetReadiness(const RenderRayTracingDiagnostics& stats)
    {
        return DescribeRayTracingBudgetReadiness(stats) +
               ", gpuTimeBudgetExceeded=" + BoolText(stats.gpuTimeBudgetExceeded) +
               ", shadowGpuTimeBudgetExceeded=" + BoolText(stats.shadowGpuTimeBudgetExceeded) +
               ", reflectionGpuTimeBudgetExceeded=" + BoolText(stats.reflectionGpuTimeBudgetExceeded) +
               ", requestedShadowSamples=" + std::to_string(stats.requestedShadowSamplesPerPixel) +
               ", shadowSamples=" + std::to_string(stats.shadowSamplesPerPixel) +
               ", requestedReflectionScale=" + std::to_string(stats.requestedReflectionResolutionScale) +
               ", requestedReflectionSamples=" + std::to_string(stats.requestedReflectionSamplesPerPixel) +
               ", reflectionSamples=" + std::to_string(stats.reflectionSamplesPerPixel) +
               ", requestedDenoiseRadius=" + std::to_string(stats.requestedReflectionDenoiseRadius) +
               ", denoiseRadius=" + std::to_string(stats.reflectionDenoiseRadius) +
               ", overBudgetFrames=" + std::to_string(stats.gpuTimeBudgetOverBudgetFrameCount) +
               ", reflectionOverBudgetFrames=" + std::to_string(stats.reflectionGpuTimeBudgetOverBudgetFrameCount);
    }

    bool IsRayTracingResourceBudgetExceeded(
        const RenderFrameFeatureDiagnostics* sceneRenderer,
        std::string& outReason)
    {
        if (!sceneRenderer)
        {
            outReason = "NoSceneRenderer";
            return false;
        }

        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
        if (stats.budgetEnabled && stats.trackedResourceBudget > 0 && stats.resourceBudgetExceeded)
        {
            outReason.clear();
            return true;
        }

        outReason = DescribeRayTracingBudgetReadiness(stats);
        return false;
    }

    bool IsRayTracingGpuBudgetApplied(
        const RenderFrameFeatureDiagnostics* sceneRenderer,
        std::string& outReason)
    {
        if (!sceneRenderer)
        {
            outReason = "NoSceneRenderer";
            return false;
        }

        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
        const bool gpuBudgetConfigured = stats.gpuTimeBudget > 0.0f ||
                                         stats.shadowGpuTimeBudget > 0.0f ||
                                         stats.reflectionGpuTimeBudget > 0.0f;
        const bool rayTracingRequested = stats.shadowRequested || stats.reflectionRequested;
        const bool budgetExceeded = stats.gpuTimeBudgetExceeded ||
                                    stats.shadowGpuTimeBudgetExceeded ||
                                    stats.reflectionGpuTimeBudgetExceeded;
        const bool pathBudgetApplied = stats.shadowGpuTimeBudgetApplied ||
                                       stats.reflectionGpuTimeBudgetApplied;
        const bool qualityScaleReduced = stats.gpuTimeBudgetQualityScale < 0.999f ||
                                         stats.shadowGpuTimeBudgetQualityScale < 0.999f ||
                                         stats.reflectionGpuTimeBudgetQualityScale < 0.999f;
        const bool shadowQualityReduced = stats.shadowRequested &&
                                          stats.shadowSamplesPerPixel < stats.requestedShadowSamplesPerPixel;
        const bool reflectionQualityReduced = stats.reflectionRequested &&
                                              (stats.reflectionResolutionScale + 0.0001f <
                                                   stats.requestedReflectionResolutionScale ||
                                               stats.reflectionSamplesPerPixel <
                                                   stats.requestedReflectionSamplesPerPixel ||
                                               stats.reflectionDenoiseRadius <
                                                   stats.requestedReflectionDenoiseRadius);
        if (stats.budgetEnabled && gpuBudgetConfigured && rayTracingRequested && stats.measuredGpuTimeAvailable &&
            budgetExceeded && stats.gpuTimeBudgetApplied && pathBudgetApplied && qualityScaleReduced &&
            (shadowQualityReduced || reflectionQualityReduced))
        {
            outReason.clear();
            return true;
        }

        outReason = DescribeRayTracingGpuBudgetReadiness(stats);
        return false;
    }

    std::string DescribeRayTracingGpuBudgetRecoveryReadiness(const RenderRayTracingDiagnostics& stats,
                                                            float baselineReflectionScale,
                                                            float baselineReflectionQualityScale)
    {
        return DescribeRayTracingGpuBudgetReadiness(stats) +
               ", baselineReflectionScale=" + std::to_string(baselineReflectionScale) +
               ", baselineReflectionGpuQualityScale=" + std::to_string(baselineReflectionQualityScale) +
               ", reflectionUnderBudgetFrames=" +
               std::to_string(stats.reflectionGpuTimeBudgetUnderBudgetFrameCount);
    }

    bool IsRayTracingGpuBudgetRecovered(
                                        const RenderFrameFeatureDiagnostics* sceneRenderer,
                                        float baselineReflectionScale,
                                        float baselineReflectionQualityScale,
                                        std::string& outReason)
    {
        if (!sceneRenderer)
        {
            outReason = "NoSceneRenderer";
            return false;
        }

        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
        const bool validBaseline = baselineReflectionScale > 0.0f &&
                                   baselineReflectionScale < 0.999f &&
                                   baselineReflectionQualityScale > 0.0f &&
                                   baselineReflectionQualityScale < 0.999f;
        const bool reflectionBudgetConfigured = stats.gpuTimeBudget > 0.0f ||
                                                stats.reflectionGpuTimeBudget > 0.0f;
        const bool reflectionQualityRecovered =
            stats.reflectionGpuTimeBudgetQualityScale > baselineReflectionQualityScale + 0.0001f;
        const bool reflectionResolutionRecovered =
            stats.reflectionResolutionScale > baselineReflectionScale + 0.0001f;
        if (validBaseline &&
            stats.budgetEnabled &&
            stats.reflectionRequested &&
            stats.measuredGpuTimeAvailable &&
            reflectionBudgetConfigured &&
            !stats.gpuTimeBudgetExceeded &&
            !stats.reflectionGpuTimeBudgetExceeded &&
            stats.reflectionGpuTimeBudgetApplied &&
            reflectionQualityRecovered &&
            reflectionResolutionRecovered)
        {
            outReason.clear();
            return true;
        }

        outReason = DescribeRayTracingGpuBudgetRecoveryReadiness(stats,
                                                                 baselineReflectionScale,
                                                                 baselineReflectionQualityScale);
        return false;
    }

    void ApplyRayTracingGpuRecoveryBudgetSettings(
        Engine& engine,
        RenderFrameSettings& frameSettings,
        const ModelViewerOptions& options)
    {
        frameSettings.rayTracing.budgetEnabled = true;
        if (options.rayTracingRecoveryMaxGpuMs > 0.0f)
        {
            frameSettings.rayTracing.maxMeasuredGpuMs =
                options.rayTracingRecoveryMaxGpuMs;
        }
        if (options.rayTracingRecoveryMaxShadowGpuMs > 0.0f)
        {
            frameSettings.rayTracing.maxShadowMeasuredGpuMs =
                options.rayTracingRecoveryMaxShadowGpuMs;
        }
        if (options.rayTracingRecoveryMaxReflectionGpuMs > 0.0f)
        {
            frameSettings.rayTracing.maxReflectionMeasuredGpuMs =
                options.rayTracingRecoveryMaxReflectionGpuMs;
        }
        if (options.rayTracingGpuAdjustmentFrameCount > 0)
        {
            frameSettings.rayTracing.gpuTimingAdjustmentFrameCount =
                options.rayTracingGpuAdjustmentFrameCount;
        }
        static_cast<void>(engine.SetRenderFrameSettings(frameSettings));
        RVX_CORE_INFO("ModelViewer ray tracing recovery GPU budget: maxGpuMs={:.6f}, "
                      "maxShadowGpuMs={:.6f}, maxReflectionGpuMs={:.6f}, adjustFrames={}",
                      frameSettings.rayTracing.maxMeasuredGpuMs,
                      frameSettings.rayTracing.maxShadowMeasuredGpuMs,
                      frameSettings.rayTracing.maxReflectionMeasuredGpuMs,
                      frameSettings.rayTracing.gpuTimingAdjustmentFrameCount);
    }

    bool IsRayTracedShadowReady(
        const RenderFrameFeatureDiagnostics* sceneRenderer,
        std::string& outReason)
    {
        if (!sceneRenderer)
        {
            outReason = "NoSceneRenderer";
            return false;
        }

        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
        const bool ready = stats.scenePrepared && stats.tlasAvailable &&
                           stats.shadowRequested && stats.shadowSupported && stats.shadowRecorded &&
                           stats.shadowWidth > 0 && stats.shadowHeight > 0;
        if (ready)
        {
            outReason.clear();
            return true;
        }

        outReason = DescribeRayTracingShadowReadiness(stats);
        return false;
    }

    bool IsRayTracedShadowAlphaReady(
        const RenderFrameFeatureDiagnostics* sceneRenderer,
        std::string& outReason)
    {
        if (!sceneRenderer)
        {
            outReason = "NoSceneRenderer";
            return false;
        }

        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
        const bool materialTexturesBound = stats.shadowMaterialTextureCount > 0 &&
                                           stats.shadowMaterialTexturesBound == stats.shadowMaterialTextureCount;
        const bool alphaTexturesBound = stats.shadowAlphaTextureCount > 0 &&
                                        stats.shadowAlphaTexturesBound == stats.shadowAlphaTextureCount;
        const bool ready = stats.scenePrepared && stats.tlasAvailable &&
                           stats.shadowRequested && stats.shadowSupported && stats.shadowRecorded &&
                           stats.shadowMaterialTextureTableAvailable &&
                           stats.shadowAlphaMetadataAvailable &&
                           stats.shadowAlphaTextureTableAvailable &&
                           stats.shadowAlphaGeometryTableAvailable &&
                           materialTexturesBound && alphaTexturesBound &&
                           stats.shadowAlphaIndexBufferCount > 0 &&
                           stats.shadowAlphaUVBufferCount > 0;
        if (ready)
        {
            outReason.clear();
            return true;
        }

        outReason = DescribeRayTracingShadowAlphaReadiness(stats);
        return false;
    }

    bool IsRayTracedReflectionReady(const RenderFrameFeatureDiagnostics* sceneRenderer,
                                    bool requireDenoise,
                                    std::string& outReason)
    {
        if (!sceneRenderer)
        {
            outReason = "NoSceneRenderer";
            return false;
        }

        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
        const bool denoiseReady = !requireDenoise ||
                                  (stats.reflectionDenoiseRequested &&
                                   stats.reflectionDenoiseSupported &&
                                   stats.reflectionDenoiseRecorded &&
                                   !stats.denoiseFallbackToRaw);
        const bool ready = stats.scenePrepared && stats.tlasAvailable &&
                           stats.reflectionRequested && stats.reflectionSupported && stats.reflectionRecorded &&
                           denoiseReady &&
                           stats.reflectionCompositeRequested &&
                           stats.reflectionCompositeSupported &&
                           stats.reflectionCompositeRecorded &&
                           stats.reflectionWidth > 0 && stats.reflectionHeight > 0;
        if (ready)
        {
            outReason.clear();
            return true;
        }

        outReason = DescribeRayTracingReflectionReadiness(stats);
        return false;
    }

    bool IsRayTracedReflectionAssetReady(
        const RenderFrameFeatureDiagnostics* sceneRenderer,
        std::string& outReason)
    {
        if (!sceneRenderer)
        {
            outReason = "NoSceneRenderer";
            return false;
        }

        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
        const bool materialTexturesBound = stats.reflectionMaterialTextureCount > 0 &&
                                           stats.reflectionMaterialTexturesBound == stats.reflectionMaterialTextureCount;
        const bool geometryBuffersBound = stats.reflectionGeometryIndexBufferCount > 0 &&
                                         stats.reflectionGeometryUVBufferCount > 0 &&
                                         stats.reflectionGeometryNormalBufferCount > 0 &&
                                         stats.reflectionGeometryTangentBufferCount > 0;
        const bool ready = stats.scenePrepared && stats.tlasAvailable &&
                           stats.reflectionRequested && stats.reflectionSupported && stats.reflectionRecorded &&
                           stats.reflectionMaterialTextureTableAvailable &&
                           stats.reflectionGeometryMetadataAvailable &&
                           stats.reflectionGeometryTableAvailable &&
                           materialTexturesBound && geometryBuffersBound;
        if (ready)
        {
            outReason.clear();
            return true;
        }

        outReason = DescribeRayTracingReflectionAssetReadiness(stats);
        return false;
    }

    bool IsPBRMaterialReady(const RenderFrameFeatureDiagnostics* sceneRenderer,
                            std::string& outReason)
    {
        if (!sceneRenderer)
        {
            outReason = "NoSceneRenderer";
            return false;
        }

        const RenderMaterialFeatureDiagnostics& result =
            sceneRenderer->material;
        const uint32 requiredFlags = result.requiredTextureFlags;
        const bool hasAllTextureFlags = (result.textureFlags & requiredFlags) == requiredFlags;
        if (result.ready &&
            !result.usedFallback &&
            result.constantsUpdated &&
            result.descriptorSetAvailable &&
            hasAllTextureFlags)
        {
            outReason.clear();
            return true;
        }

        outReason = "ready=";
        outReason += result.ready ? "true" : "false";
        outReason += ", usedFallback=";
        outReason += result.usedFallback ? "true" : "false";
        outReason += ", constantsUpdated=";
        outReason += result.constantsUpdated ? "true" : "false";
        outReason += ", descriptorSet=";
        outReason += result.descriptorSetAvailable ? "true" : "false";
        outReason += ", textureFlags=" + std::to_string(result.textureFlags);
        outReason += ", requiredTextureFlags=" + std::to_string(requiredFlags);
        outReason += ", materialName='" + result.materialName + "'";
        if (!result.message.empty())
        {
            outReason += ", message='" + result.message + "'";
        }
        return false;
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

    bool ConfigureShadowTestLight(SceneManager* sceneManager)
    {
        if (!sceneManager)
        {
            return false;
        }

        ActorSpawnParams lightParams;
        lightParams.name = "ModelViewerShadowTestSun";
        SceneEntity* lightEntity = sceneManager->SpawnActor(lightParams);
        LightComponent* light = lightEntity ? lightEntity->AddComponent<LightComponent>() : nullptr;
        if (!lightEntity || !light)
        {
            RVX_CORE_ERROR("ModelViewer shadow test scene could not create a directional light");
            return false;
        }

        const Vec3 lightDirection = normalize(Vec3(-0.25f, -0.55f, -0.65f));
        lightEntity->SetRotation(MakeLookRotation(lightDirection, Vec3(0.0f, 1.0f, 0.0f)));
        light->SetLightType(LightType::Directional);
        light->SetColor(Vec3(1.0f, 0.96f, 0.88f));
        light->SetIntensity(5.0f);
        light->SetCastsShadow(true);
        light->SetShadowBias(0.0008f);

        RVX_CORE_INFO("ModelViewer shadow test directional light configured: dir=({}, {}, {})",
                      lightDirection.x,
                      lightDirection.y,
                      lightDirection.z);
        return true;
    }

    bool ConfigureMaterialTestLight(SceneManager* sceneManager)
    {
        if (!sceneManager)
        {
            return false;
        }

        ActorSpawnParams lightParams;
        lightParams.name = "ModelViewerMaterialTestKeyLight";
        SceneEntity* lightEntity = sceneManager->SpawnActor(lightParams);
        LightComponent* light = lightEntity ? lightEntity->AddComponent<LightComponent>() : nullptr;
        if (!lightEntity || !light)
        {
            RVX_CORE_ERROR("ModelViewer material test scene could not create a directional light");
            return false;
        }

        const Vec3 lightDirection = normalize(Vec3(-0.25f, -0.35f, -0.85f));
        lightEntity->SetRotation(MakeLookRotation(lightDirection, Vec3(0.0f, 1.0f, 0.0f)));
        light->SetLightType(LightType::Directional);
        light->SetColor(Vec3(1.0f, 0.96f, 0.9f));
        light->SetIntensity(4.0f);
        light->SetCastsShadow(false);

        RVX_CORE_INFO("ModelViewer material test directional light configured: dir=({}, {}, {})",
                      lightDirection.x,
                      lightDirection.y,
                      lightDirection.z);
        return true;
    }

    bool WriteScreenshotPPM(const RenderFrameCaptureResult& screenshot,
                            const std::filesystem::path& path)
    {
        if (!screenshot.IsComplete() || screenshot.bytesPerPixel != 4)
        {
            RVX_CORE_ERROR("ModelViewer smoke capture: invalid screenshot data");
            return false;
        }

        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent);
        }

        std::ofstream stream(path, std::ios::binary);
        if (!stream)
        {
            RVX_CORE_ERROR("ModelViewer smoke capture: failed to create screenshot {}", path.string());
            return false;
        }

        stream << "P6\n" << screenshot.width << " " << screenshot.height << "\n255\n";

        const uint8* data = screenshot.bytes.data();
        const bool bgra = screenshot.format == RHIFormat::BGRA8_UNORM ||
                          screenshot.format == RHIFormat::BGRA8_UNORM_SRGB;
        for (uint32 y = 0; y < screenshot.height; ++y)
        {
            const uint32 sourceY = screenshot.originBottomLeft ? (screenshot.height - 1u - y) : y;
            const uint8* row = data + static_cast<uint64>(sourceY) * screenshot.rowPitch;
            for (uint32 x = 0; x < screenshot.width; ++x)
            {
                const uint8* pixel = row + static_cast<uint64>(x) * screenshot.bytesPerPixel;
                uint8 rgb[3] = {};
                if (bgra)
                {
                    rgb[0] = pixel[2];
                    rgb[1] = pixel[1];
                    rgb[2] = pixel[0];
                }
                else
                {
                    rgb[0] = pixel[0];
                    rgb[1] = pixel[1];
                    rgb[2] = pixel[2];
                }

                stream.write(reinterpret_cast<const char*>(rgb), sizeof(rgb));
            }
        }

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
        RVX_CORE_INFO("Smoke mode: backend={}, frames={}, resolution={}x{}, validation={}, gpuValidation={}",
                      ToString(options.backend),
                      options.frames,
                      options.width,
                      options.height,
                      options.enableValidation,
                      options.enableGPUValidation);
    }

    // Create and configure engine
    Engine engine;
    EngineConfig engineConfig;
    engineConfig.enableJobSystem = false;
    engineConfig.renderRuntime.backendType = options.backend;
    engineConfig.renderRuntime.enableValidation = options.enableValidation;
    engineConfig.renderRuntime.enableGPUValidation = options.enableGPUValidation;
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
    windowConfig.graphicsApi = options.backend == RHIBackendType::OpenGL ?
        WindowGraphicsApi::OpenGL :
        WindowGraphicsApi::None;
    windowSubsystem->SetConfig(windowConfig);

    // Add resource subsystem (must be added before loading any resources)
    auto* resourceSubsystem = engine.AddSubsystem<Resource::ResourceSubsystem>();
    (void)resourceSubsystem;  // Will be used via ResourceManager::Get()
    ResourceSceneAdapters::RegisterDefaults();

    // Add input subsystem for mouse control
    auto* inputSubsystem = engine.AddSubsystem<InputSubsystem>();
    (void)inputSubsystem;  // Will be retrieved after init

    // Add render subsystem
    auto* renderSubsystem = engine.AddSubsystem<RenderSubsystem>();
    
    // Add particle subsystem after its render/resource dependencies are registered.
    auto* particleSubsystem = engine.AddSubsystem<Particle::ParticleSubsystem>();
    if (options.particleTestScene)
    {
        auto& particleConfig = particleSubsystem->GetConfig();
        particleConfig.enableGPUSimulation = false;
        particleConfig.enableSorting = false;
        particleConfig.enableSoftParticles = false;
        particleConfig.deterministicCpuSimulation = true;
        particleConfig.cpuSimulationSeed = 0x5EED1234u;
    }

    // Initialize engine
    engine.Initialize();

    if (!engine.IsInitialized())
    {
        RVX_CORE_ERROR("Failed to initialize engine");
        return -1;
    }

    RenderFrameSettings frameSettings = engine.GetRenderFrameSettings();
    RenderToneMappingOperator toneMapping =
        RenderToneMappingOperator::None;
    if (TryGetToneMappingOperator(options.tonemapSelection, toneMapping))
        frameSettings.postProcess.toneMappingOperator = toneMapping;
    if (options.cameraEV100Set)
    {
        frameSettings.postProcess.exposureMode = RenderExposureMode::CameraEV100;
        frameSettings.postProcess.cameraEV100 = options.cameraEV100;
        frameSettings.postProcess.exposureCompensationEV =
            options.exposureCompensationSet ? options.exposureCompensationEV
                                            : 0.0f;
    }
    else if (options.postExposureSet)
    {
        frameSettings.postProcess.exposureMode =
            RenderExposureMode::ManualMultiplier;
        frameSettings.postProcess.exposure = options.postExposure;
    }
    if (options.displayGammaSet)
        frameSettings.postProcess.gamma = options.displayGamma;
    if (options.bloomIntensitySet)
        frameSettings.postProcess.bloomIntensity = options.bloomIntensity;
    if (options.bloomThresholdSet)
        frameSettings.postProcess.bloomThreshold = options.bloomThreshold;
    if (options.bloomRadiusSet)
        frameSettings.postProcess.bloomRadius = options.bloomRadius;
    frameSettings.postProcess.enableRayTracedReflectionDenoise =
        options.enableRayTracedReflectionDenoise;

    const bool rayTracedShadowRequested =
        options.expectRayTracedShadowReady ||
        options.expectRayTracedShadowAlphaReady ||
        options.expectRayTracedShadowHistoryReady ||
        options.expectRayTracedShadowHistoryResetReady ||
        options.expectRayTracedShadowHistoryResizeReady;
    frameSettings.rayTracing.enabled =
        options.enableRayTracedReflections || rayTracedShadowRequested;
    frameSettings.rayTracing.enableReflections =
        options.enableRayTracedReflections;
    frameSettings.rayTracing.enableShadows = rayTracedShadowRequested;
    frameSettings.rayTracing.budgetEnabled =
        options.rayTracingMaxRayCount > 0 ||
        options.rayTracingMaxDenoiseTapCount > 0 ||
        options.rayTracingMaxResourceBytes > 0 ||
        options.rayTracingMaxGpuMs > 0.0f ||
        options.rayTracingMaxShadowGpuMs > 0.0f ||
        options.rayTracingMaxReflectionGpuMs > 0.0f;
    frameSettings.rayTracing.maxRayCount = options.rayTracingMaxRayCount;
    frameSettings.rayTracing.maxDenoiseTapCount =
        options.rayTracingMaxDenoiseTapCount;
    frameSettings.rayTracing.maxTrackedResourceBytes =
        options.rayTracingMaxResourceBytes;
    frameSettings.rayTracing.maxMeasuredGpuMs = options.rayTracingMaxGpuMs;
    frameSettings.rayTracing.maxShadowMeasuredGpuMs =
        options.rayTracingMaxShadowGpuMs;
    frameSettings.rayTracing.maxReflectionMeasuredGpuMs =
        options.rayTracingMaxReflectionGpuMs;
    if (options.rayTracingGpuAdjustmentFrameCount > 0)
        frameSettings.rayTracing.gpuTimingAdjustmentFrameCount =
            options.rayTracingGpuAdjustmentFrameCount;

    frameSettings.shadows =
        MakeShadowQualityConfig(options.shadowQualityPreset);
    frameSettings.gpuCulling.enabled = !options.disableGPUDrivenCulling;
    if (options.gpuDrivenCullingTestScene)
    {
        frameSettings.gpuCulling.enableDistanceCulling = true;
        frameSettings.gpuCulling.enableOcclusionCulling = false;
        frameSettings.gpuCulling.maxDrawDistance = 5.0f;
    }
    if (!engine.SetRenderFrameSettings(frameSettings))
    {
        RVX_CORE_ERROR("ModelViewer render frame settings were rejected");
        engine.Shutdown();
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
    Vec3 cameraPos = options.materialTestScene ? Vec3(0.0f, 0.45f, 2.4f) :
                     (options.shadowTestScene ? Vec3(0.0f, 1.35f, 4.0f) :
                      (options.smoke ? Vec3(0.0f, 1.5f, 4.0f) : Vec3(0.0f, 2.0f, 5.0f)));
    Vec3 target = options.materialTestScene ? Vec3(0.0f, 0.18f, 0.0f) :
                  (options.shadowTestScene ? Vec3(0.0f, 0.35f, 0.0f) : Vec3(0.0f, 0.0f, 0.0f));
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

    if (options.shadowTestScene && !ConfigureShadowTestLight(sceneManager))
    {
        engine.Shutdown();
        return -1;
    }

    if (options.materialTestScene && !ConfigureMaterialTestLight(sceneManager))
    {
        engine.Shutdown();
        return -1;
    }

    HDRIEnvironmentResources hdriEnvironmentResources;
    ProceduralIBLResources proceduralIBLResources;
    if (!options.hdriPath.empty())
    {
        ActorSpawnParams skyboxParams;
        skyboxParams.name = "ModelViewerHDRIEnvironment";
        SceneEntity* skyboxEntity = sceneManager->SpawnActor(skyboxParams);
        SkyboxComponent* skyboxComponent = skyboxEntity ? skyboxEntity->AddComponent<SkyboxComponent>() : nullptr;
        if (!skyboxComponent)
        {
            RVX_CORE_ERROR("ModelViewer could not create HDRI SkyboxComponent");
            engine.Shutdown();
            return -1;
        }

        hdriEnvironmentResources = LoadHDRIEnvironment(options.hdriPath, options.smoke);
        if (!hdriEnvironmentResources.IsValid())
        {
            engine.Shutdown();
            return -1;
        }

        skyboxComponent->SetCubemap(hdriEnvironmentResources.environment);
        skyboxComponent->SetIrradianceMap(hdriEnvironmentResources.irradiance);
        skyboxComponent->SetPrefilteredMap(hdriEnvironmentResources.prefiltered);
        skyboxComponent->SetBRDFLUT(hdriEnvironmentResources.brdfLUT);
        skyboxComponent->SetExposure(1.0f);
        skyboxComponent->SetContributesToLighting(true);

        if (PublishHDRIEnvironment(resourceSubsystem, hdriEnvironmentResources))
        {
            RVX_CORE_INFO("ModelViewer HDRI environment resources published");
        }
        else
        {
            RVX_CORE_ERROR("ModelViewer HDRI environment resource publication failed");
            engine.Shutdown();
            return -1;
        }
    }
    else if (options.enableProceduralIBL)
    {
        ActorSpawnParams skyboxParams;
        skyboxParams.name = "ModelViewerProceduralIBL";
        SceneEntity* skyboxEntity = sceneManager->SpawnActor(skyboxParams);
        SkyboxComponent* skyboxComponent = skyboxEntity ? skyboxEntity->AddComponent<SkyboxComponent>() : nullptr;
        if (skyboxComponent)
        {
            proceduralIBLResources = CreateProceduralIBLResources(options.smoke);
            skyboxComponent->SetSkyboxType(SkyboxType::Procedural);
            skyboxComponent->SetSunDirection(Vec3{0.35f, 0.65f, 0.45f});
            skyboxComponent->SetSunColor(Vec3{1.0f, 0.94f, 0.82f});
            skyboxComponent->SetZenithColor(Vec3{0.12f, 0.28f, 0.58f});
            skyboxComponent->SetHorizonColor(Vec3{0.55f, 0.68f, 0.82f});
            skyboxComponent->SetGroundColor(Vec3{0.08f, 0.09f, 0.11f});
            skyboxComponent->SetScatteringIntensity(0.65f);
            skyboxComponent->SetIrradianceMap(proceduralIBLResources.irradiance);
            skyboxComponent->SetPrefilteredMap(proceduralIBLResources.prefiltered);
            skyboxComponent->SetBRDFLUT(proceduralIBLResources.brdfLUT);
            skyboxComponent->SetExposure(1.0f);
            skyboxComponent->SetContributesToLighting(true);

            if (PublishProceduralIBL(resourceSubsystem, proceduralIBLResources))
            {
                RVX_CORE_INFO("ModelViewer procedural IBL resources published");
            }
            else
            {
                RVX_CORE_WARN("ModelViewer procedural IBL resource publication failed");
            }
        }
        else
        {
            RVX_CORE_WARN("ModelViewer could not create procedural IBL SkyboxComponent");
        }
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

                if (options.gpuDrivenCullingTestScene)
                {
                    SceneEntity* culledEntity = modelHandle->Instantiate(sceneManager);
                    if (!culledEntity)
                    {
                        RVX_CORE_ERROR("Failed to instantiate GPU-driven culling test entity");
                        engine.Shutdown();
                        return -1;
                    }

                    culledEntity->SetPosition(Vec3(3.0f, 0.0f, -8.0f));
                    RVX_CORE_INFO("GPU-driven culling test entity ready: {} at ({}, {}, {})",
                                  culledEntity->GetName(),
                                  culledEntity->GetPosition().x,
                                  culledEntity->GetPosition().y,
                                  culledEntity->GetPosition().z);
                }
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

    if (options.particleTestScene)
    {
        ActorSpawnParams particleParams;
        particleParams.name = "ModelViewerParticleTest";
        particleParams.localPosition = Vec3(0.0f, 0.65f, 1.15f);

        SceneEntity* particleEntity = sceneManager->SpawnActor(particleParams);
        auto* particleComponent = particleEntity ? particleEntity->AddComponent<Particle::ParticleComponent>() : nullptr;
        if (!particleEntity || !particleComponent)
        {
            RVX_CORE_ERROR("ModelViewer could not create particle test scene");
            engine.Shutdown();
            return -1;
        }

        auto particleSystem = Particle::ParticleSystem::CreateSimple("ModelViewerParticleSmoke");
        particleSystem->maxParticles = 64;
        particleSystem->duration = 4.0f;
        particleSystem->looping = true;
        particleSystem->prewarm = false;
        particleSystem->renderMode = Particle::ParticleRenderMode::Billboard;
        particleSystem->blendMode = Particle::ParticleBlendMode::Additive;
        particleSystem->sortMode = Particle::ParticleSortMode::None;
        particleSystem->softParticleConfig.enabled = false;
        particleSystem->modules.clear();

        if (auto* emitter = particleSystem->GetEmitter(0))
        {
            emitter->emissionRate = 360.0f;
            emitter->initialLifetime = Particle::FloatRange(4.0f);
            emitter->initialSpeed = Particle::FloatRange(0.0f);
            emitter->initialVelocityDirection = Particle::Vec3Range(Vec3(0.0f, 1.0f, 0.0f));
            emitter->initialColor = Particle::Vec4Range(Vec4(1.0f, 0.62f, 0.18f, 0.85f));
            emitter->initialSize = Particle::FloatRange(0.55f);
            emitter->initialRotation = Particle::FloatRange(0.0f);
            emitter->rotationSpeed = Particle::FloatRange(0.0f);
            emitter->useShapeVelocity = false;
        }

        particleComponent->SetParticleSystem(particleSystem);
        particleComponent->Play();

        RVX_CORE_INFO("ModelViewer particle test scene ready at ({}, {}, {})",
                      particleParams.localPosition.x,
                      particleParams.localPosition.y,
                      particleParams.localPosition.z);
    }

    if (options.smoke)
    {
        RVX_CORE_INFO("Running deterministic smoke loop");
        bool smokeSucceeded = true;
        bool rayTracedShadowHistoryResetObserved = false;
        bool rayTracedShadowHistoryResizeObserved = false;
        bool rayTracedReflectionHistoryResetObserved = false;
        bool rayTracedReflectionHistoryResizeObserved = false;
        bool gpuBudgetRecoveryTriggered = false;
        float gpuBudgetRecoveryBaselineReflectionScale = 0.0f;
        float gpuBudgetRecoveryBaselineReflectionQualityScale = 1.0f;
        uint32 gpuBudgetRecoveryTriggerFrame = 0;
        RuntimeFrameDriver frameDriver(engine, *renderSubsystem);
        RenderDiagnosticsSnapshot lastDiagnostics =
            renderSubsystem->GetDiagnosticsSnapshot();
        constexpr uint64 captureRequestId = 1;

        for (uint32 frameIndex = 0; frameIndex < options.frames; ++frameIndex)
        {
            cameraPos = options.materialTestScene ? Vec3(0.0f, 0.45f, 2.4f) :
                        (options.shadowTestScene ? Vec3(0.0f, 1.35f, 4.0f) : Vec3(0.0f, 1.5f, 4.0f));
            camera->SetPosition(cameraPos);
            camera->LookAt(target);

            const uint32 smokeFrameNumber = frameIndex + 1;
            const bool captureFrame = !options.screenshotPath.empty() && (smokeFrameNumber == options.frames);

            if (options.rayTracingHistoryResetFrame > 0u &&
                smokeFrameNumber == options.rayTracingHistoryResetFrame)
            {
                static_cast<void>(engine.RequestRenderTemporalReset());
                RVX_CORE_INFO("ModelViewer smoke requested temporal history reset on frame {}", smokeFrameNumber);
            }

            if (options.rayTracingResizeFrame > 0u && smokeFrameNumber == options.rayTracingResizeFrame)
            {
                if (!engine.RequestRenderSurfaceResize(
                        options.rayTracingResizeWidth,
                        options.rayTracingResizeHeight))
                {
                    RVX_CORE_ERROR("ModelViewer smoke render resize request was rejected");
                    smokeSucceeded = false;
                }
                camera->SetPerspective(glm::radians(45.0f),
                                       static_cast<float>(options.rayTracingResizeWidth) /
                                           static_cast<float>(options.rayTracingResizeHeight),
                                       0.1f,
                                       1000.0f);
                RVX_CORE_INFO("ModelViewer smoke resized RT history viewport on frame {}: {}x{}",
                              smokeFrameNumber,
                              options.rayTracingResizeWidth,
                              options.rayTracingResizeHeight);
            }

            if (captureFrame)
            {
                RenderFrameCaptureRequest capture;
                capture.requestId = captureRequestId;
                capture.kind = RenderFrameCaptureKind::Color;
                capture.width = options.rayTracingResizeFrame > 0
                                    ? options.rayTracingResizeWidth
                                    : options.width;
                capture.height = options.rayTracingResizeFrame > 0
                                     ? options.rayTracingResizeHeight
                                     : options.height;
                if (!engine.RequestRenderFrameCapture(capture))
                {
                    RVX_CORE_ERROR("ModelViewer smoke frame capture request was rejected");
                    smokeSucceeded = false;
                }
            }

            const uint64 nextPresentedSequence =
                lastDiagnostics.lastPresentedFrameSequence + 1;
            lastDiagnostics = frameDriver.TickOnce(kSmokeDeltaSeconds);
            RuntimeFrameWaitRequest waitRequest;
            waitRequest.minimumPresentedSequence = nextPresentedSequence;
            waitRequest.captureRequestId = captureFrame ? captureRequestId : 0;
            waitRequest.maxTicks = options.enableGPUValidation ? 120000 : 5000;
            waitRequest.advanceEngine = false;
            const RuntimeFrameWaitResult waitResult =
                frameDriver.WaitFor(waitRequest);
            lastDiagnostics = waitResult.diagnostics;
            if (!waitResult.Reached())
            {
                RVX_CORE_ERROR(
                    "ModelViewer smoke frame {} did not reach Render: waitCode={}, "
                    "lifecycle={}, lastPresented={}, failure={}",
                    smokeFrameNumber,
                    static_cast<uint32>(waitResult.code),
                    static_cast<uint32>(lastDiagnostics.lifecycle),
                    lastDiagnostics.lastPresentedFrameSequence,
                    lastDiagnostics.lastFailure.context);
                smokeSucceeded = false;
                break;
            }
            const RenderFrameFeatureDiagnostics* sceneRenderer =
                &lastDiagnostics.frameFeatures;

            if (options.expectRayTracingGpuBudgetRecovered &&
                !gpuBudgetRecoveryTriggered &&
                (frameIndex + 1 < options.frames))
            {
                std::string gpuBudgetFallbackReason;
                if (IsRayTracingGpuBudgetApplied(sceneRenderer, gpuBudgetFallbackReason))
                {
        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
                    const bool validReflectionBaseline =
                        stats.reflectionRequested &&
                        stats.reflectionGpuTimeBudgetApplied &&
                        stats.reflectionResolutionScale > 0.0f &&
                        stats.reflectionResolutionScale < 0.999f &&
                        stats.reflectionGpuTimeBudgetQualityScale > 0.0f &&
                        stats.reflectionGpuTimeBudgetQualityScale < 0.999f;
                    if (validReflectionBaseline)
                    {
                        gpuBudgetRecoveryBaselineReflectionScale = stats.reflectionResolutionScale;
                        gpuBudgetRecoveryBaselineReflectionQualityScale =
                            stats.reflectionGpuTimeBudgetQualityScale;
                        gpuBudgetRecoveryTriggerFrame = frameIndex + 1;
                        ApplyRayTracingGpuRecoveryBudgetSettings(
                            engine,
                            frameSettings,
                            options);
                        gpuBudgetRecoveryTriggered = true;
                        RVX_CORE_INFO("ModelViewer smoke ray tracing GPU budget recovery armed: frame={}, "
                                      "baselineReflectionScale={:.3f}, baselineReflectionGpuScale={:.3f}, "
                                      "measuredReflectionMs={:.6f}, maxReflectionGpuMs={:.6f}",
                                      gpuBudgetRecoveryTriggerFrame,
                                      gpuBudgetRecoveryBaselineReflectionScale,
                                      gpuBudgetRecoveryBaselineReflectionQualityScale,
                                      stats.measuredReflectionGpuTimeForBudgetMs,
                                      stats.reflectionGpuTimeBudget);
                    }
                }
            }

            if (options.expectIBLReady && (frameIndex + 1 == options.frames))
            {
                if (!sceneRenderer || !sceneRenderer->textureIBLEnabled)
                {
                    const std::string fallbackReason =
                        sceneRenderer && !sceneRenderer->fallbackReasons.empty()
                            ? sceneRenderer->fallbackReasons.front()
                            : "TextureIBLDisabled";
                    RVX_CORE_ERROR("ModelViewer smoke expected texture IBL ready; fallback reason: {}",
                                   fallbackReason);
                    smokeSucceeded = false;
                }
            }

            if (options.expectSkyboxReady && (frameIndex + 1 == options.frames))
            {
                std::string skyboxFallbackReason;
                if (!IsSkyboxPassReady(sceneRenderer, skyboxFallbackReason))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected SkyboxPass ready; fallback reason: {}",
                                   skyboxFallbackReason);
                    smokeSucceeded = false;
                }
            }

            if (options.expectShadowReady && (frameIndex + 1 == options.frames))
            {
                std::string shadowFallbackReason;
                if (!IsDirectionalShadowReady(sceneRenderer, shadowFallbackReason))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected directional shadow ready; fallback reason: {}",
                                   shadowFallbackReason);
                    smokeSucceeded = false;
                }
                else
                {
                    RVX_CORE_INFO("ModelViewer smoke directional shadow ready: shadowSamplingEnabled=true, "
                                  "fallbackReason=None");
                }
            }

            if (options.expectGPUDrivenCullingReady && (frameIndex + 1 == options.frames))
            {
                std::string gpuDrivenFallbackReason;
                if (!IsGPUDrivenCullingReady(sceneRenderer,
                                             options.gpuDrivenCullingTestScene,
                                             gpuDrivenFallbackReason))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected GPU-driven culling ready; stats: {}",
                                   gpuDrivenFallbackReason);
                    smokeSucceeded = false;
                }
                else
                {
                    const RenderGPUDrivenCullingDiagnostics& stats =
                        sceneRenderer->gpuDrivenCulling;
                    RVX_CORE_INFO("ModelViewer smoke GPU-driven culling ready: graphInput={}, "
                                  "visibleCullable={}, opaqueIndirectBatches={}, opaqueIndirectDraws={}",
                                  stats.graphInputDrawItemCount,
                                  stats.visibleCullableDrawItemCount,
                                  stats.opaqueGpuDrivenIndirectBatchCount,
                                  stats.opaqueGpuDrivenIndirectDrawCount);
                }
            }

            if (options.expectParticlesReady && (frameIndex + 1 == options.frames))
            {
                std::string particleFallbackReason;
                if (!IsParticleRuntimeReady(sceneRenderer, particleFallbackReason))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected particles ready; stats: {}",
                                   particleFallbackReason);
                    smokeSucceeded = false;
                }
                else
                {
                    const RenderParticleFeatureDiagnostics& stats =
                        sceneRenderer->particles;
                    RVX_CORE_INFO("ModelViewer smoke particles ready: items={}, payloadReady={}, totalAlive={}",
                                  stats.itemCount,
                                  stats.renderPayloadReadyItemCount,
                                  stats.totalAliveParticles);
                }
            }

            if (options.expectRayTracedShadowReady && (frameIndex + 1 == options.frames))
            {
                std::string rayTracingFallbackReason;
                if (!IsRayTracedShadowReady(sceneRenderer, rayTracingFallbackReason))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected ray-traced shadow ready; stats: {}",
                                   rayTracingFallbackReason);
                    smokeSucceeded = false;
                }
                else
                {
        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
                    RVX_CORE_INFO("ModelViewer smoke ray-traced shadow ready: size={}x{}, estimatedRays={}",
                                  stats.shadowWidth,
                                  stats.shadowHeight,
                                  stats.estimatedShadowRayCount);
                }
            }


            if (options.expectRayTracedShadowAlphaReady && (frameIndex + 1 == options.frames))
            {
                std::string rayTracingAlphaFallbackReason;
                if (!IsRayTracedShadowAlphaReady(sceneRenderer, rayTracingAlphaFallbackReason))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected ray-traced shadow alpha resources ready; stats: {}",
                                   rayTracingAlphaFallbackReason);
                    smokeSucceeded = false;
                }
                else
                {
        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
                    RVX_CORE_INFO("ModelViewer smoke ray-traced shadow alpha ready: alphaTextures={}/{}, "
                                  "materialTextures={}/{}, alphaIndexBuffers={}, alphaUVBuffers={}",
                                  stats.shadowAlphaTexturesBound,
                                  stats.shadowAlphaTextureCount,
                                  stats.shadowMaterialTexturesBound,
                                  stats.shadowMaterialTextureCount,
                                  stats.shadowAlphaIndexBufferCount,
                                  stats.shadowAlphaUVBufferCount);
                }
            }

            if (options.expectRayTracedShadowHistoryResetReady &&
                smokeFrameNumber == options.rayTracingHistoryResetFrame)
            {
                std::string rayTracingHistoryResetFallbackReason;
                if (!IsRayTracedShadowHistoryResetReady(sceneRenderer, rayTracingHistoryResetFallbackReason))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected ray-traced shadow history reset ready; stats: {}",
                                   rayTracingHistoryResetFallbackReason);
                    smokeSucceeded = false;
                }
                else
                {
        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
                    rayTracedShadowHistoryResetObserved = true;
                    RVX_CORE_INFO("ModelViewer smoke ray-traced shadow history reset ready: frame={}, "
                                  "historyReset={}, temporalAccumulated={}",
                                  smokeFrameNumber,
                                  stats.shadowHistoryReset,
                                  stats.shadowTemporalAccumulated);
                }
            }

            if (options.expectRayTracedShadowHistoryResizeReady &&
                smokeFrameNumber == options.rayTracingResizeFrame)
            {
                std::string rayTracingHistoryResizeFallbackReason;
                if (!IsRayTracedShadowHistoryResizeReady(sceneRenderer,
                                                         options.rayTracingResizeWidth,
                                                         options.rayTracingResizeHeight,
                                                         rayTracingHistoryResizeFallbackReason))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected ray-traced shadow history resize ready; stats: {}",
                                   rayTracingHistoryResizeFallbackReason);
                    smokeSucceeded = false;
                }
                else
                {
        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
                    rayTracedShadowHistoryResizeObserved = true;
                    RVX_CORE_INFO("ModelViewer smoke ray-traced shadow history resize ready: frame={}, "
                                  "size={}x{}, historyRecreated={}, historyResolutionChanged={}, "
                                  "temporalAccumulated={}",
                                  smokeFrameNumber,
                                  stats.shadowWidth,
                                  stats.shadowHeight,
                                  stats.shadowHistoryRecreated,
                                  stats.shadowHistoryResolutionChanged,
                                  stats.shadowTemporalAccumulated);
                }
            }

            if (options.expectRayTracedShadowHistoryReady && (frameIndex + 1 == options.frames))
            {
                std::string rayTracingHistoryFallbackReason;
                if (!IsRayTracedShadowHistoryReady(sceneRenderer, rayTracingHistoryFallbackReason))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected ray-traced shadow history ready; stats: {}",
                                   rayTracingHistoryFallbackReason);
                    smokeSucceeded = false;
                }
                else
                {
        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
                    RVX_CORE_INFO("ModelViewer smoke ray-traced shadow history ready: historyAvailable={}, "
                                  "depthHistory={}, normalHistory={}, temporalAccumulated={}",
                                  stats.shadowHistoryAvailable,
                                  stats.shadowDepthHistoryAvailable,
                                  stats.shadowNormalHistoryAvailable,
                                  stats.shadowTemporalAccumulated);
                }
            }

            if (options.expectRayTracedReflectionReady && (frameIndex + 1 == options.frames))
            {
                std::string rayTracingFallbackReason;
                if (!IsRayTracedReflectionReady(sceneRenderer,
                                                   options.enableRayTracedReflectionDenoise,
                                                   rayTracingFallbackReason))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected ray-traced reflection ready; stats: {}",
                                   rayTracingFallbackReason);
                    smokeSucceeded = false;
                }
                else
                {
        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
                    RVX_CORE_INFO("ModelViewer smoke ray-traced reflection ready: size={}x{}, denoiseRadius={}, "
                                  "estimatedRays={}, estimatedDenoiseTaps={}",
                                  stats.reflectionWidth,
                                  stats.reflectionHeight,
                                  stats.reflectionDenoiseRadius,
                                  stats.estimatedReflectionRayCount,
                                  stats.estimatedReflectionDenoiseTapCount);
                }
            }


            if (options.expectRayTracedReflectionAssetReady && (frameIndex + 1 == options.frames))
            {
                std::string rayTracingAssetFallbackReason;
                if (!IsRayTracedReflectionAssetReady(sceneRenderer, rayTracingAssetFallbackReason))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected ray-traced reflection asset resources ready; stats: {}",
                                   rayTracingAssetFallbackReason);
                    smokeSucceeded = false;
                }
                else
                {
        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
                    RVX_CORE_INFO("ModelViewer smoke ray-traced reflection asset ready: materialTextures={}/{}, "
                                  "geometryBuffers=index:{}, uv:{}, normal:{}, tangent:{}",
                                  stats.reflectionMaterialTexturesBound,
                                  stats.reflectionMaterialTextureCount,
                                  stats.reflectionGeometryIndexBufferCount,
                                  stats.reflectionGeometryUVBufferCount,
                                  stats.reflectionGeometryNormalBufferCount,
                                  stats.reflectionGeometryTangentBufferCount);
                }
            }
            if (options.expectRayTracedReflectionHistoryResetReady &&
                smokeFrameNumber == options.rayTracingHistoryResetFrame)
            {
                std::string rayTracingHistoryResetFallbackReason;
                if (!IsRayTracedReflectionHistoryResetReady(sceneRenderer, rayTracingHistoryResetFallbackReason))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected ray-traced reflection history reset ready; stats: {}",
                                   rayTracingHistoryResetFallbackReason);
                    smokeSucceeded = false;
                }
                else
                {
        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
                    rayTracedReflectionHistoryResetObserved = true;
                    RVX_CORE_INFO("ModelViewer smoke ray-traced reflection history reset ready: frame={}, "
                                  "historyReset={}, temporalAccumulated={}",
                                  smokeFrameNumber,
                                  stats.reflectionHistoryReset,
                                  stats.reflectionTemporalAccumulated);
                }
            }

            if (options.expectRayTracedReflectionHistoryResizeReady &&
                smokeFrameNumber == options.rayTracingResizeFrame)
            {
                std::string rayTracingHistoryResizeFallbackReason;
                if (!IsRayTracedReflectionHistoryResizeReady(sceneRenderer,
                                                             options.rayTracingResizeWidth,
                                                             options.rayTracingResizeHeight,
                                                             rayTracingHistoryResizeFallbackReason))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected ray-traced reflection history resize ready; stats: {}",
                                   rayTracingHistoryResizeFallbackReason);
                    smokeSucceeded = false;
                }
                else
                {
        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
                    rayTracedReflectionHistoryResizeObserved = true;
                    RVX_CORE_INFO("ModelViewer smoke ray-traced reflection history resize ready: frame={}, "
                                  "size={}x{}, historyRecreated={}, historyResolutionChanged={}, "
                                  "temporalAccumulated={}",
                                  smokeFrameNumber,
                                  stats.reflectionWidth,
                                  stats.reflectionHeight,
                                  stats.reflectionHistoryRecreated,
                                  stats.reflectionHistoryResolutionChanged,
                                  stats.reflectionTemporalAccumulated);
                }
            }

            if (options.expectRayTracedReflectionHistoryReady && (frameIndex + 1 == options.frames))
            {
                std::string rayTracingHistoryFallbackReason;
                if (!IsRayTracedReflectionHistoryReady(sceneRenderer, rayTracingHistoryFallbackReason))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected ray-traced reflection history ready; stats: {}",
                                   rayTracingHistoryFallbackReason);
                    smokeSucceeded = false;
                }
                else
                {
        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
                    RVX_CORE_INFO("ModelViewer smoke ray-traced reflection history ready: historyAvailable={}, "
                                  "depthHistory={}, normalHistory={}, temporalAccumulated={}, size={}x{}",
                                  stats.reflectionHistoryAvailable,
                                  stats.reflectionDepthHistoryAvailable,
                                  stats.reflectionNormalHistoryAvailable,
                                  stats.reflectionTemporalAccumulated,
                                  stats.reflectionWidth,
                                  stats.reflectionHeight);
                }
            }
            if (options.expectRayTracedShadowHistoryResetReady &&
                !rayTracedShadowHistoryResetObserved &&
                (frameIndex + 1 == options.frames))
            {
                RVX_CORE_ERROR("ModelViewer smoke expected ray-traced shadow history reset, but reset frame {} "
                               "was not observed",
                               options.rayTracingHistoryResetFrame);
                smokeSucceeded = false;
            }

            if (options.expectRayTracedReflectionHistoryResetReady &&
                !rayTracedReflectionHistoryResetObserved &&
                (frameIndex + 1 == options.frames))
            {
                RVX_CORE_ERROR("ModelViewer smoke expected ray-traced reflection history reset, but reset frame {} "
                               "was not observed",
                               options.rayTracingHistoryResetFrame);
                smokeSucceeded = false;
            }

            if (options.expectRayTracedShadowHistoryResizeReady &&
                !rayTracedShadowHistoryResizeObserved &&
                (frameIndex + 1 == options.frames))
            {
                RVX_CORE_ERROR("ModelViewer smoke expected ray-traced shadow history resize, but resize frame {} "
                               "was not observed",
                               options.rayTracingResizeFrame);
                smokeSucceeded = false;
            }

            if (options.expectRayTracedReflectionHistoryResizeReady &&
                !rayTracedReflectionHistoryResizeObserved &&
                (frameIndex + 1 == options.frames))
            {
                RVX_CORE_ERROR("ModelViewer smoke expected ray-traced reflection history resize, but resize frame {} "
                               "was not observed",
                               options.rayTracingResizeFrame);
                smokeSucceeded = false;
            }

            if (options.expectRayTracingGpuTimingReady && (frameIndex + 1 == options.frames))
            {
                std::string timingFallbackReason;
                if (!IsRayTracingGpuTimingReady(sceneRenderer, timingFallbackReason))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected ray tracing GPU timing ready; stats: {}",
                                   timingFallbackReason);
                    smokeSucceeded = false;
                }
                else
                {
        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
                    RVX_CORE_INFO("ModelViewer smoke ray tracing GPU timing ready: shadowResult={}, "
                                  "shadowMs={:.4f}, reflectionResult={}, reflectionMs={:.4f}",
                                  stats.shadowGpuTimingResultAvailable,
                                  stats.shadowGpuTimingElapsedMs,
                                  stats.reflectionGpuTimingResultAvailable,
                                  stats.reflectionGpuTimingElapsedMs);
                }
            }

            if (options.expectRayTracingGpuBudgetApplied && (frameIndex + 1 == options.frames))
            {
                std::string gpuBudgetFallbackReason;
                if (!IsRayTracingGpuBudgetApplied(sceneRenderer, gpuBudgetFallbackReason))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected ray tracing GPU budget applied; stats: {}",
                                   gpuBudgetFallbackReason);
                    smokeSucceeded = false;
                }
                else
                {
        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
                    RVX_CORE_INFO("ModelViewer smoke ray tracing GPU budget applied: measuredGpuMs={:.6f}, "
                                  "measuredReflectionMs={:.6f}, maxGpuMs={:.6f}, maxReflectionGpuMs={:.6f}, "
                                  "gpuScale={:.3f}, reflectionGpuScale={:.3f}, reflectionScale={:.3f}",
                                  stats.measuredGpuTimeForBudgetMs,
                                  stats.measuredReflectionGpuTimeForBudgetMs,
                                  stats.gpuTimeBudget,
                                  stats.reflectionGpuTimeBudget,
                                  stats.gpuTimeBudgetQualityScale,
                                  stats.reflectionGpuTimeBudgetQualityScale,
                                  stats.reflectionResolutionScale);
                }
            }

            if (options.expectRayTracingGpuBudgetRecovered && (frameIndex + 1 == options.frames))
            {
                std::string recoveryFallbackReason;
                if (!gpuBudgetRecoveryTriggered)
                {
                    const RenderRayTracingDiagnostics stats =
                        sceneRenderer ? sceneRenderer->rayTracing
                                      : RenderRayTracingDiagnostics{};
                    RVX_CORE_ERROR("ModelViewer smoke expected ray tracing GPU budget recovery, but no "
                                   "degraded reflection baseline was observed; stats: {}",
                                   DescribeRayTracingGpuBudgetReadiness(stats));
                    smokeSucceeded = false;
                }
                else if (!IsRayTracingGpuBudgetRecovered(sceneRenderer,
                                                         gpuBudgetRecoveryBaselineReflectionScale,
                                                         gpuBudgetRecoveryBaselineReflectionQualityScale,
                                                         recoveryFallbackReason))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected ray tracing GPU budget recovered; stats: {}",
                                   recoveryFallbackReason);
                    smokeSucceeded = false;
                }
                else
                {
        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
                    RVX_CORE_INFO("ModelViewer smoke ray tracing GPU budget recovered: triggerFrame={}, "
                                  "baselineReflectionScale={:.3f}, reflectionScale={:.3f}, "
                                  "baselineReflectionGpuScale={:.3f}, reflectionGpuScale={:.3f}, "
                                  "measuredReflectionMs={:.6f}, maxReflectionGpuMs={:.6f}",
                                  gpuBudgetRecoveryTriggerFrame,
                                  gpuBudgetRecoveryBaselineReflectionScale,
                                  stats.reflectionResolutionScale,
                                  gpuBudgetRecoveryBaselineReflectionQualityScale,
                                  stats.reflectionGpuTimeBudgetQualityScale,
                                  stats.measuredReflectionGpuTimeForBudgetMs,
                                  stats.reflectionGpuTimeBudget);
                }
            }

            if (options.expectRayTracingResourceBudgetExceeded && (frameIndex + 1 == options.frames))
            {
                std::string resourceBudgetFallbackReason;
                if (!IsRayTracingResourceBudgetExceeded(sceneRenderer, resourceBudgetFallbackReason))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected ray tracing resource budget exceeded; stats: {}",
                                   resourceBudgetFallbackReason);
                    smokeSucceeded = false;
                }
                else
                {
        const RenderRayTracingDiagnostics stats = sceneRenderer->rayTracing;
                    RVX_CORE_INFO("ModelViewer smoke ray tracing resource budget exceeded: trackedBytes={} > maxBytes={}",
                                  stats.totalTrackedResourceBytes,
                                  stats.trackedResourceBudget);
                }
            }

            if ((options.expectRayTracingBudgetApplied || options.expectRayTracingRayBudgetRespected) &&
                (frameIndex + 1 == options.frames))
            {
                const RenderRayTracingDiagnostics stats =
                    sceneRenderer ? sceneRenderer->rayTracing
                                  : RenderRayTracingDiagnostics{};
                if (options.expectRayTracingBudgetApplied && (!stats.budgetEnabled || !stats.budgetApplied))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected ray tracing budget applied; stats: {}",
                                   DescribeRayTracingBudgetReadiness(stats));
                    smokeSucceeded = false;
                }
                else if (options.expectRayTracingBudgetApplied)
                {
                    RVX_CORE_INFO("ModelViewer smoke ray tracing budget applied: maxRays={}, estimatedRays={}, "
                                  "maxDenoiseTaps={}, estimatedDenoiseTaps={}, reflectionScale={:.3f}, "
                                  "reflectionSize={}x{}",
                                  stats.rayBudget,
                                  stats.estimatedTotalRayCount,
                                  stats.denoiseTapBudget,
                                  stats.estimatedReflectionDenoiseTapCount,
                                  stats.reflectionResolutionScale,
                                  stats.reflectionWidth,
                                  stats.reflectionHeight);
                }

                if (options.expectRayTracingRayBudgetRespected &&
                    (stats.rayBudget == 0 || stats.estimatedTotalRayCount > stats.rayBudget))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected ray tracing ray budget respected; stats: {}",
                                   DescribeRayTracingBudgetReadiness(stats));
                    smokeSucceeded = false;
                }
                else if (options.expectRayTracingRayBudgetRespected)
                {
                    RVX_CORE_INFO("ModelViewer smoke ray tracing ray budget respected: estimatedRays={} <= maxRays={}",
                                  stats.estimatedTotalRayCount,
                                  stats.rayBudget);
                }
            }

            if (options.expectMaterialReady && (frameIndex + 1 == options.frames))
            {
                std::string materialFallbackReason;
                if (!IsPBRMaterialReady(sceneRenderer, materialFallbackReason))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected PBR material ready; binding: {}",
                                   materialFallbackReason);
                    smokeSucceeded = false;
                }
                else
                {
                    const RenderMaterialFeatureDiagnostics& result =
                        sceneRenderer->material;
                    RVX_CORE_INFO("ModelViewer smoke PBR material ready: material='{}', textureFlags={}",
                                  result.materialName,
                                  result.textureFlags);
                }
            }

            if (options.expectProceduralIBLQuality && (frameIndex + 1 == options.frames))
            {
                if (!ValidateProceduralIBLQuality(proceduralIBLResources,
                                                  options.smoke,
                                                  resourceSubsystem))
                {
                    smokeSucceeded = false;
                }
            }

            if (captureFrame && smokeSucceeded)
            {
                if (!WriteScreenshotPPM(lastDiagnostics.lastCapture,
                                        options.screenshotPath))
                {
                    smokeSucceeded = false;
                }
            }

            if (options.disableGPUDrivenCulling &&
                options.gpuDrivenCullingTestScene &&
                (frameIndex + 1 == options.frames))
            {
                std::string gpuDrivenFallbackReason;
                if (!IsGPUDrivenDirectFallbackReady(sceneRenderer,
                                                    gpuDrivenFallbackReason))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected GPU-driven direct-draw fallback; stats: {}",
                                   gpuDrivenFallbackReason);
                    smokeSucceeded = false;
                }
                else
                {
                    const RenderGPUDrivenCullingDiagnostics& stats =
                        sceneRenderer->gpuDrivenCulling;
                    RVX_CORE_INFO("ModelViewer smoke GPU-driven direct-draw fallback ready: directDraws={}, fallbackReason={}",
                                  stats.opaqueDirectDrawCount,
                                  GetGPUDrivenDrawFallbackReasonName(
                                      stats.opaqueFallbackReason));
                }
            }

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
    const bool isVulkan =
        renderSubsystem->GetDiagnosticsSnapshot().backend ==
        RHIBackendType::Vulkan;
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
        // 3. Resource upload request publication
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
