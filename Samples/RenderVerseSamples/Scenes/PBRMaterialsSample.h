#pragma once

/** @file PBRMaterialsSample.h @brief Controlled PBR material response scene. */

#include "Core/Math/AABB.h"
#include "Samples/ModelCameraFraming.h"
#include "Samples/Sample.h"
#include "Samples/SampleEnvironmentLoader.h"
#include "Samples/SampleModelLoader.h"
#include "Samples/SampleOrbitCameraController.h"

#include <cstddef>

namespace RVX
{
    enum class PBRReferenceWorkflow : uint8
    {
        None = 0,
        Factor,
        Texture,
    };

    class PBRMaterialsSample final : public ISample
    {
    public:
        [[nodiscard]] const SampleInfo& GetInfo() const noexcept override;
        bool Setup(SampleContext& context, std::string& outError) override;
        void Update(SampleContext& context, float deltaTime) override;
        void OnInput(SampleContext& context) override;
        void AppendReport(SampleFeatureReporter& reporter) const override;
        bool IsReady(const SampleRenderDiagnostics& diagnostics,
                     std::string& outPendingReason) const override;
        bool ValidateResult(const SampleRenderDiagnostics& diagnostics,
                            std::string& outError) const override;
        void Shutdown(SampleContext& context) override;

    private:
        bool InspectMaterials(PBRReferenceWorkflow referenceWorkflow,
                              std::string& outError);
        bool ApplyFactorMaterialOverrides(Scene& scene,
                                          std::string& outError);

        LoadedSampleModel m_model;
        LoadedSampleEnvironment m_environment;
        AABB m_bounds;
        ModelCameraFrame m_cameraFrame;
        SampleOrbitCameraController m_orbitCamera;
        size_t m_pbrMaterialCount = 0;
        size_t m_factorMaterialCount = 0;
        size_t m_textureMaterialCount = 0;
        size_t m_sceneMeshInstanceCount = 0;
        size_t m_uniqueMeshResourceCount = 0;
        size_t m_materialOverrideCount = 0;
        uint32 m_expectedDrawPacketCount = 0;
        uint32 m_metallicRoughnessTextureWidth = 0;
        uint32 m_metallicRoughnessTextureHeight = 0;
        bool m_metallicRoughnessTextureLoaded = false;
        PBRReferenceWorkflow m_referenceWorkflow =
            PBRReferenceWorkflow::None;
        bool m_referenceCubeValidated = false;
        bool m_skyboxCreated = false;
        bool m_lightCreated = false;
    };
} // namespace RVX
