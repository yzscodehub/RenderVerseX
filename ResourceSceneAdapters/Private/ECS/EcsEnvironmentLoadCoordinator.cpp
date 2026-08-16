/** @file EcsEnvironmentLoadCoordinator.cpp @brief Pure ECS Environment lifecycle. */

#include "ResourceSceneAdapters/ECS/EcsEnvironmentLoadCoordinator.h"

#include "Core/Assert.h"
#include "ECS/Query.h"
#include "Scene/ECS/RetirementFragments.h"

#include <algorithm>
#include <array>
#include <utility>

namespace RVX::ResourceSceneAdapters
{
    namespace
    {
        [[nodiscard]] uint64 GetStructuralRevision(const ECS::Registry& registry)
        {
            return registry.GetStructuralJournal().GetNextSequence();
        }

        [[nodiscard]] EcsEnvironmentAdoptionReceipt MakeRejectedAdoptionReceipt(
            const ECS::Registry& registry,
            ECS::SceneRuntimeId sceneRuntimeId,
            EcsEnvironmentAdoptionError error)
        {
            return {
                .error = error,
                .sceneRuntimeId = sceneRuntimeId,
                .sceneStructuralRevisionBefore = GetStructuralRevision(registry),
                .sceneStructuralRevisionAfter = GetStructuralRevision(registry),
            };
        }

        [[nodiscard]] bool HasCompleteEnvironmentAssetSet(
            const EcsEnvironmentAdoptionBatch& batch) noexcept
        {
            return batch.sourceEnvironmentAssetId.IsValid() &&
                   batch.skybox.environmentAssetId.IsValid() &&
                   batch.skybox.irradianceAssetId.IsValid() &&
                   batch.skybox.prefilteredEnvironmentAssetId.IsValid() &&
                   batch.skybox.brdfLutAssetId.IsValid();
        }

        [[nodiscard]] bool MatchesExactly(const EcsSceneAssetRetirementRequest& expected,
                                          const EcsSceneAssetRetirementRequest& actual)
        {
            return expected.sceneRuntimeId == actual.sceneRuntimeId &&
                   expected.rootEntity == actual.rootEntity &&
                   expected.members == actual.members;
        }

        [[nodiscard]] bool IsReleasable(EcsEnvironmentLoadState state) noexcept
        {
            return state == EcsEnvironmentLoadState::Cancelled ||
                   state == EcsEnvironmentLoadState::Recycled;
        }
    } // namespace

