/** @file AssetStreamingSample.cpp @brief Async asset-lifetime probe. */

#include "Scenes/AssetStreamingSample.h"

#include "Core/MathTypes.h"
#include "Resource/ResourceDiagnosticsView.h"
#include "Resource/ResourcePublicationView.h"
#include "Scene/ECS/RenderFragments.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleContext.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

namespace RVX
{
    namespace
    {
        constexpr float32 StreamingDisplayExtent = 2.60f;
        constexpr float32 StreamingDisplaySpacing = 3.50f;
        constexpr float32 StreamingVerticalFov = radians(45.0f);
        constexpr float32 PlacementGroundTolerance = 0.025f;
        constexpr float32 PlacementExtentTolerance = 0.035f;

        const SampleInfo AssetStreamingInfo{
            "asset-streaming",
            "Asset Streaming",
            "Deterministically validates asynchronous model residency, cancellation, retirement, and reload",
            "water-bottle",
            SampleAssetPolicy::Fixed,
            "",
            SampleEnvironmentPolicy::None,
            true,
            SampleRenderPath::Auto,
            {"corset", "crytek-sponza"}};

        const std::array<AssessmentAction, 12> StreamingActions = {
            AssessmentAction{
                AssessmentCode("RESOURCE.ACTION.QUEUE_THREE_MODELS_AND_DUPLICATE_SUBSCRIBER"),
                "Water Bottle, Corset, Crytek Sponza, and a second Water Bottle subscriber were queued through catalog ids."},
            AssessmentAction{
                AssessmentCode("RESOURCE.ACTION.PRESENT_FALLBACK_FRAME"),
                "A completed frame presented the Water Bottle minimum-resident fallback before full residency."},
            AssessmentAction{
                AssessmentCode("RESOURCE.ACTION.VERIFY_REQUEST_COALESCING"),
                "Both Water Bottle subscribers resolved to one Resource request identity."},
            AssessmentAction{
                AssessmentCode("RESOURCE.ACTION.CANCEL_DUPLICATE_SUBSCRIBER"),
                "Exactly one coalesced Water Bottle subscriber completed retirement while the other remained live."},
            AssessmentAction{
                AssessmentCode("RESOURCE.ACTION.REQUEST_UNKNOWN_ASSET_AND_ROLLBACK"),
                "A catalog-unknown id was rejected before coordinator or Resource mutation."},
            AssessmentAction{
                AssessmentCode("RESOURCE.ACTION.INSTANTIATE_MINIMUM_RESIDENT_MODELS"),
                "All three verified models reached CPU readiness and their Scene roots were positioned."},
            AssessmentAction{
                AssessmentCode("RESOURCE.ACTION.COMMIT_REAL_TEXTURES"),
                "Water Bottle streamed texture content advanced from fallback without a full Scene rebuild."},
            AssessmentAction{
                AssessmentCode("RESOURCE.ACTION.CANCEL_PUBLISHED_LARGE_MODEL"),
                "The fully resident Crytek Sponza subscriber entered completion-owned retirement."},
            AssessmentAction{
                AssessmentCode("RESOURCE.ACTION.AWAIT_EXACT_RETIREMENT"),
                "Crytek Sponza retirement completed and advanced the exact closure-unload ledger."},
            AssessmentAction{
                AssessmentCode("RESOURCE.ACTION.RELOAD_SAME_ASSET_ID"),
                "Crytek Sponza was requested again by the same catalog id after retirement."},
            AssessmentAction{
                AssessmentCode("RESOURCE.ACTION.PRESENT_RELOADED_REVISION"),
                "The reloaded Crytek Sponza lifecycle and content identity reached a completed presentation."},
            AssessmentAction{
                AssessmentCode("RESOURCE.ACTION.DRAIN_ALL_QUEUES"),
                "All pending Resource decode, publication, upload, rollback, and retirement queues drained."}};

        const AssessmentInvariant CoalescedRequestInvariant{
            AssessmentCode("RESOURCE.LOAD.COALESCED_SUBSCRIBERS"),
            "Equivalent model requests share one Resource request identity while keeping independent subscribers."};
        const AssessmentInvariant AsyncFailureInvariant{
            AssessmentCode("RESOURCE.LOAD.FAILURE_ROLLBACK"),
            "A catalog-unknown model id is rejected with typed NotFound before producing a request, cache entry, Scene instance, or GPU residency."};
        const AssessmentInvariant MinimumResidentInvariant{
            AssessmentCode("RESOURCE.RESIDENCY.FALLBACK_BEFORE_FULL"),
            "Primary model fallback bindings were presented before texture streaming and FullyResident."};
        const AssessmentInvariant TextureCommitInvariant{
            AssessmentCode("RESOURCE.TEXTURE.COMMITTED_CONTENT_ADVANCED"),
            "A real streamed texture committed after its fallback baseline."};
        const AssessmentInvariant RetirementInvariant{
            AssessmentCode("RESOURCE.RETIREMENT.CLOSURE_COMPLETED"),
            "Published Crytek Sponza cancellation completed through the coordinator retirement receipt and requested closure release."};
        const AssessmentInvariant ReloadIdentityInvariant{
            AssessmentCode("RESOURCE.RELOAD.CONTENT_IDENTITY_AND_GENERATION"),
            "Reloaded Crytek Sponza retained verified source content identity while receiving a new lifecycle generation."};
        const AssessmentInvariant IncrementalTextureInvariant{
            AssessmentCode("RESOURCE.TEXTURE.INCREMENTAL_PUBLICATION"),
            "Fallback-to-real texture publication advanced committed content without a full Scene rebuild."};
        const AssessmentInvariant StableQueuesInvariant{
            AssessmentCode("RESOURCE.RETIREMENT.STABLE_QUEUES"),
            "At stability every asynchronous, decode, publication, replacement, retirement, and queued-lease-unload queue is empty."};
        const AssessmentInvariant FirstFrameInvariant{
            AssessmentCode("SAMPLE.STARTUP.SETUP_NONBLOCKING"),
            "Setup only configured the default scene and queued asynchronous work."};

        const AssessmentMetric DecodeCompletedMetric{
            AssessmentCode("RESOURCE.METRIC.DECODE_COMPLETED"),
            "count", "Completed model texture decode count."};
        const AssessmentMetric DecodeBudgetMetric{
            AssessmentCode("RESOURCE.METRIC.DECODE_PEAK_BYTES"),
            "bytes", "Peak reserved decoded bytes observed by the resource service."};
        const AssessmentMetric ClosureRequestMetric{
            AssessmentCode("RESOURCE.METRIC.CLOSURE_UNLOAD_REQUESTS"),
            "count", "Exact asset-closure unload requests observed."};
        const AssessmentMetric PendingRequestMetric{
            AssessmentCode("RESOURCE.METRIC.PENDING_REQUESTS"),
            "count", "Pending asynchronous Resource operation count."};

        const AssessmentCapability ResourceDiagnosticsCapability{
            AssessmentCode("RESOURCE.CAPABILITY.DIAGNOSTICS"),
            "The update thread exposes exact resource streaming and retirement diagnostics.",
            true,
            "Asset-streaming cannot establish lifetime completion without Resource diagnostics."};
        const AssessmentCapability TexturePublicationCapability{
            AssessmentCode("RESOURCE.CAPABILITY.TEXTURE_PUBLICATION"),
            "The update thread exposes exact texture committed-content revisions.",
            true,
            "Asset-streaming cannot verify fallback-to-real texture replacement without publication status."};

        const char* GetPublicationCodeName(
            Resource::ResourcePublicationQueryCode code) noexcept
        {
            using Code = Resource::ResourcePublicationQueryCode;
            switch (code)
            {
                case Code::Resolved: return "Resolved";
                case Code::InvalidResourceId: return "InvalidResourceId";
                case Code::UnsupportedResourceType: return "UnsupportedResourceType";
                case Code::NotInitialized: return "NotInitialized";
                case Code::WrongThread: return "WrongThread";
                case Code::GatewayUnavailable: return "GatewayUnavailable";
                case Code::NotFound: return "NotFound";
                case Code::KindMismatch: return "KindMismatch";
                case Code::StaleGeneration: return "StaleGeneration";
                default: return "Invalid";
            }
        }

        [[nodiscard]] SceneECS::Skybox MakeProceduralSky()
        {
            return {
                .mode = SceneECS::SkyboxMode::Procedural,
                .sunDirection = normalize(Vec3(0.32f, 0.68f, 0.42f)),
                .sunColor = Vec3(1.0f, 0.95f, 0.86f),
                .zenithColor = Vec3(0.10f, 0.23f, 0.50f),
                .horizonColor = Vec3(0.55f, 0.66f, 0.79f),
                .groundColor = Vec3(0.07f, 0.08f, 0.10f),
                .scatteringIntensity = 0.62f,
                .contributesToLighting = false,
            };
        }

        [[nodiscard]] SceneECS::Light MakeDirectionalLight()
        {
            return {
                .type = SceneECS::LightType::Directional,
                .color = Vec3(1.0f, 0.95f, 0.86f),
                .intensity = 3.5f,
                .shadowBias = 0.001f,
                .castsShadows = true,
            };
        }

        [[nodiscard]] bool HasAuthorizedTextureStreaming(
            const ResourceSceneAdapters::EcsSceneAssetLoadStatus& status) noexcept
        {
            const std::optional<
                ResourceSceneAdapters::EcsModelTextureStreamingStartReceipt>& receipt =
                status.textureStreamingStartReceipt;
            return receipt.has_value() && receipt->IsAuthorized() &&
                   receipt->authorizedFrozenSourceSnapshotRevision ==
                       status.minimumResidentFrozenSourceSnapshotRevision &&
                   receipt->authorizedRenderSceneRevision ==
                       status.minimumResidentRenderSceneRevision &&
                   receipt->authorizedPresentedFrameSequence ==
                       status.minimumResidentPresentedFrameSequence &&
                   receipt->textureAssetIds ==
                       status.modelMetadata.streamingTextureAssetIds;
        }

        [[nodiscard]] bool IsEcsModelFailure(
            const ResourceSceneAdapters::EcsSceneAssetLoadStatus& status) noexcept
        {
            using State = ResourceSceneAdapters::EcsSceneAssetLoadState;
            return status.state == State::Failed ||
                   status.state == State::FailedRetained ||
                   status.state == State::DeviceLost;
        }
    } // namespace

    const SampleInfo& AssetStreamingSample::GetInfo() const noexcept
    {
        return AssetStreamingInfo;
    }

    SampleAssessmentContract AssetStreamingSample::GetAssessmentContract() const
    {
        SampleAssessmentContract contract;
        contract.code = AssessmentCode("SAMPLE.ASSET_STREAMING");
        contract.revision = "2";
        contract.checkpoints = {
            AssessmentCheckpoints::EngineBaseline,
            AssessmentCheckpoints::ScenarioSetup,
            AssessmentCheckpoints::ActionRequested,
            AssessmentCheckpoints::ActionApplied,
            AssessmentCheckpoints::ScenarioStable,
            AssessmentCheckpoints::TeardownBefore,
            AssessmentCheckpoints::TeardownSceneComplete,
            AssessmentCheckpoints::TeardownRenderDrained,
            AssessmentCheckpoints::EngineShutdownComplete};
        contract.actions.assign(StreamingActions.begin(), StreamingActions.end());
        contract.invariants = {
            CoalescedRequestInvariant,
            AsyncFailureInvariant,
            MinimumResidentInvariant,
            TextureCommitInvariant,
            IncrementalTextureInvariant,
            RetirementInvariant,
            ReloadIdentityInvariant,
            StableQueuesInvariant,
            FirstFrameInvariant};
        contract.metrics = {
            DecodeCompletedMetric,
            DecodeBudgetMetric,
            ClosureRequestMetric,
            PendingRequestMetric};
        contract.capabilities = {
            ResourceDiagnosticsCapability,
            TexturePublicationCapability};
        return contract;
    }

