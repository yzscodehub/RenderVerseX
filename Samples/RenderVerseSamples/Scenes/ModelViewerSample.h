#pragma once

/** @file ModelViewerSample.h @brief Interactive model inspection scene. */

#include "Core/Math/AABB.h"
#include "Samples/ModelCameraFraming.h"
#include "Samples/Sample.h"
#include "Samples/SampleEnvironmentLoader.h"
#include "Samples/SampleModelLoader.h"
#include "Samples/SampleOrbitCameraController.h"

namespace RVX
{
    class ModelViewerSample final : public ISample
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
        LoadedSampleModel m_model;
        LoadedSampleEnvironment m_environment;
        AABB m_bounds;
        ModelCameraFrame m_cameraFrame;
        SampleOrbitCameraController m_orbitCamera;
        bool m_textureEnvironment = false;
        bool m_skyboxCreated = false;
        bool m_lightCreated = false;
    };
} // namespace RVX