    EcsEnvironmentAdoptionReceipt AdoptEcsEnvironmentBatch(
        SceneECS::SceneEcsRuntime& runtime,
        const EcsEnvironmentAdoptionBatch& batch,
        ECS::SceneRuntimeId expectedSceneRuntimeId,
        const EcsEnvironmentAdoptionOptions& options)
    {
        ECS::Registry& registry = runtime.GetMutableRegistryForEcsEnvironmentAdoption();
        const ECS::SceneRuntimeId sceneRuntimeId = runtime.GetSceneRuntimeId();
        if (!expectedSceneRuntimeId.IsValid())
        {
            return MakeRejectedAdoptionReceipt(
                registry, sceneRuntimeId,
                EcsEnvironmentAdoptionError::InvalidExpectedSceneRuntime);
        }
        if (expectedSceneRuntimeId != sceneRuntimeId)
        {
            return MakeRejectedAdoptionReceipt(
                registry, sceneRuntimeId, EcsEnvironmentAdoptionError::SceneRuntimeMismatch);
        }
        if (!batch.sourceEnvironmentAssetId.IsValid())
        {
            return MakeRejectedAdoptionReceipt(
                registry, sceneRuntimeId,
                EcsEnvironmentAdoptionError::InvalidEnvironmentAssetPrerequisite);
        }
        if (batch.skybox.mode != SceneECS::SkyboxMode::Cubemap ||
            !HasCompleteEnvironmentAssetSet(batch))
        {
            return MakeRejectedAdoptionReceipt(
                registry, sceneRuntimeId,
                EcsEnvironmentAdoptionError::InvalidSkyboxAssetPrerequisite);
        }
        bool existingLiveSkybox = false;
        registry.Query<ECS::Read<SceneECS::Skybox>, ECS::Read<SceneECS::EntityLifecycleState>>()
            .Each([&existingLiveSkybox](ECS::EntityHandle,
                                        const SceneECS::Skybox&,
                                        const SceneECS::EntityLifecycleState& lifecycle)
            {
                existingLiveSkybox |= lifecycle.phase == SceneECS::EntityLifecyclePhase::Alive;
            });
        if (existingLiveSkybox)
        {
            return MakeRejectedAdoptionReceipt(
                registry, sceneRuntimeId, EcsEnvironmentAdoptionError::ExistingLiveSkybox);
        }

        EcsEnvironmentAdoptionReceipt result;
        result.sceneRuntimeId = sceneRuntimeId;
        result.sceneStructuralRevisionBefore = GetStructuralRevision(registry);

        ECS::EntityTransaction transaction = registry.BeginTransaction();
        ECS::EntityReceipt entity = transaction.Create();
        bool recorded = entity.IsQueued();
        recorded = recorded &&
                   transaction.Add<SceneECS::LocalTransform>(entity).IsQueued();
        recorded = recorded &&
                   transaction.Add<SceneECS::SimulationWorldTransform>(entity).IsQueued();
        recorded = recorded &&
                   transaction.Add<SceneECS::PreviousSimulationWorldTransform>(entity).IsQueued();
        recorded = recorded &&
                   transaction.Add<SceneECS::RenderWorldTransform>(entity).IsQueued();
        recorded = recorded && transaction.Add<SceneECS::Bounds>(entity).IsQueued();
        recorded = recorded && transaction.Add<SceneECS::Active>(entity).IsQueued();
        recorded = recorded && transaction.Add<SceneECS::Layer>(entity).IsQueued();
        recorded = recorded &&
                   transaction.Add<SceneECS::EntityLifecycleState>(entity).IsQueued();
        recorded = recorded && transaction.Add<SceneECS::Skybox>(entity, batch.skybox).IsQueued();
        recorded = recorded && transaction.Add<EnvironmentInstanceFragment>(
                                 entity,
                                 {
                                     .sourceEnvironmentAssetId = batch.sourceEnvironmentAssetId,
                                     .environmentAssetId = batch.skybox.environmentAssetId,
                                     .irradianceAssetId = batch.skybox.irradianceAssetId,
                                     .prefilteredEnvironmentAssetId =
                                         batch.skybox.prefilteredEnvironmentAssetId,
                                     .brdfLutAssetId = batch.skybox.brdfLutAssetId,
                                 }).IsQueued();
        recorded = recorded &&
                   transaction.Add<ECS::Tag<SceneECS::SpecializedRenderRetirement>>(entity).IsQueued();
        if (recorded && options.fault == EcsEnvironmentAdoptionFault::RejectAfterRecording)
        {
            recorded = transaction.Add<SceneECS::Skybox>(entity, batch.skybox).IsQueued();
        }
        if (!recorded)
        {
            return MakeRejectedAdoptionReceipt(
                registry, sceneRuntimeId, EcsEnvironmentAdoptionError::RecordingRejected);
        }

        const ECS::TransactionReceipt transactionResult = transaction.Commit();
        result.transactionStatus = transactionResult.GetStatus();
        result.transactionError = transactionResult.GetError();
        result.sceneStructuralRevisionAfter = GetStructuralRevision(registry);
        if (!transactionResult.IsApplied() || !entity.IsResolved())
        {
            result.error = EcsEnvironmentAdoptionError::TransactionRejected;
            result.sceneStructuralRevisionAfter = result.sceneStructuralRevisionBefore;
            return result;
        }
        result.skyboxEntity = entity.GetEntity();
        return result;
    }

    struct EcsEnvironmentLoadCoordinator::Entry
    {
        EcsEnvironmentLoadHandle handle = InvalidEcsEnvironmentLoadHandle;
        EcsEnvironmentLoadStatus status;
        Resource::ResourceLoadHandle<Resource::EnvironmentResource> request;
        Resource::ResourceHandle<Resource::EnvironmentResource> environment;
        Resource::AssetResidencyLease residencyLease;
        EcsEnvironmentAdoptionBatch batch;
        EcsSceneAssetRetirementRequest retirementRequest;
        EcsSceneAssetRetirementBeginReceipt retirementAdmission;
        bool adopted = false;
    };

    EcsEnvironmentLoadCoordinator::EcsEnvironmentLoadCoordinator(
        SceneECS::SceneEcsRuntime& runtime,
        Resource::ResourceSubsystem& resources,
        IEcsSceneAssetRetirementProofGateway* retirementProofGateway) noexcept
        : m_runtime(runtime)
        , m_resources(resources)
        , m_retirementProofGateway(retirementProofGateway)
        , m_ownerThread(std::this_thread::get_id())
    {
    }

