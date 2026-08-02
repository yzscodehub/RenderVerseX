#include "Render/Policy/RenderPolicyDiagnostics.h"
#include "Render/RenderDiagnostics.h"

#include <gtest/gtest.h>

#include <array>
#include <type_traits>
#include <utility>
#include <vector>

using namespace RVX;

namespace
{
    template <typename Enum>
    struct EnumCase
    {
        Enum value;
        uint8 numericValue;
        const char* name;
    };

    template <typename Enum, size_t Count>
    void ExpectEnumCases(
        const std::array<EnumCase<Enum>, Count>& cases,
        const char* (*getName)(Enum))
    {
        for (const EnumCase<Enum>& testCase : cases)
        {
            EXPECT_EQ(testCase.numericValue, static_cast<uint8>(testCase.value));
            EXPECT_STREQ(testCase.name, getName(testCase.value));
        }
    }

    static_assert(std::is_copy_constructible_v<DrawPacketRange>);
    static_assert(std::is_move_constructible_v<DrawPacketRange>);
    static_assert(std::is_copy_constructible_v<RenderFramePolicyRequest>);
    static_assert(std::is_move_constructible_v<RenderFramePolicyRequest>);
    static_assert(std::is_copy_constructible_v<RenderViewPolicy>);
    static_assert(std::is_move_constructible_v<RenderViewPolicy>);
    static_assert(std::is_copy_constructible_v<RenderCapabilitySnapshot>);
    static_assert(std::is_move_constructible_v<RenderCapabilitySnapshot>);
    static_assert(std::is_copy_constructible_v<RenderQualificationSnapshot>);
    static_assert(std::is_move_constructible_v<RenderQualificationSnapshot>);
    static_assert(std::is_copy_constructible_v<RenderPassExecutionPlan>);
    static_assert(std::is_move_constructible_v<RenderPassExecutionPlan>);
    static_assert(std::is_copy_constructible_v<RenderFrameExecutionPlan>);
    static_assert(std::is_move_constructible_v<RenderFrameExecutionPlan>);
    static_assert(std::is_copy_constructible_v<RenderPassLaneExecutionReport>);
    static_assert(std::is_move_constructible_v<RenderPassLaneExecutionReport>);
    static_assert(std::is_copy_constructible_v<RenderPassExecutionReport>);
    static_assert(std::is_move_constructible_v<RenderPassExecutionReport>);
    static_assert(std::is_copy_constructible_v<RenderFrameExecutionReport>);
    static_assert(std::is_move_constructible_v<RenderFrameExecutionReport>);
    static_assert(std::is_copy_constructible_v<RenderPolicyDiagnostics>);
    static_assert(std::is_move_constructible_v<RenderPolicyDiagnostics>);

    static_assert(!std::is_pointer_v<decltype(
        std::declval<RenderFramePolicyRequest>().gpuDrivenMode)>);
    static_assert(!std::is_pointer_v<decltype(
        std::declval<RenderFrameExecutionPlan>().viewPolicy)>);
    static_assert(!std::is_pointer_v<decltype(
        std::declval<RenderFrameExecutionPlan>().passes)>);
    static_assert(std::is_same_v<
        decltype(std::declval<RenderFrameExecutionPlan>().passes),
        std::vector<RenderPassExecutionPlan>>);
    static_assert(!std::is_pointer_v<decltype(
        std::declval<RenderPassExecutionPlan>().gpuEligiblePackets)>);
    static_assert(!std::is_pointer_v<decltype(
        std::declval<RenderPassExecutionPlan>().directPackets)>);
    static_assert(!std::is_pointer_v<decltype(
        std::declval<RenderPassExecutionReport>().gpuDrivenLane)>);
    static_assert(!std::is_pointer_v<decltype(
        std::declval<RenderPassExecutionReport>().directLane)>);
    static_assert(!std::is_pointer_v<decltype(
        std::declval<RenderFrameExecutionReport>().passes)>);
    static_assert(std::is_same_v<
        decltype(std::declval<RenderFrameExecutionReport>().passes),
        std::vector<RenderPassExecutionReport>>);
    static_assert(!std::is_pointer_v<decltype(
        std::declval<RenderPolicyDiagnostics>().selectedPlan)>);
    static_assert(!std::is_pointer_v<decltype(
        std::declval<RenderPolicyDiagnostics>().executionReport)>);

