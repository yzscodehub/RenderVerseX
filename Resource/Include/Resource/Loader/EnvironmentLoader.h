#pragma once

/**
 * @file EnvironmentLoader.h
 * @brief Prepared HDR/IBL environment loader with immutable admission state.
 */

#include "Resource/Loader/HDRTextureLoader.h"
#include "Resource/ResourceManager.h"
#include "Resource/Types/EnvironmentResource.h"

#include <mutex>
#include <string>
#include <vector>

namespace RVX::Resource
{
    /** @brief User-facing inputs which determine a baked environment product. */
    struct EnvironmentLoadOptions
    {
        HDRIBLQualityProfile quality = HDRIBLQualityProfile::Default;
        float32 exposure = 1.0f;
        bool applyGamma = false;
    };

    /**
     * @brief Immutable environment admission state.
     *
     * An owner captures this before submitting the request and copies
     * importOptionsHash into ResourceLoadOptions. Prepare consumes this value
     * directly; it never observes mutable loader configuration.
     */
    struct EnvironmentPreparationState final : ResourceLoadPreparationState
    {
        HDRLoadOptions hdrOptions;
        uint64 importOptionsHash = 0;

        [[nodiscard]] bool IsValid() const;
    };

    /**
     * @brief Builds EnvironmentResource bundles from HDR/EXR input.
     *
     * Worker preparation creates only un-published Resource objects through a
     * manager-null HDRTextureLoader. Cache insertion, notifications and scene
     * use remain owner-thread responsibilities of ResourceManager.
     */
    class EnvironmentLoader final : public IResourceLoader
    {
    public:
        EnvironmentLoader() = default;
        explicit EnvironmentLoader(EnvironmentLoadOptions options);
        ~EnvironmentLoader() override = default;

        // =====================================================================
        // IResourceLoader Interface
        // =====================================================================

        ResourceType GetResourceType() const override
        {
            return EnvironmentResource::StaticResourceType;
        }
        std::vector<std::string> GetSupportedExtensions() const override;
        IResource* Load(const std::string& path) override;
        bool Prepare(const ResourceLoadPreparationContext& context,
                     PreparedResourceBundle& outBundle,
                     ResourceLoadError& outError) override;
        bool SupportsPreparedLoading() const override { return true; }
        bool CanLoad(const std::string& path) const override;

        // =====================================================================
        // Admission State
        // =====================================================================

        void SetOptions(EnvironmentLoadOptions options);
        [[nodiscard]] EnvironmentLoadOptions GetOptions() const;

        /** @brief Capture a deterministic immutable state for one admission. */
        [[nodiscard]] EnvironmentPreparationState CapturePreparationState() const;

        /** @brief Compute the stable import hash for a complete options value. */
        [[nodiscard]] static uint64 ComputeImportOptionsHash(const EnvironmentLoadOptions& options);

        bool CapturePreparationState(
            uint64 requestedImportOptionsHash,
            ResourceLoadPreparationStateRef& outState,
            uint64& outCanonicalImportOptionsHash,
            ResourceLoadError& outError) const override;

        /**
         * @brief Prepare using a previously captured admission state.
         *
         * This is the production entry point for a request that carries a
         * known importOptionsHash. It fails closed if context and state differ.
         */
        bool Prepare(const ResourceLoadPreparationContext& context,
                     const EnvironmentPreparationState& state,
                     PreparedResourceBundle& outBundle,
                     ResourceLoadError& outError) const;

    private:
        static bool ValidateOptions(const EnvironmentLoadOptions& options);
        static bool ValidateHDRLoadOptions(const HDRLoadOptions& options);
        static std::string BuildDependencyIdentity(const ResourceLoadPreparationContext& context,
                                                   const char* suffix);
        static void AssignPreparedIdentity(TextureResource& texture,
                                           const std::string& identity,
                                           const std::string& sourcePath,
                                           const char* displayName);

        mutable std::mutex m_optionsMutex;
        EnvironmentLoadOptions m_options;
    };
} // namespace RVX::Resource

namespace RVX
{
    using Resource::EnvironmentLoader;
    using Resource::EnvironmentLoadOptions;
    using Resource::EnvironmentPreparationState;
} // namespace RVX