    EcsEnvironmentLoadCoordinator::~EcsEnvironmentLoadCoordinator() noexcept
    {
        const bool hasLiveEntries = std::any_of(
            m_entries.begin(), m_entries.end(),
            [](const std::unique_ptr<Entry>& entry) { return entry != nullptr; });
        const bool clean = !hasLiveEntries || PrepareForHostShutdown();
        const bool drained = std::none_of(
            m_entries.begin(), m_entries.end(),
            [](const std::unique_ptr<Entry>& entry) { return entry != nullptr; });
        RVX_ASSERT_MSG(clean && drained,
                       "ECS environment coordinator destruction requires every exact lease and "
                       "Skybox retirement to be drained first.");
    }

    bool EcsEnvironmentLoadCoordinator::IsOwnerThread() const noexcept
    {
        return m_ownerThread == std::this_thread::get_id();
    }

    EcsEnvironmentLoadHandle EcsEnvironmentLoadCoordinator::Allocate(std::unique_ptr<Entry> entry)
    {
        if (entry == nullptr)
        {
            return InvalidEcsEnvironmentLoadHandle;
        }
        const EcsEnvironmentLoadHandle handle = m_handles.Allocate();
        try
        {
            if (handle.GetIndex() >= m_entries.size())
            {
                m_entries.resize(static_cast<size_t>(handle.GetIndex()) + 1u);
            }
            entry->handle = handle;
            m_entries[handle.GetIndex()] = std::move(entry);
            return handle;
        }
        catch (...)
        {
            static_cast<void>(m_handles.TryFree(handle));
            return InvalidEcsEnvironmentLoadHandle;
        }
    }

    EcsEnvironmentLoadCoordinator::Entry* EcsEnvironmentLoadCoordinator::Resolve(
        EcsEnvironmentLoadHandle handle)
    {
        return m_handles.IsValid(handle) && handle.GetIndex() < m_entries.size() ?
                   m_entries[handle.GetIndex()].get() :
                   nullptr;
    }

    const EcsEnvironmentLoadCoordinator::Entry* EcsEnvironmentLoadCoordinator::Resolve(
        EcsEnvironmentLoadHandle handle) const
    {
        return m_handles.IsValid(handle) && handle.GetIndex() < m_entries.size() ?
                   m_entries[handle.GetIndex()].get() :
                   nullptr;
    }

    EcsEnvironmentLoadHandle EcsEnvironmentLoadCoordinator::RequestEnvironment(
        EcsEnvironmentLoadDesc desc, std::string& outError)
    {
        outError.clear();
        if (!IsOwnerThread() || m_hostShutdown || desc.path.empty())
        {
            outError = !IsOwnerThread() ?
                           "ECS environment requests require the coordinator owner thread." :
                           "ECS environment request was rejected during host shutdown or for an empty path.";
            return InvalidEcsEnvironmentLoadHandle;
        }

        const ECS::SceneRuntimeId sceneRuntimeId = m_runtime.GetSceneRuntimeId();
        if (!desc.expectedSceneRuntimeId.IsValid() ||
            desc.expectedSceneRuntimeId != sceneRuntimeId)
        {
            outError = !desc.expectedSceneRuntimeId.IsValid() ?
                           "ECS environment requests require an exact Scene runtime id." :
                           "ECS environment request specified a foreign Scene runtime id.";
            return InvalidEcsEnvironmentLoadHandle;
        }

        uint64 canonicalImportOptionsHash = 0;
        Resource::ResourceLoadError preparationError;
        Resource::ResourceLoadPreparationStateRef preparationState =
            Resource::EnvironmentLoader::CreatePreparationState(
                desc.environmentOptions,
                canonicalImportOptionsHash,
                preparationError);
        if (preparationState == nullptr)
        {
            outError = preparationError.message.empty() ?
                           "ECS environment request could not capture immutable bake options." :
                           preparationError.message;
            return InvalidEcsEnvironmentLoadHandle;
        }
        if (desc.resourceOptions.importOptionsHash != 0 &&
            desc.resourceOptions.importOptionsHash != canonicalImportOptionsHash)
        {
            outError =
                "Environment ResourceLoadOptions hash conflicts with its immutable options.";
            return InvalidEcsEnvironmentLoadHandle;
        }
        desc.resourceOptions.importOptionsHash = canonicalImportOptionsHash;
        Resource::ResourceLoadHandle<Resource::EnvironmentResource> request =
            m_resources.RequestAsync<Resource::EnvironmentResource>(
                desc.path,
                std::move(desc.resourceOptions),
                std::move(preparationState));
        if (!request.IsValid())
        {
            outError = "ResourceSubsystem rejected the prepared Environment request.";
            return InvalidEcsEnvironmentLoadHandle;
        }

        auto entry = std::make_unique<Entry>();
        entry->request = std::move(request);
        entry->status.request = entry->request.GetSnapshot();
        entry->status.assetKey = entry->status.request.assetKey;
        entry->status.environmentOptions = desc.environmentOptions;
        entry->status.canonicalImportOptionsHash = canonicalImportOptionsHash;
        entry->status.sceneRuntimeId = sceneRuntimeId;
        const EcsEnvironmentLoadHandle handle = Allocate(std::move(entry));
        if (!handle.IsValid())
        {
            outError = "ECS environment coordinator could not allocate a generation-safe handle.";
        }
        return handle;
    }

