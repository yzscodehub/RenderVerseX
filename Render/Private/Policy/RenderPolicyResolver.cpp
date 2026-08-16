#include "Render/Policy/RenderPolicyResolver.h"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace RVX
{
    namespace
    {
        constexpr std::array<RenderPassKind, 4> kCanonicalPassOrder = {
            RenderPassKind::Depth,
            RenderPassKind::Opaque,
            RenderPassKind::Shadow,
            RenderPassKind::Transparent,
        };

        bool IsValid(RenderGPUDrivenMode mode)
        {
            return mode == RenderGPUDrivenMode::Auto ||
                   mode == RenderGPUDrivenMode::ForceEnabled ||
                   mode == RenderGPUDrivenMode::ForceDisabled;
        }

        bool IsValid(RenderPassKind pass)
        {
            return pass == RenderPassKind::Depth ||
                   pass == RenderPassKind::Opaque ||
                   pass == RenderPassKind::Shadow ||
                   pass == RenderPassKind::Transparent;
        }

        bool IsValid(RenderVisibilityMode mode)
        {
            return mode == RenderVisibilityMode::Cpu ||
                   mode == RenderVisibilityMode::GpuFrustum ||
                   mode == RenderVisibilityMode::GpuFrustumAndDistance ||
                   mode == RenderVisibilityMode::GpuOcclusion;
        }

        bool IsValid(RenderSubmissionMode mode)
        {
            return mode == RenderSubmissionMode::Direct ||
                   mode == RenderSubmissionMode::FixedCountIndirect ||
                   mode == RenderSubmissionMode::MultiDrawIndirectCount ||
                   mode == RenderSubmissionMode::EncodedCommandBuffer;
        }

        bool IsValid(RenderPolicyReason reason)
        {
            return static_cast<uint8>(reason) <
                   static_cast<uint8>(RenderPolicyReason::Count);
        }

        bool IsValid(GPUResidentSceneSelectionReason reason)
        {
            return static_cast<uint8>(reason) <
                   static_cast<uint8>(GPUResidentSceneSelectionReason::Count);
        }

        bool IsResolverSelectedTier(GPUDrivenTier tier)
        {
            return tier == GPUDrivenTier::Direct ||
                   tier == GPUDrivenTier::IndirectGrouped ||
                   tier == GPUDrivenTier::GPUResidentScene;
        }

        bool IsValid(RenderPolicyReadiness readiness)
        {
            return readiness == RenderPolicyReadiness::Unavailable ||
                   readiness == RenderPolicyReadiness::Pending ||
                   readiness == RenderPolicyReadiness::Ready;
        }

        bool IsConcreteBackend(RHIBackendType backend)
        {
            switch (backend)
            {
                case RHIBackendType::DX11:
                case RHIBackendType::DX12:
                case RHIBackendType::Vulkan:
                case RHIBackendType::Metal:
                case RHIBackendType::OpenGL:
                    return true;
                default:
                    return false;
            }
        }

        bool IsValidBackend(RHIBackendType backend)
        {
            return backend == RHIBackendType::None ||
                   backend == RHIBackendType::Auto ||
                   IsConcreteBackend(backend);
        }

        bool IsQualificationStructurallyConsistent(
            const RenderPolicyResolverInput& input)
        {
            return input.qualification.IsValidManifest() &&
                   IsConcreteBackend(input.qualification.backend) &&
                   input.qualification.backend == input.capabilities.backend;
        }

        bool HasConsistentCounts(const RenderPassPolicyFacts& facts)
        {
            if (facts.relevantPacketCount > facts.inputPacketCount ||
                facts.candidatePacketCount > facts.relevantPacketCount ||
                (facts.candidatePacketCount == 0) !=
                    (facts.drawGroupCount == 0) ||
                facts.drawGroupCount > facts.candidatePacketCount)
            {
                return false;
            }

            const uint64 relevantCount =
                static_cast<uint64>(facts.candidatePacketCount) +
                static_cast<uint64>(facts.directPacketCount);
            return relevantCount == facts.relevantPacketCount &&
                   static_cast<uint64>(facts.relevantPacketCount) +
                       static_cast<uint64>(facts.skippedPacketCount) ==
                       facts.inputPacketCount;
        }

        bool ArePassReadinessValuesValid(const RenderPassPolicyFacts& facts)
        {
            return IsValid(facts.directShaderReadiness) &&
                   IsValid(facts.directPipelineReadiness) &&
                   IsValid(facts.directResourceReadiness) &&
                   IsValid(facts.gpuDrivenShaderReadiness) &&
                   IsValid(facts.gpuDrivenPipelineReadiness) &&
                   IsValid(facts.gpuDrivenResourceReadiness);
        }

        bool AreGPUResidentSceneFactsValid(
            const RenderGPUResidentSceneFacts& facts)
        {
            return IsValid(facts.implementationReadiness) &&
                   IsValid(facts.shaderReadiness) &&
                   IsValid(facts.pipelineReadiness) &&
                   IsValid(facts.resourceReadiness) &&
                   IsValid(facts.bindingReadiness);
        }

        GPUResidentSceneSelectionReason GetGPUResidentSceneReadinessFailure(
            const RenderGPUResidentSceneFacts& facts)
        {
            if (facts.implementationReadiness == RenderPolicyReadiness::Pending)
            {
                return GPUResidentSceneSelectionReason::ImplementationPending;
            }
            if (facts.implementationReadiness != RenderPolicyReadiness::Ready)
            {
                return GPUResidentSceneSelectionReason::ImplementationUnavailable;
            }
            if (facts.shaderReadiness == RenderPolicyReadiness::Pending)
            {
                return GPUResidentSceneSelectionReason::ShaderPending;
            }
            if (facts.shaderReadiness != RenderPolicyReadiness::Ready)
            {
                return GPUResidentSceneSelectionReason::ShaderUnavailable;
            }
            if (facts.pipelineReadiness == RenderPolicyReadiness::Pending)
            {
                return GPUResidentSceneSelectionReason::PipelinePending;
            }
            if (facts.pipelineReadiness != RenderPolicyReadiness::Ready)
            {
                return GPUResidentSceneSelectionReason::PipelineUnavailable;
            }
            if (facts.resourceReadiness == RenderPolicyReadiness::Pending)
            {
                return GPUResidentSceneSelectionReason::ResourcePending;
            }
            // A zero version is a missing resident resource, not a separate
            // capability. Keep that fail-closed result in the resource reason
            // family so the stable enum remains readiness-only.
            if (facts.resourceReadiness != RenderPolicyReadiness::Ready ||
                facts.requiredResidentVersion == 0)
            {
                return GPUResidentSceneSelectionReason::ResourceUnavailable;
            }
            if (facts.bindingReadiness == RenderPolicyReadiness::Pending)
            {
                return GPUResidentSceneSelectionReason::BindingPending;
            }
            if (facts.bindingReadiness != RenderPolicyReadiness::Ready)
            {
                return GPUResidentSceneSelectionReason::BindingUnavailable;
            }
            return GPUResidentSceneSelectionReason::Ready;
        }

        RenderPolicyReason GetReadinessFailure(
            RenderPolicyReadiness shader,
            RenderPolicyReadiness pipeline,
            RenderPolicyReadiness resources)
        {
            if (shader == RenderPolicyReadiness::Pending)
            {
                return RenderPolicyReason::ShaderPending;
            }
            if (shader != RenderPolicyReadiness::Ready)
            {
                return RenderPolicyReason::ShaderUnavailable;
            }
            if (pipeline == RenderPolicyReadiness::Pending)
            {
                return RenderPolicyReason::PipelinePending;
            }
            if (pipeline != RenderPolicyReadiness::Ready)
            {
                return RenderPolicyReason::PipelineUnavailable;
            }
            if (resources == RenderPolicyReadiness::Pending)
            {
                return RenderPolicyReason::ResourcesPending;
            }
            if (resources != RenderPolicyReadiness::Ready)
            {
                return RenderPolicyReason::ResourcesUnavailable;
            }
            return RenderPolicyReason::None;
        }

        RenderPolicyReason GetViewReadinessFailure(
            const RenderPolicyViewFacts& view)
        {
            if (view.visibilityShaderReadiness ==
                RenderPolicyReadiness::Pending)
            {
                return RenderPolicyReason::ShaderPending;
            }
            if (view.visibilityShaderReadiness != RenderPolicyReadiness::Ready)
            {
                return RenderPolicyReason::ShaderUnavailable;
            }
            if (view.visibilityPipelineReadiness ==
                RenderPolicyReadiness::Pending)
            {
                return RenderPolicyReason::PipelinePending;
            }
            if (view.visibilityPipelineReadiness != RenderPolicyReadiness::Ready)
            {
                return RenderPolicyReason::PipelineUnavailable;
            }
            if (view.sharedResourceReadiness == RenderPolicyReadiness::Pending)
            {
                return RenderPolicyReason::ResourcesPending;
            }
            if (view.sharedResourceReadiness != RenderPolicyReadiness::Ready)
            {
                return RenderPolicyReason::ResourcesUnavailable;
            }
            if (view.requiredBindingReadiness != RenderPolicyReadiness::Ready)
            {
                return view.requiredBindingReadiness ==
                               RenderPolicyReadiness::Pending
                           ? RenderPolicyReason::BindingsPending
                           : RenderPolicyReason::BindingsUnavailable;
            }
            return RenderPolicyReason::None;
        }

        bool IsDirectReady(const RenderPassPolicyFacts& facts,
                           RenderPolicyReason* failureReason)
        {
            if (!facts.directAllowed)
            {
                if (failureReason)
                {
                    *failureReason = RenderPolicyReason::PassUnsupported;
                }
                return false;
            }

            const RenderPolicyReason readiness = GetReadinessFailure(
                facts.directShaderReadiness,
                facts.directPipelineReadiness,
                facts.directResourceReadiness);
            if (failureReason)
            {
                *failureReason = readiness;
            }
            return readiness == RenderPolicyReason::None;
        }

        RenderSubmissionMode SelectGPUDrivenSubmission(
            const RenderCapabilitySnapshot& capabilities,
            const RenderPassPolicyFacts& facts)
        {
            if (capabilities.supportsEncodedCommandBuffer)
            {
                return RenderSubmissionMode::EncodedCommandBuffer;
            }
            if (capabilities.indexedIndirectExecution.supportsCountBuffer)
            {
                return RenderSubmissionMode::MultiDrawIndirectCount;
            }
            if (capabilities.indexedIndirectExecution.supportsFixedCount &&
                facts.fixedCountIndirectAllowed)
            {
                return RenderSubmissionMode::FixedCountIndirect;
            }
            return RenderSubmissionMode::Direct;
        }

        RenderPacketPartitionSummary MakeSourceSummary(
            const RenderPassPolicyFacts& facts)
        {
            RenderPacketPartitionSummary summary;
            summary.inputPacketCount = facts.inputPacketCount;
            summary.relevantPacketCount = facts.relevantPacketCount;
            summary.candidatePacketCount = facts.candidatePacketCount;
            summary.skippedPacketCount = facts.skippedPacketCount;
            return summary;
        }

        RenderPassPolicyDecision MakeDirectOrSkipDecision(
            const RenderPassPolicyFacts& facts,
            RenderPolicyReason reason,
            bool passDisabled = false)
        {
            RenderPassPolicyDecision decision;
            decision.pass = facts.pass;
            decision.reason = reason;
            decision.partition = MakeSourceSummary(facts);

            if (passDisabled)
            {
                decision.partition.skippedPacketCount = facts.inputPacketCount;
                return decision;
            }

            RenderPolicyReason directFailure = RenderPolicyReason::None;
            if (IsDirectReady(facts, &directFailure))
            {
                decision.partition.directPacketCount =
                    facts.candidatePacketCount + facts.directPacketCount;
            }
            else
            {
                decision.partition.skippedPacketCount +=
                    facts.candidatePacketCount + facts.directPacketCount;
                if (facts.relevantPacketCount != 0)
                {
                    decision.reason = directFailure;
                }
            }
            return decision;
        }

        RenderPolicyReason GetGlobalGPUFailure(
            const RenderPolicyResolverInput& input)
        {
            if (!input.view.rendererAllowsGPUDriven)
            {
                return RenderPolicyReason::RendererPathUnsupported;
            }
            if (!input.view.viewAllowsGPUDriven ||
                input.view.requestedVisibility == RenderVisibilityMode::Cpu)
            {
                return RenderPolicyReason::ViewUnsupported;
            }
            if (!input.view.implementationAvailable)
            {
                return RenderPolicyReason::BackendUnsupported;
            }

            const bool hasIndirectStrategy =
                input.capabilities.supportsEncodedCommandBuffer ||
                input.capabilities.indexedIndirectExecution.supportsCountBuffer ||
                input.capabilities.indexedIndirectExecution.supportsFixedCount;
            if (!input.capabilities.supportsComputeVisibility ||
                !input.capabilities.supportsDescriptorResourceBindings ||
                !hasIndirectStrategy)
            {
                return RenderPolicyReason::CapabilityUnavailable;
            }

            if (!IsQualificationStructurallyConsistent(input))
            {
                return RenderPolicyReason::QualificationInvalid;
            }
            if (input.request.gpuDrivenMode == RenderGPUDrivenMode::Auto &&
                !input.qualification.IsQualified())
            {
                return RenderPolicyReason::BackendNotQualified;
            }

            return GetViewReadinessFailure(input.view);
        }

        bool IsValidPartition(const RenderPacketPartitionSummary& partition)
        {
            if (partition.relevantPacketCount > partition.inputPacketCount ||
                partition.candidatePacketCount > partition.relevantPacketCount ||
                partition.gpuDrivenPacketCount >
                    partition.candidatePacketCount ||
                partition.drawGroupCount > partition.gpuDrivenPacketCount)
            {
                return false;
            }

            const uint64 resolvedPacketCount =
                static_cast<uint64>(partition.gpuDrivenPacketCount) +
                static_cast<uint64>(partition.directPacketCount) +
                static_cast<uint64>(partition.skippedPacketCount);
            const uint64 selectedRelevantPacketCount =
                static_cast<uint64>(partition.gpuDrivenPacketCount) +
                static_cast<uint64>(partition.directPacketCount);
            return resolvedPacketCount == partition.inputPacketCount &&
                   selectedRelevantPacketCount <=
                       partition.relevantPacketCount;
        }

        bool IsRangeInBounds(DrawPacketRange range, size_t size)
        {
            const uint64 end = static_cast<uint64>(range.first) + range.count;
            return end <= size;
        }

        bool IsSubmissionSupported(
            RenderSubmissionMode submission,
            const RenderCapabilitySnapshot& capabilities)
        {
            switch (submission)
            {
                case RenderSubmissionMode::Direct:
                    return true;
                case RenderSubmissionMode::FixedCountIndirect:
                    return capabilities.indexedIndirectExecution.supportsFixedCount;
                case RenderSubmissionMode::MultiDrawIndirectCount:
                    return capabilities.indexedIndirectExecution.supportsCountBuffer;
                case RenderSubmissionMode::EncodedCommandBuffer:
                    return capabilities.supportsEncodedCommandBuffer;
                default:
                    return false;
            }
        }

        bool HasValidImmediateFallbackPair(
            GPUDrivenTier selectedTier,
            GPUDrivenTier immediateFallbackTier)
        {
            switch (selectedTier)
            {
                case GPUDrivenTier::Direct:
                    return immediateFallbackTier == GPUDrivenTier::Direct;
                case GPUDrivenTier::IndirectGrouped:
                    return immediateFallbackTier == GPUDrivenTier::Direct;
                case GPUDrivenTier::GPUResidentScene:
                    return immediateFallbackTier ==
                           GPUDrivenTier::IndirectGrouped;
                default:
                    // Meshlet is intentionally not selected by Task11D-D3.
                    return false;
            }
        }

        bool IsSelectedViewPolicyConsistent(
            const RenderViewPolicy& viewPolicy,
            bool hasGPUDrivenPackets,
            bool qualificationMatches,
            const RenderQualificationSnapshot& qualification,
            const RenderCapabilitySnapshot& capabilities)
        {
            if (!IsResolverSelectedTier(viewPolicy.selectedTier) ||
                !HasValidImmediateFallbackPair(
                    viewPolicy.selectedTier,
                    viewPolicy.immediateFallbackTier) ||
                !IsValid(viewPolicy.gpuResidentSceneReason))
            {
                return false;
            }

            if (!hasGPUDrivenPackets)
            {
                if (viewPolicy.selectedTier != GPUDrivenTier::Direct)
                {
                    return false;
                }
                if (viewPolicy.gpuResidentSceneReason !=
                        GPUResidentSceneSelectionReason::NotEvaluated ||
                    viewPolicy.requiredResidentVersion != 0)
                {
                    return false;
                }
                if (viewPolicy.requestedMode ==
                    RenderGPUDrivenMode::ForceDisabled)
                {
                    return viewPolicy.reason == RenderPolicyReason::ForcedDirect;
                }
                return viewPolicy.reason != RenderPolicyReason::None &&
                       viewPolicy.reason != RenderPolicyReason::ForcedDirect &&
                       viewPolicy.reason !=
                           RenderPolicyReason::ForcedGPUDriven;
            }

            if (viewPolicy.selectedTier == GPUDrivenTier::Direct ||
                viewPolicy.requestedMode == RenderGPUDrivenMode::ForceDisabled ||
                !qualificationMatches ||
                !capabilities.supportsComputeVisibility ||
                !capabilities.supportsDescriptorResourceBindings)
            {
                return false;
            }

            const bool requestReasonMatches =
                viewPolicy.requestedMode == RenderGPUDrivenMode::Auto
                    ? qualification.level == GPUDrivenQualificationLevel::Qualified &&
                          viewPolicy.reason == RenderPolicyReason::None
                    : viewPolicy.requestedMode ==
                              RenderGPUDrivenMode::ForceEnabled &&
                          viewPolicy.reason == RenderPolicyReason::ForcedGPUDriven;
            if (!requestReasonMatches)
            {
                return false;
            }

            if (viewPolicy.selectedTier == GPUDrivenTier::GPUResidentScene)
            {
                return capabilities.SupportsGPUResidentSceneBase() &&
                       viewPolicy.gpuResidentSceneReason ==
                           GPUResidentSceneSelectionReason::Ready &&
                       viewPolicy.requiredResidentVersion != 0;
            }

            if (viewPolicy.selectedTier != GPUDrivenTier::IndirectGrouped)
            {
                return false;
            }

            const bool residentBaseAvailable =
                capabilities.SupportsGPUResidentSceneBase();
            if (!residentBaseAvailable)
            {
                return viewPolicy.gpuResidentSceneReason ==
                       GPUResidentSceneSelectionReason::CapabilityUnavailable;
            }

            return viewPolicy.gpuResidentSceneReason !=
                       GPUResidentSceneSelectionReason::NotEvaluated &&
                   viewPolicy.gpuResidentSceneReason !=
                       GPUResidentSceneSelectionReason::Ready &&
                   viewPolicy.gpuResidentSceneReason !=
                       GPUResidentSceneSelectionReason::CapabilityUnavailable;
        }

        bool IsPassDecisionReasonConsistent(
            RenderGPUDrivenMode requestedMode,
            bool hasGPUDrivenPackets,
            RenderPolicyReason reason)
        {
            if (reason == RenderPolicyReason::None)
            {
                return hasGPUDrivenPackets &&
                       requestedMode == RenderGPUDrivenMode::Auto;
            }
            if (reason == RenderPolicyReason::ForcedDirect)
            {
                return !hasGPUDrivenPackets &&
                       requestedMode == RenderGPUDrivenMode::ForceDisabled;
            }
            if (reason == RenderPolicyReason::ForcedGPUDriven)
            {
                return hasGPUDrivenPackets &&
                       requestedMode == RenderGPUDrivenMode::ForceEnabled;
            }
            return true;
        }
    } // namespace

    bool ValidateRenderPolicyResolverInput(const RenderPolicyResolverInput& input)
    {
        if (!IsValid(input.request.gpuDrivenMode) ||
            !IsValidBackend(input.capabilities.backend) ||
            !IsValidBackend(input.qualification.backend) ||
            !IsValid(input.view.requestedVisibility) ||
            !IsValid(input.view.visibilityShaderReadiness) ||
            !IsValid(input.view.visibilityPipelineReadiness) ||
            !IsValid(input.view.sharedResourceReadiness) ||
            !IsValid(input.view.requiredBindingReadiness) ||
            !AreGPUResidentSceneFactsValid(input.gpuResidentScene))
        {
            return false;
        }

        std::array<bool, kCanonicalPassOrder.size()> seenPasses{};
        for (const RenderPassPolicyFacts& facts : input.passes)
        {
            if (!IsValid(facts.pass) || !ArePassReadinessValuesValid(facts) ||
                !HasConsistentCounts(facts))
            {
                return false;
            }

            const size_t passIndex = static_cast<size_t>(facts.pass) - 1;
            if (seenPasses[passIndex])
            {
                return false;
            }
            seenPasses[passIndex] = true;
        }
        return true;
    }

    RenderPolicyResolution ResolveRenderPolicy(
        const RenderPolicyResolverInput& input)
    {
        RenderPolicyResolution resolution;
        resolution.frameSequence = input.request.frameSequence;
        resolution.viewOrdinal = input.viewOrdinal;
        resolution.capabilities = input.capabilities;
        resolution.qualification = MakeRenderQualificationSnapshot(
            input.qualification);
        resolution.viewPolicy.requestedMode = input.request.gpuDrivenMode;

        if (!IsValid(input.request.gpuDrivenMode))
        {
            resolution.viewPolicy.reason = RenderPolicyReason::InvalidRequest;
            return resolution;
        }
        if (!ValidateRenderPolicyResolverInput(input))
        {
            resolution.viewPolicy.reason = RenderPolicyReason::InconsistentFacts;
            return resolution;
        }

        RenderPolicyReason globalFailure = RenderPolicyReason::None;
        if (input.request.gpuDrivenMode == RenderGPUDrivenMode::ForceDisabled)
        {
            globalFailure = RenderPolicyReason::ForcedDirect;
        }
        else
        {
            globalFailure = GetGlobalGPUFailure(input);
        }

        const bool gpuEnabled = globalFailure == RenderPolicyReason::None;
        const RenderPolicyReason enabledReason =
            input.request.gpuDrivenMode == RenderGPUDrivenMode::ForceEnabled
                ? RenderPolicyReason::ForcedGPUDriven
                : RenderPolicyReason::None;

        std::vector<RenderPassPolicyFacts> canonicalFacts = input.passes;
        std::sort(canonicalFacts.begin(), canonicalFacts.end(),
                  [](const RenderPassPolicyFacts& lhs,
                     const RenderPassPolicyFacts& rhs)
                  {
                      return static_cast<uint8>(lhs.pass) <
                             static_cast<uint8>(rhs.pass);
                  });

        RenderPolicyReason firstPassFailure = globalFailure;
        bool anyGPUDrivenPackets = false;
        for (const RenderPassPolicyFacts& facts : canonicalFacts)
        {
            RenderPassPolicyDecision decision;
            if (!facts.requested)
            {
                decision = MakeDirectOrSkipDecision(
                    facts, RenderPolicyReason::PassDisabled, true);
            }
            else if (!facts.supported)
            {
                decision = MakeDirectOrSkipDecision(
                    facts, RenderPolicyReason::PassUnsupported, true);
            }
            else if (!gpuEnabled)
            {
                decision = MakeDirectOrSkipDecision(facts, globalFailure);
            }
            else if (!facts.gpuDrivenAllowed)
            {
                decision = MakeDirectOrSkipDecision(
                    facts, RenderPolicyReason::PassUnsupported);
            }
            else if (facts.candidatePacketCount == 0)
            {
                decision = MakeDirectOrSkipDecision(
                    facts, RenderPolicyReason::NoEligiblePackets);
            }
            else if (input.request.gpuDrivenMode == RenderGPUDrivenMode::Auto &&
                     !facts.workloadBeneficial)
            {
                decision = MakeDirectOrSkipDecision(
                    facts, RenderPolicyReason::WorkloadNotBeneficial);
            }
            else
            {
                const RenderPolicyReason readinessFailure = GetReadinessFailure(
                    facts.gpuDrivenShaderReadiness,
                    facts.gpuDrivenPipelineReadiness,
                    facts.gpuDrivenResourceReadiness);
                const RenderSubmissionMode gpuSubmission =
                    SelectGPUDrivenSubmission(input.capabilities, facts);
                if (readinessFailure != RenderPolicyReason::None)
                {
                    decision = MakeDirectOrSkipDecision(facts, readinessFailure);
                }
                else if (gpuSubmission == RenderSubmissionMode::Direct)
                {
                    decision = MakeDirectOrSkipDecision(
                        facts, RenderPolicyReason::CapabilityUnavailable);
                }
                else
                {
                    decision.pass = facts.pass;
                    decision.visibility = input.view.requestedVisibility;
                    decision.preferredSubmission = gpuSubmission;
                    decision.fallbackSubmission = RenderSubmissionMode::Direct;
                    decision.reason = enabledReason;
                    decision.partition = MakeSourceSummary(facts);
                    decision.partition.gpuDrivenPacketCount =
                        facts.candidatePacketCount;
                    decision.partition.drawGroupCount = facts.drawGroupCount;

                    RenderPolicyReason directFailure = RenderPolicyReason::None;
                    if (IsDirectReady(facts, &directFailure))
                    {
                        decision.partition.directPacketCount =
                            facts.directPacketCount;
                    }
                    else
                    {
                        decision.partition.skippedPacketCount +=
                            facts.directPacketCount;
                        if (facts.directPacketCount != 0)
                        {
                            decision.reason = directFailure;
                        }
                    }
                }
            }

            anyGPUDrivenPackets |= decision.partition.gpuDrivenPacketCount != 0;
            if (firstPassFailure == RenderPolicyReason::None &&
                decision.reason != RenderPolicyReason::None &&
                decision.reason != RenderPolicyReason::ForcedGPUDriven)
            {
                firstPassFailure = decision.reason;
            }
            resolution.canonicalPassDecisions.push_back(decision);
        }

        if (anyGPUDrivenPackets)
        {
            resolution.viewPolicy.selectedTier = GPUDrivenTier::IndirectGrouped;
            resolution.viewPolicy.immediateFallbackTier = GPUDrivenTier::Direct;
            resolution.viewPolicy.reason = enabledReason;

            // Tier 2 is intentionally considered only after the canonical
            // pass decisions already contain GPU work. It never repartitions
            // packets or changes the Tier 1 submission plan opportunistically.
            resolution.viewPolicy.requiredResidentVersion =
                input.gpuResidentScene.requiredResidentVersion;
            if (!input.capabilities.SupportsGPUResidentSceneBase())
            {
                resolution.viewPolicy.gpuResidentSceneReason =
                    GPUResidentSceneSelectionReason::CapabilityUnavailable;
            }
            else
            {
                resolution.viewPolicy.gpuResidentSceneReason =
                    GetGPUResidentSceneReadinessFailure(input.gpuResidentScene);
            }

            if (resolution.viewPolicy.gpuResidentSceneReason ==
                GPUResidentSceneSelectionReason::Ready)
            {
                resolution.viewPolicy.selectedTier =
                    GPUDrivenTier::GPUResidentScene;
                resolution.viewPolicy.immediateFallbackTier =
                    GPUDrivenTier::IndirectGrouped;
            }
        }
        else
        {
            resolution.viewPolicy.selectedTier = GPUDrivenTier::Direct;
            resolution.viewPolicy.immediateFallbackTier = GPUDrivenTier::Direct;
            resolution.viewPolicy.reason = firstPassFailure == RenderPolicyReason::None
                ? RenderPolicyReason::NoEligiblePackets
                : firstPassFailure;
        }
        return resolution;
    }

    bool ValidateRenderPolicyResolution(const RenderPolicyResolution& resolution)
    {
        const bool usesNoBackend =
            resolution.capabilities.backend == RHIBackendType::None &&
            resolution.qualification.backend == RHIBackendType::None;
        if (!IsValid(resolution.viewPolicy.requestedMode) ||
            !IsValid(resolution.viewPolicy.reason) ||
            !IsValidBackend(resolution.capabilities.backend) ||
            !IsValidBackend(resolution.qualification.backend))
        {
            return false;
        }

        const bool qualificationMatches =
            IsRenderQualificationSnapshotValid(resolution.qualification) &&
            (usesNoBackend ||
             (IsConcreteBackend(resolution.capabilities.backend) &&
              resolution.qualification.backend ==
                  resolution.capabilities.backend));

        RenderPassKind previousPass = RenderPassKind::None;
        bool hasGPUDrivenPackets = false;
        for (const RenderPassPolicyDecision& decision :
             resolution.canonicalPassDecisions)
        {
            if (!IsValid(decision.pass) ||
                static_cast<uint8>(decision.pass) <=
                    static_cast<uint8>(previousPass) ||
                !IsValid(decision.visibility) ||
                !IsValid(decision.preferredSubmission) ||
                !IsValid(decision.fallbackSubmission) ||
                decision.fallbackSubmission != RenderSubmissionMode::Direct ||
                !IsSubmissionSupported(
                    decision.preferredSubmission,
                    resolution.capabilities) ||
                !IsValid(decision.reason) ||
                !IsValidPartition(decision.partition))
            {
                return false;
            }
            if (decision.partition.gpuDrivenPacketCount != 0 &&
                (decision.preferredSubmission == RenderSubmissionMode::Direct ||
                 decision.visibility == RenderVisibilityMode::Cpu))
            {
                return false;
            }
            if (decision.partition.gpuDrivenPacketCount == 0 &&
                (decision.preferredSubmission != RenderSubmissionMode::Direct ||
                 decision.visibility != RenderVisibilityMode::Cpu))
            {
                return false;
            }
            if (!IsPassDecisionReasonConsistent(
                    resolution.viewPolicy.requestedMode,
                    decision.partition.gpuDrivenPacketCount != 0,
                    decision.reason))
            {
                return false;
            }
            hasGPUDrivenPackets |= decision.partition.gpuDrivenPacketCount != 0;
            previousPass = decision.pass;
        }

        return IsSelectedViewPolicyConsistent(
            resolution.viewPolicy,
            hasGPUDrivenPackets,
            qualificationMatches,
            resolution.qualification,
            resolution.capabilities);
    }

    bool ValidateRenderFrameExecutionPlan(const RenderFrameExecutionPlan& plan)
    {
        const bool usesNoBackend =
            plan.capabilities.backend == RHIBackendType::None &&
            plan.qualification.backend == RHIBackendType::None;
        if (!IsValid(plan.viewPolicy.requestedMode) ||
            !IsValid(plan.viewPolicy.reason) ||
            !IsValidBackend(plan.capabilities.backend) ||
            !IsValidBackend(plan.qualification.backend))
        {
            return false;
        }

        const bool qualificationMatches =
            IsRenderQualificationSnapshotValid(plan.qualification) &&
            (usesNoBackend ||
             (IsConcreteBackend(plan.capabilities.backend) &&
              plan.qualification.backend == plan.capabilities.backend));

        std::vector<bool> referenced(plan.packetReferences.size(), false);
        uint64 canonicalCursor = 0;
        RenderPassKind previousPass = RenderPassKind::None;
        bool hasGPUDrivenPackets = false;
        for (const RenderPassExecutionPlan& passPlan : plan.passes)
        {
            if (!IsValid(passPlan.pass) ||
                static_cast<uint8>(passPlan.pass) <=
                    static_cast<uint8>(previousPass) ||
                !IsValid(passPlan.visibility) ||
                !IsValid(passPlan.preferredSubmission) ||
                !IsValid(passPlan.fallbackSubmission) ||
                passPlan.fallbackSubmission != RenderSubmissionMode::Direct ||
                !IsSubmissionSupported(
                    passPlan.preferredSubmission, plan.capabilities) ||
                !IsValid(passPlan.reason) ||
                !IsValidPartition(passPlan.partition) ||
                passPlan.gpuEligiblePackets.count !=
                    passPlan.partition.gpuDrivenPacketCount ||
                passPlan.directPackets.count != passPlan.partition.directPacketCount ||
                passPlan.skippedPackets.count != passPlan.partition.skippedPacketCount ||
                !IsRangeInBounds(passPlan.gpuEligiblePackets,
                                 plan.packetReferences.size()) ||
                !IsRangeInBounds(passPlan.directPackets,
                                 plan.packetReferences.size()) ||
                !IsRangeInBounds(passPlan.skippedPackets,
                                 plan.packetReferences.size()))
            {
                return false;
            }

            uint64 reasonCountTotal = 0;
            for (uint32 count : passPlan.reasonCounts)
            {
                reasonCountTotal += count;
            }
            if (reasonCountTotal != passPlan.partition.inputPacketCount)
            {
                return false;
            }

            const bool passHasGPUDrivenPackets =
                passPlan.partition.gpuDrivenPacketCount != 0;
            if (passHasGPUDrivenPackets &&
                (passPlan.preferredSubmission == RenderSubmissionMode::Direct ||
                 passPlan.visibility == RenderVisibilityMode::Cpu))
            {
                return false;
            }
            if (!passHasGPUDrivenPackets &&
                (passPlan.preferredSubmission != RenderSubmissionMode::Direct ||
                 passPlan.visibility != RenderVisibilityMode::Cpu))
            {
                return false;
            }
            if (!IsPassDecisionReasonConsistent(
                    plan.viewPolicy.requestedMode,
                    passHasGPUDrivenPackets,
                    passPlan.reason))
            {
                return false;
            }
            hasGPUDrivenPackets |= passHasGPUDrivenPackets;

            const std::array<DrawPacketRange, 3> ranges = {
                passPlan.gpuEligiblePackets,
                passPlan.directPackets,
                passPlan.skippedPackets,
            };
            std::vector<bool> sourceIndices(
                passPlan.partition.inputPacketCount, false);
            uint64 terminalPacketCount = 0;
            for (const DrawPacketRange range : ranges)
            {
                if (range.first != canonicalCursor)
                {
                    return false;
                }
                const uint64 end = static_cast<uint64>(range.first) + range.count;
                terminalPacketCount += range.count;
                for (uint64 index = range.first; index < end; ++index)
                {
                    const RenderDrawPacketReference& reference =
                        plan.packetReferences[static_cast<size_t>(index)];
                    const RenderDrawPacketId& packetId = reference.packetId;
                    const RenderPreparedDrawPacketSignature& sourceSignature =
                        reference.sourceSignature;
                    if (referenced[static_cast<size_t>(index)] ||
                        reference.pass != passPlan.pass ||
                        reference.sourcePacketIndex >= sourceIndices.size() ||
                        sourceIndices[reference.sourcePacketIndex] ||
                        packetId.frameSequence != plan.frameSequence ||
                        packetId.viewOrdinal != plan.viewOrdinal ||
                        packetId.pass != reference.pass ||
                        packetId.sourcePacketIndex !=
                            reference.sourcePacketIndex ||
                        packetId.sourceOrdinal != reference.sourceOrdinal ||
                        sourceSignature.packet.pass != reference.pass ||
                        sourceSignature.sourceOrdinal !=
                            reference.sourceOrdinal ||
                        static_cast<uint8>(sourceSignature.disposition) >
                            static_cast<uint8>(MeshPassDisposition::Skip) ||
                        static_cast<uint8>(sourceSignature.reason) >=
                            static_cast<uint8>(
                                MeshPassEligibilityReason::Count) ||
                        packetId.objectId !=
                            sourceSignature.packet.objectId ||
                        packetId.primitiveData !=
                            sourceSignature.packet.primitiveData ||
                        packetId.mesh !=
                            sourceSignature.packet.geometryKey.mesh ||
                        packetId.logicalSubmeshIndex !=
                            sourceSignature.packet.submeshIndex ||
                        packetId.geometrySubmeshIndex !=
                            sourceSignature.packet.geometryKey.submeshIndex)
                    {
                        return false;
                    }
                    referenced[static_cast<size_t>(index)] = true;
                    sourceIndices[reference.sourcePacketIndex] = true;
                }
                canonicalCursor = end;
            }
            const uint32 unaccountedPacketIdCount = static_cast<uint32>(
                std::count(sourceIndices.begin(), sourceIndices.end(), false));
            if (terminalPacketCount > std::numeric_limits<uint32>::max())
            {
                return false;
            }
            const RenderPacketIdentityAccounting actualAccounting = {
                passPlan.partition.inputPacketCount,
                static_cast<uint32>(terminalPacketCount),
                static_cast<uint32>(terminalPacketCount),
                0,
                unaccountedPacketIdCount,
            };
            if (actualAccounting != passPlan.identityAccounting ||
                !actualAccounting.IsExactlyOnce())
            {
                return false;
            }
            previousPass = passPlan.pass;
        }

        const bool allReferencesCovered =
            std::all_of(referenced.begin(), referenced.end(),
                        [](bool value) { return value; });
        if (!allReferencesCovered ||
            canonicalCursor != plan.packetReferences.size())
        {
            return false;
        }

        return IsSelectedViewPolicyConsistent(
            plan.viewPolicy,
            hasGPUDrivenPackets,
            qualificationMatches,
            plan.qualification,
            plan.capabilities);
    }
} // namespace RVX
