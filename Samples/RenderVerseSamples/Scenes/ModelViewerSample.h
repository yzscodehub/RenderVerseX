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
        void OnViewportResize(SampleContext& context,
                              uint32 width,
                              uint32 height) override;
        bool PrepareQualificationCapture(SampleContext& context,
                                         std::string& outError) override;
        void AppendReport(SampleFeatureReporter& reporter) const override;
        SampleReadiness GetReadiness(
            const SampleRenderDiagnostics& diagnostics) const override;
        bool ValidateResult(const SampleRenderDiagnostics& diagnostics,
                            std::string& outError) const override;
        void Shutdown(SampleContext& context) override;

    private:
        bool ActivateModel(SampleContext& context);

        LoadedSampleModel m_model;
        LoadedSampleEnvironment m_environment;
        AABB m_bounds;
        ModelCameraFrame m_cameraFrame;
        SampleOrbitCameraController m_orbitCamera;
        SampleRenderPath m_renderPath = SampleRenderPath::Auto;
        uint32 m_automationFrameCount = 0;
        uint32 m_automationZoomEventCount = 0;
        std::string m_modelActivationError;
        bool m_modelActivationAttempted = false;
        bool m_modelActivated = false;
        bool m_renderablesEnabled = false;
        bool m_textureEnvironment = false;
        bool m_qualificationCapturePrepared = false;
        bool m_skyboxCreated = false;
        bool m_lightCreated = false;
    };
} // namespace RVX
