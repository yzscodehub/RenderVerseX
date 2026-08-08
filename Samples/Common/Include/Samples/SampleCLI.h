#pragma once

/**
 * @file SampleCLI.h
 * @brief Shared command-line and JSON report helpers for sample applications.
 */

#include "Core/Types.h"
#include "RHI/RHIDefinitions.h"

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace RVX
{
    struct SampleCLIOptions
    {
        RHIBackendType backend = RHIBackendType::Auto;
        bool smoke = false;
        uint32 frames = 0;
        std::filesystem::path screenshotPath;
        std::filesystem::path reportPath;
        uint32 width = 1280;
        uint32 height = 720;
        std::string quality = "default";
        bool diagnostics = false;
        bool enableValidation = true;
        bool showHelp = false;
    };

    struct SampleRenderDiagnostics
    {
        bool available = false;
        bool renderAttempted = false;
        bool rendered = false;
        bool graphBuilt = false;
        bool graphCompiled = false;
        uint32 renderGraphTotalPasses = 0;
        uint32 visibleObjectCount = 0;
        uint32 renderSceneLightCount = 0;
        uint32 requestedPostProcessEffectCount = 0;
        uint32 enabledPostProcessEffectCount = 0;
        uint32 unsupportedPostProcessSkippedCount = 0;
        uint32 postProcessGraphPassCount = 0;
        bool clusteredLightingInitialized = false;
        uint32 clusteredLightingActiveClusters = 0;
        bool textureIBLEnabled = false;
        bool directionalShadowSamplingEnabled = false;
        std::string directionalShadowReason;
        bool materialReady = false;
        bool materialUsedFallback = false;
        bool materialConstantsUpdated = false;
        bool materialDescriptorSetAvailable = false;
        uint32 materialTextureFlags = 0;
        bool opaqueExecutionCompleted = false;
        bool opaqueExecutedDrawCountAvailable = false;
        uint32 opaqueExecutedDrawCount = 0;
        std::string instancingRequestedMode;
        bool opaqueInstancingPlanAvailable = false;
        bool opaqueInstancingPreflightSucceeded = false;
        uint32 opaqueInstancingPlannedPacketCount = 0;
        uint32 opaqueInstancingPlannedDrawCount = 0;
        uint32 opaqueInstancingPlannedInstanceCount = 0;
        uint32 opaqueInstancingPlannedBatchCount = 0;
        uint32 opaqueInstancingExecutedPacketCount = 0;
        uint32 opaqueInstancingSubmittedDrawCount = 0;
        uint32 opaqueInstancingSubmittedInstanceCount = 0;
        uint32 opaqueInstancingBatchCount = 0;
        uint32 opaqueInstancingFallbackBatchCount = 0;
        bool opaqueMaterialBindingsAvailable = false;
        uint32 opaqueMaterialBindingCount = 0;
        uint32 opaqueMaterialFallbackBindingCount = 0;
        uint32 opaqueMaterialTextureFlags = 0;
        uint32 opaqueMaterialFallbackTextureFlags = 0;
        bool gpuDrivenPolicyDecisionAvailable = false;
        std::string gpuDrivenRequestedMode;
        std::string gpuDrivenPolicyReason;
        std::string gpuDrivenQualification;
        bool gpuDrivenBackendQualified = false;
        bool gpuDrivenCapabilitiesReady = false;
        bool gpuDrivenPipelineReady = false;
        bool gpuDrivenEnabled = false;
        bool gpuDrivenGraphPassAdded = false;
        bool gpuDrivenGraphPassRecorded = false;
        bool gpuDrivenExecutionRecorded = false;
        uint32 gpuDrivenGraphInputDrawItemCount = 0;
        uint32 gpuDrivenVisibilityCandidateCount = 0;
        uint32 gpuDrivenVisibleCullableDrawItemCount = 0;
        uint32 gpuDrivenCpuReferenceVisibleCount = 0;
        uint32 gpuDrivenCpuReferenceCulledCount = 0;
        bool gpuDrivenOpaqueIndirectRequested = false;
        bool gpuDrivenOpaqueIndirectEligible = false;
        bool gpuDrivenOpaqueIndirectSubmitted = false;
        uint32 gpuDrivenOpaqueDirectDrawCount = 0;
        uint32 gpuDrivenOpaqueIndirectBatchCount = 0;
        uint32 gpuDrivenOpaqueIndirectDrawUpperBound = 0;
        std::string gpuDrivenOpaqueFallbackReason;
        bool renderPolicyPlanAvailable = false;
        bool renderPolicyReportAvailable = false;
        std::string renderPolicySelectedTier;
        std::string renderPolicyExecutedTier;
        std::string renderPolicyTierFallbackReason;
    };

    /** @brief One resolved asset and its provenance in a sample report. */
    struct SampleReportAsset
    {
        std::string role;
        std::string id;
        std::filesystem::path path;
        std::string kind;
        std::filesystem::path catalogPath;
        std::filesystem::path assetRoot;
        std::string licenseSpdx;
        std::filesystem::path licenseFile;
        std::string sourceName;
        std::string sourceUri;
        std::string author;
        std::string attribution;
        bool redistributable = false;
        bool loaded = false;
    };

    /** @brief Bounded sample readiness state recorded by the shared host. */
    struct SampleReportReadiness
    {
        bool waitRequested = false;
        bool ready = true;
        uint32 minimumFrames = 0;
        uint32 maximumFrames = 0;
        uint32 timeoutMs = 0;
        std::string reason;
    };

    struct SampleReport
    {
        std::string schemaId = "RVX.SampleReport";
        uint32 schemaVersion = 1;
        std::string sampleName;
        std::string category = "sample";
        RHIBackendType requestedBackend = RHIBackendType::Auto;
        RHIBackendType backend = RHIBackendType::Auto;
        uint32 frameCount = 0;
        uint64 submittedFrameSequence = 0;
        uint64 presentedFrameSequence = 0;
        uint32 width = 0;
        uint32 height = 0;
        std::string quality = "default";
        std::string renderPath = "auto";
        bool diagnostics = false;
        std::filesystem::path screenshotPath;
        std::string assetId;
        std::filesystem::path assetPath;
        std::string assetKind;
        std::filesystem::path catalogPath;
        std::filesystem::path assetRoot;
        std::string assetLicenseSpdx;
        std::filesystem::path assetLicenseFile;
        std::string assetSourceName;
        std::string assetSourceUri;
        std::string assetAuthor;
        std::string assetAttribution;
        bool assetRedistributable = false;
        bool assetLoaded = false;
        std::vector<SampleReportAsset> assets;
        SampleReportReadiness readiness;
        std::vector<std::string> enabledFeatures;
        std::vector<std::string> unsupportedFeatures;
        std::vector<std::string> fallbackReasons;
        std::vector<std::string> resourceDiagnostics;
        SampleRenderDiagnostics renderDiagnostics;
        bool pass = false;
    };

    struct SampleAppDesc
    {
        std::string sampleName;
        std::string category = "sample";
        std::vector<std::string> enabledFeatures;
        std::vector<std::string> unsupportedFeatures;
        std::vector<std::string> fallbackReasons;
        std::vector<std::string> resourceDiagnostics;
        SampleRenderDiagnostics renderDiagnostics;
        bool supportsScreenshot = false;
        bool supportsQualityProfiles = false;
    };

    struct SampleRunContext
    {
        SampleCLIOptions options;
        RHIBackendType resolvedBackend = RHIBackendType::Auto;
        uint32 frameCount = 0;
    };

    class SampleFeatureReporter
    {
    public:
        explicit SampleFeatureReporter(SampleReport& report);

        void Enable(std::string feature);
        void Unsupported(std::string feature);
        void Fallback(std::string reason);
        void ResourceDiagnostic(std::string diagnostic);

    private:
        SampleReport* m_report = nullptr;
    };
    bool ParseSampleBackend(const std::string& text, RHIBackendType& outBackend);
    const char* GetSampleBackendName(RHIBackendType backend);

    bool ParseSampleCLI(int argc,
                        const char* const* argv,
                        SampleCLIOptions& options,
                        std::string* outError = nullptr);

    void PrintSampleCLIUsage(std::ostream& stream, const char* executableName);

    void WriteSampleReportJson(std::ostream& stream, const SampleReport& report);
    bool WriteSampleReportJson(const SampleReport& report,
                               const std::filesystem::path& path,
                               std::string* outError = nullptr);

    SampleReport BuildSampleReport(const SampleAppDesc& desc, const SampleRunContext& context);
    int RunReportOnlySample(int argc, char* argv[], const SampleAppDesc& desc);
} // namespace RVX
