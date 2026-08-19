#include "Scenes/AssetStreamingSample.h"
#include "Core/Math/AABB.h"
#include "Samples/SampleCLI.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace RVX
{
    struct AssetStreamingSampleTestAccess
    {
        static bool IsFallbackBaselineEligible(bool minimumResidentPresented,
                                               bool fullyResident)
        {
            return AssetStreamingSample::IsFallbackBaselineEligible(
                minimumResidentPresented, fullyResident);
        }

        static bool IsReloadInstanceReady(bool fullyResident,
                                          bool instanceValid,
                                          bool rootValid,
                                          uint64 sceneRevision)
        {
            return AssetStreamingSample::IsReloadInstanceReady(
                fullyResident, instanceValid, rootValid, sceneRevision);
        }

        static bool IsRequestedRenderPathExecuted(
            SampleRenderPath requestedPath,
            const SampleRenderDiagnostics& diagnostics)
        {
            return AssetStreamingSample::IsRequestedRenderPathExecuted(
                requestedPath, diagnostics);
        }

        static bool IsActionPresentationCovered(
            AssetStreamingSample& sample,
            const SampleRenderDiagnostics& diagnostics,
            uint64 targetSourceFrameSequence,
            uint64 minimumPresentationSequence)
        {
            AssetStreamingSample::ScenarioActionEvidence evidence;
            evidence.targetSourceFrameSequence = targetSourceFrameSequence;
            evidence.minimumPresentationSequence = minimumPresentationSequence;
            return sample.IsActionPresentationCovered(diagnostics, evidence);
        }

        static bool HasReadyTextureReceipt(
            RenderResourceHandle texture,
            uint64 fallbackRevision,
            uint64 fallbackPresentation,
            uint64 fallbackKey,
            uint64 fallbackDescriptorRevision,
            uint64 readyRevision,
            uint64 readyPresentation,
            uint64 readyKey,
            uint64 readyDescriptorRevision,
            bool readyFrameQualified = true)
        {
            AssetStreamingSample::StreamingTextureReceiptEvidence evidence;
            evidence.texture = texture;
            evidence.fallbackCommittedContentRevision = fallbackRevision;
            evidence.fallbackPresentationSequence = fallbackPresentation;
            evidence.fallbackDescriptorContentKey = fallbackKey;
            evidence.fallbackDescriptorRevision = fallbackDescriptorRevision;
            evidence.readyCommittedContentRevision = readyRevision;
            evidence.readyPresentationSequence = readyPresentation;
            evidence.readyDescriptorContentKey = readyKey;
            evidence.readyDescriptorRevision = readyDescriptorRevision;
            evidence.readyNoRebuildPresentationSequence =
                readyFrameQualified ? readyPresentation : 0;
            return evidence.HasReadyReceipt();
        }

        static bool IsReadyTextureReceiptFrameQualified(
            bool sceneWorkAvailable,
            bool evidenceAvailable,
            bool evidenceSaturated,
            uint64 completedFrameSequence,
            uint64 receiptFrameSequence,
            uint64 lastPresentedFrameSequence,
            uint64 receiptPresentationSequence,
            uint64 requiredSceneRevision,
            uint64 appliedSceneRevision,
            uint64 rebuiltObjectCount,
            uint64 removedObjectCount)
        {
            SampleRenderDiagnostics diagnostics;
            diagnostics.sceneWorkAvailable = sceneWorkAvailable;
            diagnostics.renderSceneValuesAvailable = true;
            diagnostics.renderSceneFrameSequence = lastPresentedFrameSequence;
            diagnostics.sceneAppliedRevision = appliedSceneRevision;
            diagnostics.renderSceneAppliedRevision = appliedSceneRevision;
            diagnostics.renderSceneRequiredRevision = requiredSceneRevision;
            diagnostics.sceneFullRebuildCount = 4;
            diagnostics.sceneLastRebuiltObjectCount =
                static_cast<uint32>(rebuiltObjectCount);
            diagnostics.sceneLastRemovedObjectCount =
                static_cast<uint32>(removedObjectCount);
            diagnostics.mutationEvidence.available = evidenceAvailable;
            diagnostics.mutationEvidence.saturated = evidenceSaturated;
            diagnostics.mutationEvidence.evidenceEpoch = 5;
            diagnostics.mutationEvidence.completedFrameSequence =
                completedFrameSequence;
            diagnostics.mutationEvidence.requiredSceneRevision =
                requiredSceneRevision;
            diagnostics.mutationEvidence.appliedSceneRevision =
                appliedSceneRevision;
            diagnostics.mutationEvidence.sceneFullRebuildCount = 4;
            diagnostics.lastPresentedFrameSequence =
                DiagnosticValue<uint64>::Available(lastPresentedFrameSequence);

            return AssetStreamingSample::IsReadyTextureReceiptFrameQualified(
                diagnostics,
                receiptFrameSequence,
                receiptPresentationSequence);
        }

        static uint8 ClassifyTexturePublication(
            RenderResourcePublicState state,
            uint64 committedContentRevision,
            bool validHandle = true)
        {
            Resource::ResourcePublicationQueryResult publication;
            publication.code = Resource::ResourcePublicationQueryCode::Resolved;
            publication.handle = validHandle ? RenderResourceHandle{71, 3}
                                             : RenderResourceHandle{};
            publication.status.code = RenderResourceStatusCode::Current;
            publication.status.state = state;
            publication.status.committedContentRevision =
                committedContentRevision;
            return static_cast<uint8>(
                AssetStreamingSample::ClassifyTexturePublication(publication));
        }

        static std::array<bool, 3>
            ObserveFallbackReceiptBeforeCoordinatorConfirmation()
        {
            AssetStreamingSample sample;
            constexpr RenderResourceHandle Texture{71, 3};
            AssetStreamingSample::StreamingTextureReceiptEvidence evidence;
            evidence.texture = Texture;
            evidence.fallbackCommittedContentRevision = 9;
            sample.m_primaryTextureEvidence.emplace(301, evidence);

            SampleRenderDiagnostics diagnostics;
            diagnostics.presentedMaterialBindingsAvailable = true;
            SamplePresentedMaterialBindingReceipt receipt;
            receipt.material = {81, 2};
            receipt.contentRevision = 4;
            receipt.descriptorContentKey = 101;
            receipt.descriptorRevision = 1;
            receipt.frameSequence = 39;
            receipt.presentationSequence = 40;
            MaterialBindingTextureEntry entry;
            entry.texture = Texture;
            entry.contentRevision = 9;
            // A streaming placeholder is valid committed texture content. It
            // is not MaterialSystem's missing/default-resource fallback.
            entry.fallbackUsed = false;
            receipt.textureEntries.push_back(entry);
            diagnostics.presentedMaterialBindings.push_back(receipt);

            sample.ObservePrimaryTextureReceipts(diagnostics);
            const bool candidateRecorded =
                sample.m_primaryTextureEvidence.at(301).HasFallbackReceipt();
            const bool frozenBeforeCoordinator =
                sample.m_primaryTextureFallbackBaselineFrozen;
            sample.m_primaryFallbackPresentationSequence = 40;
            sample.ObservePrimaryTextureReceipts(diagnostics);
            return {candidateRecorded,
                    frozenBeforeCoordinator,
                    sample.m_primaryTextureFallbackBaselineFrozen};
        }

        static bool AreTerminalQueuesDrained(
            const Resource::ResourceDiagnosticsSnapshot& diagnostics)
        {
            return AssetStreamingSample::AreTerminalQueuesDrained(
                AssetStreamingSample::MakeQueueSnapshot(diagnostics));
        }

        static bool IsExactOldSponzaRetirementSatisfied(
            bool cancellationValid,
            bool oldLifecycleHandleValid,
            bool oldResourceRequestValid,
            bool cancellationResourceRequestValid,
            bool lifecycleHandleMatches,
            bool resourceRequestMatches,
            bool retirementComplete)
        {
            return AssetStreamingSample::IsExactOldSponzaRetirementSatisfied(
                cancellationValid,
                oldLifecycleHandleValid,
                oldResourceRequestValid,
                cancellationResourceRequestValid,
                lifecycleHandleMatches,
                resourceRequestMatches,
                retirementComplete);
        }

        static bool IsNormalizedGroundedPlacement(const AABB& sourceBounds,
                                                  const AABB& placedBounds,
                                                  float32 targetExtent)
        {
            return AssetStreamingSample::IsNormalizedGroundedPlacement(
                sourceBounds, placedBounds, targetExtent);
        }

        static bool ArePresentationBoundsEquivalent(const AABB& expected,
                                                    const AABB& actual)
        {
            return AssetStreamingSample::ArePresentationBoundsEquivalent(
                expected, actual);
        }

        static bool ShouldConsumeOrbitInput(bool smoke,
                                            bool presentationReady,
                                            bool orbitInitialized,
                                            bool inputAvailable)
        {
            return AssetStreamingSample::ShouldConsumeOrbitInput(
                smoke, presentationReady, orbitInitialized, inputAvailable);
        }
    };

    namespace
    {
        bool ContainsCode(const auto& values, std::string_view code)
        {
            return std::any_of(values.begin(), values.end(),
                               [code](const auto& value)
                               {
                                   return value.code.GetValue() == code;
                               });
        }

        std::string ReadSource(std::string_view file)
        {
            const std::filesystem::path path =
                std::filesystem::path(RVX_SOURCE_DIR) / file;
            std::ifstream stream(path, std::ios::binary);
            return {std::istreambuf_iterator<char>(stream),
                    std::istreambuf_iterator<char>()};
        }
    } // namespace

    TEST(AssetStreamingSampleValidation, DeclaresTheHermeticAssetSet)
    {
        AssetStreamingSample sample;
        const SampleInfo& info = sample.GetInfo();

        EXPECT_EQ(info.id, "asset-streaming");
        EXPECT_EQ(info.defaultAssetId, "water-bottle");
        EXPECT_EQ(info.assetPolicy, SampleAssetPolicy::Fixed);
        EXPECT_EQ(info.environmentPolicy, SampleEnvironmentPolicy::None);
        EXPECT_TRUE(info.supportsRenderPathSelection);
        ASSERT_EQ(info.additionalDefaultAssetIds.size(), 2u);
        EXPECT_EQ(info.additionalDefaultAssetIds[0], "corset");
        EXPECT_EQ(info.additionalDefaultAssetIds[1], "crytek-sponza");
    }

    TEST(AssetStreamingSampleValidation, DeclaresStreamingLifetimeContract)
    {
        AssetStreamingSample sample;
        const SampleAssessmentContract contract = sample.GetAssessmentContract();

        EXPECT_TRUE(contract.IsValid());
        EXPECT_EQ(contract.code.GetValue(), "SAMPLE.ASSET_STREAMING");
        EXPECT_EQ(contract.revision, "2");
        EXPECT_TRUE(ContainsCode(contract.actions,
                                 "RESOURCE.ACTION.QUEUE_THREE_MODELS_AND_DUPLICATE_SUBSCRIBER"));
        EXPECT_TRUE(ContainsCode(contract.actions,
                                 "RESOURCE.ACTION.PRESENT_FALLBACK_FRAME"));
        EXPECT_TRUE(ContainsCode(contract.actions,
                                 "RESOURCE.ACTION.VERIFY_REQUEST_COALESCING"));
        EXPECT_TRUE(ContainsCode(contract.actions,
                                 "RESOURCE.ACTION.CANCEL_DUPLICATE_SUBSCRIBER"));
        EXPECT_TRUE(ContainsCode(contract.actions,
                                 "RESOURCE.ACTION.REQUEST_UNKNOWN_ASSET_AND_ROLLBACK"));
        EXPECT_TRUE(ContainsCode(contract.actions,
                                 "RESOURCE.ACTION.INSTANTIATE_MINIMUM_RESIDENT_MODELS"));
        EXPECT_TRUE(ContainsCode(contract.actions,
                                 "RESOURCE.ACTION.COMMIT_REAL_TEXTURES"));
        EXPECT_TRUE(ContainsCode(contract.actions,
                                 "RESOURCE.ACTION.CANCEL_PUBLISHED_LARGE_MODEL"));
        EXPECT_TRUE(ContainsCode(contract.actions,
                                 "RESOURCE.ACTION.AWAIT_EXACT_RETIREMENT"));
        EXPECT_TRUE(ContainsCode(contract.actions,
                                 "RESOURCE.ACTION.RELOAD_SAME_ASSET_ID"));
        EXPECT_TRUE(ContainsCode(contract.actions,
                                 "RESOURCE.ACTION.PRESENT_RELOADED_REVISION"));
        EXPECT_TRUE(ContainsCode(contract.actions,
                                 "RESOURCE.ACTION.DRAIN_ALL_QUEUES"));
        EXPECT_TRUE(ContainsCode(contract.invariants,
                                 "RESOURCE.LOAD.COALESCED_SUBSCRIBERS"));
        EXPECT_TRUE(ContainsCode(contract.invariants,
                                 "RESOURCE.LOAD.FAILURE_ROLLBACK"));
        EXPECT_TRUE(ContainsCode(contract.invariants,
                                 "RESOURCE.RESIDENCY.FALLBACK_BEFORE_FULL"));
        EXPECT_TRUE(ContainsCode(contract.invariants,
                                 "RESOURCE.RETIREMENT.CLOSURE_COMPLETED"));
        EXPECT_TRUE(ContainsCode(contract.invariants,
                                 "RESOURCE.RETIREMENT.STABLE_QUEUES"));
        EXPECT_TRUE(ContainsCode(contract.invariants,
                                 "RESOURCE.RELOAD.CONTENT_IDENTITY_AND_GENERATION"));
        EXPECT_TRUE(ContainsCode(contract.invariants,
                                 "RESOURCE.TEXTURE.INCREMENTAL_PUBLICATION"));
        EXPECT_TRUE(ContainsCode(contract.metrics,
                                 "RESOURCE.METRIC.DECODE_COMPLETED"));
        EXPECT_TRUE(ContainsCode(contract.capabilities,
                                 "RESOURCE.CAPABILITY.DIAGNOSTICS"));
        EXPECT_TRUE(ContainsCode(contract.capabilities,
                                 "RESOURCE.CAPABILITY.TEXTURE_PUBLICATION"));
    }

    TEST(AssetStreamingSampleValidation, UsesOnlySampleFacingAsyncServices)
    {
        const std::string source = ReadSource(
            "Samples/RenderVerseSamples/Scenes/AssetStreamingSample.cpp");
        ASSERT_FALSE(source.empty());
        EXPECT_EQ(source.find("WaitForLoad"), std::string::npos);
        EXPECT_EQ(source.find("LoadSync"), std::string::npos);
        EXPECT_EQ(source.find("RenderGraph"), std::string::npos);
        EXPECT_EQ(source.find("RHIBackendType"), std::string::npos);
        EXPECT_NE(source.find("context.models.RequestByAssetId("),
                  std::string::npos);
        EXPECT_NE(source.find("CancelForRetirement"), std::string::npos);
        EXPECT_NE(source.find("m_duplicateCancellationRequested"),
                  std::string::npos);
        EXPECT_NE(source.find("QueryRetirementStatus"), std::string::npos);
        EXPECT_EQ(source.find("Cancel(m_duplicateSubscriber)"),
                  std::string::npos);
        EXPECT_NE(source.find("rvx-intentional-unknown-asset"),
                  std::string::npos);
        EXPECT_NE(source.find("QueueThreeModelsAndDuplicateSubscriber"),
                  std::string::npos);
        EXPECT_NE(source.find("PresentReloadedRevision"),
                  std::string::npos);
        EXPECT_NE(source.find("AppendScenarioAction"), std::string::npos);
        EXPECT_NE(source.find("DescriptorRevisionEvidence="),
                  std::string::npos);
        EXPECT_NE(source.find("ObservePrimaryTextureReceipts"),
                  std::string::npos);
        EXPECT_NE(source.find("fallbackDescriptorContentKey"),
                  std::string::npos);
        EXPECT_NE(source.find("preCoordinatorRejection"),
                  std::string::npos);
        EXPECT_NE(source.find("minimumPresentationSequence"),
                  std::string::npos);
        EXPECT_NE(source.find("IsActionCompletionGateSatisfied"),
                  std::string::npos);
        EXPECT_NE(source.find("IsRequestedRenderPathExecuted"),
                  std::string::npos);
    }

    TEST(AssetStreamingSampleValidation,
         OldPresentationCannotCompleteCancelOrReloadEvidence)
    {
        AssetStreamingSample sample;
        SampleRenderDiagnostics diagnostics;
        diagnostics.engineRenderRuntimeAvailable = true;
        diagnostics.renderSceneValuesAvailable = true;
        diagnostics.renderSceneAppliedRevision = 3;
        diagnostics.engineRequiredSceneRevision = 3;
        diagnostics.renderSceneFrameSequence = 17;
        diagnostics.engineRequiredSceneFrameSequence = 17;
        diagnostics.lastPresentedFrameSequence =
            DiagnosticValue<uint64>::Available(17);
        diagnostics.gpuDrivenPolicyDecisionAvailable = true;
        diagnostics.gpuDrivenRequestedMode = "Auto";
        diagnostics.gpuDrivenEnabled = false;
        diagnostics.opaqueExecutionCompleted = true;
        diagnostics.gpuDrivenOpaqueDirectDrawCount = 1;

        EXPECT_FALSE(AssetStreamingSampleTestAccess::IsActionPresentationCovered(
            sample, diagnostics, 17, 17));

        diagnostics.lastPresentedFrameSequence =
            DiagnosticValue<uint64>::Available(18);
        EXPECT_TRUE(AssetStreamingSampleTestAccess::IsActionPresentationCovered(
            sample, diagnostics, 17, 17));
    }

    TEST(AssetStreamingSampleValidation,
         FullyResidentFirstCannotFreezeFallbackBaseline)
    {
        EXPECT_FALSE(AssetStreamingSampleTestAccess::IsFallbackBaselineEligible(
            true, true));
        EXPECT_FALSE(AssetStreamingSampleTestAccess::IsFallbackBaselineEligible(
            false, false));
        EXPECT_TRUE(AssetStreamingSampleTestAccess::IsFallbackBaselineEligible(
            true, false));
    }

    TEST(AssetStreamingSampleValidation,
         TexturePublicationDistinguishesPendingCommitAndInvalidTerminalStates)
    {
        constexpr uint8 Pending = 0;
        constexpr uint8 Committed = 1;
        constexpr uint8 Invalid = 2;

        EXPECT_EQ(AssetStreamingSampleTestAccess::ClassifyTexturePublication(
                      RenderResourcePublicState::Reserved, 0),
                  Pending);
        EXPECT_EQ(AssetStreamingSampleTestAccess::ClassifyTexturePublication(
                      RenderResourcePublicState::UploadQueued, 0),
                  Pending);
        EXPECT_EQ(AssetStreamingSampleTestAccess::ClassifyTexturePublication(
                      RenderResourcePublicState::Uploading, 0),
                  Pending);
        EXPECT_EQ(AssetStreamingSampleTestAccess::ClassifyTexturePublication(
                      RenderResourcePublicState::GPUReady, 9),
                  Committed);

        EXPECT_EQ(AssetStreamingSampleTestAccess::ClassifyTexturePublication(
                      RenderResourcePublicState::GPUReady, 0),
                  Invalid);
        EXPECT_EQ(AssetStreamingSampleTestAccess::ClassifyTexturePublication(
                      RenderResourcePublicState::Failed, 0),
                  Invalid);
        EXPECT_EQ(AssetStreamingSampleTestAccess::ClassifyTexturePublication(
                      RenderResourcePublicState::Released, 0),
                  Invalid);
        EXPECT_EQ(AssetStreamingSampleTestAccess::ClassifyTexturePublication(
                      RenderResourcePublicState::Uploading, 0, false),
                  Invalid);
    }

    TEST(AssetStreamingSampleValidation,
         StreamingFallbackReceiptIsCachedBeforeCoordinatorConfirmation)
    {
        const std::array<bool, 3> observed =
            AssetStreamingSampleTestAccess::
                ObserveFallbackReceiptBeforeCoordinatorConfirmation();
        EXPECT_TRUE(observed[0]);
        EXPECT_FALSE(observed[1]);
        EXPECT_TRUE(observed[2]);
    }

    TEST(AssetStreamingSampleValidation,
         ExactPresentedTextureReceiptsRejectWrongHandleRevisionOldPresentationAndUnchangedKey)
    {
        const RenderResourceHandle texture{71, 3};
        EXPECT_TRUE(AssetStreamingSampleTestAccess::HasReadyTextureReceipt(
            texture, 10, 20, 30, 1, 11, 21, 31, 2));
        EXPECT_FALSE(AssetStreamingSampleTestAccess::HasReadyTextureReceipt(
            RenderResourceHandle{}, 10, 20, 30, 1, 11, 21, 31, 2));
        EXPECT_FALSE(AssetStreamingSampleTestAccess::HasReadyTextureReceipt(
            texture, 10, 20, 30, 1, 10, 21, 31, 2));
        EXPECT_FALSE(AssetStreamingSampleTestAccess::HasReadyTextureReceipt(
            texture, 10, 20, 30, 1, 11, 20, 31, 2));
        EXPECT_FALSE(AssetStreamingSampleTestAccess::HasReadyTextureReceipt(
            texture, 10, 20, 30, 1, 11, 21, 30, 2));
        EXPECT_FALSE(AssetStreamingSampleTestAccess::HasReadyTextureReceipt(
            texture, 10, 20, 30, 1, 11, 21, 31, 2, false));
    }

    TEST(AssetStreamingSampleValidation,
         ReadyTextureReceiptRequiresTheSameCompletedPresentationWithNoSceneObjectRebuild)
    {
        EXPECT_TRUE(
            AssetStreamingSampleTestAccess::IsReadyTextureReceiptFrameQualified(
                true, true, false, 42, 42, 42, 42, 9, 9, 0, 0));
        EXPECT_FALSE(
            AssetStreamingSampleTestAccess::IsReadyTextureReceiptFrameQualified(
                false, true, false, 42, 42, 42, 42, 9, 9, 0, 0));
        EXPECT_FALSE(
            AssetStreamingSampleTestAccess::IsReadyTextureReceiptFrameQualified(
                true, false, false, 42, 42, 42, 42, 9, 9, 0, 0));
        EXPECT_FALSE(
            AssetStreamingSampleTestAccess::IsReadyTextureReceiptFrameQualified(
                true, true, true, 42, 42, 42, 42, 9, 9, 0, 0));
        EXPECT_FALSE(
            AssetStreamingSampleTestAccess::IsReadyTextureReceiptFrameQualified(
                true, true, false, 42, 41, 42, 42, 9, 9, 0, 0));
        EXPECT_FALSE(
            AssetStreamingSampleTestAccess::IsReadyTextureReceiptFrameQualified(
                true, true, false, 42, 42, 42, 41, 9, 9, 0, 0));
        EXPECT_FALSE(
            AssetStreamingSampleTestAccess::IsReadyTextureReceiptFrameQualified(
                true, true, false, 42, 42, 42, 42, 10, 9, 0, 0));
        EXPECT_FALSE(
            AssetStreamingSampleTestAccess::IsReadyTextureReceiptFrameQualified(
                true, true, false, 42, 42, 42, 42, 9, 9, 1, 0));
        EXPECT_FALSE(
            AssetStreamingSampleTestAccess::IsReadyTextureReceiptFrameQualified(
                true, true, false, 42, 42, 42, 42, 9, 9, 0, 1));
    }

    TEST(AssetStreamingSampleValidation,
         ReloadWithoutLiveInstanceRootOrRevisionCannotComplete)
    {
        EXPECT_FALSE(AssetStreamingSampleTestAccess::IsReloadInstanceReady(
            true, false, true, 8));
        EXPECT_FALSE(AssetStreamingSampleTestAccess::IsReloadInstanceReady(
            true, true, false, 8));
        EXPECT_FALSE(AssetStreamingSampleTestAccess::IsReloadInstanceReady(
            true, true, true, 0));
        EXPECT_TRUE(AssetStreamingSampleTestAccess::IsReloadInstanceReady(
            true, true, true, 8));
    }

    TEST(AssetStreamingSampleValidation,
         NormalizedGroundedPlacementRequiresFiniteTargetExtentAndGroundContact)
    {
        const AABB source(Vec3(-18.0f, -2.0f, -11.0f),
                          Vec3(19.0f, 14.0f, 12.0f));
        const AABB placed(Vec3(-1.30f, 0.0f, -0.81f),
                          Vec3(1.30f, 1.12f, 0.81f));
        EXPECT_TRUE(AssetStreamingSampleTestAccess::IsNormalizedGroundedPlacement(
            source, placed, 2.60f));
        EXPECT_FALSE(AssetStreamingSampleTestAccess::IsNormalizedGroundedPlacement(
            source,
            AABB(Vec3(-1.30f, 0.10f, -0.81f),
                 Vec3(1.30f, 1.22f, 0.81f)),
            2.60f));
        EXPECT_FALSE(AssetStreamingSampleTestAccess::IsNormalizedGroundedPlacement(
            source, placed, 0.0f));
    }

    TEST(AssetStreamingSampleValidation,
         ReloadedSponzaMustReproduceTheOriginalPresentationBounds)
    {
        const AABB initial(Vec3(2.10f, 0.0f, -0.81f),
                           Vec3(4.70f, 1.12f, 0.81f));
        EXPECT_TRUE(AssetStreamingSampleTestAccess::ArePresentationBoundsEquivalent(
            initial,
            AABB(Vec3(2.101f, 0.0f, -0.81f),
                 Vec3(4.701f, 1.12f, 0.81f))));
        EXPECT_FALSE(AssetStreamingSampleTestAccess::ArePresentationBoundsEquivalent(
            initial,
            AABB(Vec3(2.60f, 0.0f, -0.81f),
                 Vec3(5.20f, 1.12f, 0.81f))));
    }

    TEST(AssetStreamingSampleValidation,
         SmokeQualificationNeverConsumesOrbitInput)
    {
        EXPECT_FALSE(AssetStreamingSampleTestAccess::ShouldConsumeOrbitInput(
            true, true, true, true));
        EXPECT_FALSE(AssetStreamingSampleTestAccess::ShouldConsumeOrbitInput(
            false, false, true, true));
        EXPECT_FALSE(AssetStreamingSampleTestAccess::ShouldConsumeOrbitInput(
            false, true, false, true));
        EXPECT_FALSE(AssetStreamingSampleTestAccess::ShouldConsumeOrbitInput(
            false, true, true, false));
        EXPECT_TRUE(AssetStreamingSampleTestAccess::ShouldConsumeOrbitInput(
            false, true, true, true));
    }

    TEST(AssetStreamingSampleValidation,
         TerminalQueueDrainRejectsEveryOutstandingResourceQueue)
    {
        const Resource::ResourceDiagnosticsSnapshot baseline;
        EXPECT_TRUE(AssetStreamingSampleTestAccess::AreTerminalQueuesDrained(
            baseline));

        using TerminalQueueMember =
            uint64 Resource::ResourceDiagnosticsSnapshot::*;
        const std::array<TerminalQueueMember, 12> terminalQueues = {
            &Resource::ResourceDiagnosticsSnapshot::activeOperations,
            &Resource::ResourceDiagnosticsSnapshot::activeSubscribers,
            &Resource::ResourceDiagnosticsSnapshot::pendingAsyncJobs,
            &Resource::ResourceDiagnosticsSnapshot::pendingAsyncCompletions,
            &Resource::ResourceDiagnosticsSnapshot::decodeQueuedCount,
            &Resource::ResourceDiagnosticsSnapshot::decodeActiveCount,
            &Resource::ResourceDiagnosticsSnapshot::pendingPublicationCount,
            &Resource::ResourceDiagnosticsSnapshot::pendingUploadCount,
            &Resource::ResourceDiagnosticsSnapshot::pendingReplacementCount,
            &Resource::ResourceDiagnosticsSnapshot::pendingRollbackCount,
            &Resource::ResourceDiagnosticsSnapshot::pendingRetirementCount,
            &Resource::ResourceDiagnosticsSnapshot::queuedLeaseUnloadCount};
        for (const TerminalQueueMember queue : terminalQueues)
        {
            Resource::ResourceDiagnosticsSnapshot outstanding = baseline;
            outstanding.*queue = 1;
            EXPECT_FALSE(
                AssetStreamingSampleTestAccess::AreTerminalQueuesDrained(
                    outstanding));
        }
    }

    TEST(AssetStreamingSampleValidation,
         ExactOldSponzaRetirementRejectsLifecycleOrRequestResidue)
    {
        const auto isExact =
            [](bool cancellationValid,
               bool oldLifecycleHandleValid,
               bool oldResourceRequestValid,
               bool cancellationResourceRequestValid,
               bool lifecycleHandleMatches,
               bool resourceRequestMatches,
               bool retirementComplete)
            {
                return AssetStreamingSampleTestAccess::
                    IsExactOldSponzaRetirementSatisfied(
                        cancellationValid,
                        oldLifecycleHandleValid,
                        oldResourceRequestValid,
                        cancellationResourceRequestValid,
                        lifecycleHandleMatches,
                        resourceRequestMatches,
                        retirementComplete);
            };

        EXPECT_TRUE(isExact(true, true, true, true, true, true, true));
        EXPECT_FALSE(isExact(false, true, true, true, true, true, true));
        EXPECT_FALSE(isExact(true, false, true, true, true, true, true));
        EXPECT_FALSE(isExact(true, true, false, true, true, true, true));
        EXPECT_FALSE(isExact(true, true, true, false, true, true, true));
        EXPECT_FALSE(isExact(true, true, true, true, false, true, true));
        EXPECT_FALSE(isExact(true, true, true, true, true, false, true));
        EXPECT_FALSE(isExact(true, true, true, true, true, true, false));
    }

    TEST(AssetStreamingSampleValidation,
         ForcedRenderPathsRejectFallbackOrUnexpectedIndirectExecution)
    {
        SampleRenderDiagnostics gpuFallback;
        gpuFallback.gpuDrivenPolicyDecisionAvailable = true;
        gpuFallback.gpuDrivenRequestedMode = "ForceEnabled";
        gpuFallback.gpuDrivenPolicyReason = "Unsupported";
        gpuFallback.gpuDrivenEnabled = false;
        gpuFallback.opaqueExecutionCompleted = true;
        gpuFallback.gpuDrivenOpaqueDirectDrawCount = 1;
        EXPECT_FALSE(AssetStreamingSampleTestAccess::IsRequestedRenderPathExecuted(
            SampleRenderPath::GPUDriven, gpuFallback));

        SampleRenderDiagnostics directWithIndirect;
        directWithIndirect.gpuDrivenPolicyDecisionAvailable = true;
        directWithIndirect.gpuDrivenRequestedMode = "ForceDisabled";
        directWithIndirect.gpuDrivenPolicyReason = "ForcedDisabled";
        directWithIndirect.gpuDrivenEnabled = false;
        directWithIndirect.gpuDrivenOpaqueIndirectRequested = true;
        directWithIndirect.gpuDrivenOpaqueIndirectEligible = true;
        directWithIndirect.gpuDrivenOpaqueIndirectSubmitted = true;
        directWithIndirect.gpuDrivenOpaqueIndirectBatchCount = 1;
        directWithIndirect.gpuDrivenOpaqueIndirectDrawUpperBound = 1;
        directWithIndirect.gpuDrivenOpaqueDirectDrawCount = 1;
        directWithIndirect.opaqueExecutionCompleted = true;
        EXPECT_FALSE(AssetStreamingSampleTestAccess::IsRequestedRenderPathExecuted(
            SampleRenderPath::Direct, directWithIndirect));
    }
} // namespace RVX
