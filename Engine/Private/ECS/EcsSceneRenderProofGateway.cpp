/**
 * @file EcsSceneRenderProofGateway.cpp
 * @brief Engine-private ECS Render presentation and retirement proof implementation.
 */

#include "EcsSceneRenderProofGateway.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace RVX
{
namespace
{
    constexpr size_t RVX_ECS_RENDER_PROOF_MAX_MEMBER_HISTORY = 4096;
    constexpr size_t RVX_ECS_RENDER_PROOF_MAX_REMOVAL_HISTORY = 4096;
    constexpr size_t RVX_ECS_RENDER_PROOF_MAX_PRESENTATION_CANDIDATES = 64;

    [[nodiscard]] bool SameIdentity(
        const EcsSceneRenderProofGateway::RenderedMember& lhs,
        const EcsSceneRenderProofGateway::RenderedMember& rhs) noexcept
    {
        return lhs.sceneRuntimeId == rhs.sceneRuntimeId &&
               lhs.entity == rhs.entity && lhs.type == rhs.type;
    }

    [[nodiscard]] bool HasIdentity(
        const std::vector<EcsSceneRenderProofGateway::RenderedMember>& members,
        const EcsSceneRenderProofGateway::RenderedMember& target) noexcept
    {
        return std::any_of(members.begin(), members.end(),
                           [&target](const auto& value)
                           { return SameIdentity(value, target); });
    }

    [[nodiscard]] bool IsRenderableMemberType(
        EcsRenderSceneRetainedMemberType type) noexcept
    {
        return type == EcsRenderSceneRetainedMemberType::Mesh ||
               type == EcsRenderSceneRetainedMemberType::Light ||
               type == EcsRenderSceneRetainedMemberType::Skybox ||
               type == EcsRenderSceneRetainedMemberType::Particle ||
               type == EcsRenderSceneRetainedMemberType::Water ||
               type == EcsRenderSceneRetainedMemberType::Terrain;
    }

    [[nodiscard]] bool IsKnownDisposition(
        EcsFramePublicationDisposition disposition) noexcept
    {
        switch (disposition)
        {
            case EcsFramePublicationDisposition::Accepted:
            case EcsFramePublicationDisposition::NotAccepted:
            case EcsFramePublicationDisposition::SceneUpdateAcceptedWithoutFrame:
                return true;
            default: return false;
        }
    }

    [[nodiscard]] bool IsStrictlySortedVersions(
        std::span<const ResourceSceneAdapters::
                      EcsSceneAssetRenderableVisibilityVersion> versions) noexcept
    {
        for (size_t index = 0; index < versions.size(); ++index)
        {
            if (!versions[index].entity.IsValid() ||
                versions[index].entityVisibilityWriteVersion == 0 ||
                (index != 0 && !(versions[index - 1].entity < versions[index].entity)))
            {
                return false;
            }
        }
        return !versions.empty();
    }

    [[nodiscard]] bool IsValidRequest(
        const ResourceSceneAdapters::EcsSceneAssetRetirementRequest& request) noexcept
    {
        if (!request.sceneRuntimeId.IsValid() || !request.rootEntity.IsValid() ||
            request.members.empty())
        {
            return false;
        }
        for (size_t index = 0; index < request.members.size(); ++index)
        {
            if (!request.members[index].IsValid() ||
                std::find(request.members.begin(), request.members.begin() + index,
                          request.members[index]) != request.members.begin() + index)
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool RequestContains(
        const ResourceSceneAdapters::EcsSceneAssetRetirementRequest& request,
        ECS::EntityHandle entity) noexcept
    {
        return std::find(request.members.begin(), request.members.end(), entity) !=
               request.members.end();
    }

    void AddUnique(std::vector<EcsSceneRenderProofGateway::RenderedMember>& target,
                   const EcsSceneRenderProofGateway::RenderedMember& value)
    {
        if (!HasIdentity(target, value))
        {
            target.push_back(value);
        }
    }

    void AddOrReplaceEvidence(
        std::vector<EcsSceneRenderProofGateway::RemovalEvidence>& target,
        EcsSceneRenderProofGateway::RemovalEvidence value,
        bool& outContinuityLost)
    {
        const auto existing = std::find_if(
            target.begin(), target.end(), [&value](const auto& current)
            { return SameIdentity(current.member, value.member); });
        if (existing != target.end())
        {
            *existing = std::move(value);
            return;
        }
        if (target.size() >= RVX_ECS_RENDER_PROOF_MAX_REMOVAL_HISTORY)
        {
            // Retaining a conservative failure mode is safer than silently
            // forgetting a prior RenderScene identity and calling it absent.
            outContinuityLost = true;
            return;
        }
        target.push_back(std::move(value));
    }
} // namespace

EcsSceneRenderProofGateway::EcsSceneRenderProofGateway(
    EcsFrameExtractor& extractor) noexcept
    : m_extractor(extractor)
{
}

EcsSceneRenderProofGateway::~EcsSceneRenderProofGateway() = default;

bool EcsSceneRenderProofGateway::ObserveExtractionCandidate(
    const EcsFrozenSceneBridgeOutput& source,
    const EcsFrameExtractionResult& extraction)
{
    const bool frameOnly = extraction.IsFrameOnly();
    if (m_pendingCandidate.has_value() || !extraction.IsComplete() ||
        (!frameOnly && extraction.sceneUpdate == nullptr) ||
        (frameOnly && extraction.sceneUpdate != nullptr) ||
        extraction.frameV5 == nullptr ||
        !source.sceneRuntimeId.IsValid() || source.snapshotRevision == 0 ||
        source.sceneRuntimeId != extraction.sourceSceneRuntimeId ||
        source.snapshotRevision != extraction.sourceSnapshotRevision ||
        extraction.targetRenderSceneRevision == 0 ||
        (!frameOnly && extraction.sceneUpdate->targetSceneRevision !=
                            extraction.targetRenderSceneRevision) ||
        extraction.frameV5->GetHeader().sequence == 0 ||
        extraction.frameV5->GetHeader().requiredSceneRevision !=
            extraction.targetRenderSceneRevision ||
        !m_extractor.MatchesPendingCandidate(
            extraction.candidateIdentity,
            source.sceneRuntimeId,
            source.snapshotRevision,
            extraction.targetRenderSceneRevision,
            extraction.frameV5->GetHeader().sequence))
    {
        return false;
    }

    Candidate candidate;
    candidate.identity = extraction.candidateIdentity;
    candidate.sceneRuntimeId = source.sceneRuntimeId;
    candidate.frozenSourceSnapshotRevision = source.snapshotRevision;
    candidate.renderSceneRevision = extraction.targetRenderSceneRevision;
    candidate.frameSequence = extraction.frameV5->GetHeader().sequence;
    candidate.acceptedSceneRuntimeIdAtObserve =
        m_extractor.GetAcceptedSceneRuntimeId();
    candidate.acceptedSceneRevisionAtObserve =
        m_extractor.GetAcceptedSceneRevision();
    candidate.acceptedFrameSequenceAtObserve =
        m_extractor.GetAcceptedFrameSequence();
    candidate.fullReset = !frameOnly && extraction.sceneUpdate->fullReset;
    candidate.members.reserve(source.primitiveTemporalValues.size() +
                              source.lightValues.size() + source.skyboxes.size() +
                              source.particles.size() + source.water.size() +
                              source.terrain.size());

    std::vector<uint64> primitiveIds;
    primitiveIds.reserve(source.renderProxies.primitives.size());
    for (const RenderPrimitiveProxy& primitive : source.renderProxies.primitives)
    {
        if (!primitive.id.IsValid() ||
            std::find(primitiveIds.begin(), primitiveIds.end(), primitive.id.value) !=
                primitiveIds.end())
        {
            return false;
        }
        primitiveIds.push_back(primitive.id.value);
    }
    for (const EcsFrozenScenePrimitiveTemporalValue& temporal :
         source.primitiveTemporalValues)
    {
        if (!temporal.primitiveId.IsValid() || temporal.ownerId == 0 ||
            temporal.ownerId != temporal.primitiveId.value ||
            !temporal.sourceEntity.IsValid() ||
            std::find(primitiveIds.begin(), primitiveIds.end(),
                      temporal.primitiveId.value) == primitiveIds.end())
        {
            return false;
        }
        RenderedMember member{source.sceneRuntimeId,
                              temporal.sourceEntity,
                              EcsRenderSceneRetainedMemberType::Mesh,
                              temporal.visibilityWriteVersion};
        if (HasIdentity(candidate.members, member))
        {
            return false;
        }
        candidate.members.push_back(member);
    }
    if (candidate.members.size() != source.renderProxies.primitives.size())
    {
        return false;
    }

    for (const EcsFrozenSceneLightValue& light : source.lightValues)
    {
        if (!light.id || !light.sourceEntity.IsValid())
        {
            return false;
        }
        // Invisible lights are source-complete values but do not own a
        // retained RenderScene light and therefore need no removal barrier.
        if (!light.visible)
        {
            continue;
        }
        RenderedMember member{source.sceneRuntimeId,
                              light.sourceEntity,
                              EcsRenderSceneRetainedMemberType::Light,
                              0};
        if (HasIdentity(candidate.members, member))
        {
            return false;
        }
        candidate.members.push_back(member);
    }
    for (const EcsFrozenSceneSkyboxValue& skybox : source.skyboxes)
    {
        const uint64 expectedId = EcsFrozenSceneBridge::DeriveRenderId(
            {source.sceneRuntimeId,
             skybox.sourceEntity,
             SceneECS::FrozenSceneObjectType::Skybox,
             0});
        if (source.skyboxes.size() != 1 || !skybox.id ||
            !skybox.sourceEntity.IsValid() || skybox.id != expectedId)
        {
            return false;
        }
        RenderedMember member{source.sceneRuntimeId,
                              skybox.sourceEntity,
                              EcsRenderSceneRetainedMemberType::Skybox,
                              0,
                              skybox.skyboxWriteVersion};
        if (HasIdentity(candidate.members, member))
        {
            return false;
        }
        candidate.members.push_back(member);
    }
    for (const EcsFrozenSceneParticleValue& particle : source.particles)
    {
        const uint64 expectedId = EcsFrozenSceneBridge::DeriveRenderId(
            {source.sceneRuntimeId,
             particle.sourceEntity,
             SceneECS::FrozenSceneObjectType::Particle,
             0});
        if (particle.id == 0 || !particle.sourceEntity.IsValid() ||
            particle.id != expectedId || particle.state.instanceId != particle.id ||
            particle.state.systemId != particle.id)
        {
            return false;
        }
        RenderedMember member{source.sceneRuntimeId,
                              particle.sourceEntity,
                              EcsRenderSceneRetainedMemberType::Particle,
                              0};
        if (HasIdentity(candidate.members, member))
        {
            return false;
        }
        candidate.members.push_back(member);
    }
    for (const EcsFrozenSceneWaterValue& water : source.water)
    {
        const uint64 expectedId = EcsFrozenSceneBridge::DeriveRenderId(
            {source.sceneRuntimeId,
             water.sourceEntity,
             SceneECS::FrozenSceneObjectType::Water,
             0});
        if (water.id == 0 || !water.sourceEntity.IsValid() ||
            water.id != expectedId || water.state.componentId != water.id)
        {
            return false;
        }
        RenderedMember member{source.sceneRuntimeId,
                              water.sourceEntity,
                              EcsRenderSceneRetainedMemberType::Water,
                              0};
        if (HasIdentity(candidate.members, member))
        {
            return false;
        }
        candidate.members.push_back(member);
    }
    for (const EcsFrozenSceneTerrainValue& terrain : source.terrain)
    {
        const uint64 expectedId = EcsFrozenSceneBridge::DeriveRenderId(
            {source.sceneRuntimeId,
             terrain.sourceEntity,
             SceneECS::FrozenSceneObjectType::Terrain,
             0});
        if (terrain.id == 0 || !terrain.sourceEntity.IsValid() ||
            terrain.id != expectedId || terrain.state.componentId != terrain.id)
        {
            return false;
        }
        RenderedMember member{source.sceneRuntimeId,
                              terrain.sourceEntity,
                              EcsRenderSceneRetainedMemberType::Terrain,
                              0};
        if (HasIdentity(candidate.members, member))
        {
            return false;
        }
        candidate.members.push_back(member);
    }

    m_pendingCandidate = std::move(candidate);
    return true;
}

bool EcsSceneRenderProofGateway::IsProgressMonotonic(
    const EcsSceneRenderPublicationProgress& progress) const noexcept
{
    if (m_progress.deviceLost)
    {
        return progress.deviceLost;
    }
    return progress.deviceLost ||
           (progress.appliedRenderSceneRevision >=
                m_progress.appliedRenderSceneRevision &&
            progress.presentedFrameSequence >= m_progress.presentedFrameSequence);
}

void EcsSceneRenderProofGateway::ApplyProgress(
    EcsSceneRenderPublicationProgress progress) noexcept
{
    m_progress = progress;
    if (!m_progress.deviceLost)
    {
        return;
    }
    for (RetirementEntry& entry : m_retirements)
    {
        if (!entry.token.IsValid() ||
            entry.state == ResourceSceneAdapters::EcsSceneAssetRetirementProofState::Presented ||
            entry.state == ResourceSceneAdapters::EcsSceneAssetRetirementProofState::NeverPublished ||
            entry.state == ResourceSceneAdapters::EcsSceneAssetRetirementProofState::Failed)
        {
            continue;
        }
        entry.state = ResourceSceneAdapters::EcsSceneAssetRetirementProofState::DeviceLost;
        entry.diagnostic = "Render reported device loss before ECS proof completion.";
    }
}

bool EcsSceneRenderProofGateway::ObservePublicationProgress(
    EcsSceneRenderPublicationProgress progress)
{
    if (!IsProgressMonotonic(progress))
    {
        return false;
    }
    ApplyProgress(progress);
    RefreshRetirementEntries();
    return true;
}

void EcsSceneRenderProofGateway::RecordReliableCandidate(
    const Candidate& candidate,
    bool frameAccepted) noexcept
{
    const auto addKnown = [this](const RenderedMember& member)
    {
        if (HasIdentity(m_knownReliableMembers, member))
        {
            return;
        }
        if (m_knownReliableMembers.size() >= RVX_ECS_RENDER_PROOF_MAX_MEMBER_HISTORY)
        {
            m_historyContinuityLost = true;
            return;
        }
        m_knownReliableMembers.push_back(member);
    };

    for (const RenderedMember& member : candidate.members)
    {
        addKnown(member);
    }

    const auto recordRemoval = [this, &candidate](const RenderedMember& member)
    {
        AddOrReplaceEvidence(m_removalEvidence,
                             {.member = member,
                              .renderSceneRevision = candidate.renderSceneRevision,
                              .carryingFrameSequence = candidate.frameSequence},
                             m_historyContinuityLost);
        std::erase_if(m_unpresentedRemovals,
                      [&member](const RenderedMember& current)
                      { return SameIdentity(current, member); });
    };

    for (const RenderedMember& previous : m_currentReliableMembers)
    {
        if (HasIdentity(candidate.members, previous))
        {
            continue;
        }
        if (frameAccepted)
        {
            recordRemoval(previous);
        }
        else
        {
            AddUnique(m_unpresentedRemovals, previous);
        }
    }

    if (frameAccepted)
    {
        // A scene-only acceptance forces the extractor's next candidate to a
        // full reset. Its accepted frame is the first valid presentation of
        // every removal which was applied scene-only in the meantime.
        const std::vector<RenderedMember> pending = m_unpresentedRemovals;
        for (const RenderedMember& removed : pending)
        {
            if (!HasIdentity(candidate.members, removed))
            {
                recordRemoval(removed);
            }
            else
            {
                std::erase_if(m_unpresentedRemovals,
                              [&removed](const RenderedMember& current)
                              { return SameIdentity(current, removed); });
            }
        }
        m_lastPresentedMembers = candidate.members;
        m_presentedCandidates.push_back(candidate);
        if (m_presentedCandidates.size() >
            RVX_ECS_RENDER_PROOF_MAX_PRESENTATION_CANDIDATES)
        {
            m_presentedCandidates.erase(m_presentedCandidates.begin());
            m_presentationHistoryContinuityLost = true;
        }
    }

    m_currentReliableRuntime = candidate.sceneRuntimeId;
    m_currentReliableMembers = candidate.members;
    m_hasTrustedReliableSnapshot = true;
}

bool EcsSceneRenderProofGateway::ResolveObservedCandidate(
    EcsFramePublicationDisposition disposition,
    const EcsRenderSceneRetirementResolution& retirementResolution,
    EcsSceneRenderPublicationProgress progress)
{
    if (!m_pendingCandidate.has_value() || !retirementResolution.resolvedCandidate ||
        !retirementResolution.candidateIdentity.IsValid() ||
        retirementResolution.candidateIdentity != m_pendingCandidate->identity ||
        retirementResolution.disposition != disposition ||
        !IsKnownDisposition(disposition) || !IsProgressMonotonic(progress) ||
        retirementResolution.bindingCount > retirementResolution.bindings.size() ||
        (disposition != EcsFramePublicationDisposition::Accepted &&
         retirementResolution.bindingCount != 0))
    {
        return false;
    }

    const Candidate candidate = *m_pendingCandidate;
    if (m_extractor.HasPendingCandidate() ||
        m_extractor.GetPendingCandidateIdentity().IsValid())
    {
        return false;
    }

    switch (disposition)
    {
        case EcsFramePublicationDisposition::Accepted:
            if (m_extractor.GetAcceptedSceneRuntimeId() != candidate.sceneRuntimeId ||
                m_extractor.GetAcceptedSceneRevision() != candidate.renderSceneRevision ||
                m_extractor.GetAcceptedFrameSequence() != candidate.frameSequence)
            {
                return false;
            }
            break;
        case EcsFramePublicationDisposition::SceneUpdateAcceptedWithoutFrame:
            if (m_extractor.GetAcceptedSceneRuntimeId() != candidate.sceneRuntimeId ||
                m_extractor.GetAcceptedSceneRevision() != candidate.renderSceneRevision ||
                candidate.renderSceneRevision <=
                    candidate.acceptedSceneRevisionAtObserve ||
                m_extractor.GetAcceptedFrameSequence() !=
                    candidate.acceptedFrameSequenceAtObserve)
            {
                return false;
            }
            break;
        case EcsFramePublicationDisposition::NotAccepted:
            if (m_extractor.GetAcceptedSceneRuntimeId() !=
                    candidate.acceptedSceneRuntimeIdAtObserve ||
                m_extractor.GetAcceptedSceneRevision() !=
                    candidate.acceptedSceneRevisionAtObserve ||
                m_extractor.GetAcceptedFrameSequence() !=
                    candidate.acceptedFrameSequenceAtObserve)
            {
                return false;
            }
            break;
        default: return false;
    }

    for (uint32 index = 0; index < retirementResolution.bindingCount; ++index)
    {
        const EcsRenderSceneRetirementBinding& binding =
            retirementResolution.bindings[index];
        if (!binding.IsValid() || binding.minimumFrameSequence != candidate.frameSequence ||
            binding.targetSceneRevision > candidate.renderSceneRevision ||
            std::any_of(retirementResolution.bindings.begin(),
                        retirementResolution.bindings.begin() + index,
                        [&binding](const auto& previous)
                        { return previous.correlation == binding.correlation; }))
        {
            return false;
        }

        const auto found = std::find_if(
            m_retirements.begin(), m_retirements.end(), [&binding](const auto& entry)
            { return entry.token.IsValid() && entry.correlation == binding.correlation; });
        if (found == m_retirements.end())
        {
            continue;
        }
        if (!found->awaitingExtractorBarrier ||
            std::any_of(found->barrierMembers.begin(), found->barrierMembers.end(),
                        [&candidate](const RenderedMember& member)
                        { return HasIdentity(candidate.members, member); }))
        {
            return false;
        }
    }

    ApplyProgress(progress);
    if (disposition == EcsFramePublicationDisposition::Accepted ||
        disposition == EcsFramePublicationDisposition::SceneUpdateAcceptedWithoutFrame)
    {
        const bool frameAccepted =
            disposition == EcsFramePublicationDisposition::Accepted;
        RecordReliableCandidate(candidate, frameAccepted);

        for (uint32 index = 0; index < retirementResolution.bindingCount; ++index)
        {
            const EcsRenderSceneRetirementBinding& binding =
                retirementResolution.bindings[index];
            const auto found = std::find_if(
                m_retirements.begin(), m_retirements.end(), [&binding](const auto& entry)
                { return entry.token.IsValid() && entry.correlation == binding.correlation; });
            if (found == m_retirements.end())
            {
                // A different Engine-private owner can observe its own
                // correlation in the same extractor; it is not our proof.
                continue;
            }
            found->awaitingExtractorBarrier = false;
            found->appliedRenderSceneRevision = std::max(
                found->appliedRenderSceneRevision, binding.targetSceneRevision);
            found->requiredPresentedFrameSequence = std::max(
                found->requiredPresentedFrameSequence, binding.minimumFrameSequence);
            found->state =
                ResourceSceneAdapters::EcsSceneAssetRetirementProofState::AppliedNotPresented;
        }
    }

    // NotAccepted deliberately changes neither source history nor any proof.
    // Scene-only updates deliberately do not add a presentation candidate.
    m_pendingCandidate.reset();
    RefreshRetirementEntries();
    return true;
}

bool EcsSceneRenderProofGateway::IsTrustedFor(
    ECS::SceneRuntimeId sceneRuntimeId) const noexcept
{
    if (!m_hasTrustedReliableSnapshot)
    {
        return false;
    }
    return m_currentReliableRuntime == sceneRuntimeId ||
           std::any_of(m_knownReliableMembers.begin(), m_knownReliableMembers.end(),
                       [sceneRuntimeId](const RenderedMember& member)
                       { return member.sceneRuntimeId == sceneRuntimeId; });
}

std::optional<EcsSceneRenderProofGateway::RemovalEvidence>
EcsSceneRenderProofGateway::FindRemovalEvidence(
    const RenderedMember& member) const
{
    const auto found = std::find_if(
        m_removalEvidence.begin(), m_removalEvidence.end(), [&member](const auto& evidence)
        { return SameIdentity(evidence.member, member); });
    return found != m_removalEvidence.end() ? std::optional<RemovalEvidence>(*found) :
                                             std::nullopt;
}

void EcsSceneRenderProofGateway::RefreshRetirementEntries() noexcept
{
    for (RetirementEntry& entry : m_retirements)
    {
        if (!entry.token.IsValid() ||
            entry.state == ResourceSceneAdapters::EcsSceneAssetRetirementProofState::Presented ||
            entry.state == ResourceSceneAdapters::EcsSceneAssetRetirementProofState::NeverPublished ||
            entry.state == ResourceSceneAdapters::EcsSceneAssetRetirementProofState::Failed ||
            entry.state == ResourceSceneAdapters::EcsSceneAssetRetirementProofState::DeviceLost)
        {
            continue;
        }
        if (m_progress.deviceLost)
        {
            entry.state = ResourceSceneAdapters::EcsSceneAssetRetirementProofState::DeviceLost;
            entry.diagnostic = "Render reported device loss before ECS proof completion.";
            continue;
        }

        bool waitingForRemoval = false;
        for (const RenderedMember& pending : entry.awaitingUnpresentedRemovals)
        {
            if (HasIdentity(m_currentReliableMembers, pending))
            {
                entry.state = ResourceSceneAdapters::EcsSceneAssetRetirementProofState::Failed;
                entry.diagnostic =
                    "A scene-only retired Render identity reappeared before an accepted frame proved removal.";
                waitingForRemoval = false;
                break;
            }
            const std::optional<RemovalEvidence> evidence = FindRemovalEvidence(pending);
            if (!evidence.has_value())
            {
                waitingForRemoval = true;
                continue;
            }
            entry.appliedRenderSceneRevision = std::max(
                entry.appliedRenderSceneRevision, evidence->renderSceneRevision);
            entry.requiredPresentedFrameSequence = std::max(
                entry.requiredPresentedFrameSequence, evidence->carryingFrameSequence);
        }
        if (entry.state == ResourceSceneAdapters::EcsSceneAssetRetirementProofState::Failed)
        {
            continue;
        }
        if (waitingForRemoval)
        {
            entry.state = ResourceSceneAdapters::EcsSceneAssetRetirementProofState::Pending;
            continue;
        }
        entry.awaitingUnpresentedRemovals.clear();
        if (entry.awaitingExtractorBarrier)
        {
            entry.state = ResourceSceneAdapters::EcsSceneAssetRetirementProofState::Pending;
            continue;
        }
        if (entry.appliedRenderSceneRevision == 0 ||
            entry.requiredPresentedFrameSequence == 0)
        {
            entry.state = ResourceSceneAdapters::EcsSceneAssetRetirementProofState::Pending;
            continue;
        }
        entry.state =
            m_progress.appliedRenderSceneRevision >= entry.appliedRenderSceneRevision &&
                    m_progress.presentedFrameSequence >=
                        entry.requiredPresentedFrameSequence ?
                ResourceSceneAdapters::EcsSceneAssetRetirementProofState::Presented :
                ResourceSceneAdapters::EcsSceneAssetRetirementProofState::AppliedNotPresented;
    }
}

std::optional<ResourceSceneAdapters::EcsSceneAssetMinimumResidentPresentationReceipt>
EcsSceneRenderProofGateway::BuildMinimumResidentPresentationReceipt(
    ECS::SceneRuntimeId sceneRuntimeId,
    ECS::EntityHandle rootEntity,
    std::span<const ResourceSceneAdapters::EcsSceneAssetRenderableVisibilityVersion>
        renderableVisibilityVersions) const
{
    if (m_progress.deviceLost || !sceneRuntimeId.IsValid() || !rootEntity.IsValid() ||
        !IsStrictlySortedVersions(renderableVisibilityVersions))
    {
        return std::nullopt;
    }

    for (auto candidate = m_presentedCandidates.rbegin();
         candidate != m_presentedCandidates.rend(); ++candidate)
    {
        if (candidate->sceneRuntimeId != sceneRuntimeId ||
            m_progress.appliedRenderSceneRevision < candidate->renderSceneRevision ||
            m_progress.presentedFrameSequence < candidate->frameSequence)
        {
            continue;
        }

        const bool containsEveryExactVersion = std::all_of(
            renderableVisibilityVersions.begin(), renderableVisibilityVersions.end(),
            [&candidate, sceneRuntimeId](const auto& expected)
            {
                return std::any_of(
                    candidate->members.begin(), candidate->members.end(),
                    [&expected, sceneRuntimeId](const RenderedMember& carried)
                    {
                        return carried.sceneRuntimeId == sceneRuntimeId &&
                               carried.type == EcsRenderSceneRetainedMemberType::Mesh &&
                               carried.entity == expected.entity &&
                               carried.visibilityWriteVersion ==
                                   expected.entityVisibilityWriteVersion;
                    });
            });
        if (!containsEveryExactVersion)
        {
            continue;
        }

        return ResourceSceneAdapters::EcsSceneAssetMinimumResidentPresentationReceipt{
            .sceneRuntimeId = sceneRuntimeId,
            .rootEntity = rootEntity,
            .renderableVisibilityVersions = {
                renderableVisibilityVersions.begin(), renderableVisibilityVersions.end()},
            .frozenSourceSnapshotRevision = candidate->frozenSourceSnapshotRevision,
            .renderSceneRevision = candidate->renderSceneRevision,
            .carryingPresentedFrameSequence = candidate->frameSequence,
        };
    }
    return std::nullopt;
}

std::optional<ResourceSceneAdapters::EcsEnvironmentPresentationReceipt>
EcsSceneRenderProofGateway::BuildEnvironmentPresentationReceipt(
    ECS::SceneRuntimeId sceneRuntimeId,
    ECS::EntityHandle skyboxEntity,
    uint64 skyboxWriteVersion) const
{
    if (m_progress.deviceLost || m_historyContinuityLost ||
        m_presentationHistoryContinuityLost || !sceneRuntimeId.IsValid() ||
        !skyboxEntity.IsValid() ||
        m_presentedCandidates.empty())
    {
        return std::nullopt;
    }

    const auto currentSkybox = std::find_if(
        m_currentReliableMembers.begin(), m_currentReliableMembers.end(),
        [sceneRuntimeId, skyboxEntity](const RenderedMember& member)
        {
            return member.sceneRuntimeId == sceneRuntimeId &&
                   member.type == EcsRenderSceneRetainedMemberType::Skybox &&
                   member.entity == skyboxEntity;
        });
    if (m_currentReliableRuntime != sceneRuntimeId ||
        currentSkybox == m_currentReliableMembers.end() ||
        currentSkybox->skyboxWriteVersion != skyboxWriteVersion ||
        std::count_if(m_currentReliableMembers.begin(), m_currentReliableMembers.end(),
                      [sceneRuntimeId](const RenderedMember& member)
                      {
                          return member.sceneRuntimeId == sceneRuntimeId &&
                                 member.type ==
                                     EcsRenderSceneRetainedMemberType::Skybox;
                      }) != 1)
    {
        return std::nullopt;
    }

    // Accepted candidates can lead completed presentation by one or more
    // update-thread frames. Requiring the newest accepted candidate itself to
    // be presented creates a moving-target wait under continuous publication.
    // The current reliable baseline above already proves that no newer Skybox
    // value replaced this exact source version, so use the newest candidate
    // carrying the same value whose frame has actually completed presentation.
    const auto candidate = std::find_if(
        m_presentedCandidates.rbegin(), m_presentedCandidates.rend(),
        [this, sceneRuntimeId, skyboxEntity, skyboxWriteVersion](const Candidate& value)
        {
            if (value.sceneRuntimeId != sceneRuntimeId ||
                m_progress.appliedRenderSceneRevision < value.renderSceneRevision ||
                m_progress.presentedFrameSequence < value.frameSequence)
            {
                return false;
            }
            const auto exactSkybox = std::find_if(
                value.members.begin(), value.members.end(),
                [sceneRuntimeId, skyboxEntity](const RenderedMember& member)
                {
                    return member.sceneRuntimeId == sceneRuntimeId &&
                           member.type == EcsRenderSceneRetainedMemberType::Skybox &&
                           member.entity == skyboxEntity;
                });
            return exactSkybox != value.members.end() &&
                   exactSkybox->skyboxWriteVersion == skyboxWriteVersion &&
                   std::count_if(
                       value.members.begin(), value.members.end(),
                       [sceneRuntimeId](const RenderedMember& member)
                       {
                           return member.sceneRuntimeId == sceneRuntimeId &&
                                  member.type ==
                                      EcsRenderSceneRetainedMemberType::Skybox;
                       }) == 1;
        });
    if (candidate == m_presentedCandidates.rend())
    {
        return std::nullopt;
    }

    return ResourceSceneAdapters::EcsEnvironmentPresentationReceipt{
        .sceneRuntimeId = sceneRuntimeId,
        .skyboxEntity = skyboxEntity,
        .skyboxWriteVersion = skyboxWriteVersion,
        .frozenSourceSnapshotRevision = candidate->frozenSourceSnapshotRevision,
        .renderSceneRevision = candidate->renderSceneRevision,
        .carryingPresentedFrameSequence = candidate->frameSequence,
    };
}

ResourceSceneAdapters::EcsSceneAssetRetirementBeginReceipt
EcsSceneRenderProofGateway::BeginRetirement(
    const ResourceSceneAdapters::EcsSceneAssetRetirementRequest& request)
{
    using namespace ResourceSceneAdapters;
    const auto reject = [&request](EcsSceneAssetRetirementBeginCode code,
                                   std::string diagnostic)
    {
        return EcsSceneAssetRetirementBeginReceipt{
            .code = code, .request = request, .diagnostic = std::move(diagnostic)};
    };
    if (!IsValidRequest(request))
    {
        return reject(EcsSceneAssetRetirementBeginCode::Rejected,
                      "Render retirement request is not an exact non-empty ECS identity set.");
    }
    if (m_progress.deviceLost)
    {
        return reject(EcsSceneAssetRetirementBeginCode::DeviceLost,
                      "Render device is already lost.");
    }
    if (m_pendingCandidate.has_value())
    {
        return reject(EcsSceneAssetRetirementBeginCode::Rejected,
                      "Render retirement cannot be admitted while an ECS candidate is unresolved.");
    }
    if (!IsTrustedFor(request.sceneRuntimeId))
    {
        return reject(EcsSceneAssetRetirementBeginCode::FailedRetained,
                      "Render proof history does not cover this ECS Scene runtime.");
    }

    RetirementEntry* entry = nullptr;
    size_t entryIndex = 0;
    for (; entryIndex < m_retirements.size(); ++entryIndex)
    {
        if (!m_retirements[entryIndex].token.IsValid())
        {
            entry = &m_retirements[entryIndex];
            break;
        }
    }
    if (entry == nullptr)
    {
        m_retirements.emplace_back();
        entryIndex = m_retirements.size() - 1;
        entry = &m_retirements.back();
    }
    if (entry->nextGeneration == 0)
    {
        return reject(EcsSceneAssetRetirementBeginCode::FailedRetained,
                      "Render proof token generation overflowed.");
    }
    const uint64 tokenValue = static_cast<uint64>(entryIndex) + 1u;
    entry->token = {.value = tokenValue, .generation = entry->nextGeneration};
    entry->request = request;
    entry->correlation = {};
    entry->state = EcsSceneAssetRetirementProofState::Pending;
    entry->appliedRenderSceneRevision = 0;
    entry->requiredPresentedFrameSequence = 0;
    entry->awaitingExtractorBarrier = false;
    entry->barrierMembers.clear();
    entry->awaitingUnpresentedRemovals.clear();
    entry->diagnostic.clear();

    std::vector<RenderedMember> current;
    std::vector<RenderedMember> historical;
    for (const RenderedMember& member : m_currentReliableMembers)
    {
        if (member.sceneRuntimeId == request.sceneRuntimeId &&
            RequestContains(request, member.entity))
        {
            AddUnique(current, member);
        }
    }
    for (const RenderedMember& member : m_knownReliableMembers)
    {
        if (member.sceneRuntimeId == request.sceneRuntimeId &&
            RequestContains(request, member.entity))
        {
            AddUnique(historical, member);
        }
    }

    for (const RenderedMember& member : historical)
    {
        if (HasIdentity(current, member))
        {
            continue;
        }
        const std::optional<RemovalEvidence> evidence = FindRemovalEvidence(member);
        if (evidence.has_value())
        {
            entry->appliedRenderSceneRevision = std::max(
                entry->appliedRenderSceneRevision, evidence->renderSceneRevision);
            entry->requiredPresentedFrameSequence = std::max(
                entry->requiredPresentedFrameSequence,
                evidence->carryingFrameSequence);
            continue;
        }
        if (HasIdentity(m_unpresentedRemovals, member))
        {
            AddUnique(entry->awaitingUnpresentedRemovals, member);
            continue;
        }
        entry->token = {};
        if (m_historyContinuityLost)
        {
            return reject(EcsSceneAssetRetirementBeginCode::FailedRetained,
                          "Bounded Render identity history lost continuity for an absent member.");
        }
        return reject(EcsSceneAssetRetirementBeginCode::FailedRetained,
                      "A previously retained Render identity has no removal proof.");
    }

    if (current.empty())
    {
        if (historical.empty())
        {
            if (m_historyContinuityLost)
            {
                entry->token = {};
                return reject(EcsSceneAssetRetirementBeginCode::FailedRetained,
                              "Bounded Render identity history cannot prove this absent member was never published.");
            }
            entry->state = EcsSceneAssetRetirementProofState::NeverPublished;
            entry->diagnostic =
                "Exact ECS members never entered the trusted RenderScene baseline.";
        }
        else
        {
            RefreshRetirementEntries();
        }
        return {.code = EcsSceneAssetRetirementBeginCode::Accepted,
                .token = entry->token,
                .request = request,
                .diagnostic = entry->diagnostic};
    }

    EcsRenderSceneRetirementBarrier barrier;
    barrier.sceneRuntimeId = request.sceneRuntimeId;
    barrier.members.reserve(current.size());
    for (const RenderedMember& member : current)
    {
        if (!IsRenderableMemberType(member.type))
        {
            entry->token = {};
            return reject(EcsSceneAssetRetirementBeginCode::FailedRetained,
                          "Render proof encountered an unsupported ECS retained member type.");
        }
        barrier.members.push_back({.entity = member.entity, .type = member.type});
    }
    do
    {
        barrier.correlation.value = m_nextCorrelation++;
        if (m_nextCorrelation == 0)
        {
            m_nextCorrelation = 1;
        }
    } while (!barrier.correlation.IsValid());

    const EcsRenderSceneRetirementBarrierSubmitResult submitted =
        m_extractor.SubmitRetirementBarrier(barrier);
    if (!submitted.IsAccepted())
    {
        entry->token = {};
        return reject(EcsSceneAssetRetirementBeginCode::FailedRetained,
                      "ECS Render extractor could not admit the exact retirement barrier.");
    }
    entry->correlation = barrier.correlation;
    entry->awaitingExtractorBarrier = true;
    entry->barrierMembers = current;
    return {.code = EcsSceneAssetRetirementBeginCode::Accepted,
            .token = entry->token,
            .request = request};
}

EcsSceneRenderProofGateway::RetirementEntry* EcsSceneRenderProofGateway::Resolve(
    ResourceSceneAdapters::EcsSceneAssetRetirementToken token) noexcept
{
    if (!token.IsValid() || token.value == 0 ||
        token.value > static_cast<uint64>(m_retirements.size()))
    {
        return nullptr;
    }
    RetirementEntry& entry = m_retirements[static_cast<size_t>(token.value - 1u)];
    return entry.token == token ? &entry : nullptr;
}

const EcsSceneRenderProofGateway::RetirementEntry*
EcsSceneRenderProofGateway::Resolve(
    ResourceSceneAdapters::EcsSceneAssetRetirementToken token) const noexcept
{
    if (!token.IsValid() || token.value == 0 ||
        token.value > static_cast<uint64>(m_retirements.size()))
    {
        return nullptr;
    }
    const RetirementEntry& entry =
        m_retirements[static_cast<size_t>(token.value - 1u)];
    return entry.token == token ? &entry : nullptr;
}

ResourceSceneAdapters::EcsSceneAssetRetirementProof
EcsSceneRenderProofGateway::QueryRetirementProof(
    ResourceSceneAdapters::EcsSceneAssetRetirementToken token) const
{
    using namespace ResourceSceneAdapters;
    const RetirementEntry* entry = Resolve(token);
    if (entry == nullptr)
    {
        return {.state = EcsSceneAssetRetirementProofState::Failed,
                .diagnostic = "ECS Render retirement token is stale or foreign."};
    }
    return {.state = entry->state,
            .request = entry->request,
            .appliedRenderSceneRevision = entry->state ==
                                                  EcsSceneAssetRetirementProofState::NeverPublished
                                              ? 0u : entry->appliedRenderSceneRevision,
            .presentedFrameSequence = entry->state ==
                                               EcsSceneAssetRetirementProofState::NeverPublished
                                           ? 0u : entry->requiredPresentedFrameSequence,
            .diagnostic = entry->diagnostic};
}

bool EcsSceneRenderProofGateway::AcknowledgeRetirementProof(
    ResourceSceneAdapters::EcsSceneAssetRetirementToken token)
{
    RetirementEntry* entry = Resolve(token);
    if (entry == nullptr ||
        (entry->state != ResourceSceneAdapters::EcsSceneAssetRetirementProofState::Presented &&
         entry->state != ResourceSceneAdapters::EcsSceneAssetRetirementProofState::NeverPublished))
    {
        return false;
    }
    entry->token = {};
    if (entry->nextGeneration == std::numeric_limits<uint64>::max())
    {
        entry->nextGeneration = 0;
    }
    else
    {
        ++entry->nextGeneration;
    }
    entry->request = {};
    entry->correlation = {};
    entry->state = ResourceSceneAdapters::EcsSceneAssetRetirementProofState::Pending;
    entry->appliedRenderSceneRevision = 0;
    entry->requiredPresentedFrameSequence = 0;
    entry->awaitingExtractorBarrier = false;
    entry->barrierMembers.clear();
    entry->awaitingUnpresentedRemovals.clear();
    entry->diagnostic.clear();
    return true;
}

bool EcsSceneRenderProofGateway::HasOutstandingProofs() const noexcept
{
    if (m_pendingCandidate.has_value() || m_extractor.HasPendingCandidate())
    {
        return true;
    }
    return std::any_of(
        m_retirements.begin(), m_retirements.end(),
        [](const RetirementEntry& entry) { return entry.token.IsValid(); });
}
} // namespace RVX
