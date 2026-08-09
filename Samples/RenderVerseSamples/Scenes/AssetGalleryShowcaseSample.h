#pragma once

/** @file AssetGalleryShowcaseSample.h @brief Catalog-backed model gallery. */

#include "Core/Math/AABB.h"
#include "Samples/ModelCameraFraming.h"
#include "Samples/Sample.h"
#include "Samples/SampleModelLoader.h"
#include "Samples/SampleOrbitCameraController.h"

#include <string>
#include <vector>

namespace RVX
{
    /**
     * @brief Static gallery using host-resolved catalog assets and production
     * ResourceManager -> ModelResource -> Scene instantiation.
     */
    class AssetGalleryShowcaseSample final : public ISample
    {
    public:
        [[nodiscard]] const SampleInfo& GetInfo() const noexcept override;
        bool Setup(SampleContext& context, std::string& outError) override;
        void Update(SampleContext& context, float deltaTime) override;
        void OnInput(SampleContext& context) override;
        void AppendReport(SampleFeatureReporter& reporter) const override;
        SampleReadiness GetReadiness(
            const SampleRenderDiagnostics& diagnostics) const override;
        bool ValidateResult(const SampleRenderDiagnostics& diagnostics,
                            std::string& outError) const override;
        void Shutdown(SampleContext& context) override;

    private:
        bool PlaceModels(SampleContext& context, std::string& outError);
        void DisableUnplacedModels(SampleContext& context) const;
        void FailAndCancel(SampleContext& context, std::string reason);
        std::vector<LoadedSampleModel> m_models;
        std::vector<std::string> m_assetIds;
        AABB m_galleryBounds;
        ModelCameraFrame m_cameraFrame;
        SampleOrbitCameraController m_orbitCamera;
        uint32 m_expectedVisibleObjects = 0;
        std::string m_failureReason;
        bool m_modelsPlaced = false;
        bool m_renderablesActivated = false;
        bool m_staticCatalogGallery = false;
        bool m_skyboxCreated = false;
        bool m_lightCreated = false;
    };
} // namespace RVX