    TEST(RenderPolicyValidation, EnumerationsHaveStableValuesAndNames)
    {
        ExpectEnumCases(
            std::array{
                EnumCase<GPUDrivenTier>{GPUDrivenTier::Direct, 0, "Direct"},
                EnumCase<GPUDrivenTier>{GPUDrivenTier::IndirectGrouped, 1, "IndirectGrouped"},
                EnumCase<GPUDrivenTier>{GPUDrivenTier::GPUResidentScene, 2, "GPUResidentScene"},
                EnumCase<GPUDrivenTier>{GPUDrivenTier::Meshlet, 3, "Meshlet"},
            },
            GetGPUDrivenTierName);

        ExpectEnumCases(
            std::array{
                EnumCase<RenderVisibilityMode>{RenderVisibilityMode::Cpu, 0, "Cpu"},
                EnumCase<RenderVisibilityMode>{RenderVisibilityMode::GpuFrustum, 1, "GpuFrustum"},
                EnumCase<RenderVisibilityMode>{RenderVisibilityMode::GpuFrustumAndDistance, 2, "GpuFrustumAndDistance"},
                EnumCase<RenderVisibilityMode>{RenderVisibilityMode::GpuOcclusion, 3, "GpuOcclusion"},
            },
            GetRenderVisibilityModeName);

        ExpectEnumCases(
            std::array{
                EnumCase<RenderSubmissionMode>{RenderSubmissionMode::Direct, 0, "Direct"},
                EnumCase<RenderSubmissionMode>{RenderSubmissionMode::FixedCountIndirect, 1, "FixedCountIndirect"},
                EnumCase<RenderSubmissionMode>{RenderSubmissionMode::MultiDrawIndirectCount, 2, "MultiDrawIndirectCount"},
                EnumCase<RenderSubmissionMode>{RenderSubmissionMode::EncodedCommandBuffer, 3, "EncodedCommandBuffer"},
            },
            GetRenderSubmissionModeName);

        ExpectEnumCases(
            std::array{
                EnumCase<RenderPassKind>{RenderPassKind::None, 0, "None"},
                EnumCase<RenderPassKind>{RenderPassKind::Depth, 1, "Depth"},
                EnumCase<RenderPassKind>{RenderPassKind::Opaque, 2, "Opaque"},
                EnumCase<RenderPassKind>{RenderPassKind::Shadow, 3, "Shadow"},
                EnumCase<RenderPassKind>{RenderPassKind::Transparent, 4, "Transparent"},
            },
            GetRenderPassKindName);

        ExpectEnumCases(
            std::array{
                EnumCase<RenderExecutionStatus>{RenderExecutionStatus::NotAttempted, 0, "NotAttempted"},
                EnumCase<RenderExecutionStatus>{RenderExecutionStatus::Completed, 1, "Completed"},
                EnumCase<RenderExecutionStatus>{RenderExecutionStatus::Failed, 2, "Failed"},
            },
            GetRenderExecutionStatusName);

        ExpectEnumCases(
            std::array{
                EnumCase<RenderPolicyReason>{RenderPolicyReason::None, 0, "None"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::ConservativeDefault, 1, "ConservativeDefault"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::InvalidRequest, 2, "InvalidRequest"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::ForcedDirect, 3, "ForcedDirect"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::ForcedGPUDriven, 4, "ForcedGPUDriven"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::BackendUnsupported, 5, "BackendUnsupported"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::BackendNotQualified, 6, "BackendNotQualified"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::CapabilityUnavailable, 7, "CapabilityUnavailable"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::PipelineUnavailable, 8, "PipelineUnavailable"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::ResourcesUnavailable, 9, "ResourcesUnavailable"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::PassUnsupported, 10, "PassUnsupported"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::NoEligiblePackets, 11, "NoEligiblePackets"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::WorkloadNotBeneficial, 12, "WorkloadNotBeneficial"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::PlannedFallback, 13, "PlannedFallback"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::UnexpectedRecordingFailure, 14, "UnexpectedRecordingFailure"},
            },
            GetRenderPolicyReasonName);
    }