    bool AssetStreamingSample::Setup(SampleContext& context,
                                     std::string& outError)
    {
        m_renderPath = context.options.renderPath;
        switch (m_renderPath)
        {
            case SampleRenderPath::Auto:
                context.renderSettings.gpuCulling.mode = RenderGPUDrivenMode::Auto;
                break;
            case SampleRenderPath::Direct:
                context.renderSettings.gpuCulling.mode =
                    RenderGPUDrivenMode::ForceDisabled;
                break;
            case SampleRenderPath::GPUDriven:
                context.renderSettings.gpuCulling.mode =
                    RenderGPUDrivenMode::ForceEnabled;
                break;
            default:
                outError = "Asset-streaming received an invalid render-path policy";
                return false;
        }

        const float32 aspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        if (!context.cameras.SetPerspective(
                context.camera, radians(45.0f), aspect, 0.05f, 500.0f) ||
            !context.cameras.SetPose(
                context.camera, {.position = Vec3(0.0f, 2.5f, 8.0f)}) ||
            !context.cameras.LookAt(context.camera, Vec3(0.0f, 0.65f, 0.0f)))
        {
            outError = "Asset-streaming could not configure its ECS camera";
            return false;
        }
        context.renderSettings.shadows.enabled = true;
        context.renderSettings.shadows.atlasResolution = 1024u;
        context.renderSettings.shadows.cascadeCount = 2u;
        context.renderSettings.postProcess.enabled = true;
        context.renderSettings.postProcess.enableTAA = false;
        context.renderSettings.postProcess.enableBloom = false;
        context.renderSettings.postProcess.enableSSAO = false;
        context.renderSettings.postProcess.enableSSR = false;

        if (!ConfigureDefaultScene(context, outError) ||
            !QueueRequests(context, outError))
        {
            return false;
        }

        const Resource::ResourceDiagnosticsQueryResult diagnostics =
            context.resourceDiagnostics.QueryResourceDiagnostics();
        if (diagnostics.IsAvailable())
        {
            m_initialClosureUnloadRequests =
                diagnostics.snapshot.GetValue()->closureUnloadRequestCount;
            static_cast<void>(context.assessment.TryPublish(
                AssessmentCapabilityObservation{
                    ResourceDiagnosticsCapability,
                    AssessmentCheckpoints::ScenarioSetup,
                    DiagnosticValue<bool>::Available(true),
                    "Resource diagnostics are available on the update thread."}));
            m_resourceDiagnosticsCapabilityObserved = true;
        }
        else
        {
            static_cast<void>(context.assessment.TryPublish(
                AssessmentCapabilityObservation{
                    ResourceDiagnosticsCapability,
                    AssessmentCheckpoints::ScenarioSetup,
                    DiagnosticValue<bool>::Available(false),
                    diagnostics.snapshot.GetReason()}));
            m_resourceDiagnosticsCapabilityObserved = true;
            ReportUnavailable(context,
                              ResourceDiagnosticsCapability.code,
                              diagnostics.snapshot.GetReason());
            return false;
        }

        RecordScenarioAction(
            context,
            ScenarioAction::QueueThreeModelsAndDuplicateSubscriber);
        static_cast<void>(context.assessment.TryPublish(
            AssessmentInvariantObservation{
            FirstFrameInvariant, AssessmentCheckpoints::ScenarioSetup}));
        static_cast<void>(context.assessment.MarkCheckpoint(
            AssessmentCheckpoints::ScenarioSetup));
        static_cast<void>(context.assessment.MarkCheckpoint(
            AssessmentCheckpoints::ActionRequested));
        return true;
    }