    void EcsEnvironmentLoadCoordinator::Fail(Entry& entry,
                                              EcsEnvironmentLoadState state,
                                              std::string diagnostic)
    {
        entry.status.state = state;
        entry.status.diagnostic = std::move(diagnostic);
    }

    bool EcsEnvironmentLoadCoordinator::CheckGpuReadiness(Entry& entry)
    {
        bool pending = false;
        const std::array<AssetId, 4> textures = {
            entry.batch.skybox.environmentAssetId,
            entry.batch.skybox.irradianceAssetId,
            entry.batch.skybox.prefilteredEnvironmentAssetId,
            entry.batch.skybox.brdfLutAssetId,
        };
        for (AssetId assetId : textures)
        {
            const Resource::RenderResourceResolveResult resolved =
                m_resources.ResolveRenderResource(assetId, RenderResourceKind::Texture);
            if (resolved.code == Resource::RenderResourceResolveCode::NotFound)
            {
                pending = true;
                continue;
            }
            if (resolved.code != Resource::RenderResourceResolveCode::Resolved ||
                resolved.status.code == RenderResourceStatusCode::StaleGeneration ||
                resolved.status.code == RenderResourceStatusCode::InvalidHandle ||
                resolved.status.state == RenderResourcePublicState::Failed ||
                resolved.status.state == RenderResourcePublicState::Released ||
                resolved.status.state == RenderResourcePublicState::Evicting)
            {
                Fail(entry, EcsEnvironmentLoadState::Failed,
                     "An exact Environment texture failed before ECS Skybox adoption.");
                return false;
            }
            if (resolved.status.state != RenderResourcePublicState::GPUReady)
            {
                pending = true;
            }
        }
        return !pending;
    }

    bool EcsEnvironmentLoadCoordinator::Adopt(Entry& entry)
    {
        entry.residencyLease = m_resources.GetManager().AcquireAssetResidencyLease(
            entry.status.assetKey);
        if (!entry.residencyLease.IsValid())
        {
            Fail(entry, EcsEnvironmentLoadState::Failed,
                 "The exact published Environment AssetKey could not acquire a residency lease.");
            return false;
        }
        const EcsEnvironmentAdoptionReceipt adopted = AdoptEcsEnvironmentBatch(
            m_runtime, entry.batch, entry.status.sceneRuntimeId);
        if (!adopted.IsApplied())
        {
            entry.residencyLease.Reset();
            Fail(entry, EcsEnvironmentLoadState::Failed,
                 "Atomic Environment Skybox adoption rejected the expected ECS runtime.");
            return false;
        }
        entry.adopted = true;
        entry.status.sceneRuntimeId = adopted.sceneRuntimeId;
        entry.status.skyboxEntity = adopted.skyboxEntity;
        entry.status.adoptedStructuralRevision = adopted.sceneStructuralRevisionAfter;
        // Structural creation has no entity-local fragment-write version. Emit
        // one exact value-preserving source write after the all-or-nothing
        // transaction so the immutable frozen snapshot can prove this precise
        // Skybox instance to Render and presentation.
        if (!m_runtime.SetFragment<SceneECS::Skybox>(
                adopted.skyboxEntity, entry.batch.skybox))
        {
            Fail(entry, EcsEnvironmentLoadState::FailedRetained,
                 "Atomic Environment adoption could not publish its exact Skybox source write.");
            static_cast<void>(BeginRetirement(entry));
            return false;
        }
        const uint64 skyboxWriteVersion =
            m_runtime.GetRegistry().GetFragmentWriteVersion<SceneECS::Skybox>(
                adopted.skyboxEntity);
        if (skyboxWriteVersion == 0)
        {
            Fail(entry, EcsEnvironmentLoadState::FailedRetained,
                 "Environment Skybox activation did not produce an entity-local write version.");
            static_cast<void>(BeginRetirement(entry));
            return false;
        }
        entry.status.skyboxWriteVersion = skyboxWriteVersion;
        entry.status.state = EcsEnvironmentLoadState::PendingPresentation;
        return true;
    }

