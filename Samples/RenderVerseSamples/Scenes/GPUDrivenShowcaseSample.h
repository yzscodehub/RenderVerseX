#pragma once

/** @file GPUDrivenShowcaseSample.h @brief Direct/GPU-driven parity scene. */

#include "Core/Math/AABB.h"
#include "Samples/ModelCameraFraming.h"
#include "Samples/Sample.h"
#include "Samples/SampleModelLoader.h"

#include <vector>

namespace RVX
{
    /**
     * @brief Builds one scene and selects only the engine render-path policy.
     *
     * This sample never creates backend commands, indirect buffers, or a
     * private RenderGraph path. Direct and GPU-driven runs share the same
     * model, transforms, materials, camera, and lights.
     */
    class GPUDrivenShowcaseSample final : public ISample
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
        bool IsGPUDrivenReady(const SampleRenderDiagnostics& diagnostics,
                              std::string& outPendingReason) const;
        bool IsDirectReady(const SampleRenderDiagnostics& diagnostics,
                           std::string& outPendingReason) const;

        std::vector<LoadedSampleModel> m_models;
        AABB m_bounds;
        ModelCameraFrame m_cameraFrame;
        uint32 m_sceneInstanceCount = 0;
        SampleRenderPath m_renderPath = SampleRenderPath::GPUDriven;
        bool m_skyboxCreated = false;
        bool m_lightCreated = false;
    };
} // namespace RVX
