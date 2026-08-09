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
    class SampleRegistry;

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
        bool renderPathExplicit = false;
        bool waitReady = false;
        bool deterministicCameraOrbit = false;
        bool listSamples = false;

        [[nodiscard]] bool HasLifetimeQualification() const noexcept
        {
            return !lifetimeReportPath.empty();
        }

        /** @brief Captures and qualification runs may only finish on Ready. */
        [[nodiscard]] bool RequiresReadinessWait() const noexcept
        {
            return waitReady || !common.screenshotPath.empty() ||
                   HasLifetimeQualification();
        }
    };

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
        const SampleRegistry& m_registry;
    };
} // namespace RVX