    bool EcsEnvironmentLoadCoordinator::ConfirmPresentation(
        EcsEnvironmentLoadHandle handle,
        const EcsEnvironmentPresentationReceipt& receipt)
    {
        if (!IsOwnerThread())
        {
            return false;
        }
        Entry* entry = Resolve(handle);
        if (entry == nullptr || entry->status.state != EcsEnvironmentLoadState::PendingPresentation ||
            !receipt.IsValid() || receipt.sceneRuntimeId != entry->status.sceneRuntimeId ||
            receipt.skyboxEntity != entry->status.skyboxEntity ||
            receipt.skyboxWriteVersion != entry->status.skyboxWriteVersion)
        {
            return false;
        }
        entry->status.presentedFrozenSourceSnapshotRevision =
            receipt.frozenSourceSnapshotRevision;
        entry->status.presentedRenderSceneRevision = receipt.renderSceneRevision;
        entry->status.presentedFrameSequence = receipt.carryingPresentedFrameSequence;
        entry->status.state = EcsEnvironmentLoadState::FullyResident;
        return true;
    }

    bool EcsEnvironmentLoadCoordinator::BeginRetirement(Entry& entry)
    {
        if (!entry.adopted)
        {
            return false;
        }
        if (entry.status.state == EcsEnvironmentLoadState::Retiring ||
            entry.status.state == EcsEnvironmentLoadState::AwaitingRenderProof ||
            entry.status.state == EcsEnvironmentLoadState::AwaitingResourceClosure ||
            entry.status.state == EcsEnvironmentLoadState::AwaitingEcsRecycle)
        {
            return true;
        }
        entry.retirementRequest = {
            .sceneRuntimeId = entry.status.sceneRuntimeId,
            .rootEntity = entry.status.skyboxEntity,
            .members = {entry.status.skyboxEntity},
        };
        if (m_retirementProofGateway == nullptr)
        {
            Fail(entry, EcsEnvironmentLoadState::FailedRetained,
                 "No value-only Render retirement proof gateway is installed.");
            return false;
        }
        entry.retirementAdmission = m_retirementProofGateway->BeginRetirement(
            entry.retirementRequest);
        if (!entry.retirementAdmission.IsAccepted() ||
            !MatchesExactly(entry.retirementRequest, entry.retirementAdmission.request))
        {
            Fail(entry,
                 entry.retirementAdmission.code == EcsSceneAssetRetirementBeginCode::DeviceLost ?
                     EcsEnvironmentLoadState::DeviceLost :
                     EcsEnvironmentLoadState::FailedRetained,
                 entry.retirementAdmission.diagnostic.empty() ?
                     "Render retirement admission did not capture the exact ECS Skybox identity." :
                     entry.retirementAdmission.diagnostic);
            return false;
        }
        entry.status.state = EcsEnvironmentLoadState::Retiring;
        const SceneECS::CleanupDomainMask domains =
            SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Render) |
            SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Resources);
        if (m_runtime.RequestDestroyBatch(entry.retirementRequest.members, domains) !=
            SceneECS::DestroyRequestResult::Accepted)
        {
            Fail(entry, EcsEnvironmentLoadState::FailedRetained,
                 "ECS Environment retirement could not atomically request its dedicated Skybox.");
            return false;
        }
        static_cast<void>(m_runtime.PublishPendingDestroyCleanup());
        entry.status.state = EcsEnvironmentLoadState::AwaitingRenderProof;
        return true;
    }

    bool EcsEnvironmentLoadCoordinator::AdvanceEcsRecycle(Entry& entry)
    {
        if (entry.status.state != EcsEnvironmentLoadState::AwaitingEcsRecycle)
        {
            return false;
        }
        static_cast<void>(m_runtime.AdvanceRetirements());
        static_cast<void>(m_runtime.RecycleRecyclableEntities());
        if (m_runtime.GetRegistry().IsAlive(entry.status.skyboxEntity))
        {
            return true;
        }
        entry.status.state = EcsEnvironmentLoadState::Recycled;
        return true;
    }

    bool EcsEnvironmentLoadCoordinator::AdvanceRetirement(Entry& entry)
    {
        if (entry.status.state == EcsEnvironmentLoadState::AwaitingRenderProof)
        {
            const EcsSceneAssetRetirementProof proof = m_retirementProofGateway->QueryRetirementProof(
                entry.retirementAdmission.token);
            if (!MatchesExactly(entry.retirementRequest, proof.request))
            {
                Fail(entry, EcsEnvironmentLoadState::FailedRetained,
                     "Render retirement evidence did not match the exact ECS Skybox identity.");
                return false;
            }
            if (proof.state == EcsSceneAssetRetirementProofState::DeviceLost)
            {
                Fail(entry, EcsEnvironmentLoadState::DeviceLost,
                     proof.diagnostic.empty() ? "Render device lost before ECS Environment retirement proof." :
                                                proof.diagnostic);
                return false;
            }
            if (proof.state == EcsSceneAssetRetirementProofState::Failed)
            {
                Fail(entry, EcsEnvironmentLoadState::FailedRetained,
                     proof.diagnostic.empty() ? "Render retirement proof failed." : proof.diagnostic);
                return false;
            }
            const bool neverPublished = proof.state == EcsSceneAssetRetirementProofState::NeverPublished;
            if (proof.state != EcsSceneAssetRetirementProofState::Presented && !neverPublished)
            {
                return true;
            }
            if ((!neverPublished &&
                 (proof.appliedRenderSceneRevision == 0 || proof.presentedFrameSequence == 0)) ||
                (neverPublished &&
                 (proof.appliedRenderSceneRevision != 0 || proof.presentedFrameSequence != 0)))
            {
                Fail(entry, EcsEnvironmentLoadState::FailedRetained,
                     "Render retirement evidence carried an invalid presentation revision/frame pair.");
                return false;
            }
            if (!m_runtime.AcknowledgeCleanup(
                    entry.status.skyboxEntity,
                    SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Render)))
            {
                Fail(entry, EcsEnvironmentLoadState::FailedRetained,
                     "Render proof arrived but ECS Render cleanup acknowledgement failed.");
                return false;
            }
            if (!m_retirementProofGateway->AcknowledgeRetirementProof(
                    entry.retirementAdmission.token))
            {
                Fail(entry, EcsEnvironmentLoadState::FailedRetained,
                     "Terminal Render retirement evidence could not be acknowledged.");
                return false;
            }
            const Resource::ResourceSceneClosureReleaseBeginResult release =
                m_resources.BeginSceneAssetClosureRelease(std::move(entry.residencyLease));
            entry.status.closureRelease = release.receipt;
            if (!release.IsAccepted())
            {
                Fail(entry, EcsEnvironmentLoadState::FailedRetained,
                     "Resource closure release retained the exact Environment residency lease.");
                return false;
            }
            entry.status.state = EcsEnvironmentLoadState::AwaitingResourceClosure;
        }
        if (entry.status.state == EcsEnvironmentLoadState::AwaitingEcsRecycle)
        {
            return AdvanceEcsRecycle(entry);
        }
        if (entry.status.state != EcsEnvironmentLoadState::AwaitingResourceClosure)
        {
            return true;
        }
        const std::optional<Resource::ResourceSceneClosureReleaseReceipt> receipt =
            m_resources.QuerySceneAssetClosureRelease(entry.status.closureRelease.token);
        if (!receipt.has_value())
        {
            Fail(entry, EcsEnvironmentLoadState::FailedRetained,
                 "The exact Resource closure receipt became unavailable.");
            return false;
        }
        entry.status.closureRelease = *receipt;
        if (!receipt->IsTerminal())
        {
            return true;
        }
        if (receipt->state == Resource::ResourceSceneClosureReleaseState::FailedRetained)
        {
            Fail(entry, EcsEnvironmentLoadState::FailedRetained,
                 "Resource closure reported retained failure.");
            return false;
        }
        if (receipt->state == Resource::ResourceSceneClosureReleaseState::DeviceLost)
        {
            Fail(entry, EcsEnvironmentLoadState::DeviceLost,
                 "Resource closure reported Render device loss.");
            return false;
        }
        if (!m_runtime.AcknowledgeCleanup(
                entry.status.skyboxEntity,
                SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Resources)))
        {
            Fail(entry, EcsEnvironmentLoadState::FailedRetained,
                 "Resource closure completed but ECS Resources cleanup acknowledgement failed.");
            return false;
        }
        const Resource::ResourceSceneClosureReleaseAcknowledgeResult acknowledged =
            m_resources.AcknowledgeSceneAssetClosureRelease(receipt->token);
        if (acknowledged.code != Resource::ResourceSceneClosureReleaseAcknowledgeCode::Acknowledged)
        {
            Fail(entry, EcsEnvironmentLoadState::FailedRetained,
                 "Terminal Resource closure could not be acknowledged after ECS cleanup acceptance.");
            return false;
        }
        entry.status.state = EcsEnvironmentLoadState::AwaitingEcsRecycle;
        return AdvanceEcsRecycle(entry);
    }

    bool EcsEnvironmentLoadCoordinator::UpdateEntry(Entry& entry)
    {
        entry.status.request = entry.request.GetSnapshot();
        if (entry.status.state == EcsEnvironmentLoadState::Requested ||
            entry.status.state == EcsEnvironmentLoadState::Preparing ||
            entry.status.state == EcsEnvironmentLoadState::PreparedCPU)
        {
            switch (entry.status.request.state)
            {
                case Resource::ResourceLoadState::Queued:
                    entry.status.state = EcsEnvironmentLoadState::Requested;
                    return true;
                case Resource::ResourceLoadState::Loading:
                    entry.status.state = EcsEnvironmentLoadState::Preparing;
                    return true;
                case Resource::ResourceLoadState::AwaitingPublish:
                    entry.status.state = EcsEnvironmentLoadState::PreparedCPU;
                    return true;
                case Resource::ResourceLoadState::Ready:
                {
                    entry.environment = entry.request.TryGet();
                    if (!entry.environment || !entry.environment->GetData().IsValid())
                    {
                        Fail(entry, EcsEnvironmentLoadState::Failed,
                             "The Ready Environment request did not expose a complete EnvironmentResource.");
                        return false;
                    }
                    const std::optional<Resource::AssetKey> publishedKey =
                        m_resources.GetManager().FindPublishedAssetKey(entry.environment.GetId());
                    if (!publishedKey.has_value() || *publishedKey != entry.status.request.assetKey)
                    {
                        Fail(entry, EcsEnvironmentLoadState::Failed,
                             "The published Environment AssetKey differs from the exact request identity.");
                        return false;
                    }
                    const Resource::EnvironmentResourceData& data = entry.environment->GetData();
                    entry.status.assetKey = *publishedKey;
                    entry.status.contentVerificationReceipt =
                        entry.environment->GetContentVerificationReceipt();
                    entry.status.environmentResolution = data.environmentResolution;
                    entry.status.irradianceResolution = data.irradianceResolution;
                    entry.status.prefilteredResolution = data.prefilteredResolution;
                    entry.status.prefilteredMipLevels = data.prefilteredMipLevels;
                    entry.status.brdfLUTResolution = data.brdfLUTResolution;
                    entry.status.exposure = data.intensity;
                    entry.batch = {
                        .sourceEnvironmentAssetId = AssetId{entry.environment.GetId()},
                        .skybox = {
                            .mode = SceneECS::SkyboxMode::Cubemap,
                            .environmentAssetId = AssetId{data.environment.GetId()},
                            .prefilteredEnvironmentAssetId = AssetId{data.prefiltered.GetId()},
                            .irradianceAssetId = AssetId{data.irradiance.GetId()},
                            .brdfLutAssetId = AssetId{data.brdfLUT.GetId()},
                            .exposure = data.intensity,
                            .contributesToLighting = true,
                        },
                    };
                    if (!HasCompleteEnvironmentAssetSet(entry.batch))
                    {
                        Fail(entry, EcsEnvironmentLoadState::Failed,
                             "The published Environment did not provide four exact texture AssetId values.");
                        return false;
                    }
                    entry.status.state = EcsEnvironmentLoadState::PendingGpuReadiness;
                    return true;
                }
                case Resource::ResourceLoadState::Failed:
                    Fail(entry, EcsEnvironmentLoadState::Failed, entry.status.request.error.message);
                    return false;
                case Resource::ResourceLoadState::Cancelled:
                    Fail(entry, EcsEnvironmentLoadState::Cancelled,
                         "Prepared Environment request was cancelled before ECS adoption.");
                    return true;
            }
        }
        if (entry.status.state == EcsEnvironmentLoadState::PendingGpuReadiness)
        {
            if (CheckGpuReadiness(entry))
            {
                entry.status.state = EcsEnvironmentLoadState::PendingAdopt;
            }
            return entry.status.state != EcsEnvironmentLoadState::Failed;
        }
        if (entry.status.state == EcsEnvironmentLoadState::PendingAdopt)
        {
            return Adopt(entry);
        }
        if (entry.status.state == EcsEnvironmentLoadState::AwaitingRenderProof ||
            entry.status.state == EcsEnvironmentLoadState::AwaitingResourceClosure ||
            entry.status.state == EcsEnvironmentLoadState::AwaitingEcsRecycle)
        {
            return AdvanceRetirement(entry);
        }
        return true;
    }

    void EcsEnvironmentLoadCoordinator::ReleaseEntry(EcsEnvironmentLoadHandle handle) noexcept
    {
        if (!m_handles.IsValid(handle) || handle.GetIndex() >= m_entries.size())
        {
            return;
        }
        m_entries[handle.GetIndex()].reset();
        static_cast<void>(m_handles.TryFree(handle));
    }

    bool EcsEnvironmentLoadCoordinator::Update()
    {
        if (!IsOwnerThread())
        {
            return false;
        }
        bool succeeded = true;
        for (size_t index = 0; index < m_entries.size(); ++index)
        {
            Entry* entry = m_entries[index].get();
            if (entry == nullptr)
            {
                continue;
            }
            const EcsEnvironmentLoadHandle handle = entry->handle;
            succeeded = UpdateEntry(*entry) && succeeded;
            entry = Resolve(handle);
            if (entry != nullptr && IsReleasable(entry->status.state))
            {
                ReleaseEntry(handle);
            }
        }
        return succeeded;
    }

    bool EcsEnvironmentLoadCoordinator::Cancel(EcsEnvironmentLoadHandle handle)
    {
        if (!IsOwnerThread())
        {
            return false;
        }
        Entry* entry = Resolve(handle);
        if (entry == nullptr)
        {
            return false;
        }
        if (entry->status.state == EcsEnvironmentLoadState::Cancelled ||
            entry->status.state == EcsEnvironmentLoadState::Retiring ||
            entry->status.state == EcsEnvironmentLoadState::AwaitingRenderProof ||
            entry->status.state == EcsEnvironmentLoadState::AwaitingResourceClosure ||
            entry->status.state == EcsEnvironmentLoadState::AwaitingEcsRecycle ||
            entry->status.state == EcsEnvironmentLoadState::Recycled)
        {
            return true;
        }
        if (!entry->adopted)
        {
            static_cast<void>(entry->request.Cancel());
            entry->environment = {};
            entry->batch = {};
            entry->status.state = EcsEnvironmentLoadState::Cancelled;
            entry->status.diagnostic = "Cancelled before atomic ECS Skybox adoption; no ECS mutation occurred.";
            return true;
        }
        return BeginRetirement(*entry);
    }

    bool EcsEnvironmentLoadCoordinator::PrepareForHostShutdown() noexcept
    {
        if (!IsOwnerThread())
        {
            return false;
        }
        m_hostShutdown = true;
        bool clean = true;
        for (const std::unique_ptr<Entry>& entry : m_entries)
        {
            if (entry != nullptr && !entry->status.IsTerminal())
            {
                clean = Cancel(entry->handle) && clean;
            }
        }
        for (const std::unique_ptr<Entry>& entry : m_entries)
        {
            if (entry != nullptr &&
                (entry->status.state == EcsEnvironmentLoadState::AwaitingRenderProof ||
                 entry->status.state == EcsEnvironmentLoadState::AwaitingResourceClosure ||
                 entry->status.state == EcsEnvironmentLoadState::AwaitingEcsRecycle))
            {
                clean = AdvanceRetirement(*entry) && clean;
            }
            if (entry != nullptr && !entry->status.IsTerminal())
            {
                clean = false;
            }
        }
        for (size_t index = 0; index < m_entries.size(); ++index)
        {
            if (m_entries[index] != nullptr && IsReleasable(m_entries[index]->status.state))
            {
                ReleaseEntry(m_entries[index]->handle);
            }
        }
        return std::all_of(
            m_entries.begin(), m_entries.end(),
            [](const std::unique_ptr<Entry>& entry) { return entry == nullptr; }) && clean;
    }

    bool EcsEnvironmentLoadCoordinator::IsValid(EcsEnvironmentLoadHandle handle) const
    {
        return IsOwnerThread() && Resolve(handle) != nullptr;
    }

    std::optional<EcsEnvironmentLoadStatus> EcsEnvironmentLoadCoordinator::GetStatus(
        EcsEnvironmentLoadHandle handle) const
    {
        if (!IsOwnerThread())
        {
            return std::nullopt;
        }
        const Entry* entry = Resolve(handle);
        return entry != nullptr ? std::optional<EcsEnvironmentLoadStatus>(entry->status) :
                                  std::nullopt;
    }

    Resource::ResourceLoadRequestId EcsEnvironmentLoadCoordinator::GetResourceRequestId(
        EcsEnvironmentLoadHandle handle) const
    {
        if (!IsOwnerThread())
        {
            return {};
        }
        const Entry* entry = Resolve(handle);
        return entry != nullptr ? entry->status.request.requestId :
                                  Resource::ResourceLoadRequestId{};
    }
} // namespace RVX::ResourceSceneAdapters
