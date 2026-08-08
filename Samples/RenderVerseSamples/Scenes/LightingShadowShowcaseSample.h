#pragma once

/** @file LightingShadowShowcaseSample.h @brief Directional shadow scene. */

#include "Samples/Sample.h"
#include "Samples/SampleModelLoader.h"

namespace RVX
{
    /** @brief Controlled caster/receiver scene for engine-owned shadow rendering. */
    class LightingShadowShowcaseSample final : public ISample
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
        uint32 m_shadowAtlasResolution = 0;
        uint32 m_shadowCascadeCount = 0;
        bool m_skyboxCreated = false;
        bool m_shadowLightCreated = false;
    };
} // namespace RVX
