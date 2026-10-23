#pragma once

/**
 * @file SampleContext.h
 * @brief Backend-neutral services exposed to sample scenes.
 */

#include "Core/Types.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "Resource/ResourceDiagnosticsView.h"
#include "Resource/ResourcePublicationView.h"
#include "Samples/SampleAssetCatalog.h"
#include "Samples/FrameworkAssessment.h"
#include "Samples/SampleInfo.h"
#include "Samples/SampleSceneLifetimeScope.h"
#include "World/ECS/WorldEcsCameraService.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RVX
{
    class InputSubsystem;
    class IWorldEcsRuntimeServices;
    class SampleAnimationLoader;
    class SampleEnvironmentLoader;
    class SampleModelLoader;
    class SampleParticleEffectLoader;
    class World;

    /** @brief One legacy host-resolved model input; contains no loader policy. */
    struct SampleSceneModelAsset
    {
        std::string id;
        /**
         * @brief Compatibility-only resolved path.
         *
         * Catalog-backed sample scenes must request @ref id through
         * SampleModelLoader::RequestByAssetId rather than consume this path.
         */
        std::filesystem::path path;
    };

    /** @brief Immutable catalog asset available to one SampleRunner invocation. */
    struct SampleAssetRegistryEntry
    {
        std::string id;
        SampleAssetKind kind = SampleAssetKind::Model;
        /**
         * @brief Physical catalog path retained for evidence and report output.
         *
         * This is intentionally never substituted into a mounted runtime
         * resource request.  Runtime-only compositions consume @ref resourcePath.
         */
        std::filesystem::path resolvedPath;
        /**
         * @brief Logical path presented to Resource/ECS loaders.
         *
         * Runtime-only package entries use a strict `cooked://` path rooted at
         * the runner-mounted Sample asset root.  Legacy compositions leave
         * this empty and intentionally use @ref resolvedPath.
         */
        std::string resourcePath;
        /**
         * @brief Reject instead of falling back to a physical path when the
         * entry is used by a runtime-only composition.
         */
        bool requiresCookedResourcePath = false;
        Resource::ResourceContentIdentity contentIdentity;
        AssetContentId assetContentId;
        /**
         * @brief Require the runner-admitted entry to be consumed as a staged
         * cooked artifact rather than as authoring source input.
         */
        bool packageOnly = false;
    };

    /**
     * @brief Validate one package-relative cooked URI without touching disk.
     *
     * A mounted runtime root accepts only paths below that root.  Keeping this
     * validation at the Sample registry boundary prevents absolute catalog
     * paths, traversal, and alternate schemes from reaching a loader.
     */
    [[nodiscard]] inline bool IsStrictSampleCookedResourcePath(
        const std::string_view resourcePath) noexcept
    {
        constexpr std::string_view Prefix = "cooked://";
        if (!resourcePath.starts_with(Prefix))
        {
            return false;
        }
        const std::string_view logicalPath = resourcePath.substr(Prefix.size());
        if (logicalPath.empty() || logicalPath.find("://") != std::string_view::npos ||
            logicalPath.front() == '/' || logicalPath.front() == '\\')
        {
            return false;
        }

        const std::filesystem::path path{std::string(logicalPath)};
        if (path.empty() || path.is_absolute() || path.has_root_name() ||
            path.has_root_directory())
        {
            return false;
        }
        for (const std::filesystem::path& segment : path)
        {
            const std::string value = segment.generic_string();
            if (value.empty() || value == "." || value == ".." ||
                value.find(':') != std::string::npos)
            {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Return the only loader path admitted for a registry entry.
     *
     * Runtime-only entries never fall back to their physical evidence path.
     * This is deliberately shared by model, environment, animation, and
     * particle-effect loaders so a new catalog type cannot bypass the policy.
     */
    [[nodiscard]] inline bool TryGetSampleAssetResourcePath(
        const SampleAssetRegistryEntry& entry,
        std::string& outResourcePath,
        std::string& outError)
    {
        if (entry.requiresCookedResourcePath)
        {
            if (!IsStrictSampleCookedResourcePath(entry.resourcePath))
            {
                outError = "Runtime-only Sample asset has no valid package-relative cooked resource path: " +
                    entry.id;
                return false;
            }
            outResourcePath = entry.resourcePath;
            return true;
        }

        outResourcePath = entry.resourcePath.empty()
            ? entry.resolvedPath.generic_string()
            : entry.resourcePath;
        if (outResourcePath.empty())
        {
            outError = "Sample asset has no resource path: " + entry.id;
            return false;
        }
        return true;
    }

    enum class SampleAssetRegistryLookupCode : uint8
    {
        Found = 0,
        UnknownAssetId,
        KindMismatch
    };

    /** @brief Typed result used to reject invalid catalog-id requests before I/O. */
    struct SampleAssetRegistryLookupResult
    {
        const SampleAssetRegistryEntry* entry = nullptr;
        SampleAssetRegistryLookupCode code =
            SampleAssetRegistryLookupCode::UnknownAssetId;
        SampleAssetKind expectedKind = SampleAssetKind::Model;
        SampleAssetKind actualKind = SampleAssetKind::Model;

        [[nodiscard]] bool IsFound() const noexcept
        {
            return code == SampleAssetRegistryLookupCode::Found &&
                   entry != nullptr;
        }
    };

    /**
     * @brief Runner-built, immutable id-to-catalog-asset boundary for samples.
     *
     * Only successfully runner-validated catalog entries are admitted. Explicit
     * command-line paths deliberately do not enter this registry.
     */
    class SampleAssetRegistry final
    {
    public:
        SampleAssetRegistry() = default;

        explicit SampleAssetRegistry(
            std::vector<SampleAssetRegistryEntry> entries)
        {
            for (SampleAssetRegistryEntry& entry : entries)
            {
                if (!entry.id.empty() && !entry.resolvedPath.empty())
                {
                    m_entries.try_emplace(entry.id, std::move(entry));
                }
            }
        }

        [[nodiscard]] SampleAssetRegistryLookupResult Lookup(
            std::string_view id,
            SampleAssetKind expectedKind) const noexcept
        {
            SampleAssetRegistryLookupResult result;
            result.expectedKind = expectedKind;
            const auto asset = m_entries.find(id);
            if (asset == m_entries.end())
            {
                return result;
            }

            result.entry = &asset->second;
            result.actualKind = asset->second.kind;
            result.code = asset->second.kind == expectedKind
                              ? SampleAssetRegistryLookupCode::Found
                              : SampleAssetRegistryLookupCode::KindMismatch;
            return result;
        }

        [[nodiscard]] bool IsEmpty() const noexcept { return m_entries.empty(); }

    private:
        std::map<std::string, SampleAssetRegistryEntry, std::less<>> m_entries;
    };

    /** @brief Resolved, immutable scene options prepared by SampleRunner. */
    struct SampleSceneOptions
    {
        /** @brief Primary selected model catalog id. */
        std::string assetId;
        /** @brief All selected model catalog ids in deterministic request order. */
        std::vector<std::string> modelAssetIds;
        /** @brief Selected environment catalog id, if the sample uses one. */
        std::string environmentAssetId;
        /** @brief Required animation catalog ids in deterministic request order. */
        std::vector<std::string> animationAssetIds;
        /** @brief Required cooked effect catalog ids in deterministic request order. */
        std::vector<std::string> effectAssetIds;
        /** @brief Primary cooked effect id for an effect-focused VFX scenario. */
        std::string effectAssetId;

        // Compatibility-only path fields. New catalog-backed sample scenes must
        // use the ids above with RequestByAssetId().
        std::filesystem::path modelPath;
        std::vector<SampleSceneModelAsset> modelAssets;
        std::filesystem::path environmentPath;
        /** @brief Exact runner-admitted identity for an explicit environment path. */
        Resource::ResourceContentIdentity environmentContentIdentity;
        uint32 width = 1280;
        uint32 height = 720;
        std::string quality = "default";
        SampleQualityTier qualityTier = SampleQualityTier::Default;
        SamplePipelineRecipe pipelineRecipe =
            SamplePipelineRecipe::ForwardStandard;
        /** Manifest-owned rational time step for deterministic scenarios. */
        uint32 fixedTimeNumerator = 0;
        uint32 fixedTimeDenominator = 0;
        uint32 deterministicSeed = 0;
        SampleRenderPath renderPath = SampleRenderPath::Auto;
        std::optional<SampleWorkloadProfile> workloadProfile;
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
        /** Sole mutable scene authority for this exact World. */
        SceneECS::SceneEcsRuntime& scene;
        /** Engine-owned model, environment, and animation services for this World. */
        IWorldEcsRuntimeServices& runtimeServices;
        /** Read-only update-thread view of exact Resource-to-Render publication. */
        const Resource::IResourcePublicationView& resourcePublications;
        /** Read-only update-thread view of Resource lifecycle and handoff metrics. */
        const Resource::IResourceDiagnosticsView& resourceDiagnostics;
        /** Handle-and-value camera authoring boundary for this exact World. */
        WorldECS::WorldEcsCameraService& cameras;
        WorldECS::WorldEcsCameraRef camera;
        /** Setup-owned value copied to Engine after Sample::Setup succeeds. */
        RenderFrameSettings renderSettings;
        InputSubsystem* input = nullptr;
        SampleModelLoader& models;
        SampleEnvironmentLoader& environments;
        SampleAnimationLoader& animations;
        /** Cooked-only CPU billboard effect admission for VFX scenarios. */
        SampleParticleEffectLoader& particleEffects;
        /** Host-owned scope for direct procedural sample entities only. */
        SampleSceneLifetimeScope& sceneLifetime;
        const SampleSceneOptions& options;
        /** Backend-neutral, bounded channel for semantic assessment events. */
        SampleAssessmentChannel& assessment;
    };
} // namespace RVX