    TEST(RenderPolicyValidation, InvalidEnumerationsHaveInvalidNames)
    {
        EXPECT_STREQ("Invalid", GetGPUDrivenTierName(
            static_cast<GPUDrivenTier>(0xFFU)));
        EXPECT_STREQ("Invalid", GetRenderVisibilityModeName(
            static_cast<RenderVisibilityMode>(0xFFU)));
        EXPECT_STREQ("Invalid", GetRenderSubmissionModeName(
            static_cast<RenderSubmissionMode>(0xFFU)));
        EXPECT_STREQ("Invalid", GetRenderPassKindName(
            static_cast<RenderPassKind>(0xFFU)));
        EXPECT_STREQ("Invalid", GetRenderExecutionStatusName(
            static_cast<RenderExecutionStatus>(0xFFU)));
        EXPECT_STREQ("Invalid", GetRenderPolicyReasonName(
            static_cast<RenderPolicyReason>(0xFFU)));
    }

    TEST(RenderPolicyValidation, DefaultsFailClosed)
    {
        const DrawPacketRange range;
        EXPECT_EQ(0u, range.first);
        EXPECT_EQ(0u, range.count);

        const RenderFramePolicyRequest request;
        EXPECT_EQ(0u, request.frameSequence);
        EXPECT_EQ(RenderGPUDrivenMode::Auto, request.gpuDrivenMode);

        const RenderViewPolicy viewPolicy;
        EXPECT_EQ(RenderGPUDrivenMode::Auto, viewPolicy.requestedMode);
        EXPECT_EQ(GPUDrivenTier::Direct, viewPolicy.selectedTier);
        EXPECT_EQ(RenderPolicyReason::ConservativeDefault, viewPolicy.reason);

        const RenderCapabilitySnapshot capability;
        EXPECT_EQ(RHIBackendType::None, capability.backend);
        EXPECT_FALSE(capability.supportsComputeVisibility);
        EXPECT_FALSE(capability.supportsFixedCountIndirect);
        EXPECT_FALSE(capability.supportsIndirectDrawCount);
        EXPECT_FALSE(capability.supportsEncodedCommandBuffer);

        const RenderQualificationSnapshot qualification;
        EXPECT_EQ(GPUDrivenQualificationLevel::Unqualified, qualification.level);
        EXPECT_EQ(0u, qualification.revision);
        EXPECT_EQ(0u, qualification.passedGateMask);
        EXPECT_EQ(RVX_GPU_DRIVEN_REQUIRED_QUALIFICATION_GATE_MASK,
                  qualification.requiredGateMask);
        EXPECT_NE(0u, qualification.requiredGateMask &
                  ~qualification.passedGateMask);

        const RenderPassExecutionPlan passPlan;
        EXPECT_EQ(RenderPassKind::None, passPlan.pass);
        EXPECT_EQ(RenderVisibilityMode::Cpu, passPlan.visibility);
        EXPECT_EQ(RenderSubmissionMode::Direct,
                  passPlan.preferredSubmission);
        EXPECT_EQ(RenderSubmissionMode::Direct,
                  passPlan.fallbackSubmission);
        EXPECT_EQ(0u, passPlan.gpuEligiblePackets.first);
        EXPECT_EQ(0u, passPlan.gpuEligiblePackets.count);
        EXPECT_EQ(0u, passPlan.directPackets.first);
        EXPECT_EQ(0u, passPlan.directPackets.count);
        EXPECT_EQ(RenderPolicyReason::ConservativeDefault, passPlan.reason);

        const RenderFrameExecutionPlan framePlan;
        EXPECT_EQ(0u, framePlan.frameSequence);
        EXPECT_TRUE(framePlan.passes.empty());
        EXPECT_EQ(GPUDrivenTier::Direct, framePlan.viewPolicy.selectedTier);
        EXPECT_EQ(RHIBackendType::None, framePlan.capabilities.backend);
        EXPECT_EQ(GPUDrivenQualificationLevel::Unqualified,
                  framePlan.qualification.level);

        const RenderPassExecutionReport passReport;
        EXPECT_EQ(RenderPassKind::None, passReport.pass);
        EXPECT_EQ(RenderExecutionStatus::NotAttempted, passReport.status);
        EXPECT_EQ(RenderVisibilityMode::Cpu, passReport.executedVisibility);
        EXPECT_EQ(RenderExecutionStatus::NotAttempted,
                  passReport.gpuDrivenLane.status);
        EXPECT_EQ(RenderSubmissionMode::Direct,
                  passReport.gpuDrivenLane.submission);
        EXPECT_EQ(0u, passReport.gpuDrivenLane.packetRange.first);
        EXPECT_EQ(0u, passReport.gpuDrivenLane.packetRange.count);
        EXPECT_EQ(0u, passReport.gpuDrivenLane.executedPacketCount);
        EXPECT_EQ(0u, passReport.gpuDrivenLane.executedDrawCount);
        EXPECT_EQ(RenderPolicyReason::ConservativeDefault,
                  passReport.gpuDrivenLane.reason);
        EXPECT_EQ(RenderExecutionStatus::NotAttempted,
                  passReport.directLane.status);
        EXPECT_EQ(RenderSubmissionMode::Direct,
                  passReport.directLane.submission);
        EXPECT_EQ(0u, passReport.directLane.packetRange.first);
        EXPECT_EQ(0u, passReport.directLane.packetRange.count);
        EXPECT_EQ(0u, passReport.directLane.executedPacketCount);
        EXPECT_EQ(0u, passReport.directLane.executedDrawCount);
        EXPECT_EQ(RenderPolicyReason::ConservativeDefault,
                  passReport.directLane.reason);
        EXPECT_EQ(0u, passReport.skippedPacketCount);
        EXPECT_EQ(RenderPolicyReason::ConservativeDefault, passReport.reason);

        const RenderFrameExecutionReport frameReport;
        EXPECT_EQ(0u, frameReport.frameSequence);
        EXPECT_EQ(RenderExecutionStatus::NotAttempted, frameReport.status);
        EXPECT_TRUE(frameReport.passes.empty());

        const RenderPolicyDiagnostics diagnostics;
        EXPECT_FALSE(diagnostics.requestAvailable);
        EXPECT_FALSE(diagnostics.planAvailable);
        EXPECT_FALSE(diagnostics.reportAvailable);
        EXPECT_EQ(0u, diagnostics.request.frameSequence);
        EXPECT_TRUE(diagnostics.selectedPlan.passes.empty());
        EXPECT_EQ(RenderExecutionStatus::NotAttempted,
                  diagnostics.executionReport.status);

        const RenderFrameFeatureDiagnostics frameDiagnostics;
        EXPECT_FALSE(frameDiagnostics.policy.requestAvailable);
        EXPECT_FALSE(frameDiagnostics.policy.planAvailable);
        EXPECT_FALSE(frameDiagnostics.policy.reportAvailable);
    }

