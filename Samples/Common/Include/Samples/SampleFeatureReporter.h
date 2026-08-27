#pragma once

/**
 * @file SampleFeatureReporter.h
 * @brief Backend-neutral scenario reporting surface.
 */

#include "Core/Types.h"

#include <string>

namespace RVX
{
    struct SampleReport;
    struct SampleReportScenarioAction;
    struct SampleReportScenarioInvariant;
    struct SampleReportScenarioMetric;

    /**
     * @brief Scenario-only report writer.
     *
     * The concrete JSON/report storage remains private to SampleCLI.  Scenario
     * targets only need these semantic operations and therefore do not inherit
     * RHI or Render diagnostic headers through a reporting include.
     */
    class SampleFeatureReporter
    {
    public:
        explicit SampleFeatureReporter(SampleReport& report);

        void Enable(std::string feature);
        void Unsupported(std::string feature);
        void Fallback(std::string reason);
        void ResourceDiagnostic(std::string diagnostic);
        void SetScenarioContractRevision(uint32 contractRevision);
        void SetScenarioPhase(std::string phase);
        bool AppendScenarioAction(SampleReportScenarioAction action);
        bool AppendScenarioInvariant(SampleReportScenarioInvariant invariant);
        bool AppendScenarioMetric(SampleReportScenarioMetric metric);

    private:
        SampleReport* m_report = nullptr;
    };
} // namespace RVX
