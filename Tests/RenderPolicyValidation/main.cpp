#include "Render/Policy/RenderPolicyDiagnostics.h"
#include "Render/Policy/RenderFramePlanCompiler.h"
#include "Render/Policy/RenderPolicyResolver.h"
#include "Render/Passes/DirectDrawPacketBatch.h"
#include "Render/RenderDiagnostics.h"
#include "Render/Visibility/RenderVisibility.h"

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

    template <typename T>
    concept HasBindlessCapability = requires(const T& value)
    {
        value.supportsBindless;
    };

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
    static_assert(std::is_copy_constructible_v<RenderDrawPacketId>);
    static_assert(std::is_move_constructible_v<RenderDrawPacketId>);
    static_assert(
        std::is_copy_constructible_v<RenderPreparedDrawPacketSignature>);
    static_assert(
        std::is_move_constructible_v<RenderPreparedDrawPacketSignature>);
    static_assert(std::is_copy_constructible_v<RenderPacketIdentityAccounting>);
    static_assert(std::is_move_constructible_v<RenderPacketIdentityAccounting>);
    static_assert(std::is_copy_constructible_v<RenderFrameExecutionPlan>);
    static_assert(std::is_move_constructible_v<RenderFrameExecutionPlan>);
    static_assert(std::is_copy_constructible_v<RenderPassLaneExecutionReport>);
    static_assert(std::is_move_constructible_v<RenderPassLaneExecutionReport>);
    static_assert(std::is_copy_constructible_v<RenderPassExecutionReport>);
    static_assert(std::is_move_constructible_v<RenderPassExecutionReport>);
    static_assert(std::is_copy_constructible_v<RenderFrameExecutionReport>);
    static_assert(std::is_move_constructible_v<RenderFrameExecutionReport>);
    static_assert(std::is_copy_constructible_v<RenderPolicyMeasurement>);
    static_assert(std::is_move_constructible_v<RenderPolicyMeasurement>);
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
    static_assert(std::is_copy_constructible_v<RenderGPUResidentSceneFacts>);
    static_assert(std::is_copy_constructible_v<RenderPassPolicyFacts>);
    static_assert(std::is_copy_constructible_v<RenderPolicyResolverInput>);
    static_assert(std::is_copy_constructible_v<RenderPassPolicyDecision>);
    static_assert(std::is_copy_constructible_v<RenderPolicyResolution>);
    static_assert(!std::is_pointer_v<decltype(
        std::declval<RenderFrameExecutionPlan>().packetReferences)>);
    static_assert(std::is_same_v<
        decltype(std::declval<RenderFrameExecutionPlan>().packetReferences),
        std::vector<RenderDrawPacketReference>>);
    static_assert(!HasBindlessCapability<RenderCapabilitySnapshot>);

    void SetIndexedIndirectCapabilities(
        RenderCapabilitySnapshot& capabilities,
        bool fixedCount,
        bool countBuffer)
    {
        capabilities.indexedIndirectExecution.supportsFixedCount = fixedCount;
        capabilities.indexedIndirectExecution.supportsCountBuffer = countBuffer;
    }

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
        SetIndexedIndirectCapabilities(input.capabilities, true, true);
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

    void SetGPUResidentSceneReady(RenderPolicyResolverInput& input,
                                  uint64 requiredResidentVersion = 77)
    {
        input.capabilities.maxDescriptorSets = 3;
        input.capabilities.indexedIndirectExecution.supportsFirstInstance = true;
        input.gpuResidentScene.implementationReadiness =
            RenderPolicyReadiness::Ready;
        input.gpuResidentScene.shaderReadiness = RenderPolicyReadiness::Ready;
        input.gpuResidentScene.pipelineReadiness = RenderPolicyReadiness::Ready;
        input.gpuResidentScene.resourceReadiness = RenderPolicyReadiness::Ready;
        input.gpuResidentScene.bindingReadiness = RenderPolicyReadiness::Ready;
        input.gpuResidentScene.requiredResidentVersion = requiredResidentVersion;
    }

    MeshPassProcessorResult MakePreparedPacket(
        RenderPassKind pass,
        MeshPassDisposition disposition,
        MeshPassEligibilityReason reason,
        uint32 sourceOrdinal,
        RenderObjectId objectId,
        MaterialPipelineVariant variant = MaterialPipelineVariant::Opaque)
    {
        MeshPassProcessorResult result;
        result.disposition = disposition;
        result.reason = reason;
        result.sourceOrdinal = sourceOrdinal;
        result.packet.pass = pass;
        result.packet.objectId = objectId;
        result.packet.pipelineKey.materialVariant = variant;
        result.groupKey.pass = pass;
        result.groupKey.pipeline = result.packet.pipelineKey;
        return result;
    }

    RenderDrawPacketReference MakePacketReference(
        uint64 frameSequence,
        uint32 viewOrdinal,
        RenderPassKind pass,
        uint32 sourcePacketIndex,
        uint32 sourceOrdinal)
    {
        MeshPassProcessorResult source = MakePreparedPacket(
            pass,
            MeshPassDisposition::GPUCandidate,
            MeshPassEligibilityReason::None,
            sourceOrdinal,
            100U + sourcePacketIndex);
        source.packet.primitiveData = sourcePacketIndex;
        source.packet.geometryKey.mesh = {
            sourcePacketIndex + 1U, sourcePacketIndex + 10U};
        source.packet.submeshIndex = sourcePacketIndex;
        source.packet.geometryKey.submeshIndex = sourcePacketIndex;
        return RenderDrawPacketReference{
            pass,
            sourcePacketIndex,
            sourceOrdinal,
            BuildRenderDrawPacketId(frameSequence,
                                    viewOrdinal,
                                    pass,
                                    sourcePacketIndex,
                                    source),
            BuildRenderPreparedDrawPacketSignature(source)};
    }

    RenderPassPolicyFacts MakeFacts(RenderPassKind pass,
                                    const MeshPassPacketStream& stream)
    {
        RenderPassPolicyFacts facts;
        facts.pass = pass;
        facts.requested = true;
        facts.supported = true;
        facts.directAllowed = true;
        facts.gpuDrivenAllowed = pass == RenderPassKind::Depth ||
                                 pass == RenderPassKind::Opaque;
        facts.fixedCountIndirectAllowed = true;
        facts.directShaderReadiness = RenderPolicyReadiness::Ready;
        facts.directPipelineReadiness = RenderPolicyReadiness::Ready;
        facts.directResourceReadiness = RenderPolicyReadiness::Ready;
        facts.gpuDrivenShaderReadiness = RenderPolicyReadiness::Ready;
        facts.gpuDrivenPipelineReadiness = RenderPolicyReadiness::Ready;
        facts.gpuDrivenResourceReadiness = RenderPolicyReadiness::Ready;
        facts.inputPacketCount = stream.stats.inputPacketCount;
        facts.relevantPacketCount = stream.stats.relevantPacketCount;
        facts.candidatePacketCount = stream.stats.gpuCandidatePacketCount;
        facts.directPacketCount = stream.stats.directPacketCount;
        facts.skippedPacketCount = stream.stats.skippedPacketCount;
        facts.drawGroupCount = static_cast<uint32>(stream.groups.size());
        facts.workloadBeneficial = true;
        return facts;
    }

    RenderPolicyResolution ResolvePreparation(
        const SceneMeshPassPreparation& preparation,
        uint64 frameSequence,
        uint32 viewOrdinal,
        RenderPolicyReadiness directResourceReadiness =
            RenderPolicyReadiness::Ready,
        bool backendQualified = true,
        RenderGPUDrivenMode gpuDrivenMode = RenderGPUDrivenMode::Auto)
    {
        RenderPolicyResolverInput input;
        input.request.frameSequence = frameSequence;
        input.request.gpuDrivenMode = gpuDrivenMode;
        input.viewOrdinal = viewOrdinal;
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
        SetIndexedIndirectCapabilities(input.capabilities, true, true);
        input.qualification.backend = RHIBackendType::DX12;
        input.qualification.revision = 1;
        input.qualification.passedGateMask = backendQualified
            ? input.qualification.requiredGateMask
            : 0;
        input.passes = {
            MakeFacts(RenderPassKind::Transparent, preparation.transparent),
            MakeFacts(RenderPassKind::Opaque, preparation.opaque),
            MakeFacts(RenderPassKind::Shadow, preparation.shadow),
            MakeFacts(RenderPassKind::Depth, preparation.depth),
        };
        for (RenderPassPolicyFacts& facts : input.passes)
        {
            facts.directResourceReadiness = directResourceReadiness;
        }
        return ResolveRenderPolicy(input);
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
                EnumCase<RenderPolicyReason>{RenderPolicyReason::PacketIrrelevant, 26, "PacketIrrelevant"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::PacketRequiresDirect, 27, "PacketRequiresDirect"},
                EnumCase<RenderPolicyReason>{RenderPolicyReason::Count, 28, "Count"},
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

        ExpectEnumCases(
            std::array{
                EnumCase<GPUResidentSceneSelectionReason>{GPUResidentSceneSelectionReason::NotEvaluated, 0, "NotEvaluated"},
                EnumCase<GPUResidentSceneSelectionReason>{GPUResidentSceneSelectionReason::Ready, 1, "Ready"},
                EnumCase<GPUResidentSceneSelectionReason>{GPUResidentSceneSelectionReason::CapabilityUnavailable, 2, "CapabilityUnavailable"},
                EnumCase<GPUResidentSceneSelectionReason>{GPUResidentSceneSelectionReason::ImplementationPending, 3, "ImplementationPending"},
                EnumCase<GPUResidentSceneSelectionReason>{GPUResidentSceneSelectionReason::ImplementationUnavailable, 4, "ImplementationUnavailable"},
                EnumCase<GPUResidentSceneSelectionReason>{GPUResidentSceneSelectionReason::ShaderPending, 5, "ShaderPending"},
                EnumCase<GPUResidentSceneSelectionReason>{GPUResidentSceneSelectionReason::ShaderUnavailable, 6, "ShaderUnavailable"},
                EnumCase<GPUResidentSceneSelectionReason>{GPUResidentSceneSelectionReason::PipelinePending, 7, "PipelinePending"},
                EnumCase<GPUResidentSceneSelectionReason>{GPUResidentSceneSelectionReason::PipelineUnavailable, 8, "PipelineUnavailable"},
                EnumCase<GPUResidentSceneSelectionReason>{GPUResidentSceneSelectionReason::ResourcePending, 9, "ResourcePending"},
                EnumCase<GPUResidentSceneSelectionReason>{GPUResidentSceneSelectionReason::ResourceUnavailable, 10, "ResourceUnavailable"},
                EnumCase<GPUResidentSceneSelectionReason>{GPUResidentSceneSelectionReason::BindingPending, 11, "BindingPending"},
                EnumCase<GPUResidentSceneSelectionReason>{GPUResidentSceneSelectionReason::BindingUnavailable, 12, "BindingUnavailable"},
                EnumCase<GPUResidentSceneSelectionReason>{GPUResidentSceneSelectionReason::Count, 13, "Count"},
            },
            GetGPUResidentSceneSelectionReasonName);
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
        EXPECT_STREQ("Invalid", GetGPUResidentSceneSelectionReasonName(
            static_cast<GPUResidentSceneSelectionReason>(0xFFU)));
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
        EXPECT_EQ(GPUDrivenTier::Direct, viewPolicy.immediateFallbackTier);
        EXPECT_EQ(RenderPolicyReason::ConservativeDefault, viewPolicy.reason);
        EXPECT_EQ(GPUResidentSceneSelectionReason::NotEvaluated,
                  viewPolicy.gpuResidentSceneReason);
        EXPECT_EQ(0u, viewPolicy.requiredResidentVersion);

        const RenderCapabilitySnapshot capability;
        EXPECT_EQ(RHIBackendType::None, capability.backend);
        EXPECT_FALSE(capability.supportsComputeVisibility);
        EXPECT_FALSE(capability.supportsFixedCountIndirect);
        EXPECT_FALSE(capability.supportsIndirectDrawCount);
        EXPECT_FALSE(capability.supportsEncodedCommandBuffer);
        EXPECT_FALSE(capability.supportsDescriptorResourceBindings);
        EXPECT_EQ(0u, capability.maxDescriptorSets);
        EXPECT_FALSE(capability.SupportsGPUResidentSceneBase());

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
        EXPECT_EQ(0u, passReport.gpuDrivenLane.submissionCpuNanoseconds);
        EXPECT_FALSE(passReport.gpuDrivenLane.submissionCpuTimingAvailable);
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
        EXPECT_EQ(0u, diagnostics.measurement.frameSequence);
        EXPECT_EQ(0u, diagnostics.measurement.planCpuNanoseconds);
        EXPECT_EQ(0u, diagnostics.measurement.submissionCpuNanoseconds);
        EXPECT_EQ(0u, diagnostics.measurement.candidatePacketCount);
        EXPECT_EQ(0u, diagnostics.measurement.drawGroupCount);
        EXPECT_FALSE(diagnostics.measurement.planCpuTimingAvailable);
        EXPECT_FALSE(diagnostics.measurement.submissionCpuTimingAvailable);
        EXPECT_FALSE(diagnostics.measurement.averageGroupOccupancyAvailable);
        EXPECT_TRUE(diagnostics.measurement.nonGating);
        EXPECT_FALSE(diagnostics.measurement.usedForAutoDecision);

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

    TEST(RenderPolicyValidation, MeasurementsRemainValueOnlyAndNonGating)
    {
        RenderPolicyDiagnostics diagnostics;
        diagnostics.measurement.frameSequence = 23;
        diagnostics.measurement.planCpuNanoseconds = 17;
        diagnostics.measurement.submissionCpuNanoseconds = 29;
        diagnostics.measurement.candidatePacketCount = 8;
        diagnostics.measurement.drawGroupCount = 2;
        diagnostics.measurement.averageGroupOccupancy = 4.0;
        diagnostics.measurement.planCpuTimingAvailable = true;
        diagnostics.measurement.submissionCpuTimingAvailable = true;
        diagnostics.measurement.averageGroupOccupancyAvailable = true;

        RenderPolicyDiagnostics copied = diagnostics;
        copied.measurement.candidatePacketCount = 3;
        copied.measurement.usedForAutoDecision = false;

        EXPECT_EQ(8u, diagnostics.measurement.candidatePacketCount);
        EXPECT_EQ(3u, copied.measurement.candidatePacketCount);
        EXPECT_TRUE(copied.measurement.nonGating);
        EXPECT_FALSE(copied.measurement.usedForAutoDecision);

        const RenderPolicyResolution before =
            ResolveRenderPolicy(MakeValidResolverInput());
        const RenderPolicyResolution after =
            ResolveRenderPolicy(MakeValidResolverInput());
        EXPECT_EQ(before, after);
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

    TEST(RenderPolicyValidation,
         GPUResidentSceneSelectionStaysDirectForAutoCandidateWorkload)
    {
        RenderPolicyResolverInput input = MakeValidResolverInput();
        SetGPUResidentSceneReady(input);
        input.passes.front().workloadBeneficial = false;

        const RenderPolicyResolution resolution = ResolveRenderPolicy(input);
        ASSERT_TRUE(ValidateRenderPolicyResolution(resolution));
        EXPECT_EQ(GPUDrivenTier::Direct, resolution.viewPolicy.selectedTier);
        EXPECT_EQ(GPUDrivenTier::Direct,
                  resolution.viewPolicy.immediateFallbackTier);
        EXPECT_EQ(RenderPolicyReason::WorkloadNotBeneficial,
                  resolution.viewPolicy.reason);
        EXPECT_EQ(GPUResidentSceneSelectionReason::NotEvaluated,
                  resolution.viewPolicy.gpuResidentSceneReason);
        EXPECT_EQ(0u, resolution.viewPolicy.requiredResidentVersion);

        RenderPolicyResolution invalid = resolution;
        invalid.viewPolicy.selectedTier = GPUDrivenTier::GPUResidentScene;
        invalid.viewPolicy.immediateFallbackTier = GPUDrivenTier::IndirectGrouped;
        invalid.viewPolicy.gpuResidentSceneReason =
            GPUResidentSceneSelectionReason::Ready;
        invalid.viewPolicy.requiredResidentVersion = 77;
        EXPECT_FALSE(ValidateRenderPolicyResolution(invalid));
    }

    TEST(RenderPolicyValidation,
         CandidateAutoQualificationDoesNotEvaluateWarmGPUResidentScene)
    {
        RenderPolicyResolverInput input = MakeValidResolverInput();
        SetGPUResidentSceneReady(input, 78);
        input.qualification.passedGateMask =
            input.qualification.requiredGateMask & ~uint64{1};
        ASSERT_EQ(GPUDrivenQualificationLevel::Candidate,
                  input.qualification.GetLevel());

        const RenderPolicyResolution resolution = ResolveRenderPolicy(input);
        ASSERT_TRUE(ValidateRenderPolicyResolution(resolution));
        EXPECT_EQ(GPUDrivenTier::Direct, resolution.viewPolicy.selectedTier);
        EXPECT_EQ(RenderPolicyReason::BackendNotQualified,
                  resolution.viewPolicy.reason);
        EXPECT_EQ(GPUResidentSceneSelectionReason::NotEvaluated,
                  resolution.viewPolicy.gpuResidentSceneReason);
        EXPECT_EQ(0u, resolution.viewPolicy.requiredResidentVersion);
    }

    TEST(RenderPolicyValidation,
         GPUResidentSceneSelectsTierTwoOnlyAfterWarmTierOneWork)
    {
        for (const RenderGPUDrivenMode mode : {
                 RenderGPUDrivenMode::Auto,
                 RenderGPUDrivenMode::ForceEnabled})
        {
            RenderPolicyResolverInput input = MakeValidResolverInput();
            input.request.gpuDrivenMode = mode;
            SetGPUResidentSceneReady(input, 91);

            const RenderPolicyResolution resolution = ResolveRenderPolicy(input);
            ASSERT_TRUE(ValidateRenderPolicyResolution(resolution));
            EXPECT_EQ(GPUDrivenTier::GPUResidentScene,
                      resolution.viewPolicy.selectedTier);
            EXPECT_EQ(GPUDrivenTier::IndirectGrouped,
                      resolution.viewPolicy.immediateFallbackTier);
            EXPECT_EQ(GPUResidentSceneSelectionReason::Ready,
                      resolution.viewPolicy.gpuResidentSceneReason);
            EXPECT_EQ(91u, resolution.viewPolicy.requiredResidentVersion);
            EXPECT_EQ(RenderSubmissionMode::MultiDrawIndirectCount,
                      resolution.canonicalPassDecisions.front().preferredSubmission);
        }
    }

    TEST(RenderPolicyValidation,
         GPUResidentSceneReadinessFailuresPreserveTierOneWithPreciseReason)
    {
        struct ReadinessCase
        {
            RenderPolicyReadiness RenderGPUResidentSceneFacts::* member;
            RenderPolicyReadiness readiness;
            GPUResidentSceneSelectionReason reason;
        };
        constexpr std::array cases = {
            ReadinessCase{&RenderGPUResidentSceneFacts::implementationReadiness,
                          RenderPolicyReadiness::Pending,
                          GPUResidentSceneSelectionReason::ImplementationPending},
            ReadinessCase{&RenderGPUResidentSceneFacts::implementationReadiness,
                          RenderPolicyReadiness::Unavailable,
                          GPUResidentSceneSelectionReason::ImplementationUnavailable},
            ReadinessCase{&RenderGPUResidentSceneFacts::shaderReadiness,
                          RenderPolicyReadiness::Pending,
                          GPUResidentSceneSelectionReason::ShaderPending},
            ReadinessCase{&RenderGPUResidentSceneFacts::shaderReadiness,
                          RenderPolicyReadiness::Unavailable,
                          GPUResidentSceneSelectionReason::ShaderUnavailable},
            ReadinessCase{&RenderGPUResidentSceneFacts::pipelineReadiness,
                          RenderPolicyReadiness::Pending,
                          GPUResidentSceneSelectionReason::PipelinePending},
            ReadinessCase{&RenderGPUResidentSceneFacts::pipelineReadiness,
                          RenderPolicyReadiness::Unavailable,
                          GPUResidentSceneSelectionReason::PipelineUnavailable},
            ReadinessCase{&RenderGPUResidentSceneFacts::resourceReadiness,
                          RenderPolicyReadiness::Pending,
                          GPUResidentSceneSelectionReason::ResourcePending},
            ReadinessCase{&RenderGPUResidentSceneFacts::resourceReadiness,
                          RenderPolicyReadiness::Unavailable,
                          GPUResidentSceneSelectionReason::ResourceUnavailable},
            ReadinessCase{&RenderGPUResidentSceneFacts::bindingReadiness,
                          RenderPolicyReadiness::Pending,
                          GPUResidentSceneSelectionReason::BindingPending},
            ReadinessCase{&RenderGPUResidentSceneFacts::bindingReadiness,
                          RenderPolicyReadiness::Unavailable,
                          GPUResidentSceneSelectionReason::BindingUnavailable},
        };

        for (const ReadinessCase& testCase : cases)
        {
            RenderPolicyResolverInput input = MakeValidResolverInput();
            SetGPUResidentSceneReady(input, 92);
            input.gpuResidentScene.*(testCase.member) = testCase.readiness;
            const RenderPolicyResolution resolution = ResolveRenderPolicy(input);
            ASSERT_TRUE(ValidateRenderPolicyResolution(resolution));
            EXPECT_EQ(GPUDrivenTier::IndirectGrouped,
                      resolution.viewPolicy.selectedTier);
            EXPECT_EQ(GPUDrivenTier::Direct,
                      resolution.viewPolicy.immediateFallbackTier);
            EXPECT_EQ(testCase.reason, resolution.viewPolicy.gpuResidentSceneReason);
            EXPECT_EQ(92u, resolution.viewPolicy.requiredResidentVersion);
        }

        RenderPolicyResolverInput zeroVersion = MakeValidResolverInput();
        SetGPUResidentSceneReady(zeroVersion, 0);
        const RenderPolicyResolution resolution = ResolveRenderPolicy(zeroVersion);
        ASSERT_TRUE(ValidateRenderPolicyResolution(resolution));
        EXPECT_EQ(GPUDrivenTier::IndirectGrouped,
                  resolution.viewPolicy.selectedTier);
        EXPECT_EQ(GPUResidentSceneSelectionReason::ResourceUnavailable,
                  resolution.viewPolicy.gpuResidentSceneReason);
        EXPECT_EQ(0u, resolution.viewPolicy.requiredResidentVersion);
    }

    TEST(RenderPolicyValidation,
         GPUResidentSceneBaseRequirementsDoNotConstrainTierOneOrUseBindless)
    {
        struct CapabilityCase
        {
            void (*disable)(RenderCapabilitySnapshot&);
        };
        constexpr std::array cases = {
            CapabilityCase{[](RenderCapabilitySnapshot& capabilities)
                           { capabilities.maxDescriptorSets = 2; }},
            CapabilityCase{[](RenderCapabilitySnapshot& capabilities)
                           { capabilities.indexedIndirectExecution.supportsCountBuffer = false; }},
            CapabilityCase{[](RenderCapabilitySnapshot& capabilities)
                           { capabilities.indexedIndirectExecution.supportsFirstInstance = false; }},
        };

        for (const CapabilityCase& testCase : cases)
        {
            RenderPolicyResolverInput input = MakeValidResolverInput();
            SetGPUResidentSceneReady(input, 93);
            testCase.disable(input.capabilities);
            EXPECT_FALSE(input.capabilities.SupportsGPUResidentSceneBase());

            const RenderPolicyResolution resolution = ResolveRenderPolicy(input);
            ASSERT_TRUE(ValidateRenderPolicyResolution(resolution));
            EXPECT_EQ(GPUDrivenTier::IndirectGrouped,
                      resolution.viewPolicy.selectedTier);
            EXPECT_EQ(GPUDrivenTier::Direct,
                      resolution.viewPolicy.immediateFallbackTier);
            EXPECT_EQ(GPUResidentSceneSelectionReason::CapabilityUnavailable,
                      resolution.viewPolicy.gpuResidentSceneReason);
        }
    }

    TEST(RenderPolicyValidation,
         GPUResidentSceneValidationRejectsInvalidFactsReasonsPairsVersionsAndTiers)
    {
        RenderPolicyResolverInput invalidInput = MakeValidResolverInput();
        invalidInput.gpuResidentScene.bindingReadiness =
            RenderPolicyReadiness::Count;
        EXPECT_FALSE(ValidateRenderPolicyResolverInput(invalidInput));

        RenderPolicyResolverInput input = MakeValidResolverInput();
        SetGPUResidentSceneReady(input, 94);
        const RenderPolicyResolution resolution = ResolveRenderPolicy(input);
        ASSERT_TRUE(ValidateRenderPolicyResolution(resolution));

        RenderPolicyResolution invalid = resolution;
        invalid.viewPolicy.gpuResidentSceneReason =
            GPUResidentSceneSelectionReason::Count;
        EXPECT_FALSE(ValidateRenderPolicyResolution(invalid));

        invalid = resolution;
        invalid.viewPolicy.immediateFallbackTier = GPUDrivenTier::Direct;
        EXPECT_FALSE(ValidateRenderPolicyResolution(invalid));

        invalid = resolution;
        invalid.viewPolicy.requiredResidentVersion = 0;
        EXPECT_FALSE(ValidateRenderPolicyResolution(invalid));

        invalid = resolution;
        invalid.viewPolicy.selectedTier = GPUDrivenTier::Meshlet;
        EXPECT_FALSE(ValidateRenderPolicyResolution(invalid));

        RenderPolicyResolverInput tierOneInput = MakeValidResolverInput();
        const RenderPolicyResolution tierOne = ResolveRenderPolicy(tierOneInput);
        ASSERT_TRUE(ValidateRenderPolicyResolution(tierOne));
        invalid = tierOne;
        invalid.viewPolicy.gpuResidentSceneReason =
            GPUResidentSceneSelectionReason::ImplementationUnavailable;
        EXPECT_FALSE(ValidateRenderPolicyResolution(invalid));

        invalid = tierOne;
        invalid.viewPolicy.gpuResidentSceneReason =
            GPUResidentSceneSelectionReason::Ready;
        EXPECT_FALSE(ValidateRenderPolicyResolution(invalid));

        RenderPolicyResolverInput baseReadyInput = MakeValidResolverInput();
        SetGPUResidentSceneReady(baseReadyInput, 95);
        baseReadyInput.gpuResidentScene.implementationReadiness =
            RenderPolicyReadiness::Unavailable;
        const RenderPolicyResolution baseReadyTierOne =
            ResolveRenderPolicy(baseReadyInput);
        ASSERT_TRUE(ValidateRenderPolicyResolution(baseReadyTierOne));
        ASSERT_EQ(GPUDrivenTier::IndirectGrouped,
                  baseReadyTierOne.viewPolicy.selectedTier);
        ASSERT_EQ(GPUResidentSceneSelectionReason::ImplementationUnavailable,
                  baseReadyTierOne.viewPolicy.gpuResidentSceneReason);
        invalid = baseReadyTierOne;
        invalid.viewPolicy.gpuResidentSceneReason =
            GPUResidentSceneSelectionReason::CapabilityUnavailable;
        EXPECT_FALSE(ValidateRenderPolicyResolution(invalid));
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
        input.capabilities.indexedIndirectExecution.supportsCountBuffer = false;
        resolution = ResolveRenderPolicy(input);
        EXPECT_EQ(RenderSubmissionMode::FixedCountIndirect,
                  resolution.canonicalPassDecisions.front().preferredSubmission);

        input = MakeValidResolverInput();
        input.capabilities.supportsIndirectDrawCount = false;
        resolution = ResolveRenderPolicy(input);
        EXPECT_EQ(RenderSubmissionMode::MultiDrawIndirectCount,
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
                    SetIndexedIndirectCapabilities(
                        input.capabilities,
                        false,
                        (gates & kIndirectBit) != 0);
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
                             !input.capabilities.indexedIndirectExecution.supportsCountBuffer)
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

    TEST(RenderPolicyValidation,
         FramePlanCompilerPreservesCanonicalLaneAndSourceOrdering)
    {
        SceneMeshPassPreparation preparation;
        preparation.depth.Record(MakePreparedPacket(
            RenderPassKind::Depth, MeshPassDisposition::GPUCandidate,
            MeshPassEligibilityReason::None, 7, 20,
            MaterialPipelineVariant::Masked));
        preparation.depth.Record(MakePreparedPacket(
            RenderPassKind::Depth, MeshPassDisposition::GPUCandidate,
            MeshPassEligibilityReason::None, 7, 10));
        preparation.depth.Record(MakePreparedPacket(
            RenderPassKind::Depth, MeshPassDisposition::Skip,
            MeshPassEligibilityReason::PassIrrelevant, 9, 30));
        preparation.opaque.Record(MakePreparedPacket(
            RenderPassKind::Opaque, MeshPassDisposition::GPUCandidate,
            MeshPassEligibilityReason::None, 0, 40));
        preparation.shadow.Record(MakePreparedPacket(
            RenderPassKind::Shadow, MeshPassDisposition::Direct,
            MeshPassEligibilityReason::PassRequiresDirect, 3, 50));
        preparation.transparent.Record(MakePreparedPacket(
            RenderPassKind::Transparent, MeshPassDisposition::Direct,
            MeshPassEligibilityReason::Transparent, 5, 60));
        preparation.transparent.Record(MakePreparedPacket(
            RenderPassKind::Transparent, MeshPassDisposition::Direct,
            MeshPassEligibilityReason::Transparent, 4, 70));
        preparation.depth.FinalizeGroups();
        preparation.opaque.FinalizeGroups();
        preparation.shadow.FinalizeGroups();
        preparation.transparent.FinalizeGroups();

        const RenderPolicyResolution resolution =
            ResolvePreparation(preparation, 101, 0);
        ASSERT_TRUE(ValidateRenderPolicyResolution(resolution));
        const RenderFramePlanCompileResult compiled =
            CompileRenderFrameExecutionPlan(resolution, preparation);
        ASSERT_TRUE(compiled.succeeded);
        ASSERT_TRUE(ValidateRenderFrameExecutionPlan(compiled.plan));
        ASSERT_EQ(4u, compiled.plan.passes.size());
        EXPECT_EQ(RenderPassKind::Depth, compiled.plan.passes[0].pass);
        EXPECT_EQ(RenderPassKind::Opaque, compiled.plan.passes[1].pass);
        EXPECT_EQ(RenderPassKind::Shadow, compiled.plan.passes[2].pass);
        EXPECT_EQ(RenderPassKind::Transparent, compiled.plan.passes[3].pass);

        const RenderPassExecutionPlan& depth = compiled.plan.passes[0];
        ASSERT_EQ(2u, depth.gpuEligiblePackets.count);
        EXPECT_EQ((RenderPacketIdentityAccounting{3, 3, 3, 0, 0}),
                  depth.identityAccounting);
        EXPECT_EQ(1u, compiled.plan.packetReferences[0].sourcePacketIndex);
        EXPECT_EQ(0u, compiled.plan.packetReferences[1].sourcePacketIndex);
        EXPECT_EQ(2u, compiled.plan.packetReferences[2].sourcePacketIndex);
        EXPECT_EQ(2u, depth.reasonCounts[static_cast<size_t>(
                          RenderPolicyReason::None)]);
        EXPECT_EQ(1u, depth.reasonCounts[static_cast<size_t>(
                          RenderPolicyReason::PacketIrrelevant)]);

        const RenderPassExecutionPlan& transparent = compiled.plan.passes[3];
        ASSERT_EQ(2u, transparent.directPackets.count);
        const uint32 first = transparent.directPackets.first;
        EXPECT_EQ(0u,
                  compiled.plan.packetReferences[first].sourcePacketIndex);
        EXPECT_EQ(1u,
                  compiled.plan.packetReferences[first + 1].sourcePacketIndex);
    }

    TEST(RenderPolicyValidation,
         FramePlanCompilerPreservesMixedPassHybridLanes)
    {
        SceneMeshPassPreparation preparation;
        preparation.depth.FinalizeGroups();
        preparation.shadow.FinalizeGroups();
        preparation.transparent.FinalizeGroups();

        MeshPassProcessorResult direct = MakePreparedPacket(
            RenderPassKind::Opaque, MeshPassDisposition::Direct,
            MeshPassEligibilityReason::Skinned, 11, 2);
        direct.directLayout.vertexStreams =
            MeshPassVertexStreams::Position;
        direct.directLayout.primitiveDataBinding =
            PrimitiveDataBinding::PerDrawConstants;
        direct.groupKey.layout = direct.directLayout;

        preparation.opaque.Record(MakePreparedPacket(
            RenderPassKind::Opaque, MeshPassDisposition::GPUCandidate,
            MeshPassEligibilityReason::None, 11, 1));
        preparation.opaque.Record(MakePreparedPacket(
            RenderPassKind::Opaque, MeshPassDisposition::GPUCandidate,
            MeshPassEligibilityReason::None, 12, 3));
        preparation.opaque.Record(direct);
        preparation.opaque.FinalizeGroups();
        EXPECT_EQ(2u, preparation.opaque.sortedGPUCandidatePacketIndices.size());

        const RenderPolicyResolution resolution =
            ResolvePreparation(preparation, 202, 1);
        EXPECT_EQ(2u, resolution.canonicalPassDecisions[1]
                          .partition.gpuDrivenPacketCount);
        EXPECT_EQ(1u, resolution.canonicalPassDecisions[1]
                          .partition.directPacketCount);

        const RenderFramePlanCompileResult first =
            CompileRenderFrameExecutionPlan(resolution, preparation);
        const RenderFramePlanCompileResult second =
            CompileRenderFrameExecutionPlan(resolution, preparation);
        ASSERT_TRUE(first.succeeded);
        EXPECT_EQ(first, second);
        EXPECT_EQ(202u, first.plan.frameSequence);
        EXPECT_EQ(1u, first.plan.viewOrdinal);
        EXPECT_EQ(GPUDrivenTier::IndirectGrouped,
                  first.plan.viewPolicy.selectedTier);
        EXPECT_EQ(RenderPolicyReason::None, first.plan.viewPolicy.reason);

        const RenderPassExecutionPlan& opaque = first.plan.passes[1];
        EXPECT_EQ(RenderSubmissionMode::MultiDrawIndirectCount,
                  opaque.preferredSubmission);
        EXPECT_EQ(RenderVisibilityMode::GpuFrustum, opaque.visibility);
        EXPECT_EQ(RenderPolicyReason::None, opaque.reason);
        EXPECT_EQ(2u, opaque.gpuEligiblePackets.count);
        EXPECT_EQ(1u, opaque.directPackets.count);
        EXPECT_EQ(0u, opaque.skippedPackets.count);
        EXPECT_EQ((RenderPacketIdentityAccounting{3, 3, 3, 0, 0}),
                  opaque.identityAccounting);
        const uint32 expectedGpuSource0 =
            preparation.opaque.sortedGPUCandidatePacketIndices[0];
        const uint32 expectedGpuSource1 =
            preparation.opaque.sortedGPUCandidatePacketIndices[1];
        EXPECT_EQ(expectedGpuSource0,
                  first.plan.packetReferences[opaque.gpuEligiblePackets.first]
                      .sourcePacketIndex);
        EXPECT_EQ(expectedGpuSource1,
                  first.plan.packetReferences[opaque.gpuEligiblePackets.first + 1]
                      .sourcePacketIndex);
        EXPECT_EQ(2u,
                  first.plan.packetReferences[opaque.directPackets.first]
                      .sourcePacketIndex);

        EXPECT_TRUE(ValidatePlannedGPUDrivenPacketRange(
            first.plan, RenderPassKind::Opaque, preparation.opaque));

        DirectDrawPacketBatchBuildResult batch =
            BuildDirectDrawPacketBatch(first.plan,
                                      RenderPassKind::Opaque,
                                      preparation.opaque);
        ASSERT_TRUE(batch.succeeded);
        ASSERT_EQ(1u, batch.batch.packets.size());

        // The plan keeps ownership of its Direct packet even when the
        // frame-owned, pass-aware CPU result filters it before recording.
        RenderVisibilityResult hiddenDirect;
        RenderVisibilityPassResult& opaqueVisibility =
            hiddenDirect.passes[static_cast<size_t>(RenderPassKind::Opaque)];
        opaqueVisibility.pass = RenderPassKind::Opaque;
        opaqueVisibility.sourcePacketKnown.resize(3, 0);
        opaqueVisibility.cpuVisibleBySourcePacket.resize(3, 0);
        opaqueVisibility.sourcePacketKnown[2] = 1;
        DirectDrawPacketBatchBuildResult filtered =
            BuildDirectDrawPacketBatch(first.plan,
                                       RenderPassKind::Opaque,
                                       preparation.opaque,
                                       &hiddenDirect);
        EXPECT_TRUE(filtered.succeeded);
        EXPECT_TRUE(filtered.batch.packets.empty());

        RenderFrameExecutionPlan invalidReference = first.plan;
        invalidReference.passes[1].directPackets.count = 0;
        EXPECT_FALSE(BuildDirectDrawPacketBatch(invalidReference,
                                                RenderPassKind::Opaque,
                                                preparation.opaque)
                         .succeeded);

        invalidReference = first.plan;
        invalidReference.passes[1].directPackets.first += 1u;
        EXPECT_FALSE(BuildDirectDrawPacketBatch(invalidReference,
                                                RenderPassKind::Opaque,
                                                preparation.opaque)
                         .succeeded);

        invalidReference = first.plan;
        auto& directReference =
            invalidReference.packetReferences[opaque.directPackets.first];
        ++directReference.sourceOrdinal;
        EXPECT_FALSE(BuildDirectDrawPacketBatch(invalidReference,
                                                RenderPassKind::Opaque,
                                                preparation.opaque)
                         .succeeded);

        invalidReference = first.plan;
        invalidReference.passes[1].directPackets.count = 2u;
        EXPECT_FALSE(ValidatePlannedGPUDrivenPacketRange(
            invalidReference, RenderPassKind::Opaque, preparation.opaque));
    }

    TEST(RenderPolicyValidation,
         FramePlanCompilerFailsClosedOnMalformedSourceMapping)
    {
        SceneMeshPassPreparation preparation;
        preparation.depth.Record(MakePreparedPacket(
            RenderPassKind::Depth, MeshPassDisposition::GPUCandidate,
            MeshPassEligibilityReason::None, 4, 1));
        preparation.depth.FinalizeGroups();
        preparation.opaque.FinalizeGroups();
        preparation.shadow.FinalizeGroups();
        preparation.transparent.FinalizeGroups();
        const RenderPolicyResolution resolution =
            ResolvePreparation(preparation, 303, 0);
        ASSERT_TRUE(CompileRenderFrameExecutionPlan(resolution, preparation)
                        .succeeded);

        preparation.depth.sortedGPUCandidatePacketIndices.front() = 9;
        const RenderFramePlanCompileResult malformed =
            CompileRenderFrameExecutionPlan(resolution, preparation);
        EXPECT_FALSE(malformed.succeeded);
        EXPECT_TRUE(malformed.plan.passes.empty());
        EXPECT_TRUE(malformed.plan.packetReferences.empty());
    }

    TEST(RenderPolicyValidation,
         FramePlanCompilerKeepsGPUWhenDirectResourcesArePending)
    {
        SceneMeshPassPreparation preparation;
        preparation.depth.FinalizeGroups();
        preparation.shadow.FinalizeGroups();
        preparation.transparent.FinalizeGroups();

        preparation.opaque.Record(MakePreparedPacket(
            RenderPassKind::Opaque,
            MeshPassDisposition::GPUCandidate,
            MeshPassEligibilityReason::None,
            4,
            10));
        preparation.opaque.Record(MakePreparedPacket(
            RenderPassKind::Opaque,
            MeshPassDisposition::Direct,
            MeshPassEligibilityReason::Skinned,
            5,
            11));
        preparation.opaque.Record(MakePreparedPacket(
            RenderPassKind::Opaque,
            MeshPassDisposition::Skip,
            MeshPassEligibilityReason::PassIrrelevant,
            6,
            12));
        preparation.opaque.FinalizeGroups();

        const RenderPolicyResolution resolution = ResolvePreparation(
            preparation,
            212,
            2,
            RenderPolicyReadiness::Pending);
        const RenderPassPolicyDecision& decision =
            resolution.canonicalPassDecisions[1];
        ASSERT_TRUE(ValidateRenderPolicyResolution(resolution));
        EXPECT_EQ(1u, decision.partition.gpuDrivenPacketCount);
        EXPECT_EQ(0u, decision.partition.directPacketCount);
        EXPECT_EQ(2u, decision.partition.skippedPacketCount);
        EXPECT_EQ(RenderPolicyReason::ResourcesPending, decision.reason);

        const RenderFramePlanCompileResult compiled =
            CompileRenderFrameExecutionPlan(resolution, preparation);
        ASSERT_TRUE(compiled.succeeded);
        const RenderPassExecutionPlan& opaque = compiled.plan.passes[1];
        EXPECT_EQ(1u, opaque.gpuEligiblePackets.count);
        EXPECT_EQ(0u, opaque.directPackets.count);
        EXPECT_EQ(2u, opaque.skippedPackets.count);
        EXPECT_EQ(0u,
                  compiled.plan.packetReferences[
                      opaque.gpuEligiblePackets.first]
                      .sourcePacketIndex);
        EXPECT_EQ(1u,
                  compiled.plan.packetReferences[opaque.skippedPackets.first]
                      .sourcePacketIndex);
        EXPECT_EQ(2u,
                  compiled.plan.packetReferences[
                      opaque.skippedPackets.first + 1]
                      .sourcePacketIndex);
        EXPECT_EQ(1u,
                  opaque.reasonCounts[static_cast<size_t>(
                      RenderPolicyReason::None)]);
        EXPECT_EQ(1u,
                  opaque.reasonCounts[static_cast<size_t>(
                      RenderPolicyReason::ResourcesPending)]);
        EXPECT_EQ(1u,
                  opaque.reasonCounts[static_cast<size_t>(
                      RenderPolicyReason::PacketIrrelevant)]);
        EXPECT_TRUE(ValidatePlannedGPUDrivenPacketRange(
            compiled.plan,
            RenderPassKind::Opaque,
            preparation.opaque));

        const DirectDrawPacketBatchBuildResult directBatch =
            BuildDirectDrawPacketBatch(compiled.plan,
                                       RenderPassKind::Opaque,
                                       preparation.opaque);
        ASSERT_TRUE(directBatch.succeeded);
        EXPECT_TRUE(directBatch.batch.packets.empty());
    }

    TEST(RenderPolicyValidation,
         FramePlanCompilerFailsClosedOnIncompleteCanonicalPassSet)
    {
        SceneMeshPassPreparation preparation;
        preparation.depth.FinalizeGroups();
        preparation.opaque.FinalizeGroups();
        preparation.shadow.FinalizeGroups();
        preparation.transparent.FinalizeGroups();

        RenderPolicyResolution resolution =
            ResolvePreparation(preparation, 304, 0);
        const RenderFramePlanCompileResult emptyPlan =
            CompileRenderFrameExecutionPlan(resolution, preparation);
        ASSERT_TRUE(emptyPlan.succeeded);
        ASSERT_EQ(4u, emptyPlan.plan.passes.size());
        for (const RenderPassExecutionPlan& passPlan : emptyPlan.plan.passes)
        {
            EXPECT_EQ((RenderPacketIdentityAccounting{}),
                      passPlan.identityAccounting);
            EXPECT_TRUE(passPlan.identityAccounting.IsExactlyOnce());
        }
        ASSERT_EQ(4u, resolution.canonicalPassDecisions.size());
        resolution.canonicalPassDecisions.pop_back();
        ASSERT_TRUE(ValidateRenderPolicyResolution(resolution));

        const RenderFramePlanCompileResult incomplete =
            CompileRenderFrameExecutionPlan(resolution, preparation);
        EXPECT_FALSE(incomplete.succeeded);
        EXPECT_TRUE(incomplete.plan.passes.empty());
        EXPECT_TRUE(incomplete.plan.packetReferences.empty());

        resolution = ResolvePreparation(preparation, 305, 0);
        ASSERT_EQ(4u, resolution.canonicalPassDecisions.size());
        std::swap(resolution.canonicalPassDecisions[1],
                  resolution.canonicalPassDecisions[2]);
        EXPECT_FALSE(ValidateRenderPolicyResolution(resolution));

        const RenderFramePlanCompileResult reordered =
            CompileRenderFrameExecutionPlan(resolution, preparation);
        EXPECT_FALSE(reordered.succeeded);
        EXPECT_TRUE(reordered.plan.passes.empty());
        EXPECT_TRUE(reordered.plan.packetReferences.empty());
    }

    TEST(RenderPolicyValidation,
         FramePlanCompilerPreservesGlobalReasonWhenDirectReadinessAlsoFails)
    {
        SceneMeshPassPreparation preparation;
        preparation.depth.FinalizeGroups();
        preparation.shadow.FinalizeGroups();
        preparation.transparent.FinalizeGroups();
        preparation.opaque.Record(MakePreparedPacket(
            RenderPassKind::Opaque,
            MeshPassDisposition::GPUCandidate,
            MeshPassEligibilityReason::None,
            7,
            20));
        preparation.opaque.FinalizeGroups();

        const RenderPolicyResolution resolution = ResolvePreparation(
            preparation,
            213,
            0,
            RenderPolicyReadiness::Pending,
            false);
        ASSERT_TRUE(ValidateRenderPolicyResolution(resolution));
        EXPECT_EQ(GPUDrivenTier::Direct,
                  resolution.viewPolicy.selectedTier);
        EXPECT_EQ(RenderPolicyReason::BackendNotQualified,
                  resolution.viewPolicy.reason);
        EXPECT_EQ(RenderPolicyReason::ResourcesPending,
                  resolution.canonicalPassDecisions[1].reason);

        const RenderFramePlanCompileResult compiled =
            CompileRenderFrameExecutionPlan(resolution, preparation);
        ASSERT_TRUE(compiled.succeeded);
        EXPECT_EQ(resolution.viewPolicy, compiled.plan.viewPolicy);
        EXPECT_EQ(RenderPolicyReason::ResourcesPending,
                  compiled.plan.passes[1].reason);
    }

    TEST(RenderPolicyValidation,
         FramePlanCompilerPreservesForcedGPUReasonForGPUOnlyPackets)
    {
        SceneMeshPassPreparation preparation;
        preparation.depth.FinalizeGroups();
        preparation.shadow.FinalizeGroups();
        preparation.transparent.FinalizeGroups();
        preparation.opaque.Record(MakePreparedPacket(
            RenderPassKind::Opaque,
            MeshPassDisposition::GPUCandidate,
            MeshPassEligibilityReason::None,
            8,
            21));
        preparation.opaque.FinalizeGroups();

        const RenderPolicyResolution resolution = ResolvePreparation(
            preparation,
            214,
            0,
            RenderPolicyReadiness::Ready,
            true,
            RenderGPUDrivenMode::ForceEnabled);
        ASSERT_TRUE(ValidateRenderPolicyResolution(resolution));
        EXPECT_EQ(RenderPolicyReason::ForcedGPUDriven,
                  resolution.canonicalPassDecisions[1].reason);

        const RenderFramePlanCompileResult compiled =
            CompileRenderFrameExecutionPlan(resolution, preparation);
        ASSERT_TRUE(compiled.succeeded);
        const RenderPassExecutionPlan& opaque = compiled.plan.passes[1];
        EXPECT_EQ(1u,
                  opaque.reasonCounts[static_cast<size_t>(
                      RenderPolicyReason::ForcedGPUDriven)]);
        EXPECT_EQ(0u,
                  opaque.reasonCounts[static_cast<size_t>(
                      RenderPolicyReason::None)]);
    }

    TEST(RenderPolicyValidation,
         FramePlanCompilerKeepsTwoViewsIndependentAndValueOwned)
    {
        SceneMeshPassPreparation preparation;
        preparation.opaque.Record(MakePreparedPacket(
            RenderPassKind::Opaque, MeshPassDisposition::GPUCandidate,
            MeshPassEligibilityReason::None, 8, 88));
        preparation.depth.FinalizeGroups();
        preparation.opaque.FinalizeGroups();
        preparation.shadow.FinalizeGroups();
        preparation.transparent.FinalizeGroups();

        const RenderFramePlanCompileResult view0 =
            CompileRenderFrameExecutionPlan(
                ResolvePreparation(preparation, 404, 0), preparation);
        const RenderFramePlanCompileResult view1 =
            CompileRenderFrameExecutionPlan(
                ResolvePreparation(preparation, 404, 1), preparation);
        ASSERT_TRUE(view0.succeeded);
        ASSERT_TRUE(view1.succeeded);
        EXPECT_EQ(0u, view0.plan.viewOrdinal);
        EXPECT_EQ(1u, view1.plan.viewOrdinal);
        ASSERT_EQ(view0.plan.packetReferences.size(),
                  view1.plan.packetReferences.size());
        EXPECT_NE(view0.plan.packetReferences,
                  view1.plan.packetReferences);
        EXPECT_EQ(0u,
                  view0.plan.packetReferences.front().packetId.viewOrdinal);
        EXPECT_EQ(1u,
                  view1.plan.packetReferences.front().packetId.viewOrdinal);
        EXPECT_EQ(view0.plan.packetReferences.front().sourcePacketIndex,
                  view1.plan.packetReferences.front().sourcePacketIndex);
        EXPECT_EQ(view0.plan.packetReferences.front().sourceOrdinal,
                  view1.plan.packetReferences.front().sourceOrdinal);
        RenderFrameExecutionPlan ownedCopy = view0.plan;
        ownedCopy.packetReferences.front().sourceOrdinal = 99;
        EXPECT_EQ(8u, view0.plan.packetReferences.front().sourceOrdinal);
        EXPECT_EQ(8u, view1.plan.packetReferences.front().sourceOrdinal);
    }

    TEST(RenderPolicyValidation,
         FramePlanCompilerBuildsStablePacketIdsAndExactlyOnceAccounting)
    {
        SceneMeshPassPreparation preparation;
        MeshPassProcessorResult first = MakePreparedPacket(
            RenderPassKind::Opaque,
            MeshPassDisposition::GPUCandidate,
            MeshPassEligibilityReason::None,
            7,
            91);
        first.packet.primitiveData = 5;
        first.packet.geometryKey.mesh = {3, 4};
        first.packet.submeshIndex = 2;
        first.packet.geometryKey.submeshIndex = 2;
        MeshPassProcessorResult second = first;
        second.sourceOrdinal = 7;
        preparation.opaque.Record(first);
        preparation.opaque.Record(second);
        preparation.depth.FinalizeGroups();
        preparation.opaque.FinalizeGroups();
        preparation.shadow.FinalizeGroups();
        preparation.transparent.FinalizeGroups();

        const RenderPolicyResolution resolution =
            ResolvePreparation(preparation, 601, 3);
        const RenderFramePlanCompileResult compiled =
            CompileRenderFrameExecutionPlan(resolution, preparation);
        const RenderFramePlanCompileResult repeated =
            CompileRenderFrameExecutionPlan(resolution, preparation);
        ASSERT_TRUE(compiled.succeeded);
        ASSERT_TRUE(repeated.succeeded);
        EXPECT_EQ(compiled.plan, repeated.plan);

        const RenderPassExecutionPlan& opaque = compiled.plan.passes[1];
        EXPECT_EQ((RenderPacketIdentityAccounting{2, 2, 2, 0, 0}),
                  opaque.identityAccounting);
        ASSERT_EQ(2u, opaque.gpuEligiblePackets.count);
        const RenderDrawPacketReference& firstReference =
            compiled.plan.packetReferences[opaque.gpuEligiblePackets.first];
        const RenderDrawPacketReference& secondReference =
            compiled.plan.packetReferences[opaque.gpuEligiblePackets.first + 1];
        EXPECT_EQ(firstReference.sourceOrdinal, secondReference.sourceOrdinal);
        EXPECT_NE(firstReference.packetId, secondReference.packetId);
        EXPECT_EQ(3u, firstReference.packetId.mesh.slot);
        EXPECT_EQ(4u, firstReference.packetId.mesh.generation);
        EXPECT_EQ(91u, firstReference.packetId.objectId);
        EXPECT_EQ(5u, firstReference.packetId.primitiveData);
        EXPECT_EQ(2u, firstReference.packetId.logicalSubmeshIndex);
        EXPECT_EQ(2u, firstReference.packetId.geometrySubmeshIndex);

        const RenderFramePlanCompileResult otherView =
            CompileRenderFrameExecutionPlan(
                ResolvePreparation(preparation, 601, 4), preparation);
        const RenderFramePlanCompileResult otherFrame =
            CompileRenderFrameExecutionPlan(
                ResolvePreparation(preparation, 602, 3), preparation);
        ASSERT_TRUE(otherView.succeeded);
        ASSERT_TRUE(otherFrame.succeeded);
        EXPECT_NE(compiled.plan.packetReferences.front().packetId,
                  otherView.plan.packetReferences.front().packetId);
        EXPECT_NE(compiled.plan.packetReferences.front().packetId,
                  otherFrame.plan.packetReferences.front().packetId);

        SceneMeshPassPreparation changedGeneration = preparation;
        changedGeneration.opaque.packets[0].packet.geometryKey.mesh.generation++;
        changedGeneration.opaque.FinalizeGroups();
        const RenderFramePlanCompileResult generationPlan =
            CompileRenderFrameExecutionPlan(
                ResolvePreparation(changedGeneration, 601, 3),
                changedGeneration);
        ASSERT_TRUE(generationPlan.succeeded);
        EXPECT_NE(compiled.plan.packetReferences.front().packetId,
                  generationPlan.plan.packetReferences.front().packetId);

        SceneMeshPassPreparation changedSubmesh = preparation;
        ++changedSubmesh.opaque.packets[0].packet.submeshIndex;
        ++changedSubmesh.opaque.packets[0].packet.geometryKey.submeshIndex;
        changedSubmesh.opaque.FinalizeGroups();
        const RenderFramePlanCompileResult submeshPlan =
            CompileRenderFrameExecutionPlan(
                ResolvePreparation(changedSubmesh, 601, 3),
                changedSubmesh);
        ASSERT_TRUE(submeshPlan.succeeded);
        EXPECT_NE(compiled.plan.packetReferences.front().packetId,
                  submeshPlan.plan.packetReferences.front().packetId);

        RenderFrameExecutionPlan invalid = compiled.plan;
        invalid.packetReferences[opaque.gpuEligiblePackets.first + 1].packetId =
            invalid.packetReferences[opaque.gpuEligiblePackets.first].packetId;
        EXPECT_FALSE(ValidateRenderFrameExecutionPlan(invalid));

        invalid = compiled.plan;
        invalid.passes[1].identityAccounting.uniquePacketIdCount = 1;
        invalid.passes[1].identityAccounting.duplicatePacketIdCount = 1;
        EXPECT_FALSE(ValidateRenderFrameExecutionPlan(invalid));

        MeshPassPacketStream staleGPUStream = preparation.opaque;
        ++staleGPUStream.packets[0].packet.materialKey.material.generation;
        staleGPUStream.FinalizeGroups();
        EXPECT_FALSE(ValidatePlannedGPUDrivenPacketRange(
            compiled.plan, RenderPassKind::Opaque, staleGPUStream));
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
            MakePacketReference(plan.frameSequence, plan.viewOrdinal,
                                RenderPassKind::Opaque, 0, 0),
            MakePacketReference(plan.frameSequence, plan.viewOrdinal,
                                RenderPassKind::Opaque, 1, 1),
            MakePacketReference(plan.frameSequence, plan.viewOrdinal,
                                RenderPassKind::Opaque, 2, 2),
            MakePacketReference(plan.frameSequence, plan.viewOrdinal,
                                RenderPassKind::Opaque, 3, 3),
            MakePacketReference(plan.frameSequence, plan.viewOrdinal,
                                RenderPassKind::Opaque, 4, 4),
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
        passPlan.identityAccounting = {5, 5, 5, 0, 0};
        passPlan.reason = decision.reason;
        passPlan.reasonCounts[static_cast<size_t>(decision.reason)] = 3;
        passPlan.reasonCounts[static_cast<size_t>(
            RenderPolicyReason::PacketRequiresDirect)] = 1;
        passPlan.reasonCounts[static_cast<size_t>(
            RenderPolicyReason::PacketIrrelevant)] = 1;
        plan.passes.push_back(passPlan);
        EXPECT_TRUE(ValidateRenderFrameExecutionPlan(plan));

        plan.passes.front().directPackets = {2, 1};
        EXPECT_FALSE(ValidateRenderFrameExecutionPlan(plan));
        plan.passes.front().directPackets = {3, 1};
        plan.packetReferences.pop_back();
        EXPECT_FALSE(ValidateRenderFrameExecutionPlan(plan));

        plan.packetReferences.push_back(MakePacketReference(
            plan.frameSequence,
            plan.viewOrdinal,
            RenderPassKind::Opaque,
            4,
            4));
        plan.passes.front().gpuEligiblePackets = {UINT32_MAX, 3};
        EXPECT_FALSE(ValidateRenderFrameExecutionPlan(plan));

        plan.passes.front().gpuEligiblePackets = {0, 3};
        plan.viewPolicy.selectedTier = GPUDrivenTier::Direct;
        EXPECT_FALSE(ValidateRenderFrameExecutionPlan(plan));
        plan.viewPolicy = resolution.viewPolicy;
        ASSERT_TRUE(ValidateRenderFrameExecutionPlan(plan));

        RenderFrameExecutionPlan tierTwoInvalidPlan = plan;
        tierTwoInvalidPlan.viewPolicy.immediateFallbackTier =
            GPUDrivenTier::IndirectGrouped;
        EXPECT_FALSE(ValidateRenderFrameExecutionPlan(tierTwoInvalidPlan));

        tierTwoInvalidPlan = plan;
        tierTwoInvalidPlan.viewPolicy.gpuResidentSceneReason =
            GPUResidentSceneSelectionReason::Ready;
        EXPECT_FALSE(ValidateRenderFrameExecutionPlan(tierTwoInvalidPlan));

        tierTwoInvalidPlan = plan;
        tierTwoInvalidPlan.viewPolicy.gpuResidentSceneReason =
            GPUResidentSceneSelectionReason::ImplementationUnavailable;
        EXPECT_FALSE(ValidateRenderFrameExecutionPlan(tierTwoInvalidPlan));

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

    TEST(RenderPolicyValidation,
         DirectDrawPacketBatchUsesSourceIndexAndFailsClosed)
    {
        SceneMeshPassPreparation preparation;
        MeshPassProcessorResult direct = MakePreparedPacket(
            RenderPassKind::Depth,
            MeshPassDisposition::Direct,
            MeshPassEligibilityReason::Skinned,
            17,
            42);
        direct.packet.primitiveData = 0;
        direct.packet.geometryKey.mesh = RenderResourceHandle{3, 9};
        direct.packet.geometryKey.submeshIndex = 2;
        direct.packet.geometryKey.indexType = MeshUploadIndexType::UInt32;
        direct.packet.materialKey.material = RenderResourceHandle{4, 7};
        direct.packet.arguments = RenderDrawArguments{12, 1, 8, -2, 0};
        direct.directLayout.vertexStreams = MeshPassVertexStreams::Position |
                                            MeshPassVertexStreams::BoneIndices |
                                            MeshPassVertexStreams::BoneWeights;
        direct.directLayout.bindings = MeshPassBindingRequirements::Frame |
                                       MeshPassBindingRequirements::Object |
                                       MeshPassBindingRequirements::Geometry |
                                       MeshPassBindingRequirements::Skinning;
        direct.directLayout.primitiveDataBinding =
            PrimitiveDataBinding::PerDrawConstants;
        direct.groupKey.layout = direct.directLayout;
        preparation.depth.Record(direct);
        preparation.depth.FinalizeGroups();
        preparation.opaque.FinalizeGroups();
        preparation.shadow.FinalizeGroups();
        preparation.transparent.FinalizeGroups();

        const RenderFramePlanCompileResult compiled =
            CompileRenderFrameExecutionPlan(
                ResolvePreparation(preparation, 505, 0), preparation);
        ASSERT_TRUE(compiled.succeeded);
        const DirectDrawPacketBatchBuildResult built =
            BuildDirectDrawPacketBatch(compiled.plan,
                                       RenderPassKind::Depth,
                                       preparation.depth);
        ASSERT_TRUE(built.succeeded);
        ASSERT_EQ(1u, built.batch.packets.size());
        EXPECT_EQ(0u, built.batch.packets.front().sourcePacketIndex);
        EXPECT_EQ(17u, built.batch.packets.front().sourceOrdinal);
        EXPECT_EQ(direct.packet, built.batch.packets.front().packet);
        EXPECT_EQ(direct.directLayout, built.batch.packets.front().layout);

        RenderFrameExecutionPlan invalid = compiled.plan;
        auto& depthPlan = invalid.passes.front();
        ASSERT_EQ(1u, depthPlan.directPackets.count);
        invalid.packetReferences[depthPlan.directPackets.first].sourcePacketIndex =
            99;
        EXPECT_FALSE(BuildDirectDrawPacketBatch(invalid,
                                                RenderPassKind::Depth,
                                                preparation.depth)
                         .succeeded);

        invalid = compiled.plan;
        invalid.packetReferences[invalid.passes.front().directPackets.first].pass =
            RenderPassKind::Opaque;
        EXPECT_FALSE(BuildDirectDrawPacketBatch(invalid,
                                                RenderPassKind::Depth,
                                                preparation.depth)
                         .succeeded);

        invalid = compiled.plan;
        invalid.passes.front().directPackets.count = 0;
        invalid.passes.front().partition.directPacketCount = 0;
        EXPECT_FALSE(BuildDirectDrawPacketBatch(invalid,
                                                RenderPassKind::Depth,
                                                preparation.depth)
                         .succeeded);

        MeshPassPacketStream staleGeneration = preparation.depth;
        ++staleGeneration.packets.front().packet.geometryKey.mesh.generation;
        EXPECT_FALSE(BuildDirectDrawPacketBatch(compiled.plan,
                                                RenderPassKind::Depth,
                                                staleGeneration)
                         .succeeded);

        const auto expectSourceDriftRejected =
            [&](const MeshPassPacketStream& changed)
        {
            EXPECT_FALSE(BuildDirectDrawPacketBatch(compiled.plan,
                                                    RenderPassKind::Depth,
                                                    changed)
                             .succeeded);
        };
        MeshPassPacketStream changed = preparation.depth;
        ++changed.packets.front().packet.materialKey.material.generation;
        expectSourceDriftRejected(changed);
        changed = preparation.depth;
        changed.packets.front().packet.pipelineKey.materialVariant =
            MaterialPipelineVariant::Masked;
        expectSourceDriftRejected(changed);
        changed = preparation.depth;
        ++changed.packets.front().packet.arguments.indexCount;
        expectSourceDriftRejected(changed);
        changed = preparation.depth;
        changed.packets.front().packet.flags = RenderDrawFlags::Masked;
        expectSourceDriftRejected(changed);
        changed = preparation.depth;
        changed.packets.front().directLayout.vertexStreams =
            changed.packets.front().directLayout.vertexStreams |
            MeshPassVertexStreams::Normal;
        expectSourceDriftRejected(changed);

        preparation.depth.packets.front().directLayout.vertexStreams =
            MeshPassVertexStreams::InstanceIndex;
        EXPECT_FALSE(BuildDirectDrawPacketBatch(compiled.plan,
                                                RenderPassKind::Depth,
                                                preparation.depth)
                         .succeeded);

        preparation.depth.packets.front().directLayout = direct.directLayout;
        ++preparation.depth.stats.relevantPacketCount;
        EXPECT_FALSE(BuildDirectDrawPacketBatch(compiled.plan,
                                                RenderPassKind::Depth,
                                                preparation.depth)
                         .succeeded);
    }
} // namespace
