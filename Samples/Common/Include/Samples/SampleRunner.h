#pragma once

/** @file SampleRunner.h @brief Shared executable host for sample scenes. */

#include "Samples/SampleCLI.h"
#include "Samples/SampleInfo.h"
#include "Samples/SampleLifetimeQualification.h"
#include "RenderContracts/RenderFrameTypes.h"

#include <filesystem>
#include <string>

namespace RVX
{
    struct EngineRenderRuntimeDiagnostics;
    class SampleRegistry;
    struct RenderDiagnosticsSnapshot;
    struct SampleRunnerTestAccess;

    enum class SampleAssessmentProfile : uint8
    {
        Smoke = 0,
        Qualification,
        Benchmark
    };

    [[nodiscard]] const char* GetSampleAssessmentProfileName(
        SampleAssessmentProfile profile) noexcept;

    struct SampleRunnerCLIOptions
    {
        SampleCLIOptions common;
        std::string sampleId = "model-rendering";
        std::string assetId;
        std::filesystem::path modelPath;
        std::string environmentId;
        std::filesystem::path environmentPath;
        std::filesystem::path catalogPath;
        std::filesystem::path assetRoot;
        SampleRenderPath renderPath = SampleRenderPath::Auto;
        RenderInstancingMode instancingMode = RenderInstancingMode::Auto;
        uint32 readyTimeoutMs = 120000;
        uint32 readyMaxFrames = 0;
        SampleLifetimeQualificationConfig lifetimeConfig{};
        std::filesystem::path lifetimeReportPath;
        std::filesystem::path startupReportPath;
        std::filesystem::path assessmentReportPath;
        std::filesystem::path assessmentBaselinePath;
        SampleAssessmentProfile assessmentProfile =
            SampleAssessmentProfile::Smoke;
        SampleWorkloadScale workloadScale = SampleWorkloadScale::PullRequest;
        bool renderPathExplicit = false;
        bool workloadScaleExplicit = false;
        bool readyTimeoutExplicit = false;
        bool readyMaxFramesExplicit = false;
        bool waitReady = false;
        bool deterministicCameraOrbit = false;
        bool listSamples = false;

        [[nodiscard]] bool HasLifetimeQualification() const noexcept
        {
            return !lifetimeReportPath.empty();
        }

        [[nodiscard]] bool HasFrameworkAssessment() const noexcept
        {
            return !assessmentReportPath.empty();
        }

        /** @brief Captures and qualification runs may only finish on Ready. */
        [[nodiscard]] bool RequiresReadinessWait() const noexcept
        {
            return waitReady || !common.screenshotPath.empty() ||
                   common.pixelProbeEnabled ||
                   common.gpuSceneCullingQualificationEnabled ||
                   common.directOpaqueRasterReadbackQualificationEnabled ||
                   HasLifetimeQualification() || HasFrameworkAssessment();
        }
    };

    /** @brief Effective limits for a readiness wait. */
    struct SampleReadinessWaitBudget
    {
        uint32 timeoutMs = 0;
        uint32 maximumFrames = 0;
    };

    /**
     * @brief Resolves readiness limits after the sample workload has been selected.
     * @param options Parsed command-line options.
     * @param workload Actual workload selected for the sample, if any.
     */
    [[nodiscard]] SampleReadinessWaitBudget ResolveSampleReadinessWaitBudget(
        const SampleRunnerCLIOptions& options,
        const SampleWorkloadProfile* workload) noexcept;

    bool ParseSampleRunnerCLI(int argc,
                              const char* const* argv,
                              SampleRunnerCLIOptions& options,
                              std::string* outError = nullptr);

    void PrintSampleRunnerUsage(const SampleRegistry& registry,
                                const char* executableName);

    class SampleRunner final
    {
    public:
        explicit SampleRunner(const SampleRegistry& registry) noexcept;
        int Run(int argc, char* argv[]) const;

    private:
        [[nodiscard]] static uint64 ResolveFrameWaitTargetSequence(
            uint64 publishedBeforeOuterTick,
            uint64 publishedAfterOuterTick) noexcept;

        [[nodiscard]] static bool CanBeginFinalRenderDrain(
            uint32 executedFrames,
            uint32 minimumFrames,
            uint32 maximumFrames) noexcept;

        [[nodiscard]] static uint64 ResolveSceneRemovalPublicationSequence(
            uint64 removalSceneRevision,
            const EngineRenderRuntimeDiagnostics& diagnostics) noexcept;

        [[nodiscard]] static bool HasCompletedSceneRemovalPublication(
            bool engineInitialized,
            bool deadlineExpired,
            uint64 removalSceneRevision,
            uint64 removalPublicationSequence,
            const RenderDiagnosticsSnapshot& diagnostics) noexcept;

        friend struct SampleRunnerTestAccess;

        const SampleRegistry& m_registry;
    };
} // namespace RVX