    TEST(RenderPolicyValidation, RequestPlanAndReportRemainIndependentValueContracts)
    {
        RenderPolicyDiagnostics diagnostics;
        diagnostics.request.frameSequence = 17;
        diagnostics.request.gpuDrivenMode = RenderGPUDrivenMode::ForceEnabled;
        diagnostics.selectedPlan.frameSequence = 17;
        diagnostics.selectedPlan.viewPolicy.requestedMode =
            RenderGPUDrivenMode::ForceEnabled;
        diagnostics.selectedPlan.viewPolicy.selectedTier =
            GPUDrivenTier::IndirectGrouped;
        diagnostics.executionReport.frameSequence = 17;
        diagnostics.executionReport.status = RenderExecutionStatus::Completed;
        RenderPassExecutionReport passReport;
        passReport.pass = RenderPassKind::Opaque;
        passReport.status = RenderExecutionStatus::Completed;
        passReport.executedVisibility = RenderVisibilityMode::GpuFrustum;
        passReport.directLane.status = RenderExecutionStatus::Completed;
        passReport.directLane.executedDrawCount = 5;
        passReport.reason = RenderPolicyReason::None;
        diagnostics.executionReport.passes.push_back(passReport);

        RenderPolicyDiagnostics copied = diagnostics;
        RenderPolicyDiagnostics moved = std::move(copied);
        moved.request.frameSequence = 18;
        moved.selectedPlan.viewPolicy.selectedTier = GPUDrivenTier::Meshlet;
        moved.executionReport.passes.front().directLane.executedDrawCount = 7;

        EXPECT_EQ(17u, diagnostics.request.frameSequence);
        EXPECT_EQ(GPUDrivenTier::IndirectGrouped,
                  diagnostics.selectedPlan.viewPolicy.selectedTier);
        EXPECT_EQ(5u,
                  diagnostics.executionReport.passes.front().directLane.executedDrawCount);
        EXPECT_EQ(18u, moved.request.frameSequence);
        EXPECT_EQ(GPUDrivenTier::Meshlet,
                  moved.selectedPlan.viewPolicy.selectedTier);
        EXPECT_EQ(7u,
                  moved.executionReport.passes.front().directLane.executedDrawCount);
    }