    bool AssetStreamingSample::ConfigureDefaultScene(
        SampleContext& context,
        std::string& outError)
    {
        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                {}, MakeProceduralSky()).IsValid())
        {
            outError = "Asset-streaming could not create the procedural sky";
            return false;
        }
        m_skyboxCreated = true;

        SceneECS::RuntimeEntityDesc lightDesc;
        lightDesc.localTransform.rotation =
            QuatFromEuler(Vec3(radians(-48.0f), radians(26.0f), 0.0f));
        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                lightDesc,
                MakeDirectionalLight(),
                SceneECS::Visibility{}).IsValid())
        {
            outError = "Asset-streaming could not create its directional light";
            return false;
        }
        m_lightCreated = true;
        return true;
    }

    bool AssetStreamingSample::QueueRequests(SampleContext& context,
                                              std::string& outError)
    {
        constexpr std::array<std::string_view, 3> RequiredAssetIds = {
            "water-bottle", "corset", "crytek-sponza"};
        if (context.options.modelAssetIds.size() != RequiredAssetIds.size() ||
            !std::equal(context.options.modelAssetIds.begin(),
                        context.options.modelAssetIds.end(),
                        RequiredAssetIds.begin(),
                        RequiredAssetIds.end()))
        {
            outError =
                "Asset-streaming requires the fixed Water Bottle, Corset, and Crytek Sponza catalog-id set";
            return false;
        }

        if (!context.models.RequestByAssetId("water-bottle", m_primary, outError) ||
            !context.models.RequestByAssetId("water-bottle",
                                             m_duplicateSubscriber,
                                             outError) ||
            !context.models.RequestByAssetId("corset", m_corset, outError) ||
            !context.models.RequestByAssetId("crytek-sponza",
                                             m_largeModel,
                                             outError))
        {
            return false;
        }
        return true;
    }

    void AssetStreamingSample::Update(SampleContext& context, float deltaTime)
    {
        static_cast<void>(deltaTime);
        ++m_updateCount;
        if (m_state == ScenarioState::Failed ||
            m_state == ScenarioState::Stable)
        {
            return;
        }

        if (!UpdateModelReadiness(context))
            return;

        bool advanced = false;
        switch (m_state)
        {
            case ScenarioState::QueueThreeModelsAndDuplicateSubscriber:
                // Setup queues this state atomically; the first update only
                // advances after a completed frame covers that request set.
                if (IsActionPresented(
                        ScenarioAction::QueueThreeModelsAndDuplicateSubscriber))
                {
                    m_state = ScenarioState::PresentFallbackFrame;
                }
                break;
            case ScenarioState::PresentFallbackFrame:
                advanced = PresentFallbackFrame(context);
                if (advanced)
                    m_state = ScenarioState::VerifyRequestCoalescing;
                break;
            case ScenarioState::VerifyRequestCoalescing:
                advanced = VerifyRequestCoalescing(context);
                if (advanced)
                    m_state = ScenarioState::CancelDuplicateSubscriber;
                break;
            case ScenarioState::CancelDuplicateSubscriber:
                advanced = CancelDuplicateSubscriber(context);
                if (advanced)
                    m_state = ScenarioState::RequestUnknownAssetAndRollback;
                break;
            case ScenarioState::RequestUnknownAssetAndRollback:
                advanced = RequestUnknownAssetAndRollback(context);
                if (advanced)
                    m_state = ScenarioState::InstantiateMinimumResidentModels;
                break;
            case ScenarioState::InstantiateMinimumResidentModels:
                advanced = InstantiateMinimumResidentModels(context);
                if (advanced)
                    m_state = ScenarioState::CommitRealTextures;
                break;
            case ScenarioState::CommitRealTextures:
                advanced = CommitRealTextures(context);
                if (advanced)
                    m_state = ScenarioState::CancelPublishedLargeModel;
                break;
            case ScenarioState::CancelPublishedLargeModel:
                advanced = CancelPublishedLargeModel(context);
                if (advanced)
                    m_state = ScenarioState::AwaitExactRetirement;
                break;
            case ScenarioState::AwaitExactRetirement:
                advanced = AwaitExactRetirement(context);
                if (advanced)
                    m_state = ScenarioState::ReloadSameAssetId;
                break;
            case ScenarioState::ReloadSameAssetId:
                advanced = ReloadSameAssetId(context);
                if (advanced)
                    m_state = ScenarioState::PresentReloadedRevision;
                break;
            case ScenarioState::PresentReloadedRevision:
                advanced = PresentReloadedRevision(context);
                if (advanced)
                    m_state = ScenarioState::DrainAllQueues;
                break;
            case ScenarioState::DrainAllQueues:
                advanced = DrainAllQueues(context);
                if (advanced)
                {
                    m_state = ScenarioState::Stable;
                    static_cast<void>(context.assessment.MarkCheckpoint(
                        AssessmentCheckpoints::ScenarioStable));
                }
                break;
            case ScenarioState::Stable:
            case ScenarioState::Failed:
            default:
                break;
        }
    }

    bool AssetStreamingSample::UpdateModelReadiness(SampleContext& context)
    {
        const auto updateLive = [&context, this](LoadedSampleModel& model,
                                                 std::string_view label,
                                                 AssessmentCode invariant) -> bool
        {
            if (!model.request.IsValid())
                return true;
            const ResourceSceneAdapters::EcsSceneAssetLoadStatus status =
                context.models.UpdateReadiness(model);
            using State = ResourceSceneAdapters::EcsSceneAssetLoadState;
            if (IsEcsModelFailure(status) || status.state == State::Cancelled ||
                status.state == State::Recycled)
            {
                Fail(context,
                     invariant,
                     std::string(label) + " failed: " +
                         status.diagnostic);
                return false;
            }
            return true;
        };

        const bool valid =
            updateLive(m_primary,
                       "Water Bottle primary request",
                       MinimumResidentInvariant.code) &&
            (!m_duplicateCancellationRequested ||
             updateLive(m_duplicateSubscriber,
                        "Water Bottle duplicate subscriber",
                        CoalescedRequestInvariant.code)) &&
            updateLive(m_corset,
                       "Corset request",
                       CoalescedRequestInvariant.code) &&
            updateLive(m_largeModel,
                       "Crytek Sponza request",
                       RetirementInvariant.code) &&
            updateLive(m_reloadedLargeModel,
                       "Reloaded Crytek Sponza request",
                       ReloadIdentityInvariant.code);
        if (!valid)
            return false;

        return !m_primary.IsCPUReady() || ObservePrimaryTexturePublication(context);
    }

    bool AssetStreamingSample::PresentFallbackFrame(SampleContext& context)
    {
        const ResourceSceneAdapters::EcsSceneAssetLoadStatus& primary =
            m_primary.status;
        if (m_primary.IsFullyResident() && !m_primaryMinimumBeforeFullyResident)
        {
            Fail(context,
                 MinimumResidentInvariant.code,
                 "Water Bottle reached full residency without a completed minimum-resident fallback interval");
            return false;
        }
        if (!m_primaryMinimumBeforeFullyResident &&
            m_primary.IsCPUReady() && !m_primary.IsFullyResident() &&
            primary.minimumResidentPresentedFrameSequence != 0)
        {
            m_primaryMinimumBeforeFullyResident = true;
            m_primaryFallbackPresentationSequence =
                primary.minimumResidentPresentedFrameSequence;
            m_primaryFallbackRenderSceneRevision =
                primary.minimumResidentRenderSceneRevision;
            if (!ObservePrimaryTexturePublication(context))
                return false;
            RecordScenarioAction(context, ScenarioAction::PresentFallbackFrame);
            MarkInvariant(context,
                          MinimumResidentInvariant,
                          AssessmentCheckpoints::ActionApplied);
        }

        return m_primaryMinimumBeforeFullyResident &&
               IsActionPresented(
                   ScenarioAction::QueueThreeModelsAndDuplicateSubscriber) &&
               IsActionPresented(ScenarioAction::PresentFallbackFrame);
    }

    bool AssetStreamingSample::VerifyRequestCoalescing(SampleContext& context)
    {
        if (!m_primary.resourceRequestId.IsValid() ||
            !m_duplicateSubscriber.resourceRequestId.IsValid())
        {
            return false;
        }
        if (m_primary.resourceRequestId != m_duplicateSubscriber.resourceRequestId)
        {
            Fail(context,
                 CoalescedRequestInvariant.code,
                 "Water Bottle subscribers did not resolve to one Resource request identity");
            return false;
        }
        if (!m_coalescedRequestObserved)
        {
            m_coalescedRequestObserved = true;
            RecordScenarioAction(context, ScenarioAction::VerifyRequestCoalescing);
            MarkInvariant(context,
                          CoalescedRequestInvariant,
                          AssessmentCheckpoints::ActionApplied);
        }
        return IsActionPresented(ScenarioAction::VerifyRequestCoalescing);
    }

    bool AssetStreamingSample::CancelDuplicateSubscriber(SampleContext& context)
    {
        if (!m_duplicateCancellationRequested)
        {
            if (!context.models.CancelForRetirement(
                    m_duplicateSubscriber, m_duplicateCancellation))
            {
                Fail(context,
                     CoalescedRequestInvariant.code,
                     "The duplicate Water Bottle subscriber could not enter retirement");
                return false;
            }
            m_duplicateCancellationRequested = true;
            RecordScenarioAction(context, ScenarioAction::CancelDuplicateSubscriber);
        }

        if (!m_duplicateCancelled)
        {
            if (context.models.IsRetirementComplete(m_duplicateCancellation))
            {
                m_duplicateCancelled = true;
            }
            else if (const std::optional<
                         ResourceSceneAdapters::EcsSceneAssetLoadStatus> cancellation =
                         context.models.QueryRetirementStatus(
                             m_duplicateCancellation);
                     cancellation && IsEcsModelFailure(*cancellation))
            {
                Fail(context,
                     CoalescedRequestInvariant.code,
                     "Duplicate Water Bottle subscriber retirement failed: " +
                         cancellation->diagnostic);
                return false;
            }
        }
        if (!m_primary.request.IsValid() || m_primary.status.IsTerminal())
        {
            Fail(context,
                 CoalescedRequestInvariant.code,
                 "Cancelling one coalesced subscriber cancelled the remaining Water Bottle consumer");
            return false;
        }
        return m_duplicateCancelled &&
               IsActionPresented(ScenarioAction::CancelDuplicateSubscriber);
    }

    bool AssetStreamingSample::RequestUnknownAssetAndRollback(
        SampleContext& context)
    {
        if (m_unknownRequestRejected)
        {
            return m_unknownRollbackClean &&
                   IsActionPresented(
                       ScenarioAction::RequestUnknownAssetAndRollback);
        }

        LoadedSampleModel unknown;
        SampleAssetRegistryLookupResult lookup;
        std::string error;
        const bool accepted = context.models.RequestByAssetId(
            "rvx-intentional-unknown-asset", unknown, error, &lookup);

        if (accepted || lookup.code != SampleAssetRegistryLookupCode::UnknownAssetId ||
            unknown.request.IsValid() || unknown.resourceRequestId.IsValid() ||
            unknown.status.rootEntity.IsValid() || error.find("unknown model id") ==
                                       std::string::npos)
        {
            Fail(context,
                 AsyncFailureInvariant.code,
                 "Unknown catalog id was not rejected as a structured NotFound request");
            return false;
        }
        m_unknownRequestRejected = true;
        // RequestByAssetId rejects an unknown id at SampleAssetRegistry lookup
        // before it invokes the coordinator. A global Resource snapshot is not
        // a valid comparison boundary here because legitimate asynchronous
        // work from the three accepted assets can progress concurrently.
        m_unknownRollbackClean = true;
        RecordScenarioAction(context,
                             ScenarioAction::RequestUnknownAssetAndRollback);
        MarkInvariant(context,
                      AsyncFailureInvariant,
                      AssessmentCheckpoints::ActionApplied);
        return IsActionPresented(
            ScenarioAction::RequestUnknownAssetAndRollback);
    }

    bool AssetStreamingSample::InstantiateMinimumResidentModels(
        SampleContext& context)
    {
        if (!AreInitialModelsCPUReady())
            return false;
        if (m_placedModelCount == 0 && !PlaceMinimumResidentModels(context))
            return false;
        if (!m_actions[GetScenarioActionIndex(
                ScenarioAction::InstantiateMinimumResidentModels)].requested)
        {
            RecordScenarioAction(
                context, ScenarioAction::InstantiateMinimumResidentModels);
        }
        return IsActionPresented(
            ScenarioAction::InstantiateMinimumResidentModels);
    }

    bool AssetStreamingSample::CommitRealTextures(SampleContext& context)
    {
        if (!ObservePrimaryTexturePublication(context))
            return false;
        if (!m_primary.IsFullyResident())
            return false;
        if (!m_primaryMinimumBeforeFullyResident ||
            !HasAuthorizedTextureStreaming(m_primary.status))
        {
            Fail(context,
                 MinimumResidentInvariant.code,
                 "Water Bottle texture streaming lacks proof that fallback was presented before real texture decoding");
            return false;
        }
        if (!m_primaryTextureFallbackBaselineFrozen)
        {
            return false;
        }
        if (!m_primaryTextureCommitted || m_primaryTextureCommitRevision == 0)
        {
            return false;
        }
        if (!m_textureSceneBaselineCaptured || !m_textureSceneCommitCaptured)
            return false;
        // The cumulative Scene rebuild counter is global and may legitimately
        // advance while Corset/Sponza population changes overlap Water Bottle
        // streaming.  Each exact ready descriptor receipt is instead accepted
        // only from its own completion-qualified frame with zero rebuilt or
        // removed Scene objects (see ObservePrimaryTextureReceipts).
        m_noFullSceneRebuildForTextureCommit =
            std::all_of(m_primaryTextureEvidence.begin(),
                        m_primaryTextureEvidence.end(),
                        [](const auto& pair)
                        {
                            return pair.second.HasReadyReceipt();
                        });
        if (!m_noFullSceneRebuildForTextureCommit)
        {
            Fail(context,
                 IncrementalTextureInvariant.code,
                 "Water Bottle fallback-to-real texture publication triggered a full Scene rebuild");
            return false;
        }
        if (!m_actions[GetScenarioActionIndex(
                ScenarioAction::CommitRealTextures)].requested)
        {
            RecordScenarioAction(context, ScenarioAction::CommitRealTextures);
            MarkInvariant(context,
                          TextureCommitInvariant,
                          AssessmentCheckpoints::ActionApplied);
            MarkInvariant(context,
                          IncrementalTextureInvariant,
                          AssessmentCheckpoints::ActionApplied);
        }
        return IsActionPresented(ScenarioAction::CommitRealTextures);
    }

    bool AssetStreamingSample::CancelPublishedLargeModel(SampleContext& context)
    {
        if (!m_largeCancellation.requested)
        {
            if (!AreInitialModelsFullyResident())
                return false;
            const Resource::ResourceContentVerificationReceipt& contentReceipt =
                m_largeModel.status.modelMetadata.contentVerificationReceipt;
            if (!contentReceipt.IsVerified())
            {
                Fail(context,
                     ReloadIdentityInvariant.code,
                     "Crytek Sponza reached full residency without a verified source-content identity receipt");
                return false;
            }
            m_largeContentIdentity = contentReceipt;
            m_largeLifecycleHandle = m_largeModel.request;
            m_largeResourceRequestId = m_largeModel.resourceRequestId;
            if (!m_largeLifecycleHandle.IsValid() ||
                !m_largeResourceRequestId.IsValid())
            {
                Fail(context,
                     RetirementInvariant.code,
                     "Published Crytek Sponza cancellation did not retain a valid lifecycle and Resource request identity");
                return false;
            }
            if (!context.models.CancelForRetirement(m_largeModel,
                                                    m_largeCancellation))
            {
                Fail(context,
                     RetirementInvariant.code,
                     "Published Crytek Sponza cancellation was rejected");
                return false;
            }
            if (m_largeCancellation.request !=
                    m_largeLifecycleHandle ||
                !m_largeCancellation.resourceRequestId.IsValid() ||
                m_largeCancellation.resourceRequestId !=
                    m_largeResourceRequestId)
            {
                Fail(context,
                     RetirementInvariant.code,
                     "Crytek Sponza cancellation receipt did not retain the exact old lifecycle and Resource request identities");
                return false;
            }
            m_expectedClosureUnloadRequests =
                m_initialClosureUnloadRequests + 1;
            RecordScenarioAction(context,
                                 ScenarioAction::CancelPublishedLargeModel);
        }
        return IsActionPresented(ScenarioAction::CancelPublishedLargeModel);
    }

    bool AssetStreamingSample::AwaitExactRetirement(SampleContext& context)
    {
        if (!context.models.IsRetirementComplete(m_largeCancellation))
        {
            const std::optional<ResourceSceneAdapters::EcsSceneAssetLoadStatus> retirement =
                context.models.QueryRetirementStatus(m_largeCancellation);
            if (retirement && IsEcsModelFailure(*retirement))
            {
                Fail(context,
                     RetirementInvariant.code,
                     "Crytek Sponza retirement failed while retaining ownership: " +
                         retirement->diagnostic);
            }
            return false;
        }
        m_largeCancellationCompleted = true;
        m_exactOldSponzaRetirementSatisfied =
            IsExactOldSponzaRetirementSatisfied(
                m_largeCancellation.IsValid(),
                m_largeLifecycleHandle.IsValid(),
                m_largeResourceRequestId.IsValid(),
                m_largeCancellation.resourceRequestId.IsValid(),
                m_largeCancellation.request ==
                    m_largeLifecycleHandle,
                m_largeCancellation.resourceRequestId ==
                    m_largeResourceRequestId,
                m_largeCancellationCompleted);
        if (!m_exactOldSponzaRetirementSatisfied)
        {
            Fail(context,
                 RetirementInvariant.code,
                 "Crytek Sponza retirement completed without an exact old lifecycle receipt");
            return false;
        }
        if (!UpdateDiagnostics(context, true, true))
            return false;
        if (!m_actions[GetScenarioActionIndex(
                ScenarioAction::AwaitExactRetirement)].requested)
        {
            RecordScenarioAction(context, ScenarioAction::AwaitExactRetirement);
            MarkInvariant(context,
                          RetirementInvariant,
                          AssessmentCheckpoints::ActionApplied);
        }
        return IsActionPresented(ScenarioAction::AwaitExactRetirement);
    }

    bool AssetStreamingSample::ReloadSameAssetId(SampleContext& context)
    {
        if (!m_reloadedLargeModel.request.IsValid())
        {
            if (!m_exactOldSponzaRetirementSatisfied)
            {
                Fail(context,
                     ReloadIdentityInvariant.code,
                     "Crytek Sponza reload was attempted before the exact old lifecycle retirement completed");
                return false;
            }
            std::string error;
            if (!context.models.RequestByAssetId("crytek-sponza",
                                                 m_reloadedLargeModel,
                                                 error))
            {
                Fail(context,
                     ReloadIdentityInvariant.code,
                     "Retired Crytek Sponza could not be requested again by catalog id: " + error);
                return false;
            }
            m_reloadedLargeLifecycleHandle = m_reloadedLargeModel.request;
            if (m_reloadedLargeLifecycleHandle.sceneRuntimeId !=
                    m_largeLifecycleHandle.sceneRuntimeId ||
                m_reloadedLargeLifecycleHandle.handle.GetIndex() !=
                    m_largeLifecycleHandle.handle.GetIndex() ||
                m_reloadedLargeLifecycleHandle.handle.GetGeneration() <=
                    m_largeLifecycleHandle.handle.GetGeneration())
            {
                Fail(context,
                     ReloadIdentityInvariant.code,
                     "Crytek Sponza reload did not reuse the retired lifecycle slot with a newer generation");
                return false;
            }
            RecordScenarioAction(context, ScenarioAction::ReloadSameAssetId);
        }
        return IsActionPresented(ScenarioAction::ReloadSameAssetId);
    }

    bool AssetStreamingSample::PresentReloadedRevision(SampleContext& context)
    {
        if (!m_reloadedLargeModel.IsFullyResident())
            return false;
        const Resource::ResourceContentVerificationReceipt& reloadedContentReceipt =
            m_reloadedLargeModel.status.modelMetadata.contentVerificationReceipt;
        if (!m_largeContentIdentity.has_value() ||
            !reloadedContentReceipt.IsVerified() ||
            reloadedContentReceipt.observed !=
                m_largeContentIdentity->observed)
        {
            Fail(context,
                 ReloadIdentityInvariant.code,
                 "Reloaded Crytek Sponza did not retain the verified original source-content identity");
            return false;
        }
        if (!m_reloadedPresentationPlaced &&
            !PlaceReloadedLargeModel(context))
        {
            return false;
        }
        m_reloadedFullyResident = true;
        m_reloadedIdentityMatches = true;
        m_reloadedLifecycleGenerationAdvanced = true;
        const SceneECS::SceneEntityRef reloadedRoot =
            m_reloadedLargeModel.GetRootEntityRef();
        m_reloadedInstanceValid = m_reloadedLargeModel.request.IsValid() &&
                                  m_reloadedLargeModel.status.rootEntity.IsValid();
        m_reloadedRootValid = m_reloadedPresentationPlaced &&
                              reloadedRoot.sceneRuntimeId ==
                                  context.scene.GetSceneRuntimeId() &&
                              context.scene.GetEntityRef(reloadedRoot.entity).IsValid();
        // Authoring happens before the next frozen ECS snapshot. The render
        // gate must therefore require that next immutable revision rather
        // than a legacy mutable-scene revision.
        m_reloadedSceneRevision =
            context.scene.GetDiagnosticsSnapshot().sceneSnapshotRevision + 1;
        if (!IsReloadInstanceReady(m_reloadedLargeModel.IsFullyResident(),
                                   m_reloadedInstanceValid,
                                   m_reloadedRootValid,
                                   m_reloadedSceneRevision))
        {
            Fail(context,
                 ReloadIdentityInvariant.code,
                 "Reloaded Crytek Sponza reached full residency without a live ECS request, root entity, and Scene revision");
            return false;
        }
        if (!m_actions[GetScenarioActionIndex(
                ScenarioAction::PresentReloadedRevision)].requested)
        {
            RecordScenarioAction(context,
                                 ScenarioAction::PresentReloadedRevision);
            MarkInvariant(context,
                          ReloadIdentityInvariant,
                          AssessmentCheckpoints::ActionApplied);
        }
        return IsActionPresented(ScenarioAction::PresentReloadedRevision);
    }

    bool AssetStreamingSample::DrainAllQueues(SampleContext& context)
    {
        if (!UpdateDiagnostics(context, true, true))
            return false;
        if (!m_actions[GetScenarioActionIndex(
                ScenarioAction::DrainAllQueues)].requested)
        {
            RecordScenarioAction(context, ScenarioAction::DrainAllQueues);
        }
        return IsActionPresented(ScenarioAction::DrainAllQueues);
    }

    bool AssetStreamingSample::UpdateDiagnostics(SampleContext& context,
                                                  bool requireClosureProgress,
                                                  bool requireStableQueues)
    {
        const Resource::ResourceDiagnosticsQueryResult result =
            context.resourceDiagnostics.QueryResourceDiagnostics();
        if (!result.IsAvailable())
        {
            ReportUnavailable(context,
                              ResourceDiagnosticsCapability.code,
                              result.snapshot.GetReason());
            return false;
        }

        const Resource::ResourceDiagnosticsSnapshot& diagnostics =
            *result.snapshot.GetValue();
        m_lastQueueSnapshot = MakeQueueSnapshot(diagnostics);
        static_cast<void>(context.assessment.TryPublish(AssessmentSnapshot{
            AssessmentCheckpoints::ActionApplied,
            {
                {DecodeCompletedMetric,
                 DiagnosticValue<AssessmentScalar>::Available(
                     static_cast<uint64>(diagnostics.decodeCompletedCount))},
                {DecodeBudgetMetric,
                 DiagnosticValue<AssessmentScalar>::Available(
                     static_cast<uint64>(diagnostics.decodePeakReservedBytes))},
                {ClosureRequestMetric,
                 DiagnosticValue<AssessmentScalar>::Available(
                     static_cast<uint64>(diagnostics.closureUnloadRequestCount))},
                {PendingRequestMetric,
                 DiagnosticValue<AssessmentScalar>::Available(
                     static_cast<uint64>(diagnostics.activeOperations))}}}));

        if (diagnostics.decodeCompletedCount == 0 ||
            diagnostics.decodePeakReservedBytes > diagnostics.decodeBudgetBytes)
        {
            Fail(context,
                 TextureCommitInvariant.code,
                 "Resource decode diagnostics did not prove successful in-budget texture streaming");
            return false;
        }

        if (requireClosureProgress)
        {
            m_closureProgressObserved =
                m_expectedClosureUnloadRequests != 0 &&
                diagnostics.closureUnloadRequestCount ==
                    m_expectedClosureUnloadRequests;
            if (m_expectedClosureUnloadRequests != 0 &&
                diagnostics.closureUnloadRequestCount >
                    m_expectedClosureUnloadRequests)
            {
                Fail(context,
                     RetirementInvariant.code,
                     "Crytek Sponza retirement produced more than its single exact closure-unload request");
                return false;
            }
            if (!m_closureProgressObserved)
                return false;
        }

        if (requireStableQueues)
        {
            m_stableQueuesObserved =
                AreTerminalQueuesDrained(m_lastQueueSnapshot);
            if (!m_stableQueuesObserved)
                return false;
            MarkInvariant(context,
                          StableQueuesInvariant,
                          AssessmentCheckpoints::ScenarioStable);
        }
        return true;
    }

    bool AssetStreamingSample::PlaceMinimumResidentModels(
        SampleContext& context)
    {
        if (!AreInitialModelsCPUReady())
            return false;

        const std::array<Vec3, 3> anchors = {
            Vec3(-StreamingDisplaySpacing, 0.0f, 0.0f),
            Vec3(0.0f, 0.0f, 0.0f),
            Vec3(StreamingDisplaySpacing, 0.0f, 0.0f)};
        constexpr std::array<std::string_view, 3> labels = {
            "WaterBottle", "Corset", "CrytekSponza"};
        const std::array<LoadedSampleModel*, 3> models = {
            &m_primary, &m_corset, &m_largeModel};

        AABB presentationBounds;
        presentationBounds.Reset();
        for (size_t index = 0; index < models.size(); ++index)
        {
            AABB placedBounds;
            std::string error;
            if (!PlaceModelForPresentation(context,
                                           *models[index],
                                           anchors[index],
                                           StreamingDisplayExtent,
                                           labels[index],
                                           placedBounds,
                                           error))
            {
                Fail(context,
                     CoalescedRequestInvariant.code,
                     error.empty()
                         ? "A CPU-ready model could not be placed for presentation"
                         : std::move(error));
                return false;
            }
            presentationBounds.Expand(placedBounds);
            if (index == 2u)
                m_initialLargePresentationBounds = placedBounds;
        }
        if (!presentationBounds.IsValid())
        {
            Fail(context,
                 CoalescedRequestInvariant.code,
                 "The minimum-resident model set produced invalid presentation bounds");
            return false;
        }

        std::string cameraError;
        if (!InitializePresentationCamera(context,
                                          presentationBounds,
                                          cameraError))
        {
            Fail(context,
                 CoalescedRequestInvariant.code,
                 cameraError.empty()
                     ? "The minimum-resident model set could not initialize its presentation camera"
                     : std::move(cameraError));
            return false;
        }
        m_presentationBounds = presentationBounds;
        m_placedModelCount = static_cast<uint32>(models.size());
        return true;
    }

    bool AssetStreamingSample::PlaceReloadedLargeModel(
        SampleContext& context)
    {
        AABB reloadedBounds;
        std::string error;
        if (!PlaceModelForPresentation(context,
                                       m_reloadedLargeModel,
                                       Vec3(StreamingDisplaySpacing, 0.0f, 0.0f),
                                       StreamingDisplayExtent,
                                       "ReloadedCrytekSponza",
                                       reloadedBounds,
                                       error))
        {
            Fail(context,
                 ReloadIdentityInvariant.code,
                 error.empty()
                     ? "Reloaded Crytek Sponza could not be placed for presentation"
                     : std::move(error));
            return false;
        }
        if (!ArePresentationBoundsEquivalent(m_initialLargePresentationBounds,
                                             reloadedBounds))
        {
            Fail(context,
                 ReloadIdentityInvariant.code,
                 "Reloaded Crytek Sponza did not reproduce the original bounds-normalized presentation placement");
            return false;
        }

        AABB presentationBounds;
        presentationBounds.Reset();
        const std::array<LoadedSampleModel*, 2> residentModels = {
            &m_primary, &m_corset};
        for (LoadedSampleModel* model : residentModels)
        {
            const SceneECS::SceneEntityRef root = model->GetRootEntityRef();
            if (!root.IsValid() ||
                root.sceneRuntimeId != context.scene.GetSceneRuntimeId() ||
                !context.scene.GetEntityRef(root.entity).IsValid())
            {
                Fail(context,
                     ReloadIdentityInvariant.code,
                     "A retained Asset Streaming model lost its Scene root before the Sponza reload presentation");
                return false;
            }
            AABB bounds;
            if (!TryComputeSampleModelRenderableWorldBounds(
                    *model, context.scene, bounds))
            {
                Fail(context,
                     ReloadIdentityInvariant.code,
                     "A retained Asset Streaming model has invalid bounds before the Sponza reload presentation");
                return false;
            }
            presentationBounds.Expand(bounds);
        }
        presentationBounds.Expand(reloadedBounds);
        if (!presentationBounds.IsValid())
        {
            Fail(context,
                 ReloadIdentityInvariant.code,
                 "Reloaded Asset Streaming presentation bounds are invalid");
            return false;
        }
        if (!ReframePresentationCamera(context, presentationBounds, error))
        {
            Fail(context,
                 ReloadIdentityInvariant.code,
                 error.empty()
                     ? "Reloaded Asset Streaming presentation camera could not be reframed"
                     : std::move(error));
            return false;
        }

        m_reloadedLargePresentationBounds = reloadedBounds;
        m_presentationBounds = presentationBounds;
        m_reloadedPresentationPlaced = true;
        return true;
    }

    bool AssetStreamingSample::PlaceModelForPresentation(
        SampleContext& context,
        LoadedSampleModel& model,
        const Vec3& displayAnchor,
        float32 targetExtent,
        std::string_view label,
        AABB& outBounds,
        std::string& outError)
    {
        outBounds.Reset();
        outError.clear();
        if (!std::isfinite(targetExtent) || targetExtent <= 0.0f)
        {
            outError = "Asset Streaming received an invalid presentation extent";
            return false;
        }

        const SceneECS::SceneEntityRef root = model.GetRootEntityRef();
        if (!root.IsValid() ||
            root.sceneRuntimeId != context.scene.GetSceneRuntimeId() ||
            !context.scene.GetEntityRef(root.entity).IsValid())
        {
            outError = "Asset Streaming model root is stale for " +
                       std::string(label);
            return false;
        }
        AABB sourceBounds;
        if (!TryComputeSampleModelRenderableWorldBounds(
                model, context.scene, sourceBounds))
        {
            outError = "Asset Streaming model has invalid bounds for " +
                       std::string(label);
            return false;
        }
        const Vec3 sourceSize = sourceBounds.GetSize();
        const float32 sourceExtent = std::max(
            {sourceSize.x, sourceSize.y, sourceSize.z});
        if (!sourceBounds.IsValid() || !std::isfinite(sourceExtent) ||
            sourceExtent <= 0.00001f)
        {
            outError = "Asset Streaming model has invalid bounds for " +
                       std::string(label);
            return false;
        }

        // The coordinator must retain a top-level root until its exact
        // retirement ticket is accepted. Parenting that root beneath a
        // sample-lifetime wrapper would make wrapper teardown recursively
        // destroy coordinator-owned Scene identity. Compose the same uniform
        // presentation transform directly while preserving the authored
        // rotation, scale, and translated origin instead.
        const SceneECS::LocalTransform* authored =
            context.scene.GetRegistry().TryGet<SceneECS::LocalTransform>(root.entity);
        if (authored == nullptr)
        {
            outError = "Asset Streaming model root lacks a local transform for " +
                       std::string(label);
            return false;
        }
        const float32 presentationScale = targetExtent / sourceExtent;
        SceneECS::LocalTransform transform = *authored;
        const Vec3 authoredPosition = transform.translation;
        transform.translation = authoredPosition * presentationScale;
        transform.scale *= presentationScale;
        if (!context.scene.SetLocalTransform(root.entity, transform))
        {
            outError = "Asset Streaming could not normalize the ECS model transform for " +
                       std::string(label);
            return false;
        }

        AABB scaledBounds;
        if (!TryComputeSampleModelRenderableWorldBounds(
                model, context.scene, scaledBounds))
        {
            outError = "Asset Streaming could not scale the imported bounds for " +
                       std::string(label);
            return false;
        }
        const Vec3 scaledCenter = scaledBounds.GetCenter();
        transform.translation =
            authoredPosition * presentationScale +
            Vec3(displayAnchor.x - scaledCenter.x,
                 -scaledBounds.GetMin().y,
                 displayAnchor.z - scaledCenter.z);
        if (!context.scene.SetLocalTransform(root.entity, transform))
        {
            outError = "Asset Streaming could not place the ECS model transform for " +
                       std::string(label);
            return false;
        }
        if (!TryComputeSampleModelRenderableWorldBounds(
                model, context.scene, outBounds))
        {
            outError = "Asset Streaming could not resolve the placed bounds for " +
                       std::string(label);
            return false;
        }
        if (!IsNormalizedGroundedPlacement(sourceBounds,
                                           outBounds,
                                           targetExtent))
        {
            outError = "Asset Streaming presentation bounds are not normalized and grounded for " +
                       std::string(label);
            return false;
        }
        return true;
    }

    bool AssetStreamingSample::InitializePresentationCamera(
        SampleContext& context,
        const AABB& bounds,
        std::string& outError)
    {
        outError.clear();
        const float32 aspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        m_cameraFrame = BuildModelCameraFrame(bounds,
                                              aspect,
                                              StreamingVerticalFov,
                                              1.18f);
        if (!m_cameraFrame.valid)
        {
            outError = "Asset Streaming could not frame the normalized model bounds";
            return false;
        }

        SampleOrbitCameraSettings orbitSettings;
        orbitSettings.mode = OrbitCameraMode::FreeOrbit;
        orbitSettings.bounds = bounds;
        orbitSettings.pivot = m_cameraFrame.target;
        orbitSettings.distance = m_cameraFrame.distance;
        orbitSettings.pitch = 0.20f;
        orbitSettings.minDistance =
            std::max(m_cameraFrame.distance * 0.25f, 0.001f);
        orbitSettings.maxDistance =
            std::max(m_cameraFrame.distance * 5.0f,
                     orbitSettings.minDistance * 2.0f);
        orbitSettings.zoomExponent = 0.08f;
        orbitSettings.verticalFovRadians = StreamingVerticalFov;
        orbitSettings.aspectRatio = aspect;
        m_orbitCamera.Initialize(orbitSettings, context.input);
        if (!m_orbitCamera.IsInitialized())
        {
            outError = "Asset Streaming orbit camera initialization failed";
            return false;
        }
        if (!m_orbitCamera.Apply(context.cameras, context.camera))
        {
            outError = "Asset Streaming could not apply its ECS orbit camera pose";
            return false;
        }
        const OrbitCameraRigPose orbitPose = m_orbitCamera.GetPose();
        if (!orbitPose.valid)
        {
            outError = "Asset Streaming orbit camera produced an invalid pose";
            return false;
        }
        m_cameraFrame.target = orbitPose.pivot;
        m_cameraFrame.distance = orbitPose.distance;
        m_cameraFrame.nearPlane = orbitPose.nearPlane;
        m_cameraFrame.farPlane = orbitPose.farPlane;
        return true;
    }

    bool AssetStreamingSample::ReframePresentationCamera(
        SampleContext& context,
        const AABB& bounds,
        std::string& outError)
    {
        outError.clear();
        if (!bounds.IsValid() || !m_orbitCamera.IsInitialized())
        {
            outError = "Asset Streaming cannot reframe an invalid presentation camera state";
            return false;
        }
        const float32 aspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        m_cameraFrame = BuildModelCameraFrame(bounds,
                                              aspect,
                                              StreamingVerticalFov,
                                              1.18f);
        if (!m_cameraFrame.valid)
        {
            outError = "Asset Streaming could not frame the reloaded model bounds";
            return false;
        }
        static_cast<void>(m_orbitCamera.SetFocus(bounds,
                                                  m_cameraFrame.target,
                                                  context.cameras,
                                                  context.camera));
        m_orbitCamera.CaptureResetAnchor();
        const OrbitCameraRigPose orbitPose = m_orbitCamera.GetPose();
        if (!orbitPose.valid)
        {
            outError = "Asset Streaming reload camera produced an invalid pose";
            return false;
        }
        m_cameraFrame.target = orbitPose.pivot;
        m_cameraFrame.distance = orbitPose.distance;
        m_cameraFrame.nearPlane = orbitPose.nearPlane;
        m_cameraFrame.farPlane = orbitPose.farPlane;
        return true;
    }

    bool AssetStreamingSample::IsNormalizedGroundedPlacement(
        const AABB& sourceBounds,
        const AABB& placedBounds,
        float32 targetExtent) noexcept
    {
        if (!sourceBounds.IsValid() || !placedBounds.IsValid() ||
            !std::isfinite(targetExtent) || targetExtent <= 0.0f)
        {
            return false;
        }
        const Vec3 sourceSize = sourceBounds.GetSize();
        const Vec3 placedSize = placedBounds.GetSize();
        const float32 sourceExtent = std::max(
            {sourceSize.x, sourceSize.y, sourceSize.z});
        const float32 placedExtent = std::max(
            {placedSize.x, placedSize.y, placedSize.z});
        return std::isfinite(sourceExtent) && sourceExtent > 0.00001f &&
               std::isfinite(placedExtent) &&
               std::abs(placedExtent - targetExtent) <=
                   targetExtent * PlacementExtentTolerance &&
               std::abs(placedBounds.GetMin().y) <=
                   PlacementGroundTolerance;
    }

    bool AssetStreamingSample::ArePresentationBoundsEquivalent(
        const AABB& expected,
        const AABB& actual) noexcept
    {
        if (!expected.IsValid() || !actual.IsValid())
            return false;
        const Vec3 expectedSize = expected.GetSize();
        const float32 scale = std::max(
            {std::abs(expectedSize.x),
             std::abs(expectedSize.y),
             std::abs(expectedSize.z),
             1.0f});
        const float32 tolerance = scale * 0.0025f;
        const Vec3 expectedMin = expected.GetMin();
        const Vec3 expectedMax = expected.GetMax();
        const Vec3 actualMin = actual.GetMin();
        const Vec3 actualMax = actual.GetMax();
        return std::abs(expectedMin.x - actualMin.x) <= tolerance &&
               std::abs(expectedMin.y - actualMin.y) <= tolerance &&
               std::abs(expectedMin.z - actualMin.z) <= tolerance &&
               std::abs(expectedMax.x - actualMax.x) <= tolerance &&
               std::abs(expectedMax.y - actualMax.y) <= tolerance &&
               std::abs(expectedMax.z - actualMax.z) <= tolerance;
    }

    bool AssetStreamingSample::ShouldConsumeOrbitInput(
        bool smoke,
        bool presentationReady,
        bool orbitInitialized,
        bool inputAvailable) noexcept
    {
        return !smoke && presentationReady && orbitInitialized &&
               inputAvailable;
    }

    bool AssetStreamingSample::ObservePrimaryTexturePublication(
        SampleContext& context)
    {
        if (!m_primary.IsCPUReady())
            return true;

        const std::vector<AssetId>& textures =
            m_primary.status.modelMetadata.streamingTextureAssetIds;
        if (textures.empty())
        {
            Fail(context,
                 TextureCommitInvariant.code,
                 "The primary texture model exposed no streamed texture dependencies");
            return false;
        }

        size_t committedTextureCount = 0;
        for (const AssetId texture : textures)
        {
            if (!texture.IsValid())
            {
                Fail(context,
                     TextureCommitInvariant.code,
                     "Primary model metadata exposed an invalid streamed texture identity");
                return false;
            }

            const Resource::ResourcePublicationQueryResult publication =
                context.resourcePublications.QueryResourcePublication(
                    texture.value, Resource::ResourceType::Texture);
            if (publication.IsResolved())
            {
                if (!m_texturePublicationCapabilityObserved)
                {
                    static_cast<void>(context.assessment.TryPublish(
                        AssessmentCapabilityObservation{
                            TexturePublicationCapability,
                            AssessmentCheckpoints::ActionApplied,
                            DiagnosticValue<bool>::Available(true),
                            "Exact texture publication status is available on the update thread."}));
                    m_texturePublicationCapabilityObserved = true;
                }
                const TexturePublicationObservation observation =
                    ClassifyTexturePublication(publication);
                if (observation == TexturePublicationObservation::Pending)
                    continue;
                if (observation != TexturePublicationObservation::Committed)
                {
                    Fail(context,
                         TextureCommitInvariant.code,
                         "Water Bottle texture publication reached an invalid terminal state without exact committed Render content");
                    return false;
                }
                ++committedTextureCount;

                if (!HasAuthorizedTextureStreaming(m_primary.status) &&
                    !m_primary.IsFullyResident())
                {
                    const auto [position, inserted] =
                        m_primaryTextureEvidence.try_emplace(texture.value);
                    StreamingTextureReceiptEvidence& evidence = position->second;
                    if (!inserted && evidence.texture != publication.handle)
                    {
                        Fail(context,
                             TextureCommitInvariant.code,
                             "Water Bottle fallback texture changed RenderResourceHandle generation");
                        return false;
                    }
                    evidence.texture = publication.handle;
                    if (evidence.fallbackCommittedContentRevision == 0)
                    {
                        evidence.fallbackCommittedContentRevision =
                            publication.status.committedContentRevision;
                    }
                    else if (evidence.fallbackCommittedContentRevision !=
                             publication.status.committedContentRevision)
                    {
                        Fail(context,
                             TextureCommitInvariant.code,
                             "Water Bottle fallback texture content changed before texture streaming was authorized");
                        return false;
                    }
                    continue;
                }

                const auto baseline = m_primaryTextureEvidence.find(
                    texture.value);
                if (m_primary.IsFullyResident())
                {
                    if (baseline == m_primaryTextureEvidence.end())
                    {
                        Fail(context,
                             TextureCommitInvariant.code,
                             "Water Bottle became fully resident before an exact fallback texture handle could be recorded");
                        return false;
                    }
                    StreamingTextureReceiptEvidence& evidence = baseline->second;
                    if (evidence.texture != publication.handle)
                    {
                        Fail(context,
                             TextureCommitInvariant.code,
                             "Water Bottle ready texture did not preserve its fallback RenderResourceHandle generation");
                        return false;
                    }
                    if (publication.status.committedContentRevision >
                        evidence.fallbackCommittedContentRevision)
                    {
                        evidence.readyCommittedContentRevision =
                            publication.status.committedContentRevision;
                    }
                }
                continue;
            }

            using Code = Resource::ResourcePublicationQueryCode;
            if (publication.code == Code::NotFound)
                continue;
            if (publication.code == Code::GatewayUnavailable)
            {
                if (!m_texturePublicationCapabilityObserved)
                {
                    static_cast<void>(context.assessment.TryPublish(
                        AssessmentCapabilityObservation{
                            TexturePublicationCapability,
                            AssessmentCheckpoints::ActionApplied,
                            DiagnosticValue<bool>::Available(false),
                            "Texture publication view has no active gateway."}));
                    m_texturePublicationCapabilityObserved = true;
                }
                ReportUnavailable(context,
                                  TexturePublicationCapability.code,
                                  "Texture publication view has no active gateway");
                return false;
            }
            Fail(context,
                 TextureCommitInvariant.code,
                 "Texture publication query failed with " +
                     std::string(GetPublicationCodeName(publication.code)));
            return false;
        }
        if (!HasAuthorizedTextureStreaming(m_primary.status) &&
            !m_primary.IsFullyResident() &&
            committedTextureCount == textures.size() &&
            !std::all_of(textures.begin(),
                         textures.end(),
                         [this](AssetId texture)
                         {
                             return m_primaryTextureEvidence.contains(texture.value);
                         }))
        {
            Fail(context,
                 TextureCommitInvariant.code,
                 "Water Bottle streamed texture ResourceIds could not all be paired with exact Render handles");
            return false;
        }
        return true;
    }

    void AssetStreamingSample::ObservePrimaryTextureReceipts(
        const SampleRenderDiagnostics& diagnostics)
    {
        if (!diagnostics.presentedMaterialBindingsAvailable ||
            diagnostics.presentedMaterialBindingsOverflow ||
            m_primaryTextureEvidence.empty())
        {
            return;
        }

        for (const SamplePresentedMaterialBindingReceipt& receipt :
             diagnostics.presentedMaterialBindings)
        {
            if (!receipt.IsValid())
            {
                continue;
            }
            for (const MaterialBindingTextureEntry& entry :
                 receipt.textureEntries)
            {
                if (!entry.IsValid())
                {
                    continue;
                }
                for (auto& [resourceId, evidence] : m_primaryTextureEvidence)
                {
                    static_cast<void>(resourceId);
                    if (entry.texture != evidence.texture)
                    {
                        continue;
                    }

                    if (!evidence.HasFallbackReceipt() &&
                        entry.contentRevision ==
                            evidence.fallbackCommittedContentRevision &&
                        receipt.presentationSequence != 0)
                    {
                        evidence.fallbackPresentationSequence =
                            receipt.presentationSequence;
                        evidence.fallbackDescriptorContentKey =
                            receipt.descriptorContentKey;
                        evidence.fallbackDescriptorRevision =
                            receipt.descriptorRevision;
                    }

                    if (evidence.HasFallbackReceipt() &&
                        evidence.readyCommittedContentRevision >
                            evidence.fallbackCommittedContentRevision &&
                        !entry.fallbackUsed &&
                        entry.contentRevision ==
                            evidence.readyCommittedContentRevision &&
                        receipt.presentationSequence >
                            evidence.fallbackPresentationSequence &&
                        receipt.descriptorContentKey !=
                            evidence.fallbackDescriptorContentKey &&
                        receipt.descriptorRevision >
                            evidence.fallbackDescriptorRevision &&
                        IsReadyTextureReceiptFrameQualified(diagnostics,
                                                            receipt.frameSequence,
                                                            receipt.presentationSequence))
                    {
                        evidence.readyPresentationSequence =
                            receipt.presentationSequence;
                        evidence.readyDescriptorContentKey =
                            receipt.descriptorContentKey;
                        evidence.readyDescriptorRevision =
                            receipt.descriptorRevision;
                        evidence.readyNoRebuildPresentationSequence =
                            receipt.presentationSequence;
                    }
                }
            }
        }

        m_primaryTextureFallbackBaselineFrozen =
            m_primaryFallbackPresentationSequence != 0 &&
            std::all_of(
                m_primaryTextureEvidence.begin(),
                m_primaryTextureEvidence.end(),
                [this](const auto& pair)
                {
                    return pair.second.HasFallbackReceipt() &&
                           pair.second.fallbackPresentationSequence >=
                               m_primaryFallbackPresentationSequence;
                });
        m_primaryTextureCommitted = m_primaryTextureFallbackBaselineFrozen &&
            std::all_of(m_primaryTextureEvidence.begin(),
                        m_primaryTextureEvidence.end(),
                        [](const auto& pair)
                        {
                            return pair.second.HasReadyReceipt();
                        });
        if (m_primaryTextureCommitted)
        {
            m_primaryTextureCommitRevision = 0;
            for (const auto& [resourceId, evidence] : m_primaryTextureEvidence)
            {
                static_cast<void>(resourceId);
                m_primaryTextureCommitRevision = std::max(
                    m_primaryTextureCommitRevision,
                    evidence.readyCommittedContentRevision);
            }
        }
    }

    bool AssetStreamingSample::IsReadyTextureReceiptFrameQualified(
        const SampleRenderDiagnostics& diagnostics,
        uint64 receiptFrameSequence,
        uint64 receiptPresentationSequence) noexcept
    {
        const SampleRenderMutationEvidenceDiagnostics& mutation =
            diagnostics.mutationEvidence;
        if (receiptFrameSequence == 0 || receiptPresentationSequence == 0 ||
            !diagnostics.sceneWorkAvailable ||
            !diagnostics.renderSceneValuesAvailable ||
            !mutation.available || mutation.saturated ||
            mutation.evidenceEpoch == 0 ||
            mutation.completedFrameSequence == 0 ||
            receiptFrameSequence != receiptPresentationSequence ||
            mutation.completedFrameSequence != receiptFrameSequence ||
            diagnostics.renderSceneFrameSequence !=
                receiptPresentationSequence ||
            diagnostics.sceneAppliedRevision !=
                diagnostics.renderSceneAppliedRevision ||
            diagnostics.sceneAppliedRevision != mutation.appliedSceneRevision ||
            diagnostics.renderSceneRequiredRevision !=
                mutation.requiredSceneRevision ||
            mutation.appliedSceneRevision < mutation.requiredSceneRevision ||
            diagnostics.sceneFullRebuildCount !=
                mutation.sceneFullRebuildCount ||
            diagnostics.sceneLastRebuiltObjectCount != 0 ||
            diagnostics.sceneLastRemovedObjectCount != 0 ||
            !diagnostics.lastPresentedFrameSequence.IsAvailable())
        {
            return false;
        }
        return *diagnostics.lastPresentedFrameSequence.GetValue() ==
               receiptPresentationSequence;
    }

    bool AssetStreamingSample::AreInitialModelsCPUReady() const noexcept
    {
        return m_primary.IsCPUReady() && m_corset.IsCPUReady() &&
               m_largeModel.IsCPUReady();
    }

    bool AssetStreamingSample::AreInitialModelsFullyResident() const noexcept
    {
        return m_primary.IsFullyResident() && m_corset.IsFullyResident() &&
               m_largeModel.IsFullyResident();
    }

    bool AssetStreamingSample::IsActionPresented(
        ScenarioAction action) const noexcept
    {
        const ScenarioActionEvidence& evidence =
            m_actions[GetScenarioActionIndex(action)];
        return evidence.requested &&
               evidence.completionGateSatisfied &&
               evidence.completedPresentationSequence != 0;
    }

    bool AssetStreamingSample::IsActionCompletionGateSatisfied(
        ScenarioAction action) const noexcept
    {
        switch (action)
        {
            case ScenarioAction::QueueThreeModelsAndDuplicateSubscriber:
                return m_primary.request.IsValid() &&
                       m_duplicateSubscriber.request.IsValid() &&
                       m_corset.request.IsValid() &&
                       m_largeModel.request.IsValid() &&
                       m_primary.resourceRequestId.IsValid() &&
                       m_duplicateSubscriber.resourceRequestId.IsValid() &&
                       m_corset.resourceRequestId.IsValid() &&
                       m_largeModel.resourceRequestId.IsValid();
            case ScenarioAction::PresentFallbackFrame:
                return m_primaryMinimumBeforeFullyResident &&
                       m_primaryFallbackPresentationSequence != 0 &&
                       m_primaryFallbackRenderSceneRevision != 0;
            case ScenarioAction::VerifyRequestCoalescing:
                return m_coalescedRequestObserved &&
                       m_primary.resourceRequestId.IsValid() &&
                       m_primary.resourceRequestId ==
                           m_duplicateSubscriber.resourceRequestId;
            case ScenarioAction::CancelDuplicateSubscriber:
                return m_duplicateCancellationRequested &&
                       m_duplicateCancellation.IsValid() &&
                       m_duplicateCancelled && m_primary.request.IsValid() &&
                       !m_primary.status.IsTerminal();
            case ScenarioAction::RequestUnknownAssetAndRollback:
                return m_unknownRequestRejected && m_unknownRollbackClean;
            case ScenarioAction::InstantiateMinimumResidentModels:
                return m_placedModelCount == 3 && AreInitialModelsCPUReady();
            case ScenarioAction::CommitRealTextures:
                return m_primary.IsFullyResident() &&
                       HasAuthorizedTextureStreaming(m_primary.status) &&
                       m_primaryTextureFallbackBaselineFrozen &&
                       m_primaryTextureCommitted &&
                       m_primaryTextureCommitRevision != 0 &&
                       m_noFullSceneRebuildForTextureCommit;
            case ScenarioAction::CancelPublishedLargeModel:
                return m_largeContentIdentity.has_value() &&
                       m_largeContentIdentity->IsVerified() &&
                       m_largeLifecycleHandle.IsValid() &&
                       m_largeResourceRequestId.IsValid() &&
                       m_largeCancellation.IsValid() &&
                       m_largeCancellation.request ==
                           m_largeLifecycleHandle &&
                       m_largeCancellation.resourceRequestId ==
                           m_largeResourceRequestId &&
                       !m_largeModel.request.IsValid();
            case ScenarioAction::AwaitExactRetirement:
                return m_largeCancellation.IsValid() &&
                       m_largeCancellationCompleted &&
                       m_exactOldSponzaRetirementSatisfied &&
                       m_closureProgressObserved && m_stableQueuesObserved;
            case ScenarioAction::ReloadSameAssetId:
                return m_reloadedLargeLifecycleHandle.IsValid() &&
                       m_reloadedLargeLifecycleHandle.sceneRuntimeId ==
                           m_largeLifecycleHandle.sceneRuntimeId &&
                       m_reloadedLargeLifecycleHandle.handle.GetIndex() ==
                           m_largeLifecycleHandle.handle.GetIndex() &&
                       m_reloadedLargeLifecycleHandle.handle.GetGeneration() >
                           m_largeLifecycleHandle.handle.GetGeneration();
            case ScenarioAction::PresentReloadedRevision:
                return m_reloadedFullyResident &&
                       m_reloadedIdentityMatches &&
                       m_reloadedLifecycleGenerationAdvanced &&
                       IsReloadInstanceReady(
                           m_reloadedLargeModel.IsFullyResident(),
                           m_reloadedInstanceValid,
                           m_reloadedRootValid,
                           m_reloadedSceneRevision);
            case ScenarioAction::DrainAllQueues:
                return m_stableQueuesObserved &&
                       AreTerminalQueuesDrained(m_lastQueueSnapshot);
            case ScenarioAction::Count:
            default:
                return false;
        }
    }

    bool AssetStreamingSample::IsActionPresentationCovered(
        const SampleRenderDiagnostics& diagnostics,
        const ScenarioActionEvidence& evidence) const noexcept
    {
        return evidence.targetSourceFrameSequence != 0 &&
               diagnostics.lastPresentedFrameSequence.IsAvailable() &&
               *diagnostics.lastPresentedFrameSequence.GetValue() >
                   evidence.minimumPresentationSequence &&
               diagnostics.renderSceneValuesAvailable &&
               diagnostics.renderSceneFrameSequence >=
                   evidence.targetSourceFrameSequence &&
               diagnostics.engineRenderRuntimeAvailable &&
               diagnostics.engineRequiredSceneRevision <=
                   diagnostics.renderSceneAppliedRevision &&
               diagnostics.renderSceneFrameSequence >=
                   diagnostics.engineRequiredSceneFrameSequence &&
               IsRequestedRenderPathExecuted(m_renderPath, diagnostics);
    }

    bool AssetStreamingSample::IsFallbackBaselineEligible(
        bool minimumResidentPresented,
        bool fullyResident) noexcept
    {
        return minimumResidentPresented && !fullyResident;
    }

    AssetStreamingSample::TexturePublicationObservation
        AssetStreamingSample::ClassifyTexturePublication(
            const Resource::ResourcePublicationQueryResult& publication) noexcept
    {
        if (!publication.IsResolved() || !publication.handle.IsValid() ||
            publication.status.code != RenderResourceStatusCode::Current)
        {
            return TexturePublicationObservation::Invalid;
        }

        if (publication.status.committedContentRevision != 0)
        {
            return publication.status.state == RenderResourcePublicState::GPUReady
                       ? TexturePublicationObservation::Committed
                       : TexturePublicationObservation::Invalid;
        }

        switch (publication.status.state)
        {
            case RenderResourcePublicState::Reserved:
            case RenderResourcePublicState::UploadQueued:
            case RenderResourcePublicState::Uploading:
                return TexturePublicationObservation::Pending;
            default:
                return TexturePublicationObservation::Invalid;
        }
    }

    bool AssetStreamingSample::IsReloadInstanceReady(bool fullyResident,
                                                      bool instanceValid,
                                                      bool rootValid,
                                                      uint64 sceneRevision) noexcept
    {
        return fullyResident && instanceValid && rootValid &&
               sceneRevision != 0;
    }

    bool AssetStreamingSample::IsRequestedRenderPathExecuted(
        SampleRenderPath requestedPath,
        const SampleRenderDiagnostics& diagnostics) noexcept
    {
        const bool direct = diagnostics.gpuDrivenPolicyDecisionAvailable &&
                            diagnostics.gpuDrivenRequestedMode ==
                                "ForceDisabled" &&
                            diagnostics.gpuDrivenPolicyReason ==
                                "ForcedDisabled" &&
                            !diagnostics.gpuDrivenEnabled &&
                            !diagnostics.gpuDrivenOpaqueIndirectRequested &&
                            !diagnostics.gpuDrivenOpaqueIndirectEligible &&
                            !diagnostics.gpuDrivenOpaqueIndirectSubmitted &&
                            diagnostics.gpuDrivenOpaqueIndirectBatchCount == 0 &&
                            diagnostics.gpuDrivenOpaqueIndirectDrawUpperBound == 0 &&
                            diagnostics.gpuDrivenOpaqueDirectDrawCount > 0 &&
                            diagnostics.opaqueExecutionCompleted;
        const bool gpuDriven = diagnostics.gpuDrivenPolicyDecisionAvailable &&
                               diagnostics.gpuDrivenRequestedMode ==
                                   "ForceEnabled" &&
                               diagnostics.gpuDrivenPolicyReason == "None" &&
                               diagnostics.gpuDrivenEnabled &&
                               diagnostics.gpuDrivenGraphPassAdded &&
                               diagnostics.gpuDrivenGraphPassRecorded &&
                               diagnostics.gpuDrivenExecutionRecorded &&
                               diagnostics.gpuDrivenGraphInputDrawItemCount != 0 &&
                               diagnostics.gpuDrivenOpaqueIndirectRequested &&
                               diagnostics.gpuDrivenOpaqueIndirectEligible &&
                               diagnostics.gpuDrivenOpaqueIndirectSubmitted &&
                               diagnostics.gpuDrivenOpaqueIndirectBatchCount != 0 &&
                               diagnostics.gpuDrivenOpaqueIndirectDrawUpperBound != 0;

        switch (requestedPath)
        {
            case SampleRenderPath::Direct:
                return direct;
            case SampleRenderPath::GPUDriven:
                return gpuDriven;
            case SampleRenderPath::Auto:
                if (!diagnostics.gpuDrivenPolicyDecisionAvailable ||
                    diagnostics.gpuDrivenRequestedMode != "Auto")
                {
                    return false;
                }
                if (diagnostics.gpuDrivenEnabled)
                {
                    return diagnostics.gpuDrivenPolicyReason == "None" &&
                           diagnostics.gpuDrivenGraphPassAdded &&
                           diagnostics.gpuDrivenGraphPassRecorded &&
                           diagnostics.gpuDrivenExecutionRecorded &&
                           diagnostics.gpuDrivenOpaqueIndirectRequested &&
                           diagnostics.gpuDrivenOpaqueIndirectEligible &&
                           diagnostics.gpuDrivenOpaqueIndirectSubmitted &&
                           diagnostics.gpuDrivenOpaqueIndirectBatchCount != 0;
                }
                return diagnostics.opaqueExecutionCompleted &&
                       diagnostics.gpuDrivenOpaqueDirectDrawCount != 0 &&
                       !diagnostics.gpuDrivenOpaqueIndirectSubmitted;
            default:
                return false;
        }
    }

    size_t AssetStreamingSample::GetScenarioActionIndex(
        ScenarioAction action) noexcept
    {
        return static_cast<size_t>(action);
    }

    const char* AssetStreamingSample::GetScenarioActionName(
        ScenarioAction action) noexcept
    {
        switch (action)
        {
            case ScenarioAction::QueueThreeModelsAndDuplicateSubscriber:
                return "QueueThreeModelsAndDuplicateSubscriber";
            case ScenarioAction::PresentFallbackFrame:
                return "PresentFallbackFrame";
            case ScenarioAction::VerifyRequestCoalescing:
                return "VerifyRequestCoalescing";
            case ScenarioAction::CancelDuplicateSubscriber:
                return "CancelDuplicateSubscriber";
            case ScenarioAction::RequestUnknownAssetAndRollback:
                return "RequestUnknownAssetAndRollback";
            case ScenarioAction::InstantiateMinimumResidentModels:
                return "InstantiateMinimumResidentModels";
            case ScenarioAction::CommitRealTextures:
                return "CommitRealTextures";
            case ScenarioAction::CancelPublishedLargeModel:
                return "CancelPublishedLargeModel";
            case ScenarioAction::AwaitExactRetirement:
                return "AwaitExactRetirement";
            case ScenarioAction::ReloadSameAssetId:
                return "ReloadSameAssetId";
            case ScenarioAction::PresentReloadedRevision:
                return "PresentReloadedRevision";
            case ScenarioAction::DrainAllQueues:
                return "DrainAllQueues";
            case ScenarioAction::Count:
            default:
                return "Invalid";
        }
    }

    const AssessmentAction& AssetStreamingSample::GetAssessmentAction(
        ScenarioAction action) noexcept
    {
        return StreamingActions[GetScenarioActionIndex(action)];
    }

    void AssetStreamingSample::RecordScenarioAction(SampleContext& context,
                                                     ScenarioAction action)
    {
        ScenarioActionEvidence& evidence =
            m_actions[GetScenarioActionIndex(action)];
        if (evidence.requested)
            return;
        evidence.requested = true;
        // The action may have authored fragments after the current frozen
        // snapshot. Require the next ECS snapshot so a pre-action frame can
        // never discharge this presentation gate.
        evidence.targetSourceFrameSequence =
            context.scene.GetDiagnosticsSnapshot().sceneSnapshotRevision + 1;
        evidence.minimumPresentationSequence =
            m_lastObservedPresentationSequence;
    }

    void AssetStreamingSample::ObserveActionPresentations(
        const SampleRenderDiagnostics& diagnostics,
        SampleAssessmentChannel& assessment)
    {
        if (!diagnostics.available || !diagnostics.rendered ||
            !diagnostics.renderSceneValuesAvailable ||
            !diagnostics.lastPresentedFrameSequence.IsAvailable())
        {
            return;
        }
        const uint64 presentation =
            *diagnostics.lastPresentedFrameSequence.GetValue();
        if (presentation == 0)
            return;

        m_lastObservedPresentationSequence = presentation;

        for (size_t index = 0; index < m_actions.size(); ++index)
        {
            ScenarioActionEvidence& evidence = m_actions[index];
            if (!evidence.requested ||
                evidence.completedPresentationSequence != 0 ||
                !IsActionCompletionGateSatisfied(
                    static_cast<ScenarioAction>(index)) ||
                !IsActionPresentationCovered(diagnostics, evidence))
            {
                continue;
            }
            evidence.completionGateSatisfied = true;
            evidence.appliedSceneRevision =
                diagnostics.renderSceneAppliedRevision;
            evidence.completedPresentationSequence = presentation;
            if (!evidence.assessmentReported)
            {
                static_cast<void>(assessment.MarkAction(
                    GetAssessmentAction(static_cast<ScenarioAction>(index))));
                evidence.assessmentReported = true;
            }
        }
    }

    AssetStreamingSample::ResourceQueueSnapshot
    AssetStreamingSample::MakeQueueSnapshot(
        const Resource::ResourceDiagnosticsSnapshot& diagnostics) noexcept
    {
        return {
            diagnostics.activeOperations,
            diagnostics.activeSubscribers,
            diagnostics.pendingAsyncJobs,
            diagnostics.pendingAsyncCompletions,
            diagnostics.decodeQueuedCount,
            diagnostics.decodeActiveCount,
            diagnostics.cacheEntryCount,
            diagnostics.pendingPublicationCount,
            diagnostics.pendingUploadCount,
            diagnostics.pendingReplacementCount,
            diagnostics.pendingRollbackCount,
            diagnostics.pendingRetirementCount,
            diagnostics.queuedLeaseUnloadCount,
            diagnostics.closureUnloadRequestCount};
    }

    bool AssetStreamingSample::AreTerminalQueuesDrained(
        const ResourceQueueSnapshot& snapshot) noexcept
    {
        return snapshot.activeOperations == 0 &&
               snapshot.activeSubscribers == 0 &&
               snapshot.pendingAsyncJobs == 0 &&
               snapshot.pendingAsyncCompletions == 0 &&
               snapshot.decodeQueuedCount == 0 &&
               snapshot.decodeActiveCount == 0 &&
               snapshot.pendingPublicationCount == 0 &&
               snapshot.pendingUploadCount == 0 &&
               snapshot.pendingReplacementCount == 0 &&
               snapshot.pendingRollbackCount == 0 &&
               snapshot.pendingRetirementCount == 0 &&
               snapshot.queuedLeaseUnloadCount == 0;
    }

    bool AssetStreamingSample::IsExactOldSponzaRetirementSatisfied(
        bool cancellationValid,
        bool oldLifecycleHandleValid,
        bool oldResourceRequestValid,
        bool cancellationResourceRequestValid,
        bool lifecycleHandleMatches,
        bool resourceRequestMatches,
        bool retirementComplete) noexcept
    {
        return cancellationValid && oldLifecycleHandleValid &&
               oldResourceRequestValid && cancellationResourceRequestValid &&
               lifecycleHandleMatches && resourceRequestMatches &&
               retirementComplete;
    }

    void AssetStreamingSample::OnInput(SampleContext& context)
    {
        if (ShouldConsumeOrbitInput(context.options.smoke,
                                    m_placedModelCount == 3u,
                                    m_orbitCamera.IsInitialized(),
                                    context.input != nullptr))
        {
            static_cast<void>(m_orbitCamera.Update(
                *context.input, context.cameras, context.camera));
        }
    }

    void AssetStreamingSample::OnViewportResize(SampleContext& context,
                                                uint32 width,
                                                uint32 height)
    {
        if (width == 0 || height == 0)
            return;

        const float32 aspect =
            static_cast<float32>(width) / static_cast<float32>(height);
        static_cast<void>(m_orbitCamera.SetAspectRatio(
            aspect, context.cameras, context.camera));
    }

    void AssetStreamingSample::ObserveDiagnostics(
        const SampleRenderDiagnostics& diagnostics,
        SampleAssessmentChannel& assessment)
    {
        ObserveActionPresentations(diagnostics, assessment);
        ObservePrimaryTextureReceipts(diagnostics);

        if (!diagnostics.mutationEvidence.available)
            return;

        const bool fallbackReceiptBelongsToCurrentPresentation =
            !m_primaryTextureEvidence.empty() &&
            diagnostics.lastPresentedFrameSequence.IsAvailable() &&
            std::all_of(
                m_primaryTextureEvidence.begin(),
                m_primaryTextureEvidence.end(),
                [&diagnostics](const auto& pair)
                {
                    return pair.second.HasFallbackReceipt() &&
                           pair.second.fallbackPresentationSequence ==
                               *diagnostics.lastPresentedFrameSequence.GetValue();
                });
        if (fallbackReceiptBelongsToCurrentPresentation &&
            !m_textureSceneBaselineCaptured)
        {
            m_fullSceneRebuildCountAtFallback =
                diagnostics.mutationEvidence.sceneFullRebuildCount;
            m_textureSceneBaselineCaptured = true;
        }
        if (m_primaryTextureCommitted && !m_textureSceneCommitCaptured)
        {
            m_fullSceneRebuildCountAtTextureCommit =
                diagnostics.mutationEvidence.sceneFullRebuildCount;
            m_textureSceneCommitCaptured = true;
        }
    }

    void AssetStreamingSample::AppendReport(
        SampleFeatureReporter& reporter) const
    {
        reporter.SetScenarioContractRevision(2);
        reporter.SetScenarioPhase(
            m_state == ScenarioState::Stable
                ? "stable"
                : m_state == ScenarioState::Failed ? "failed" : "in-progress");
        for (size_t index = 0; index < m_actions.size(); ++index)
        {
            const ScenarioActionEvidence& evidence = m_actions[index];
            static_cast<void>(reporter.AppendScenarioAction({
                GetScenarioActionName(static_cast<ScenarioAction>(index)),
                evidence.appliedSceneRevision,
                evidence.completedPresentationSequence,
                evidence.appliedSceneRevision,
                m_failure.empty() && evidence.requested &&
                    evidence.completionGateSatisfied &&
                    evidence.completedPresentationSequence != 0}));
        }
        static_cast<void>(reporter.AppendScenarioInvariant({
            "FirstFrameBeforeFullyResident",
            m_primaryMinimumBeforeFullyResident,
            "minimumResidentPresented=" +
                std::string(m_primaryMinimumBeforeFullyResident ? "true" : "false")}));
        static_cast<void>(reporter.AppendScenarioInvariant({
            "RequestCoalescing",
            m_coalescedRequestObserved && m_duplicateCancelled,
            "duplicateCancelled=" +
                std::string(m_duplicateCancelled ? "true" : "false")}));
        static_cast<void>(reporter.AppendScenarioInvariant({
            "UnknownAssetRollback",
            m_unknownRequestRejected && m_unknownRollbackClean,
            "typedNotFound=" +
                std::string(m_unknownRequestRejected ? "true" : "false") +
                ", preCoordinatorRejection=" +
                std::string(m_unknownRollbackClean ? "true" : "false")}));
        static_cast<void>(reporter.AppendScenarioInvariant({
            "IncrementalTexturePublication",
            m_primaryTextureCommitted && m_noFullSceneRebuildForTextureCommit,
            "fallbackGlobalFullRebuilds=" +
                std::to_string(m_fullSceneRebuildCountAtFallback) +
                ", qualifiedReadyGlobalFullRebuilds=" +
                std::to_string(m_fullSceneRebuildCountAtTextureCommit) +
                ", everyReadyReceiptFrameRebuiltObjects=0, "
                "everyReadyReceiptFrameRemovedObjects=0"}));
        static_cast<void>(reporter.AppendScenarioInvariant({
            "ExactRetirement",
            m_exactOldSponzaRetirementSatisfied &&
                m_closureProgressObserved,
            "exactOldLifecycleReceipt=" +
                std::string(m_exactOldSponzaRetirementSatisfied ? "true" : "false") +
                ", oldSceneRuntime=" +
                std::to_string(m_largeLifecycleHandle.sceneRuntimeId.GetValue()) +
                ", oldLifecycleHandle=" +
                std::to_string(m_largeLifecycleHandle.handle.GetPackedValue()) +
                ", oldResourceRequest=" +
                std::to_string(m_largeResourceRequestId.GetValue()) +
                ", closureProgress=" +
                std::string(m_closureProgressObserved ? "true" : "false") +
                " (closure ledger is global; lifecycle receipt is asset-specific)"}));
        static_cast<void>(reporter.AppendScenarioInvariant({
            "ReloadContentIdentityAndGeneration",
            m_reloadedFullyResident && m_reloadedIdentityMatches &&
                m_reloadedLifecycleGenerationAdvanced,
            "oldSceneRuntime=" +
                std::to_string(m_largeLifecycleHandle.sceneRuntimeId.GetValue()) +
                ", oldHandle=" +
                std::to_string(m_largeLifecycleHandle.handle.GetPackedValue()) +
                ", reloadedSceneRuntime=" +
                std::to_string(m_reloadedLargeLifecycleHandle.sceneRuntimeId.GetValue()) +
                ", reloadedHandle=" +
                std::to_string(m_reloadedLargeLifecycleHandle.handle.GetPackedValue())}));
        static_cast<void>(reporter.AppendScenarioInvariant({
            "DrainedQueues",
            m_stableQueuesObserved &&
                AreTerminalQueuesDrained(m_lastQueueSnapshot),
            "pendingOperations=" +
                std::to_string(m_lastQueueSnapshot.activeOperations) +
                ", activeSubscribers=" +
                std::to_string(m_lastQueueSnapshot.activeSubscribers) +
                ", pendingAsyncJobs=" +
                std::to_string(m_lastQueueSnapshot.pendingAsyncJobs) +
                ", pendingAsyncCompletions=" +
                std::to_string(m_lastQueueSnapshot.pendingAsyncCompletions) +
                ", decodeQueued=" +
                std::to_string(m_lastQueueSnapshot.decodeQueuedCount) +
                ", decodeActive=" +
                std::to_string(m_lastQueueSnapshot.decodeActiveCount) +
                ", pendingPublications=" +
                std::to_string(m_lastQueueSnapshot.pendingPublicationCount) +
                ", pendingUploads=" +
                std::to_string(m_lastQueueSnapshot.pendingUploadCount) +
                ", pendingReplacements=" +
                std::to_string(m_lastQueueSnapshot.pendingReplacementCount) +
                ", pendingRollbacks=" +
                std::to_string(m_lastQueueSnapshot.pendingRollbackCount) +
                ", pendingRetirements=" +
                std::to_string(m_lastQueueSnapshot.pendingRetirementCount) +
                ", queuedLeaseUnloads=" +
                std::to_string(m_lastQueueSnapshot.queuedLeaseUnloadCount) +
                " (resident cache and active leases remain live for the reloaded scene)"}));
        static_cast<void>(reporter.AppendScenarioMetric({
            "pendingOperations", m_lastQueueSnapshot.activeOperations}));
        static_cast<void>(reporter.AppendScenarioMetric({
            "activeSubscribers", m_lastQueueSnapshot.activeSubscribers}));
        static_cast<void>(reporter.AppendScenarioMetric({
            "pendingAsyncJobs", m_lastQueueSnapshot.pendingAsyncJobs}));
        static_cast<void>(reporter.AppendScenarioMetric({
            "pendingAsyncCompletions",
            m_lastQueueSnapshot.pendingAsyncCompletions}));
        static_cast<void>(reporter.AppendScenarioMetric({
            "decodeQueued", m_lastQueueSnapshot.decodeQueuedCount}));
        static_cast<void>(reporter.AppendScenarioMetric({
            "decodeActive", m_lastQueueSnapshot.decodeActiveCount}));
        static_cast<void>(reporter.AppendScenarioMetric({
            "pendingPublications",
            m_lastQueueSnapshot.pendingPublicationCount}));
        static_cast<void>(reporter.AppendScenarioMetric({
            "pendingUploads", m_lastQueueSnapshot.pendingUploadCount}));
        static_cast<void>(reporter.AppendScenarioMetric({
            "pendingReplacements",
            m_lastQueueSnapshot.pendingReplacementCount}));
        static_cast<void>(reporter.AppendScenarioMetric({
            "pendingRollbacks", m_lastQueueSnapshot.pendingRollbackCount}));
        static_cast<void>(reporter.AppendScenarioMetric({
            "pendingRetirements", m_lastQueueSnapshot.pendingRetirementCount}));
        static_cast<void>(reporter.AppendScenarioMetric({
            "queuedLeaseUnloads",
            m_lastQueueSnapshot.queuedLeaseUnloadCount}));
        static_cast<void>(reporter.AppendScenarioMetric({
            "closureUnloadRequests",
            m_lastQueueSnapshot.closureUnloadRequestCount}));

        reporter.Enable("AsynchronousModelRequests");
        reporter.Enable("CatalogAssetIdRequests");
        reporter.Enable("ProceduralSkyScene");
        reporter.Enable("DirectionalLightScene");
        if (m_primary.IsCPUReady() || m_corset.IsCPUReady() ||
            m_reloadedLargeModel.IsCPUReady())
        {
            reporter.Enable("ModelResourceLoad");
        }
        if (m_placedModelCount > 0)
            reporter.Enable("SceneInstantiation");
        if (m_duplicateCancelled)
            reporter.Enable("CoalescedSubscriberCancellation");
        if (m_unknownRequestRejected && m_unknownRollbackClean)
            reporter.Enable("UnknownAssetNotFoundRollback");
        if (m_primaryMinimumBeforeFullyResident)
            reporter.Enable("FallbackMinimumResidency");
        if (m_primaryTextureCommitted && m_noFullSceneRebuildForTextureCommit)
            reporter.Enable("TextureContentReplacement");
        if (m_exactOldSponzaRetirementSatisfied)
            reporter.Enable("CompletionOwnedRetirement");
        if (m_reloadedFullyResident && m_reloadedIdentityMatches &&
            m_reloadedLifecycleGenerationAdvanced)
            reporter.Enable("RetiredAssetIdReload");
        if (m_primaryTextureCommitted)
            reporter.Enable("DescriptorRevisionEvidence");
        reporter.ResourceDiagnostic(
            "placed minimum-resident models=" +
            std::to_string(m_placedModelCount));
        reporter.ResourceDiagnostic(
            "closure unload progress=" +
            std::string(m_closureProgressObserved ? "observed" : "pending"));
        reporter.ResourceDiagnostic(
            "stable resource queues=" +
            std::string(m_stableQueuesObserved ? "observed" : "pending"));
        reporter.ResourceDiagnostic(
            "exact old Crytek Sponza retirement receipt=" +
            std::string(m_exactOldSponzaRetirementSatisfied ? "observed" : "pending") +
            " (global closure ledger is not per-asset attributed)");
        reporter.ResourceDiagnostic(
            "unknown catalog request rejected before coordinator=" +
            std::string(m_unknownRollbackClean ? "true" : "false"));
        reporter.ResourceDiagnostic(
            "reloaded lifecycle generation advanced=" +
            std::string(m_reloadedLifecycleGenerationAdvanced ? "true" : "false"));
        reporter.ResourceDiagnostic(
            "DescriptorRevisionEvidence=" +
            std::string(m_primaryTextureCommitted &&
                        m_noFullSceneRebuildForTextureCommit
                            ? "Presented exact fallback-to-ready receipts verified"
                            : "Awaiting exact presented fallback-to-ready receipts"));
    }

    SampleReadiness AssetStreamingSample::GetReadiness(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (!m_failure.empty())
            return SampleReadiness::Failed(m_failure);
        if (m_state != ScenarioState::Stable)
            return SampleReadiness::Pending(
                "Asset-streaming scenario is waiting for its deterministic lifecycle actions");
        if (diagnostics.visibleObjectCount == 0)
            return SampleReadiness::Pending(
                "Asset-streaming is waiting for visible reloaded model geometry");
        if (!m_reloadedFullyResident || !m_reloadedIdentityMatches ||
            !m_reloadedLifecycleGenerationAdvanced || !m_closureProgressObserved ||
            !m_exactOldSponzaRetirementSatisfied || !m_stableQueuesObserved ||
            !AreTerminalQueuesDrained(m_lastQueueSnapshot) ||
            !m_unknownRollbackClean ||
            !m_noFullSceneRebuildForTextureCommit ||
            !IsReloadInstanceReady(m_reloadedLargeModel.IsFullyResident(),
                                   m_reloadedInstanceValid,
                                   m_reloadedRootValid,
                                   m_reloadedSceneRevision))
        {
            return SampleReadiness::Pending(
                "Asset-streaming has not reached a fully retired and reloaded stable state");
        }
        if (!diagnostics.lastPresentedFrameSequence.IsAvailable() ||
            *diagnostics.lastPresentedFrameSequence.GetValue() == 0 ||
            !diagnostics.renderSceneValuesAvailable ||
            diagnostics.renderSceneAppliedRevision !=
                diagnostics.renderSceneRequiredRevision ||
            !diagnostics.engineRenderRuntimeAvailable ||
            diagnostics.renderSceneFrameSequence <
                diagnostics.engineRequiredSceneFrameSequence ||
            diagnostics.renderSceneAppliedRevision <
                diagnostics.engineRequiredSceneRevision)
        {
            return SampleReadiness::Pending(
                "Asset-streaming is waiting for final RenderScene revision convergence");
        }
        if (!IsRequestedRenderPathExecuted(m_renderPath, diagnostics))
        {
            return SampleReadiness::Pending(
                "Asset-streaming requested render path has not produced its required Direct/GPU execution evidence");
        }
        if (std::any_of(m_actions.begin(),
                        m_actions.end(),
                        [](const ScenarioActionEvidence& action)
                        {
                            return !action.requested ||
                                   !action.completionGateSatisfied ||
                                   action.completedPresentationSequence == 0;
                        }))
        {
            return SampleReadiness::Pending(
                "Asset-streaming has an action not covered by a completed presentation");
        }
        return SampleReadiness::Ready();
    }

    bool AssetStreamingSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        const SampleReadiness readiness = GetReadiness(diagnostics);
        outError = readiness.reason;
        return readiness.IsReady();
    }

    void AssetStreamingSample::Shutdown(SampleContext& context)
    {
        CancelIfLive(context, m_primary);
        CancelIfLive(context, m_duplicateSubscriber);
        CancelIfLive(context, m_corset);
        CancelIfLive(context, m_largeModel);
        CancelIfLive(context, m_reloadedLargeModel);

        m_duplicateCancellation = {};
        m_largeCancellation = {};
        m_primaryTextureEvidence.clear();
        m_actions = {};
        m_largeContentIdentity.reset();
        m_lastQueueSnapshot = {};
        m_largeLifecycleHandle = {};
        m_largeResourceRequestId = {};
        m_reloadedLargeLifecycleHandle = {};
        m_state = ScenarioState::QueueThreeModelsAndDuplicateSubscriber;
        m_renderPath = SampleRenderPath::Auto;
        m_initialClosureUnloadRequests = 0;
        m_expectedClosureUnloadRequests = 0;
        m_fullSceneRebuildCountAtFallback = 0;
        m_fullSceneRebuildCountAtTextureCommit = 0;
        m_lastObservedPresentationSequence = 0;
        m_primaryFallbackPresentationSequence = 0;
        m_primaryFallbackRenderSceneRevision = 0;
        m_primaryTextureCommitRevision = 0;
        m_reloadedSceneRevision = 0;
        m_updateCount = 0;
        m_placedModelCount = 0;
        m_presentationBounds.Reset();
        m_initialLargePresentationBounds.Reset();
        m_reloadedLargePresentationBounds.Reset();
        m_cameraFrame = {};
        m_orbitCamera.Reset();
        m_duplicateCancellationRequested = false;
        m_duplicateCancelled = false;
        m_unknownRequestRejected = false;
        m_unknownRollbackClean = false;
        m_coalescedRequestObserved = false;
        m_primaryMinimumBeforeFullyResident = false;
        m_primaryTextureFallbackBaselineFrozen = false;
        m_primaryTextureCommitted = false;
        m_textureSceneBaselineCaptured = false;
        m_textureSceneCommitCaptured = false;
        m_noFullSceneRebuildForTextureCommit = false;
        m_largeCancellationCompleted = false;
        m_exactOldSponzaRetirementSatisfied = false;
        m_reloadedFullyResident = false;
        m_reloadedInstanceValid = false;
        m_reloadedRootValid = false;
        m_reloadedIdentityMatches = false;
        m_reloadedLifecycleGenerationAdvanced = false;
        m_reloadedPresentationPlaced = false;
        m_closureProgressObserved = false;
        m_stableQueuesObserved = false;
        m_resourceDiagnosticsCapabilityObserved = false;
        m_texturePublicationCapabilityObserved = false;
        m_skyboxCreated = false;
        m_lightCreated = false;
        m_failure.clear();
    }

    void AssetStreamingSample::MarkInvariant(
        SampleContext& context,
        const AssessmentInvariant& invariant,
        AssessmentCheckpoint checkpoint)
    {
        static_cast<void>(context.assessment.TryPublish(
            AssessmentInvariantObservation{invariant, std::move(checkpoint)}));
    }

    void AssetStreamingSample::ReportUnavailable(SampleContext& context,
                                                  AssessmentCode code,
                                                  std::string detail)
    {
        Finding finding;
        finding.code = AssessmentCode("RESOURCE.ASSET_STREAMING.INSTRUMENTATION_GAP");
        finding.subsystemCode = AssessmentCode("RESOURCE");
        finding.invariantCode = std::move(code);
        finding.checkpoint = AssessmentCheckpoints::ActionApplied;
        finding.classification = FindingClass::InstrumentationGap;
        finding.severity = FindingSeverity::Error;
        finding.confidence = FindingConfidence::Confirmed;
        finding.summary = "Asset-streaming lacks a required exact diagnostic.";
        finding.detail = detail;
        finding.expected = "The sample can observe exact resource lifecycle evidence.";
        finding.observed = detail;
        finding.gating = true;
        finding.blockingReason = "Required resource diagnostic is unavailable.";
        static_cast<void>(context.assessment.TryPublish(std::move(finding)));
        if (m_failure.empty())
            m_failure = std::move(detail);
        m_state = ScenarioState::Failed;
    }

    void AssetStreamingSample::Fail(SampleContext& context,
                                    AssessmentCode invariantCode,
                                    std::string message)
    {
        if (m_state == ScenarioState::Failed)
            return;

        m_failure = std::move(message);
        m_state = ScenarioState::Failed;
        Finding finding;
        finding.code = AssessmentCode("RESOURCE.ASSET_STREAMING.ACTION_FAILED");
        finding.subsystemCode = AssessmentCode("RESOURCE");
        finding.invariantCode = std::move(invariantCode);
        finding.checkpoint = AssessmentCheckpoints::ActionApplied;
        finding.classification = FindingClass::ContractViolation;
        finding.severity = FindingSeverity::Error;
        finding.confidence = FindingConfidence::Confirmed;
        finding.summary = "The asset-streaming architecture probe could not complete an action.";
        finding.detail = m_failure;
        finding.expected = "Asynchronous asset lifecycle contracts remain coherent.";
        finding.observed = m_failure;
        finding.gating = true;
        finding.blockingReason = "The deterministic asset-streaming probe failed.";
        static_cast<void>(context.assessment.TryPublish(std::move(finding)));
    }

    void AssetStreamingSample::CancelIfLive(SampleContext& context,
                                             LoadedSampleModel& model) noexcept
    {
        if (model.request.IsValid())
            static_cast<void>(context.models.Cancel(model));
    }
} // namespace RVX
