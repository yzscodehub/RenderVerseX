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
#include "Render/Material/MaterialSystem.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/SceneRenderer.h"
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
    bool expectMaterialReady = false;
    bool expectProceduralIBLQuality = false;
    bool materialTestScene = false;
    bool shadowTestScene = false;
    bool enableValidation = true;
    bool showHelp = false;
    bool backendSet = false;
    bool widthSet = false;
    bool heightSet = false;
    bool framesSet = false;
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
    constexpr const char* kPBRMaterialTestMaterialName = "RQ4PBRMaterial";

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
            << "  --expect-ibl-ready   Smoke mode fails unless texture IBL becomes ready\n"
            << "  --expect-skybox-ready Smoke mode fails unless SkyboxPass becomes ready\n"
            << "  --expect-shadow-ready Smoke mode fails unless directional shadow sampling is ready\n"
            << "  --expect-material-ready Smoke mode fails unless the PBR material swatch binds all texture maps\n"
            << "  --expect-procedural-ibl-quality Smoke mode fails unless default procedural IBL uses the CPU HDR pipeline\n"
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
            else if (arg == "--expect-material-ready")
            {
                options.expectMaterialReady = true;
            }
            else if (arg == "--expect-procedural-ibl-quality")
            {
                options.expectProceduralIBLQuality = true;
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

        if (options.expectProceduralIBLQuality && (!options.enableProceduralIBL || !options.hdriPath.empty()))
        {
            RVX_CORE_ERROR("--expect-procedural-ibl-quality requires the default procedural IBL path");
            return false;
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

    bool UploadProceduralIBL(RenderSubsystem* renderSubsystem, const ProceduralIBLResources& resources)
    {
        if (!renderSubsystem || !renderSubsystem->GetGPUResourceManager())
            return false;

        auto* gpuResources = renderSubsystem->GetGPUResourceManager();
        auto upload = [gpuResources](const Resource::ResourceHandle<Resource::TextureResource>& texture) -> bool
        {
            if (!texture)
                return false;

            gpuResources->UploadImmediate(texture.Get());
            return true;
        };

        const bool irradianceSubmitted = upload(resources.irradiance);
        const bool prefilteredSubmitted = upload(resources.prefiltered);
        const bool brdfLUTSubmitted = upload(resources.brdfLUT);

        auto isReady = [gpuResources](const Resource::ResourceHandle<Resource::TextureResource>& texture,
                                      const char* label) -> bool
        {
            if (!texture)
            {
                RVX_CORE_WARN("ModelViewer procedural IBL {} resource is missing", label);
                return false;
            }

            const bool ready = gpuResources->IsGPUReady(texture.GetId());
            if (!ready)
            {
                RVX_CORE_WARN("ModelViewer procedural IBL {} resource '{}' ({}) is not GPU-ready",
                              label,
                              texture->GetName(),
                              texture.GetId());
            }
            return ready;
        };

        const bool irradianceReady = isReady(resources.irradiance, "irradiance");
        const bool prefilteredReady = isReady(resources.prefiltered, "prefiltered");
        const bool brdfLUTReady = isReady(resources.brdfLUT, "BRDF LUT");

        return irradianceSubmitted && prefilteredSubmitted && brdfLUTSubmitted &&
               irradianceReady && prefilteredReady && brdfLUTReady;
    }

    bool ValidateProceduralIBLQuality(const ProceduralIBLResources& resources,
                                      bool smoke,
                                      GPUResourceManager* gpuResources)
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

        auto checkCubemap = [&valid, gpuResources](const Resource::TextureHandle& texture,
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

            if (!gpuResources || !gpuResources->IsGPUReady(resource->GetId()))
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

            if (!gpuResources || !gpuResources->IsGPUReady(brdf->GetId()))
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

    bool UploadHDRIEnvironment(RenderSubsystem* renderSubsystem, const HDRIEnvironmentResources& resources)
    {
        if (!renderSubsystem || !renderSubsystem->GetGPUResourceManager() || !resources.IsValid())
            return false;

        auto* gpuResources = renderSubsystem->GetGPUResourceManager();
        auto upload = [gpuResources](const Resource::TextureHandle& texture, const char* label) -> bool
        {
            if (!texture)
            {
                RVX_CORE_ERROR("ModelViewer HDRI {} resource is missing", label);
                return false;
            }

            gpuResources->UploadImmediate(texture.Get());
            return true;
        };

        const bool envSubmitted = upload(resources.environment, "environment");
        const bool irradianceSubmitted = upload(resources.irradiance, "irradiance");
        const bool prefilteredSubmitted = upload(resources.prefiltered, "prefiltered");
        const bool brdfLUTSubmitted = upload(resources.brdfLUT, "BRDF LUT");

        for (uint32 attempt = 0; attempt < 4; ++attempt)
        {
            gpuResources->ProcessPendingUploads(0.0f);
        }

        auto isReady = [gpuResources](const Resource::TextureHandle& texture, const char* label) -> bool
        {
            if (!texture)
                return false;

            const bool ready = gpuResources->IsGPUReady(texture.GetId());
            if (!ready)
            {
                RVX_CORE_ERROR("ModelViewer HDRI {} resource '{}' ({}) is not GPU-ready",
                               label,
                               texture->GetName(),
                               texture.GetId());
            }
            return ready;
        };

        const bool envReady = isReady(resources.environment, "environment");
        const bool irradianceReady = isReady(resources.irradiance, "irradiance");
        const bool prefilteredReady = isReady(resources.prefiltered, "prefiltered");
        const bool brdfLUTReady = isReady(resources.brdfLUT, "BRDF LUT");

        return envSubmitted && irradianceSubmitted && prefilteredSubmitted && brdfLUTSubmitted &&
               envReady && irradianceReady && prefilteredReady && brdfLUTReady;
    }

    bool IsSkyboxPassReady(SceneRenderer* sceneRenderer, std::string& outReason)
    {
        if (!sceneRenderer)
        {
            outReason = "NoSceneRenderer";
            return false;
        }

        const SceneRenderPassChainStats& stats = sceneRenderer->GetPassChainStats();
        for (const RenderPassStatus& status : stats.passStatuses)
        {
            if (status.name == "SkyboxPass")
            {
                if (status.supported && status.enabled)
                {
                    outReason.clear();
                    return true;
                }

                outReason = status.unsupportedReason.empty() ? "SkyboxPassNotReady" : status.unsupportedReason;
                return false;
            }
        }

        outReason = "SkyboxPassStatusMissing";
        return false;
    }

    bool IsDirectionalShadowReady(SceneRenderer* sceneRenderer, std::string& outReason)
    {
        if (!sceneRenderer)
        {
            outReason = "NoSceneRenderer";
            return false;
        }

        PipelineCache* pipelineCache = sceneRenderer->GetPipelineCache();
        if (!pipelineCache)
        {
            outReason = "NoPipelineCache";
            return false;
        }

        const DirectionalShadowFrameBindingResult& result =
            pipelineCache->GetLastDirectionalShadowFrameBindingResult();
        if (result.shadowSamplingEnabled && result.fallbackReason == DirectionalShadowFallbackReason::None)
        {
            outReason.clear();
            return true;
        }

        outReason = PipelineCache::GetDirectionalShadowFallbackReasonName(result.fallbackReason);
        if (outReason.empty())
        {
            outReason = result.shadowSamplingEnabled ? "UnexpectedDirectionalShadowState" : "ShadowSamplingDisabled";
        }
        return false;
    }

    const char* MaterialBindingStatusName(MaterialBindingStatus status)
    {
        switch (status)
        {
            case MaterialBindingStatus::None:
                return "None";
            case MaterialBindingStatus::Ready:
                return "Ready";
            case MaterialBindingStatus::Fallback:
                return "Fallback";
            case MaterialBindingStatus::NotInitialized:
                return "NotInitialized";
            case MaterialBindingStatus::Unavailable:
                return "Unavailable";
            case MaterialBindingStatus::Error:
                return "Error";
        }

        return "Unknown";
    }

    uint32 GetRequiredPBRMaterialTextureFlags()
    {
        return static_cast<uint32>(MaterialTextureFlags::HasBaseColor) |
               static_cast<uint32>(MaterialTextureFlags::HasNormal) |
               static_cast<uint32>(MaterialTextureFlags::HasMetallicRoughness) |
               static_cast<uint32>(MaterialTextureFlags::HasOcclusion) |
               static_cast<uint32>(MaterialTextureFlags::HasEmissive);
    }

    bool IsPBRMaterialReady(SceneRenderer* sceneRenderer, std::string& outReason)
    {
        if (!sceneRenderer)
        {
            outReason = "NoSceneRenderer";
            return false;
        }

        MaterialSystem* materialSystem = sceneRenderer->GetMaterialSystem();
        if (!materialSystem)
        {
            outReason = "NoMaterialSystem";
            return false;
        }

        const MaterialBindingResult& result = materialSystem->GetLastBindingResult();
        const uint32 requiredFlags = GetRequiredPBRMaterialTextureFlags();
        const bool hasAllTextureFlags = (result.textureFlags & requiredFlags) == requiredFlags;
        const bool materialNameMatches = result.materialName == kPBRMaterialTestMaterialName;
        if (result.status == MaterialBindingStatus::Ready &&
            !result.usedFallback &&
            result.constantsUpdated &&
            result.descriptorSet &&
            hasAllTextureFlags &&
            materialNameMatches)
        {
            outReason.clear();
            return true;
        }

        outReason = "status=";
        outReason += MaterialBindingStatusName(result.status);
        outReason += ", usedFallback=";
        outReason += result.usedFallback ? "true" : "false";
        outReason += ", constantsUpdated=";
        outReason += result.constantsUpdated ? "true" : "false";
        outReason += ", descriptorSet=";
        outReason += result.descriptorSet ? "true" : "false";
        outReason += ", textureFlags=" + std::to_string(result.textureFlags);
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

        if (UploadHDRIEnvironment(renderSubsystem, hdriEnvironmentResources))
        {
            RVX_CORE_INFO("ModelViewer HDRI environment resources uploaded");
        }
        else
        {
            RVX_CORE_ERROR("ModelViewer HDRI environment resources were created but not GPU-ready");
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

            if (UploadProceduralIBL(renderSubsystem, proceduralIBLResources))
            {
                RVX_CORE_INFO("ModelViewer procedural IBL resources uploaded");
            }
            else
            {
                RVX_CORE_WARN("ModelViewer procedural IBL resources were created but not GPU-ready");
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

            cameraPos = options.materialTestScene ? Vec3(0.0f, 0.45f, 2.4f) :
                        (options.shadowTestScene ? Vec3(0.0f, 1.35f, 4.0f) : Vec3(0.0f, 1.5f, 4.0f));
            camera->SetPosition(cameraPos);
            camera->LookAt(target);

            PendingScreenshot pendingScreenshot;
            const bool captureFrame = !options.screenshotPath.empty() && (frameIndex + 1 == options.frames);

            renderSubsystem->BeginFrame();
            renderSubsystem->Render(world, camera);

            if (options.expectIBLReady && (frameIndex + 1 == options.frames))
            {
                SceneRenderer* sceneRenderer = renderSubsystem->GetSceneRenderer();
                const SceneEnvironmentIBLStats* iblStats =
                    sceneRenderer ? &sceneRenderer->GetEnvironmentIBLStats() : nullptr;
                if (!iblStats || !iblStats->textureIBLEnabled)
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected texture IBL ready; fallback reason: {}",
                                   iblStats ? iblStats->fallbackReason : "NoSceneRenderer");
                    smokeSucceeded = false;
                }
            }

            if (options.expectSkyboxReady && (frameIndex + 1 == options.frames))
            {
                SceneRenderer* sceneRenderer = renderSubsystem->GetSceneRenderer();
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
                SceneRenderer* sceneRenderer = renderSubsystem->GetSceneRenderer();
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

            if (options.expectMaterialReady && (frameIndex + 1 == options.frames))
            {
                SceneRenderer* sceneRenderer = renderSubsystem->GetSceneRenderer();
                std::string materialFallbackReason;
                if (!IsPBRMaterialReady(sceneRenderer, materialFallbackReason))
                {
                    RVX_CORE_ERROR("ModelViewer smoke expected PBR material ready; binding: {}",
                                   materialFallbackReason);
                    smokeSucceeded = false;
                }
                else
                {
                    RVX_CORE_INFO("ModelViewer smoke PBR material ready: material='{}', textureFlags={}",
                                  kPBRMaterialTestMaterialName,
                                  GetRequiredPBRMaterialTextureFlags());
                }
            }

            if (options.expectProceduralIBLQuality && (frameIndex + 1 == options.frames))
            {
                if (!ValidateProceduralIBLQuality(proceduralIBLResources,
                                                  options.smoke,
                                                  renderSubsystem->GetGPUResourceManager()))
                {
                    smokeSucceeded = false;
                }
            }

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
