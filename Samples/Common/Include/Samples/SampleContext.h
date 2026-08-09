#pragma once

/**
 * @file SampleContext.h
 * @brief Backend-neutral services exposed to sample scenes.
 */

#include "Core/Types.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "Samples/SampleInfo.h"

#include <filesystem>
#include <string>
#include <vector>

namespace RVX
{
    class CameraComponent;
    class InputSubsystem;
    class SampleEnvironmentLoader;
    class SampleModelLoader;
    class Scene;
    class World;

    namespace Resource
    {
        class ResourceManager;
    }

    /** @brief One host-resolved model input; contains no loader policy. */
    struct SampleSceneModelAsset
    {
        std::string id;
        std::filesystem::path path;
    };

    /** @brief Resolved, immutable scene options prepared by SampleRunner. */
    struct SampleSceneOptions
    {
        std::string assetId;
        std::filesystem::path modelPath;
        std::vector<SampleSceneModelAsset> modelAssets;
        std::string environmentAssetId;
        std::filesystem::path environmentPath;
        uint32 width = 1280;
        uint32 height = 720;
        std::string quality = "default";
        SampleRenderPath renderPath = SampleRenderPath::Auto;
        bool smoke = false;
        bool diagnostics = false;
        bool deterministicCameraOrbit = false;
    };

    /**
     * @brief Narrow scene-facing contract.
     *
     * Deliberately excludes Engine, RHI devices, command lists, RenderGraph,
     * and backend identity. Samples build scene meaning and configure only
     * public frame policy values.
     */
    struct SampleContext
    {
        World& world;
        Scene& scene;
        Resource::ResourceManager& resources;
        CameraComponent& camera;
        /** Setup-owned value copied to Engine after Sample::Setup succeeds. */
        RenderFrameSettings renderSettings;
        InputSubsystem* input = nullptr;
        SampleModelLoader& models;
        SampleEnvironmentLoader& environments;
        const SampleSceneOptions& options;
    };
} // namespace RVX
