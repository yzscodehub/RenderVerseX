#include "EcsRenderEntityRetirementCoordinator.h"

#include "ECS/Query.h"
#include "Scene/ECS/RetirementFragments.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace RVX
{
namespace
{
    using namespace SceneECS;
    using namespace ResourceSceneAdapters;

    constexpr CleanupDomainMask RenderCleanupDomain =
        ToCleanupDomainMask(CleanupDomain::Render);

    [[nodiscard]] bool RequiresUnacknowledgedRenderCleanup(
        const EntityLifecycleState& lifecycle) noexcept
    {
        return (lifecycle.requiredCleanupDomains & RenderCleanupDomain) != 0 &&
               (lifecycle.acknowledgedCleanupDomains & RenderCleanupDomain) == 0 &&
               lifecycle.phase == EntityLifecyclePhase::CleanupRequired;
    }

    [[nodiscard]] bool MatchesExactRequest(const EcsSceneAssetRetirementRequest& lhs,
                                           const EcsSceneAssetRetirementRequest& rhs) noexcept
    {
        return lhs.sceneRuntimeId == rhs.sceneRuntimeId &&
               lhs.rootEntity == rhs.rootEntity && lhs.members.size() == rhs.members.size() &&
               std::equal(lhs.members.begin(), lhs.members.end(), rhs.members.begin());
    }
} // namespace

struct EcsRenderEntityRetirementCoordinator::State
{
    enum class EntryState : uint8
    {
        AwaitingAdmission = 0,
        AwaitingProof,
        AwaitingSceneAcknowledgement,
        AwaitingProofAcknowledgement,
        DeviceLost,
    };

    struct Entry
    {
        CleanupRecord record;
        EcsSceneAssetRetirementRequest request;
        EcsSceneAssetRetirementToken token;
        EntryState state = EntryState::AwaitingAdmission;
        std::string lastDiagnostic;
    };

    State(SceneECS::SceneEcsRuntime& inRuntime,
          IEcsSceneAssetRetirementProofGateway& inProofGateway) noexcept
        : runtime(inRuntime)
        , proofGateway(inProofGateway)
    {
    }

    [[nodiscard]] bool HasOutstandingRetirements() const noexcept
    {
        return !entries.empty();
    }

    [[nodiscard]] EcsRenderEntityRetirementDiagnostics GetDiagnostics() const
    {
        EcsRenderEntityRetirementDiagnostics result = diagnostics;
        result.outstandingRetirementCount = static_cast<uint32>(entries.size());
        return result;
    }

    void MarkDiagnostic(std::string diagnostic)
    {
        diagnostics.lastDiagnostic = std::move(diagnostic);
    }

    [[nodiscard]] bool HasEntry(ECS::EntityHandle entity) const noexcept
    {
        return std::any_of(entries.begin(), entries.end(), [entity](const Entry& entry)
        {
            return entry.record.entity == entity;
        });
    }

    void TrackRecord(const CleanupRecord& record, ECS::Registry& registry)
    {
        if (record.sceneRuntimeId != runtime.get().GetSceneRuntimeId())
        {
            MarkDiagnostic("Ignored a foreign Scene cleanup record.");
            return;
        }
        if ((record.requiredCleanupDomains & RenderCleanupDomain) == 0)
        {
            return;
        }

        const EntityLifecycleState* lifecycle =
            registry.TryGet<EntityLifecycleState>(record.entity);
        if (lifecycle == nullptr)
        {
            ++diagnostics.staleCleanupRecordCount;
            MarkDiagnostic("Ignored a stale Render cleanup record before proof admission.");
            return;
        }
        if (!RequiresUnacknowledgedRenderCleanup(*lifecycle))
        {
            return;
        }
        if (registry.HasTag<SpecializedRenderRetirement>(record.entity))
        {
            ++diagnostics.specializedExclusionCount;
            return;
        }
        if (HasEntry(record.entity))
        {
            return;
        }

        try
        {
            Entry entry;
            entry.record = record;
            entry.request = {
                .sceneRuntimeId = record.sceneRuntimeId,
                .rootEntity = record.entity,
                .members = {record.entity},
            };
            entries.push_back(std::move(entry));
        }
        catch (...)
        {
            needsAuthoritativeRebuild = true;
            MarkDiagnostic("Failed to retain a Render cleanup record; retrying from the durable journal.");
        }
    }

    void RebuildAuthoritatively(ECS::Registry& registry)
    {
        ++diagnostics.authoritativeRebuildCount;
        registry.Query<ECS::Read<EntityLifecycleState>>().EachIncludingDisabled(
            [this, &registry](ECS::EntityHandle entity, const EntityLifecycleState& lifecycle)
            {
                if (!RequiresUnacknowledgedRenderCleanup(lifecycle))
                {
                    return;
                }
                if (registry.HasTag<SpecializedRenderRetirement>(entity))
                {
                    ++diagnostics.specializedExclusionCount;
                    return;
                }
                if (HasEntry(entity))
                {
                    return;
                }
                try
                {
                    Entry entry;
                    entry.record = {
                        .sceneRuntimeId = runtime.get().GetSceneRuntimeId(),
                        .entity = entity,
                        .reason = CleanupReason::RequestedDestroy,
                        .requiredCleanupDomains = lifecycle.requiredCleanupDomains,
                        .acknowledgedCleanupDomains = lifecycle.acknowledgedCleanupDomains,
                    };
                    entry.request = {
                        .sceneRuntimeId = entry.record.sceneRuntimeId,
                        .rootEntity = entity,
                        .members = {entity},
                    };
                    entries.push_back(std::move(entry));
                }
                catch (...)
                {
                    needsAuthoritativeRebuild = true;
                    MarkDiagnostic(
                        "Failed to rebuild a Render cleanup entry; the next cleanup frame retries.");
                }
            });
    }

    [[nodiscard]] bool IsStillOwnedByGenericCoordinator(const Entry& entry,
                                                         ECS::Registry& registry,
                                                         bool& outStale) const
    {
        outStale = false;
        const EntityLifecycleState* lifecycle =
            registry.TryGet<EntityLifecycleState>(entry.record.entity);
        if (lifecycle == nullptr)
        {
            outStale = true;
            return false;
        }
        if (!RequiresUnacknowledgedRenderCleanup(*lifecycle))
        {
            return false;
        }
        return !registry.HasTag<SpecializedRenderRetirement>(entry.record.entity);
    }

    [[nodiscard]] bool BeginProof(Entry& entry)
    {
        const EcsSceneAssetRetirementBeginReceipt receipt =
            proofGateway.get().BeginRetirement(entry.request);
        if (!receipt.IsAccepted() || !MatchesExactRequest(receipt.request, entry.request))
        {
            ++diagnostics.retirementAdmissionFailureCount;
            entry.lastDiagnostic = receipt.diagnostic.empty() ?
                                       "Render retirement admission did not return an exact token." :
                                       receipt.diagnostic;
            MarkDiagnostic(entry.lastDiagnostic);
            if (receipt.code == EcsSceneAssetRetirementBeginCode::DeviceLost)
            {
                ++diagnostics.deviceLostCount;
                entry.state = EntryState::DeviceLost;
            }
            return false;
        }

        entry.token = receipt.token;
        entry.lastDiagnostic.clear();
        entry.state = EntryState::AwaitingProof;
        return false;
    }

    [[nodiscard]] bool PollProof(Entry& entry)
    {
        const EcsSceneAssetRetirementProof proof =
            proofGateway.get().QueryRetirementProof(entry.token);
        if (!MatchesExactRequest(proof.request, entry.request))
        {
            ++diagnostics.retirementQueryFailureCount;
            entry.lastDiagnostic = proof.diagnostic.empty() ?
                                       "Render retirement proof did not echo its exact request." :
                                       proof.diagnostic;
            MarkDiagnostic(entry.lastDiagnostic);
            return false;
        }

        switch (proof.state)
        {
            case EcsSceneAssetRetirementProofState::Pending:
            case EcsSceneAssetRetirementProofState::AppliedNotPresented:
                return false;
            case EcsSceneAssetRetirementProofState::Presented:
                entry.state = EntryState::AwaitingSceneAcknowledgement;
                return false;
            case EcsSceneAssetRetirementProofState::NeverPublished:
                ++diagnostics.neverPublishedCount;
                entry.state = EntryState::AwaitingSceneAcknowledgement;
                return false;
            case EcsSceneAssetRetirementProofState::DeviceLost:
                ++diagnostics.deviceLostCount;
                entry.lastDiagnostic = proof.diagnostic.empty() ?
                                           "Render device loss retained an outstanding ECS proof." :
                                           proof.diagnostic;
                MarkDiagnostic(entry.lastDiagnostic);
                entry.state = EntryState::DeviceLost;
                return false;
            case EcsSceneAssetRetirementProofState::Failed:
                ++diagnostics.retirementQueryFailureCount;
                entry.lastDiagnostic = proof.diagnostic.empty() ?
                                           "Render retirement proof failed; retaining token for retry and diagnosis." :
                                           proof.diagnostic;
                MarkDiagnostic(entry.lastDiagnostic);
                return false;
        }
        ++diagnostics.retirementQueryFailureCount;
        MarkDiagnostic("Render retirement proof returned an unknown state.");
        return false;
    }

    [[nodiscard]] bool AcknowledgeSceneThenProof(Entry& entry, ECS::Registry& registry)
    {
        const EntityLifecycleState* lifecycle =
            registry.TryGet<EntityLifecycleState>(entry.record.entity);
        if (lifecycle == nullptr)
        {
            ++diagnostics.staleCleanupRecordCount;
            MarkDiagnostic("Refused to acknowledge Render proof after the Scene entity became stale.");
            return false;
        }

        if ((lifecycle->acknowledgedCleanupDomains & RenderCleanupDomain) == 0)
        {
            if (!runtime.get().AcknowledgeCleanup(entry.record.entity, RenderCleanupDomain))
            {
                ++diagnostics.sceneAcknowledgementFailureCount;
                MarkDiagnostic("Scene Render cleanup acknowledgement failed; retaining terminal proof token.");
                return false;
            }
        }

        entry.state = EntryState::AwaitingProofAcknowledgement;
        return AcknowledgeProof(entry);
    }

    [[nodiscard]] bool AcknowledgeProof(Entry& entry)
    {
        if (!proofGateway.get().AcknowledgeRetirementProof(entry.token))
        {
            ++diagnostics.proofAcknowledgementFailureCount;
            MarkDiagnostic("Render proof token acknowledgement failed after Scene cleanup acknowledgement.");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ProcessEntry(Entry& entry, ECS::Registry& registry)
    {
        switch (entry.state)
        {
            case EntryState::AwaitingAdmission:
            {
                bool stale = false;
                if (!IsStillOwnedByGenericCoordinator(entry, registry, stale))
                {
                    if (stale)
                    {
                        ++diagnostics.staleCleanupRecordCount;
                        MarkDiagnostic("Discarded a stale unadmitted Render cleanup record.");
                    }
                    else if (registry.HasTag<SpecializedRenderRetirement>(entry.record.entity))
                    {
                        ++diagnostics.specializedExclusionCount;
                    }
                    return true;
                }
                return BeginProof(entry);
            }
            case EntryState::AwaitingProof:
                return PollProof(entry);
            case EntryState::AwaitingSceneAcknowledgement:
                return AcknowledgeSceneThenProof(entry, registry);
            case EntryState::AwaitingProofAcknowledgement:
                return AcknowledgeProof(entry);
            case EntryState::DeviceLost:
                return false;
        }
        return false;
    }

    void Run(ECS::ProcessorExecutionContext& context)
    {
        hasCompletedProcessorRun = true;
        try
        {
            CleanupRecordRead read = runtime.get().ReadCleanupRecords(cursor);
            const bool continuityLost = read.continuity == CleanupRecordContinuity::Lost;
            if (continuityLost)
            {
                ++diagnostics.cleanupContinuityLossCount;
                MarkDiagnostic(
                    "Render cleanup journal continuity was lost; rebuilding from live lifecycle state.");
            }
            if (continuityLost || needsAuthoritativeRebuild)
            {
                // ReadCleanupRecords advances its independent cursor before a
                // consumer can retain values.  If local storage allocation
                // failed, recover from live lifecycle state on the next frame
                // rather than silently skipping that acknowledged journal span.
                needsAuthoritativeRebuild = false;
                RebuildAuthoritatively(context.registry);
            }
            else
            {
                for (const CleanupRecord& record : read.records)
                {
                    ++diagnostics.observedCleanupRecordCount;
                    TrackRecord(record, context.registry);
                }
            }

            auto current = entries.begin();
            while (current != entries.end())
            {
                if (ProcessEntry(*current, context.registry))
                {
                    current = entries.erase(current);
                }
                else
                {
                    ++current;
                }
            }
        }
        catch (...)
        {
            MarkDiagnostic(
                "Generic Render retirement encountered an exception; all retained proof state remains pending.");
        }
    }

    std::reference_wrapper<SceneECS::SceneEcsRuntime> runtime;
    std::reference_wrapper<IEcsSceneAssetRetirementProofGateway> proofGateway;
    CleanupRecordCursor cursor;
    std::vector<Entry> entries;
    EcsRenderEntityRetirementDiagnostics diagnostics;
    bool hasCompletedProcessorRun = false;
    bool needsAuthoritativeRebuild = false;
};

EcsRenderEntityRetirementCoordinator::EcsRenderEntityRetirementCoordinator(
    SceneECS::SceneEcsRuntime& runtime,
    ResourceSceneAdapters::IEcsSceneAssetRetirementProofGateway& proofGateway)
    : m_state(std::make_shared<State>(runtime, proofGateway))
{
    m_state->diagnostics.processorRegistered = runtime.RegisterProcessor({
        .name = "engine-generic-render-entity-retirement",
        .phase = ECS::ProcessorPhase::EndFrameCleanup,
        .group = "render-retirement",
        .groupOrder = std::numeric_limits<uint32>::max(),
        .order = std::numeric_limits<uint32>::max(),
        .access = {
            .reads = {typeid(SceneECS::EntityLifecycleState),
                      typeid(ECS::Tag<SceneECS::SpecializedRenderRetirement>)},
            .writes = {typeid(SceneECS::EntityLifecycleState)},
            .resourceWrites = {typeid(ResourceSceneAdapters::IEcsSceneAssetRetirementProofGateway)},
        },
        .allowsParallel = false,
        .stepMode = ECS::ProcessorStepMode::Variable,
        .runWithContext = [state = m_state](ECS::ProcessorExecutionContext& context)
        {
            state->Run(context);
        },
    });
    if (!m_state->diagnostics.processorRegistered)
    {
        m_state->MarkDiagnostic("Failed to register the generic Render retirement cleanup processor.");
    }
}

EcsRenderEntityRetirementCoordinator::~EcsRenderEntityRetirementCoordinator() noexcept
{
    if (m_state != nullptr && m_state->HasOutstandingRetirements())
    {
        m_state->diagnostics.ownerReleasedWithOutstandingProof = true;
        m_state->MarkDiagnostic(
            "Coordinator owner was released with outstanding Render proof; host shutdown drain was required.");
    }
}

bool EcsRenderEntityRetirementCoordinator::PrepareForShutdown() noexcept
{
    if (m_state == nullptr)
    {
        return true;
    }
    m_state->diagnostics.shutdownRequested = true;
    return m_state->diagnostics.processorRegistered &&
           m_state->hasCompletedProcessorRun && !m_state->HasOutstandingRetirements();
}

bool EcsRenderEntityRetirementCoordinator::HasOutstandingRetirements() const noexcept
{
    return m_state != nullptr && m_state->HasOutstandingRetirements();
}

EcsRenderEntityRetirementDiagnostics EcsRenderEntityRetirementCoordinator::GetDiagnostics() const
{
    return m_state != nullptr ? m_state->GetDiagnostics() : EcsRenderEntityRetirementDiagnostics{};
}
} // namespace RVX
