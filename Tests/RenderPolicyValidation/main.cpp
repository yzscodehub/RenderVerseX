#include "Render/Policy/RenderPolicyDiagnostics.h"
#include "Render/Policy/RenderPolicyResolver.h"
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
    static_assert(!std::is_pointer_v<decltype(
        std::declval<RenderPolicyResolverInput>().view)>);
    static_assert(!std::is_pointer_v<decltype(
        std::declval<RenderPolicyResolverInput>().passes)>);
    static_assert(std::is_same_v<
        decltype(std::declval<RenderPolicyResolverInput>().passes),
        std::vector<RenderPassPolicyFacts>>);
    static_assert(std::is_copy_constructible_v<RenderPolicyViewFacts>);
    static_assert(std::is_copy_constructible_v<RenderPassPolicyFacts>);
    static_assert(std::is_copy_constructible_v<RenderPolicyResolverInput>);
    static_assert(std::is_copy_constructible_v<RenderPassPolicyDecision>);
    static_assert(std::is_copy_constructible_v<RenderPolicyResolution>);
    static_assert(!std::is_pointer_v<decltype(
        std::declval<RenderFrameExecutionPlan>().packetReferences)>);
    static_assert(std::is_same_v<
        decltype(std::declval<RenderFrameExecutionPlan>().packetReferences),
        std::vector<RenderDrawPacketReference>>);

    RenderPolicyResolverInput MakeValidResolverInput()
    {
        RenderPolicyResolverInput input;
        input.request.frameSequence = 31;
        input.request.gpuDrivenMode = RenderGPUDrivenMode::Auto;
        input.viewOrdinal = 2;
        input.view.rendererAllowsGPUDriven = true;
        input.view.viewAllowsGPUDriven = true;
        input.view.implementationAvailable = true;
        input.view.requestedVisibility = RenderVisibilityMode::GpuFrustum;
        input.view.visibilityShaderReadiness = RenderPolicyReadiness::Ready;
        input.view.visibilityPipelineReadiness = RenderPolicyReadiness::Ready;
        input.view.sharedResourceReadiness = RenderPolicyReadiness::Ready;
        input.view.requiredBindingReadiness = RenderPolicyReadiness::Ready;
        input.capabilities.backend = RHIBackendType::DX12;
        input.capabilities.supportsComputeVisibility = true;
        input.capabilities.supportsDescriptorResourceBindings = true;
        input.capabilities.supportsFixedCountIndirect = true;
        input.capabilities.supportsIndirectDrawCount = true;
        input.qualification.backend = RHIBackendType::DX12;
        input.qualification.revision = 1;
        input.qualification.passedGateMask = input.qualification.requiredGateMask;

        RenderPassPolicyFacts opaque;
        opaque.pass = RenderPassKind::Opaque;
        opaque.requested = true;
        opaque.supported = true;
        opaque.directAllowed = true;
        opaque.gpuDrivenAllowed = true;
        opaque.fixedCountIndirectAllowed = true;
        opaque.directShaderReadiness = RenderPolicyReadiness::Ready;
        opaque.directPipelineReadiness = RenderPolicyReadiness::Ready;
        opaque.directResourceReadiness = RenderPolicyReadiness::Ready;
        opaque.gpuDrivenShaderReadiness = RenderPolicyReadiness::Ready;
        opaque.gpuDrivenPipelineReadiness = RenderPolicyReadiness::Ready;
        opaque.gpuDrivenResourceReadiness = RenderPolicyReadiness::Ready;
        opaque.inputPacketCount = 5;
        opaque.relevantPacketCount = 4;
        opaque.candidatePacketCount = 3;
        opaque.directPacketCount = 1;
        opaque.skippedPacketCount = 1;
        opaque.drawGroupCount = 1;
        opaque.workloadBeneficial = true;
        input.passes.push_back(opaque);
        return input;
    }

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
                EnumCase<RenderPolicyReason>{RenderPolicyReason::RendererPathUnsupported, 15, "RendererPathUnsupported"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::ViewUnsupported, 16, "ViewUnsupported"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::QualificationInvalid, 17, "QualificationInvalid"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::ShaderUnavailable, 18, "ShaderUnavailable"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::ResourcesPending, 19, "ResourcesPending"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::BindingsUnavailable, 20, "BindingsUnavailable"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::PassDisabled, 21, "PassDisabled"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::InconsistentFacts, 22, "InconsistentFacts"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::ShaderPending, 23, "ShaderPending"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::PipelinePending, 24, "PipelinePending"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::BindingsPending, 25, "BindingsPending"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::Count, 26, "Count"},
            },
            GetRenderPolicyReasonName);

        ExpectEnumCases(
            std::array{
                EnumCase<RenderPolicyReadiness>{RenderPolicyReadiness::Unavailable, 0, "Unavailable"},
                EnumCase<RenderPolicyReadiness>{RenderPolicyReadiness::Pending, 1, "Pending"},
                EnumCase<RenderPolicyReadiness>{RenderPolicyReadiness::Ready, 2, "Ready"},
                EnumCase<RenderPolicyReadiness>{RenderPolicyReadiness::Count, 3, "Count"},
            },
            GetRenderPolicyReadinessName);
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
        EXPECT_FALSE(capability.supportsDescriptorResourceBindings);

        const RenderQualificationSnapshot qualification;
        EXPECT_EQ(GPUDrivenQualificationLevel::Unqualified, qualification.level);
        EXPECT_EQ(RVX_GPU_DRIVEN_QUALIFICATION_SCHEMA_VERSION,
                  qualification.schemaVersion);
        EXPECT_EQ(RHIBackendType::None, qualification.backend);
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
        EXPECT_EQ(0u, framePlan.viewOrdinal);
        EXPECT_TRUE(framePlan.packetReferences.empty());
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

    TEST(RenderPolicyValidation, ResolverProducesCanonicalOwnedHybridResolution)
    {
        RenderPolicyResolverInput input = MakeValidResolverInput();
        RenderPassPolicyFacts depth = input.passes.front();
        depth.pass = RenderPassKind::Depth;
        depth.workloadBeneficial = false;
        input.passes.insert(input.passes.begin(), depth);

        ASSERT_TRUE(ValidateRenderPolicyResolverInput(input));
        const RenderPolicyResolution resolution = ResolveRenderPolicy(input);
        ASSERT_TRUE(ValidateRenderPolicyResolution(resolution));
        ASSERT_EQ(2u, resolution.canonicalPassDecisions.size());
        EXPECT_EQ(RenderPassKind::Depth,
                  resolution.canonicalPassDecisions[0].pass);
        EXPECT_EQ(RenderPassKind::Opaque,
                  resolution.canonicalPassDecisions[1].pass);
        EXPECT_EQ(GPUDrivenTier::IndirectGrouped,
                  resolution.viewPolicy.selectedTier);

        const RenderPassPolicyDecision& opaque =
            resolution.canonicalPassDecisions[1];
        EXPECT_EQ(RenderSubmissionMode::MultiDrawIndirectCount,
                  opaque.preferredSubmission);
        EXPECT_EQ(RenderVisibilityMode::GpuFrustum, opaque.visibility);
        EXPECT_EQ(3u, opaque.partition.gpuDrivenPacketCount);
        EXPECT_EQ(1u, opaque.partition.directPacketCount);
        EXPECT_EQ(1u, opaque.partition.skippedPacketCount);
        EXPECT_EQ(1u, opaque.partition.drawGroupCount);

        std::swap(input.passes[0], input.passes[1]);
        EXPECT_EQ(resolution, ResolveRenderPolicy(input));
    }

    TEST(RenderPolicyValidation, ResolverGlobalAndPassGatesFailClosed)
    {
        RenderPolicyResolverInput input = MakeValidResolverInput();

        input.view.rendererAllowsGPUDriven = false;
        RenderPolicyResolution resolution = ResolveRenderPolicy(input);
        EXPECT_EQ(RenderPolicyReason::RendererPathUnsupported,
                  resolution.viewPolicy.reason);
        EXPECT_EQ(0u, resolution.canonicalPassDecisions.front().partition.gpuDrivenPacketCount);

        input = MakeValidResolverInput();
        input.view.sharedResourceReadiness = RenderPolicyReadiness::Pending;
        resolution = ResolveRenderPolicy(input);
        EXPECT_EQ(RenderPolicyReason::ResourcesPending,
                  resolution.viewPolicy.reason);

        input = MakeValidResolverInput();
        input.view.requiredBindingReadiness = RenderPolicyReadiness::Unavailable;
        resolution = ResolveRenderPolicy(input);
        EXPECT_EQ(RenderPolicyReason::BindingsUnavailable,
                  resolution.viewPolicy.reason);

        input = MakeValidResolverInput();
        input.request.gpuDrivenMode = RenderGPUDrivenMode::ForceDisabled;
        input.qualification.passedGateMask = 0;
        resolution = ResolveRenderPolicy(input);
        EXPECT_EQ(GPUDrivenTier::Direct, resolution.viewPolicy.selectedTier);
        EXPECT_EQ(RenderPolicyReason::ForcedDirect, resolution.viewPolicy.reason);

        input = MakeValidResolverInput();
        input.request.gpuDrivenMode = RenderGPUDrivenMode::Auto;
        input.qualification.passedGateMask = 0;
        resolution = ResolveRenderPolicy(input);
        EXPECT_EQ(RenderPolicyReason::BackendNotQualified,
                  resolution.viewPolicy.reason);

        input = MakeValidResolverInput();
        input.request.gpuDrivenMode = RenderGPUDrivenMode::ForceEnabled;
        input.qualification.schemaVersion++;
        EXPECT_TRUE(ValidateRenderPolicyResolverInput(input));
        resolution = ResolveRenderPolicy(input);
        EXPECT_EQ(RenderPolicyReason::QualificationInvalid,
                  resolution.viewPolicy.reason);

        input = MakeValidResolverInput();
        input.passes.front().workloadBeneficial = false;
        resolution = ResolveRenderPolicy(input);
        EXPECT_EQ(RenderPolicyReason::WorkloadNotBeneficial,
                  resolution.viewPolicy.reason);
        EXPECT_EQ(0u, resolution.canonicalPassDecisions.front().partition.gpuDrivenPacketCount);
    }

    TEST(RenderPolicyValidation, ResolverUsesStableGlobalGatePrecedence)
    {
        RenderPolicyResolverInput input = MakeValidResolverInput();
        input.view.rendererAllowsGPUDriven = false;
        input.view.viewAllowsGPUDriven = false;
        input.view.implementationAvailable = false;
        input.capabilities.supportsComputeVisibility = false;
        EXPECT_EQ(RenderPolicyReason::RendererPathUnsupported,
                  ResolveRenderPolicy(input).viewPolicy.reason);

        input = MakeValidResolverInput();
        input.view.viewAllowsGPUDriven = false;
        input.view.implementationAvailable = false;
        EXPECT_EQ(RenderPolicyReason::ViewUnsupported,
                  ResolveRenderPolicy(input).viewPolicy.reason);

        input = MakeValidResolverInput();
        input.view.implementationAvailable = false;
        input.capabilities.supportsComputeVisibility = false;
        EXPECT_EQ(RenderPolicyReason::BackendUnsupported,
                  ResolveRenderPolicy(input).viewPolicy.reason);

        input = MakeValidResolverInput();
        input.capabilities.supportsComputeVisibility = false;
        input.qualification.schemaVersion++;
        EXPECT_EQ(RenderPolicyReason::CapabilityUnavailable,
                  ResolveRenderPolicy(input).viewPolicy.reason);

        input = MakeValidResolverInput();
        input.qualification.schemaVersion++;
        EXPECT_EQ(RenderPolicyReason::QualificationInvalid,
                  ResolveRenderPolicy(input).viewPolicy.reason);

        input = MakeValidResolverInput();
        input.view.visibilityShaderReadiness = RenderPolicyReadiness::Unavailable;
        EXPECT_EQ(RenderPolicyReason::ShaderUnavailable,
                  ResolveRenderPolicy(input).viewPolicy.reason);

        input = MakeValidResolverInput();
        input.view.visibilityPipelineReadiness = RenderPolicyReadiness::Pending;
        EXPECT_EQ(RenderPolicyReason::PipelinePending,
                  ResolveRenderPolicy(input).viewPolicy.reason);

        input = MakeValidResolverInput();
        input.view.visibilityShaderReadiness = RenderPolicyReadiness::Pending;
        EXPECT_EQ(RenderPolicyReason::ShaderPending,
                  ResolveRenderPolicy(input).viewPolicy.reason);

        input = MakeValidResolverInput();
        input.view.requiredBindingReadiness = RenderPolicyReadiness::Pending;
        EXPECT_EQ(RenderPolicyReason::BindingsPending,
                  ResolveRenderPolicy(input).viewPolicy.reason);
    }

    TEST(RenderPolicyValidation, ResolverPassFallbackAndSubmissionStrategiesAreExplicit)
    {
        RenderPolicyResolverInput input = MakeValidResolverInput();
        RenderPassPolicyFacts& facts = input.passes.front();

        facts.requested = false;
        RenderPolicyResolution resolution = ResolveRenderPolicy(input);
        const RenderPassPolicyDecision& disabled =
            resolution.canonicalPassDecisions.front();
        EXPECT_EQ(RenderPolicyReason::PassDisabled, disabled.reason);
        EXPECT_EQ(0u, disabled.partition.directPacketCount);
        EXPECT_EQ(disabled.partition.inputPacketCount,
                  disabled.partition.skippedPacketCount);

        input = MakeValidResolverInput();
        input.passes.front().supported = false;
        resolution = ResolveRenderPolicy(input);
        const RenderPassPolicyDecision& unsupported =
            resolution.canonicalPassDecisions.front();
        EXPECT_EQ(RenderPolicyReason::PassUnsupported, unsupported.reason);
        EXPECT_EQ(0u, unsupported.partition.directPacketCount);
        EXPECT_EQ(unsupported.partition.inputPacketCount,
                  unsupported.partition.skippedPacketCount);

        input = MakeValidResolverInput();
        input.request.gpuDrivenMode = RenderGPUDrivenMode::ForceDisabled;
        input.passes.front().supported = false;
        resolution = ResolveRenderPolicy(input);
        const RenderPassPolicyDecision& forceDisabledUnsupported =
            resolution.canonicalPassDecisions.front();
        EXPECT_EQ(RenderPolicyReason::ForcedDirect,
                  resolution.viewPolicy.reason);
        EXPECT_EQ(RenderPolicyReason::PassUnsupported,
                  forceDisabledUnsupported.reason);
        EXPECT_EQ(0u,
                  forceDisabledUnsupported.partition.directPacketCount);
        EXPECT_EQ(forceDisabledUnsupported.partition.inputPacketCount,
                  forceDisabledUnsupported.partition.skippedPacketCount);
        EXPECT_TRUE(ValidateRenderPolicyResolution(resolution));

        input = MakeValidResolverInput();
        input.passes.front().gpuDrivenAllowed = false;
        resolution = ResolveRenderPolicy(input);
        const RenderPassPolicyDecision& directFallback =
            resolution.canonicalPassDecisions.front();
        EXPECT_EQ(RenderSubmissionMode::Direct,
                  directFallback.preferredSubmission);
        EXPECT_EQ(4u, directFallback.partition.directPacketCount);
        EXPECT_EQ(1u, directFallback.partition.skippedPacketCount);

        input = MakeValidResolverInput();
        input.passes.front().gpuDrivenResourceReadiness =
            RenderPolicyReadiness::Pending;
        resolution = ResolveRenderPolicy(input);
        EXPECT_EQ(RenderPolicyReason::ResourcesPending,
                  resolution.canonicalPassDecisions.front().reason);

        input = MakeValidResolverInput();
        input.passes.front().gpuDrivenShaderReadiness =
            RenderPolicyReadiness::Pending;
        resolution = ResolveRenderPolicy(input);
        EXPECT_EQ(RenderPolicyReason::ShaderPending,
                  resolution.canonicalPassDecisions.front().reason);

        input = MakeValidResolverInput();
        input.passes.front().gpuDrivenPipelineReadiness =
            RenderPolicyReadiness::Pending;
        resolution = ResolveRenderPolicy(input);
        EXPECT_EQ(RenderPolicyReason::PipelinePending,
                  resolution.canonicalPassDecisions.front().reason);

        input = MakeValidResolverInput();
        input.passes.front().candidatePacketCount = 0;
        input.passes.front().directPacketCount = 4;
        input.passes.front().drawGroupCount = 0;
        resolution = ResolveRenderPolicy(input);
        EXPECT_EQ(RenderPolicyReason::NoEligiblePackets,
                  resolution.canonicalPassDecisions.front().reason);
        EXPECT_EQ(4u, resolution.canonicalPassDecisions.front().partition.directPacketCount);

        input = MakeValidResolverInput();
        input.capabilities.supportsEncodedCommandBuffer = true;
        resolution = ResolveRenderPolicy(input);
        EXPECT_EQ(RenderSubmissionMode::EncodedCommandBuffer,
                  resolution.canonicalPassDecisions.front().preferredSubmission);

        input = MakeValidResolverInput();
        input.capabilities.supportsIndirectDrawCount = false;
        resolution = ResolveRenderPolicy(input);
        EXPECT_EQ(RenderSubmissionMode::FixedCountIndirect,
                  resolution.canonicalPassDecisions.front().preferredSubmission);
    }

    TEST(RenderPolicyValidation, ResolverExhaustiveGlobalGateMatrixIsStable)
    {
        constexpr uint32 kRendererBit = 1u << 0u;
        constexpr uint32 kViewBit = 1u << 1u;
        constexpr uint32 kImplementationBit = 1u << 2u;
        constexpr uint32 kComputeBit = 1u << 3u;
        constexpr uint32 kDescriptorBit = 1u << 4u;
        constexpr uint32 kIndirectBit = 1u << 5u;
        constexpr uint32 kShaderBit = 1u << 6u;
        constexpr uint32 kPipelineBit = 1u << 7u;
        constexpr uint32 kSharedResourceBit = 1u << 8u;
        constexpr uint32 kRequiredBindingBit = 1u << 9u;
        constexpr uint32 kAllGateBits = (1u << 10u) - 1u;

        for (uint32 modeCase = 0; modeCase < 4; ++modeCase)
        {
            const RenderGPUDrivenMode mode = modeCase < 3
                ? static_cast<RenderGPUDrivenMode>(modeCase)
                : static_cast<RenderGPUDrivenMode>(0xFFU);
            for (uint32 qualificationCase = 0;
                 qualificationCase < 4;
                 ++qualificationCase)
            {
                for (uint32 gates = 0; gates <= kAllGateBits; ++gates)
                {
                    RenderPolicyResolverInput input = MakeValidResolverInput();
                    input.request.gpuDrivenMode = mode;
                    input.view.rendererAllowsGPUDriven =
                        (gates & kRendererBit) != 0;
                    input.view.viewAllowsGPUDriven =
                        (gates & kViewBit) != 0;
                    input.view.implementationAvailable =
                        (gates & kImplementationBit) != 0;
                    input.capabilities.supportsComputeVisibility =
                        (gates & kComputeBit) != 0;
                    input.capabilities.supportsDescriptorResourceBindings =
                        (gates & kDescriptorBit) != 0;
                    input.capabilities.supportsIndirectDrawCount =
                        (gates & kIndirectBit) != 0;
                    input.capabilities.supportsFixedCountIndirect = false;
                    input.view.visibilityShaderReadiness =
                        (gates & kShaderBit) != 0
                            ? RenderPolicyReadiness::Ready
                            : RenderPolicyReadiness::Unavailable;
                    input.view.visibilityPipelineReadiness =
                        (gates & kPipelineBit) != 0
                            ? RenderPolicyReadiness::Ready
                            : RenderPolicyReadiness::Unavailable;
                    input.view.sharedResourceReadiness =
                        (gates & kSharedResourceBit) != 0
                            ? RenderPolicyReadiness::Ready
                            : RenderPolicyReadiness::Unavailable;
                    input.view.requiredBindingReadiness =
                        (gates & kRequiredBindingBit) != 0
                            ? RenderPolicyReadiness::Ready
                            : RenderPolicyReadiness::Unavailable;

                    switch (qualificationCase)
                    {
                        case 0:
                            input.qualification.schemaVersion++;
                            break;
                        case 1:
                            input.qualification.revision = 0;
                            input.qualification.passedGateMask = 0;
                            break;
                        case 2:
                            input.qualification.revision = 1;
                            input.qualification.passedGateMask = 1;
                            break;
                        case 3:
                            break;
                        default:
                            FAIL() << "Unexpected qualification case";
                    }

                    RenderPolicyReason expectedReason = RenderPolicyReason::None;
                    bool expectedGPUDriven = false;
                    if (modeCase == 3)
                    {
                        expectedReason = RenderPolicyReason::InvalidRequest;
                    }
                    else if (mode == RenderGPUDrivenMode::ForceDisabled)
                    {
                        expectedReason = RenderPolicyReason::ForcedDirect;
                    }
                    else if (!input.view.rendererAllowsGPUDriven)
                    {
                        expectedReason =
                            RenderPolicyReason::RendererPathUnsupported;
                    }
                    else if (!input.view.viewAllowsGPUDriven)
                    {
                        expectedReason = RenderPolicyReason::ViewUnsupported;
                    }
                    else if (!input.view.implementationAvailable)
                    {
                        expectedReason = RenderPolicyReason::BackendUnsupported;
                    }
                    else if (!input.capabilities.supportsComputeVisibility ||
                             !input.capabilities.supportsDescriptorResourceBindings ||
                             !input.capabilities.supportsIndirectDrawCount)
                    {
                        expectedReason = RenderPolicyReason::CapabilityUnavailable;
                    }
                    else if (qualificationCase == 0)
                    {
                        expectedReason = RenderPolicyReason::QualificationInvalid;
                    }
                    else if (mode == RenderGPUDrivenMode::Auto &&
                             qualificationCase != 3)
                    {
                        expectedReason = RenderPolicyReason::BackendNotQualified;
                    }
                    else if (input.view.visibilityShaderReadiness !=
                             RenderPolicyReadiness::Ready)
                    {
                        expectedReason = RenderPolicyReason::ShaderUnavailable;
                    }
                    else if (input.view.visibilityPipelineReadiness !=
                             RenderPolicyReadiness::Ready)
                    {
                        expectedReason = RenderPolicyReason::PipelineUnavailable;
                    }
                    else if (input.view.sharedResourceReadiness !=
                             RenderPolicyReadiness::Ready)
                    {
                        expectedReason = RenderPolicyReason::ResourcesUnavailable;
                    }
                    else if (input.view.requiredBindingReadiness !=
                             RenderPolicyReadiness::Ready)
                    {
                        expectedReason = RenderPolicyReason::BindingsUnavailable;
                    }
                    else
                    {
                        expectedGPUDriven = true;
                        expectedReason = mode == RenderGPUDrivenMode::ForceEnabled
                            ? RenderPolicyReason::ForcedGPUDriven
                            : RenderPolicyReason::None;
                    }

                    const RenderPolicyResolution resolution =
                        ResolveRenderPolicy(input);
                    EXPECT_EQ(expectedReason, resolution.viewPolicy.reason)
                        << "mode=" << modeCase
                        << " qualification=" << qualificationCase
                        << " gates=" << gates;
                    EXPECT_EQ(expectedGPUDriven
                                  ? GPUDrivenTier::IndirectGrouped
                                  : GPUDrivenTier::Direct,
                              resolution.viewPolicy.selectedTier)
                        << "mode=" << modeCase
                        << " qualification=" << qualificationCase
                        << " gates=" << gates;
                    if (modeCase < 3)
                    {
                        EXPECT_TRUE(ValidateRenderPolicyResolution(resolution))
                            << "mode=" << modeCase
                            << " qualification=" << qualificationCase
                            << " gates=" << gates;
                    }
                }
            }
        }
    }

    TEST(RenderPolicyValidation, ForceDisabledNeedsNoDeviceAndDirectFailureStaysVisible)
    {
        RenderPolicyResolverInput directOnly;
        directOnly.request.gpuDrivenMode = RenderGPUDrivenMode::ForceDisabled;
        ASSERT_TRUE(ValidateRenderPolicyResolverInput(directOnly));
        RenderPolicyResolution resolution = ResolveRenderPolicy(directOnly);
        EXPECT_EQ(RenderPolicyReason::ForcedDirect,
                  resolution.viewPolicy.reason);
        EXPECT_TRUE(ValidateRenderPolicyResolution(resolution));

        RenderPolicyResolverInput input = MakeValidResolverInput();
        input.capabilities.supportsComputeVisibility = false;
        input.passes.front().directResourceReadiness =
            RenderPolicyReadiness::Pending;
        resolution = ResolveRenderPolicy(input);
        EXPECT_EQ(RenderPolicyReason::CapabilityUnavailable,
                  resolution.viewPolicy.reason);
        EXPECT_EQ(RenderPolicyReason::ResourcesPending,
                  resolution.canonicalPassDecisions.front().reason);
        EXPECT_EQ(0u, resolution.canonicalPassDecisions.front().partition.directPacketCount);
        EXPECT_EQ(resolution.canonicalPassDecisions.front().partition.inputPacketCount,
                  resolution.canonicalPassDecisions.front().partition.skippedPacketCount);

        input = MakeValidResolverInput();
        input.request.gpuDrivenMode = RenderGPUDrivenMode::ForceEnabled;
        resolution = ResolveRenderPolicy(input);
        EXPECT_EQ(GPUDrivenTier::IndirectGrouped,
                  resolution.viewPolicy.selectedTier);
        EXPECT_EQ(RenderPolicyReason::ForcedGPUDriven,
                  resolution.viewPolicy.reason);
    }

    TEST(RenderPolicyValidation, ResolverRejectsMalformedCountsAndFramePlanRanges)
    {
        RenderPolicyResolverInput input = MakeValidResolverInput();
        input.passes.push_back(input.passes.front());
        EXPECT_FALSE(ValidateRenderPolicyResolverInput(input));
        EXPECT_EQ(RenderPolicyReason::InconsistentFacts,
                  ResolveRenderPolicy(input).viewPolicy.reason);

        input = MakeValidResolverInput();
        input.passes.front().relevantPacketCount = 5;
        EXPECT_FALSE(ValidateRenderPolicyResolverInput(input));

        input = MakeValidResolverInput();
        input.passes.front().candidatePacketCount = UINT32_MAX;
        EXPECT_FALSE(ValidateRenderPolicyResolverInput(input));

        input = MakeValidResolverInput();
        input.passes.front().pass = RenderPassKind::None;
        EXPECT_FALSE(ValidateRenderPolicyResolverInput(input));

        input = MakeValidResolverInput();
        input.view.requestedVisibility =
            static_cast<RenderVisibilityMode>(0xFFU);
        EXPECT_FALSE(ValidateRenderPolicyResolverInput(input));

        input = MakeValidResolverInput();
        input.passes.front().gpuDrivenPipelineReadiness =
            RenderPolicyReadiness::Count;
        EXPECT_FALSE(ValidateRenderPolicyResolverInput(input));

        const RenderPolicyResolution resolution =
            ResolveRenderPolicy(MakeValidResolverInput());
        ASSERT_TRUE(ValidateRenderPolicyResolution(resolution));
        const RenderPassPolicyDecision& decision =
            resolution.canonicalPassDecisions.front();

        RenderFrameExecutionPlan plan;
        plan.frameSequence = resolution.frameSequence;
        plan.viewOrdinal = resolution.viewOrdinal;
        plan.viewPolicy = resolution.viewPolicy;
        plan.capabilities = resolution.capabilities;
        plan.qualification = resolution.qualification;
        plan.packetReferences = {
            {RenderPassKind::Opaque, 11, 0},
            {RenderPassKind::Opaque, 12, 1},
            {RenderPassKind::Opaque, 13, 2},
            {RenderPassKind::Opaque, 14, 3},
            {RenderPassKind::Opaque, 15, 4},
        };
        RenderPassExecutionPlan passPlan;
        passPlan.pass = RenderPassKind::Opaque;
        passPlan.visibility = decision.visibility;
        passPlan.preferredSubmission = decision.preferredSubmission;
        passPlan.fallbackSubmission = decision.fallbackSubmission;
        passPlan.gpuEligiblePackets = {0, 3};
        passPlan.directPackets = {3, 1};
        passPlan.skippedPackets = {4, 1};
        passPlan.partition = decision.partition;
        passPlan.reason = decision.reason;
        plan.passes.push_back(passPlan);
        EXPECT_TRUE(ValidateRenderFrameExecutionPlan(plan));

        plan.passes.front().directPackets = {2, 1};
        EXPECT_FALSE(ValidateRenderFrameExecutionPlan(plan));
        plan.passes.front().directPackets = {3, 1};
        plan.packetReferences.pop_back();
        EXPECT_FALSE(ValidateRenderFrameExecutionPlan(plan));

        plan.packetReferences.push_back({RenderPassKind::Opaque, 15, 4});
        plan.passes.front().gpuEligiblePackets = {UINT32_MAX, 3};
        EXPECT_FALSE(ValidateRenderFrameExecutionPlan(plan));

        plan.passes.front().gpuEligiblePackets = {0, 3};
        plan.viewPolicy.selectedTier = GPUDrivenTier::Direct;
        EXPECT_FALSE(ValidateRenderFrameExecutionPlan(plan));
        plan.viewPolicy = resolution.viewPolicy;
        ASSERT_TRUE(ValidateRenderFrameExecutionPlan(plan));

        RenderPolicyResolution invalidResolution = resolution;
        invalidResolution.canonicalPassDecisions.front()
            .partition.gpuDrivenPacketCount = 4;
        invalidResolution.canonicalPassDecisions.front()
            .partition.directPacketCount = 0;
        EXPECT_FALSE(ValidateRenderPolicyResolution(invalidResolution));

        invalidResolution = resolution;
        invalidResolution.canonicalPassDecisions.front()
            .partition.directPacketCount = 2;
        invalidResolution.canonicalPassDecisions.front()
            .partition.skippedPacketCount = 0;
        EXPECT_FALSE(ValidateRenderPolicyResolution(invalidResolution));

        invalidResolution = resolution;
        invalidResolution.canonicalPassDecisions.front().fallbackSubmission =
            RenderSubmissionMode::MultiDrawIndirectCount;
        EXPECT_FALSE(ValidateRenderPolicyResolution(invalidResolution));

        invalidResolution = resolution;
        invalidResolution.canonicalPassDecisions.front().preferredSubmission =
            RenderSubmissionMode::EncodedCommandBuffer;
        EXPECT_FALSE(ValidateRenderPolicyResolution(invalidResolution));

        invalidResolution = resolution;
        invalidResolution.viewPolicy.requestedMode =
            RenderGPUDrivenMode::ForceDisabled;
        EXPECT_FALSE(ValidateRenderPolicyResolution(invalidResolution));

        invalidResolution = resolution;
        invalidResolution.viewPolicy.reason =
            RenderPolicyReason::ForcedGPUDriven;
        EXPECT_FALSE(ValidateRenderPolicyResolution(invalidResolution));

        invalidResolution = resolution;
        invalidResolution.canonicalPassDecisions.front().reason =
            RenderPolicyReason::ForcedGPUDriven;
        EXPECT_FALSE(ValidateRenderPolicyResolution(invalidResolution));

        RenderPolicyResolverInput directInput = MakeValidResolverInput();
        directInput.capabilities.supportsComputeVisibility = false;
        invalidResolution = ResolveRenderPolicy(directInput);
        ASSERT_EQ(GPUDrivenTier::Direct,
                  invalidResolution.viewPolicy.selectedTier);
        invalidResolution.viewPolicy.reason = RenderPolicyReason::ForcedDirect;
        EXPECT_FALSE(ValidateRenderPolicyResolution(invalidResolution));

        RenderFrameExecutionPlan invalidPlan = plan;
        invalidPlan.passes.front().partition.directPacketCount = 2;
        invalidPlan.passes.front().partition.skippedPacketCount = 0;
        invalidPlan.passes.front().directPackets.count = 2;
        invalidPlan.passes.front().skippedPackets.count = 0;
        EXPECT_FALSE(ValidateRenderFrameExecutionPlan(invalidPlan));

        invalidPlan = plan;
        invalidPlan.passes.front().fallbackSubmission =
            RenderSubmissionMode::MultiDrawIndirectCount;
        EXPECT_FALSE(ValidateRenderFrameExecutionPlan(invalidPlan));

        invalidPlan = plan;
        invalidPlan.viewPolicy.reason = RenderPolicyReason::ForcedGPUDriven;
        EXPECT_FALSE(ValidateRenderFrameExecutionPlan(invalidPlan));

        invalidPlan = plan;
        invalidPlan.passes.front().reason =
            RenderPolicyReason::ForcedGPUDriven;
        EXPECT_FALSE(ValidateRenderFrameExecutionPlan(invalidPlan));

        invalidPlan = plan;
        invalidPlan.viewPolicy.selectedTier = GPUDrivenTier::Direct;
        invalidPlan.viewPolicy.reason = RenderPolicyReason::ForcedDirect;
        invalidPlan.passes.front().visibility = RenderVisibilityMode::Cpu;
        invalidPlan.passes.front().preferredSubmission =
            RenderSubmissionMode::Direct;
        invalidPlan.passes.front().reason =
            RenderPolicyReason::CapabilityUnavailable;
        invalidPlan.passes.front().partition.gpuDrivenPacketCount = 0;
        invalidPlan.passes.front().partition.directPacketCount = 4;
        invalidPlan.passes.front().partition.drawGroupCount = 0;
        invalidPlan.passes.front().gpuEligiblePackets = {0, 0};
        invalidPlan.passes.front().directPackets = {0, 4};
        EXPECT_FALSE(ValidateRenderFrameExecutionPlan(invalidPlan));
    }
} // namespace