    TEST(RenderPolicyValidation, HybridPassReportPreservesIndependentLanes)
    {
        RenderPassExecutionReport report;
        report.pass = RenderPassKind::Opaque;
        report.status = RenderExecutionStatus::Completed;
        report.executedVisibility = RenderVisibilityMode::GpuFrustum;
        report.gpuDrivenLane = RenderPassLaneExecutionReport{
            RenderExecutionStatus::Completed,
            RenderSubmissionMode::MultiDrawIndirectCount,
            DrawPacketRange{4, 3},
            3,
            6,
            RenderPolicyReason::None,
        };
        report.directLane = RenderPassLaneExecutionReport{
            RenderExecutionStatus::Completed,
            RenderSubmissionMode::Direct,
            DrawPacketRange{7, 2},
            2,
            2,
            RenderPolicyReason::None,
        };
        report.skippedPacketCount = 1;
        report.reason = RenderPolicyReason::None;

        EXPECT_EQ(RenderSubmissionMode::MultiDrawIndirectCount,
                  report.gpuDrivenLane.submission);
        EXPECT_EQ(RenderExecutionStatus::Completed,
                  report.gpuDrivenLane.status);
        EXPECT_EQ(4u, report.gpuDrivenLane.packetRange.first);
        EXPECT_EQ(3u, report.gpuDrivenLane.packetRange.count);
        EXPECT_EQ(3u, report.gpuDrivenLane.executedPacketCount);
        EXPECT_EQ(6u, report.gpuDrivenLane.executedDrawCount);
        EXPECT_EQ(RenderSubmissionMode::Direct, report.directLane.submission);
        EXPECT_EQ(RenderExecutionStatus::Completed, report.directLane.status);
        EXPECT_EQ(7u, report.directLane.packetRange.first);
        EXPECT_EQ(2u, report.directLane.packetRange.count);
        EXPECT_EQ(2u, report.directLane.executedPacketCount);
        EXPECT_EQ(2u, report.directLane.executedDrawCount);
        EXPECT_LE(report.gpuDrivenLane.packetRange.first +
                      report.gpuDrivenLane.packetRange.count,
                  report.directLane.packetRange.first);
        EXPECT_EQ(1u, report.skippedPacketCount);
    }
} // namespace
