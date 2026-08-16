#pragma once

/**
 * @file SampleContext.h
 * @brief Backend-neutral services exposed to sample scenes.
 */

#include "Core/Types.h"
#include "Engine/ECS/IWorldEcsRuntimeServices.h"
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
    class SampleAnimationLoader;
    class SampleEnvironmentLoader;
    class SampleModelLoader;
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
        std::filesystem::path resolvedPath;
        Resource::ResourceContentIdentity contentIdentity;
        AssetContentId assetContentId;
    };

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
        /** Host-owned scope for direct procedural sample entities only. */
        SampleSceneLifetimeScope& sceneLifetime;
        const SampleSceneOptions& options;
        /** Backend-neutral, bounded channel for semantic assessment events. */
        SampleAssessmentChannel& assessment;
    };
} // namespace RVX
