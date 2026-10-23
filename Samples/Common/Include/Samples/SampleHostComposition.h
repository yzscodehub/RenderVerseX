#pragma once

/**
 * @file SampleHostComposition.h
 * @brief Narrow executable-host composition boundary for Samples.
 *
 * Sample scenarios and the shared Sample runner deliberately know nothing
 * about source importers or shader compilers.  An executable selects exactly
 * one composition at its product boundary: Development may opt into source
 * preparation, while Cooked admits only runtime artifacts.
 */

#include "Engine/EngineRuntimeDependencies.h"
#include "Resource/ResourceManager.h"
#include "ResourceRuntime/ResourceLoader.h"
#include "Samples/SampleCLI.h"

#include <filesystem>
#include <memory>
#include <string>

namespace RVX
{
    /** Source-only request owned by the development host composition. */
    struct SampleSourceEnvironmentPreparationRequest
    {
        std::string quality = "default";
        bool smoke = false;
        float32 exposure = 1.0f;
    };

    /**
     * @brief Executable-owned factory for engine/resource/source preparation.
     *
     * This is intentionally narrower than the Engine and Resource managers:
     * it provides only the three places where a Sample host needs composition
     * policy.  It is not visible to scenario code and cannot expose a source
     * importer through an ambient singleton.
     */
    class ISampleHostComposition
    {
    public:
        virtual ~ISampleHostComposition() = default;

        [[nodiscard]] virtual bool RequiresCookedOnly() const noexcept = 0;

        virtual bool CreateEngineRuntimeDependencies(
            const SampleRuntimeCompositionEvidence& evidence,
            EngineRuntimeDependencies& outDependencies,
            std::string& outError) const = 0;

        virtual bool CreateResourceManagerConfig(
            const SampleRuntimeCompositionEvidence& evidence,
            Resource::ResourceManagerConfig& outConfig,
            std::string& outError) const = 0;

        virtual bool PrepareSourceEnvironment(
            const SampleSourceEnvironmentPreparationRequest& request,
            uint64& outCanonicalImportOptionsHash,
            Resource::ResourceLoadPreparationStateRef& outState,
            Resource::ResourceLoadError& outError) const = 0;
    };

    /** Development host: source preparation is available by explicit choice. */
    [[nodiscard]] std::unique_ptr<ISampleHostComposition>
    CreateSampleDevelopmentComposition();

    /** Cooked product host: source preparation is always rejected. */
    [[nodiscard]] std::unique_ptr<ISampleHostComposition>
    CreateSampleCookedComposition();
} // namespace RVX
