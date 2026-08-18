#include "Scenes/RenderingStressSample.h"

#include "Samples/SampleCLI.h"
#include "Scene/ECS/RenderFragments.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace RVX
{
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

        RenderingStressDiagnosticsSnapshot MakeBaseline()
        {
            RenderingStressDiagnosticsSnapshot snapshot;
            snapshot.available = true;
            snapshot.sceneWorkAvailable = true;
            snapshot.gpuSceneAvailable = true;
            snapshot.extractionAvailable = true;
            snapshot.acceptedExtractionDiagnosticsAvailable = true;
            snapshot.acceptedExtractionPublicationCount = 1u;
            snapshot.acceptedExtractionLastSourceFrameSequence = 1u;
            snapshot.acceptedExtractionLastSceneRevision = 1u;
            snapshot.sourceFrameSequence = 1u;
            snapshot.renderSceneAppliedRevision = 1u;
            snapshot.renderSceneRequiredRevision = 1u;
            snapshot.mutationEvidence.available = true;
            snapshot.mutationEvidence.evidenceEpoch = 1u;
            snapshot.mutationEvidence.completedPresentationCount = 1u;
            snapshot.mutationEvidence.completedFrameSequence = 1u;
            snapshot.mutationEvidence.appliedSceneRevision = 1u;
            snapshot.mutationEvidence.requiredSceneRevision = 1u;
            snapshot.mutationEvidence.gpuCullingOwnerCount = 2u;
            snapshot.mutationEvidence.directRasterOwnerCount = 3u;
            snapshot.sceneFullRebuildCount = 1;
            snapshot.sceneIncrementalUpdateCount = 4;
            snapshot.sceneStaticReuseCount = 12;
            snapshot.drawPacketBuildCount = 8;
            snapshot.drawPacketInvalidationCount = 3;
            return snapshot;
        }

        void AdvanceAcceptedExtraction(
            RenderingStressDiagnosticsSnapshot& snapshot,
            uint64 changeFeedCount,
            uint64 actorRebuildCount,
            uint64 proxyVisitCount,
            uint64 fullScanCount = 0u,
            uint64 componentVisitCount = 0u,
            uint64 featureProviderVisitCount = 0u,
            uint64 continuityLossCount = 0u)
        {
            ++snapshot.acceptedExtractionPublicationCount;
            ++snapshot.acceptedExtractionLastSourceFrameSequence;
            ++snapshot.acceptedExtractionLastSceneRevision;
            snapshot.acceptedExtractionCumulativeFullScanCount += fullScanCount;
            snapshot.acceptedExtractionCumulativeChangeFeedChangeCount +=
                changeFeedCount;
            snapshot.acceptedExtractionCumulativeActorRebuildCount +=
                actorRebuildCount;
            snapshot.acceptedExtractionCumulativeProxyVisitCount +=
                proxyVisitCount;
            snapshot.acceptedExtractionCumulativeComponentVisitCount +=
                componentVisitCount;
            snapshot.acceptedExtractionCumulativeFeatureProviderVisitCount +=
                featureProviderVisitCount;
            snapshot.acceptedExtractionContinuityLossCount +=
                continuityLossCount;
        }

        void AdvanceGPUSceneSubmittedUploadedRows(
            RenderingStressDiagnosticsSnapshot& snapshot,
            uint64 rowCount)
        {
            for (uint64& tableRows :
                 snapshot.mutationEvidence.gpuSceneSubmittedUploadedRowCount)
            {
                tableRows += rowCount;
            }
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

    TEST(RenderingStressSampleValidation, ConsumesExactPublicWorkloadProfiles)
    {
        RenderingStressSample sample;
        const SampleInfo& info = sample.GetInfo();
        const SampleWorkloadProfile pr =
            BuildSampleWorkloadProfile(SampleWorkloadScale::PullRequest);
        const SampleWorkloadProfile nightly =
            BuildSampleWorkloadProfile(SampleWorkloadScale::Nightly);
        const SampleWorkloadProfile qualification =
            BuildSampleWorkloadProfile(SampleWorkloadScale::Qualification);

        EXPECT_EQ(info.id, "rendering-stress");
        EXPECT_EQ(info.defaultAssetId, "interior-rendering-p0a");
        EXPECT_TRUE(info.supportsRenderPathSelection);
        EXPECT_TRUE(info.supportsWorkloadScaleSelection);
        EXPECT_EQ(info.defaultWorkloadScale, SampleWorkloadScale::PullRequest);
        EXPECT_EQ(pr.objectCount, 1000u);
        EXPECT_EQ(pr.dirtyObjectCount, 10u);
        EXPECT_EQ(pr.churnObjectCount, 100u);
        EXPECT_EQ(pr.gpuCullingCapacity, 2048u);
        EXPECT_EQ(nightly.objectCount, 10000u);
        EXPECT_EQ(nightly.dirtyObjectCount, 100u);
        EXPECT_EQ(nightly.churnObjectCount, 1000u);
        EXPECT_EQ(qualification.objectCount, 100000u);
        EXPECT_EQ(qualification.dirtyObjectCount, 1000u);
        EXPECT_EQ(qualification.churnObjectCount, 10000u);
        EXPECT_EQ(qualification.gpuCullingCapacity, 131072u);

        const std::string source = ReadSource(
            "Samples/RenderVerseSamples/Scenes/RenderingStressSample.cpp");
        ASSERT_FALSE(source.empty());
        EXPECT_NE(source.find("case MutationActionKind::Dirty: return 4u"),
                  std::string::npos);
        EXPECT_NE(source.find("case MutationActionKind::Remove: return 2u"),
                  std::string::npos);
        EXPECT_NE(source.find("case MutationActionKind::Add: return 8u"),
                  std::string::npos);
        EXPECT_NE(source.find("TryMultiplyMutationCount"), std::string::npos);
    }

    TEST(RenderingStressSampleValidation,
         KeepsDistanceCullingEnabledAcrossTheFullWorkloadCameraRange)
    {
        constexpr float32 Spacing = 1.75f;
        constexpr float32 DefaultDrawDistance = 1000.0f;
        constexpr float32 MinimumWorkloadFarDistance = 5000.0f;
        // interior-rendering-p0a mesh 0 is a unit cube centered at the origin;
        // workload actors apply no scale or rotation.
        constexpr float32 UnitCubeBoundsRadius = 0.8660254f;
        const auto getExtent = [](uint32 objectCount)
        {
            uint32 extent = 1u;
            while (static_cast<uint64>(extent) * extent < objectCount)
                ++extent;
            return extent;
        };
        const auto getSpan = [=](uint32 objectCount)
        {
            return static_cast<float32>(getExtent(objectCount)) * Spacing;
        };
        const auto getFarDistance = [=](uint32 objectCount)
        {
            return std::max(MinimumWorkloadFarDistance,
                            getSpan(objectCount) * 6.0f);
        };
        const auto getFarthestVisibilityBoundary = [=](uint32 objectCount)
        {
            const uint32 extent = getExtent(objectCount);
            const float32 span = getSpan(objectCount);
            const float32 center = static_cast<float32>(extent - 1u) * 0.5f;
            const float32 cameraY = span * 0.85f;
            const float32 cameraZ = span * 1.35f;
            float32 farthestDistance = 0.0f;
            for (uint32 index = 0; index < objectCount; ++index)
            {
                const uint32 row = index / extent;
                const uint32 column = index % extent;
                const float32 x =
                    (static_cast<float32>(column) - center) * Spacing;
                const float32 z =
                    (center - static_cast<float32>(row)) * Spacing;
                const float32 cameraToBoundsCenterDistance = std::sqrt(
                    x * x +
                    cameraY * cameraY +
                    (cameraZ - z) * (cameraZ - z));
                farthestDistance = std::max(
                    farthestDistance,
                    cameraToBoundsCenterDistance - UnitCubeBoundsRadius);
            }
            return farthestDistance;
        };

        const SampleWorkloadProfile pr =
            BuildSampleWorkloadProfile(SampleWorkloadScale::PullRequest);
        const SampleWorkloadProfile nightly =
            BuildSampleWorkloadProfile(SampleWorkloadScale::Nightly);
        const SampleWorkloadProfile qualification =
            BuildSampleWorkloadProfile(SampleWorkloadScale::Qualification);
        const auto expectWorkloadIsWithinCullingRange = [=](
                                                         const SampleWorkloadProfile& profile)
        {
            const float32 farDistance = getFarDistance(profile.objectCount);
            EXPECT_FLOAT_EQ(farDistance,
                            std::max(MinimumWorkloadFarDistance,
                                     getSpan(profile.objectCount) * 6.0f));
            EXPECT_LT(getFarthestVisibilityBoundary(profile.objectCount),
                      farDistance);
        };
        expectWorkloadIsWithinCullingRange(pr);
        expectWorkloadIsWithinCullingRange(nightly);
        expectWorkloadIsWithinCullingRange(qualification);
        EXPECT_LT(DefaultDrawDistance,
                  getFarthestVisibilityBoundary(qualification.objectCount));

        const std::string source = ReadSource(
            "Samples/RenderVerseSamples/Scenes/RenderingStressSample.cpp");
        ASSERT_FALSE(source.empty());
        const size_t setupFarDistance = source.find(
            "context.renderSettings.gpuCulling.maxDrawDistance =");
        ASSERT_NE(setupFarDistance, std::string::npos);
        const size_t workloadFarDistance = source.find(
            "GetWorkloadFarDistance(m_workload.objectCount);",
            setupFarDistance);
        ASSERT_NE(workloadFarDistance, std::string::npos);
        EXPECT_LT(workloadFarDistance - setupFarDistance, 160u);
        EXPECT_NE(source.find("gpuCulling.enableDistanceCulling = true;"),
                  std::string::npos);
        const size_t configureCamera = source.find(
            "void RenderingStressSample::ConfigureCameraForWorkload");
        EXPECT_NE(configureCamera, std::string::npos);
        EXPECT_NE(source.find("const AABB workloadBounds(", configureCamera),
                  std::string::npos);
        EXPECT_NE(source.find("settings.bounds = workloadBounds;",
                              configureCamera),
                  std::string::npos);
        EXPECT_NE(source.find("m_orbitCamera.Initialize(settings",
                              configureCamera),
                  std::string::npos);
    }

    TEST(RenderingStressSampleValidation,
         SelectsExactUniqueDeterministic100KDirtyAndChurnSets)
    {
        constexpr uint32 ObjectCount = 100000u;
        const std::vector<uint32> dirty =
            RenderingStressSample::BuildDeterministicIndexSet(
                ObjectCount, 1000u, 0u);
        const std::vector<uint32> churn =
            RenderingStressSample::BuildDeterministicIndexSet(
                ObjectCount, 10000u, ObjectCount / 3u);

        EXPECT_EQ(dirty.size(), 1000u);
        EXPECT_EQ(churn.size(), 10000u);
        EXPECT_EQ(dirty,
                  RenderingStressSample::BuildDeterministicIndexSet(
                      ObjectCount, 1000u, 0u));
        EXPECT_EQ(churn,
                  RenderingStressSample::BuildDeterministicIndexSet(
                      ObjectCount, 10000u, ObjectCount / 3u));
        const std::set<uint32> uniqueDirty(dirty.begin(), dirty.end());
        const std::set<uint32> uniqueChurn(churn.begin(), churn.end());
        EXPECT_EQ(uniqueDirty.size(), dirty.size());
        EXPECT_EQ(uniqueChurn.size(), churn.size());
        EXPECT_TRUE(std::all_of(dirty.begin(), dirty.end(), [](uint32 index)
                                { return index < ObjectCount; }));
        EXPECT_TRUE(std::all_of(churn.begin(), churn.end(), [](uint32 index)
                                { return index < ObjectCount; }));
    }

    TEST(RenderingStressSampleValidation,
         PartitionsQualificationChurnIntoExactBoundedNonOverlappingBatches)
    {
        constexpr uint32 ChurnCount = 10000u;
        const std::vector<RenderingStressChurnBatch> batches =
            RenderingStressSample::BuildChurnBatchPlan(ChurnCount);
        const std::vector<uint32> churn =
            RenderingStressSample::BuildDeterministicIndexSet(
                100000u, ChurnCount, 100000u / 3u);

        ASSERT_EQ(RenderingStressSample::GetDeterministicChurnBatchSize(),
                  256u);
        ASSERT_EQ(batches.size(), 40u);
        ASSERT_EQ(batches.back().count, 16u);

        std::set<uint32> observedIndices;
        uint32 expectedOffset = 0;
        uint32 accumulatedCount = 0;
        for (const RenderingStressChurnBatch batch : batches)
        {
            EXPECT_EQ(batch.offset, expectedOffset);
            EXPECT_GT(batch.count, 0u);
            EXPECT_LE(batch.count,
                      RenderingStressSample::GetDeterministicChurnBatchSize());
            ASSERT_LE(batch.offset + batch.count, churn.size());
            for (uint32 offset = 0; offset < batch.count; ++offset)
            {
                EXPECT_TRUE(observedIndices.insert(
                                churn[batch.offset + offset])
                                .second);
            }
            expectedOffset += batch.count;
            accumulatedCount += batch.count;
        }
        EXPECT_EQ(accumulatedCount, ChurnCount);
        EXPECT_EQ(observedIndices.size(), ChurnCount);
    }

    TEST(RenderingStressSampleValidation,
         DeclaresIncrementalExtractionAndUploadAuthenticityContract)
    {
        RenderingStressSample sample;
        const SampleAssessmentContract contract = sample.GetAssessmentContract();

        EXPECT_TRUE(contract.IsValid());
        EXPECT_EQ(contract.revision, "9");
        EXPECT_TRUE(ContainsCode(
            contract.capabilities, "RENDER.CAPABILITY.INCREMENTAL_EXTRACTION"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics, "RENDER.STRESS.METRIC.EXTRACTION_FULL_SCAN_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics, "RENDER.STRESS.METRIC.EXTRACTION_CHANGE_FEED_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics, "RENDER.STRESS.METRIC.EXTRACTION_ACTOR_REBUILD_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics, "RENDER.STRESS.METRIC.EXTRACTION_PROXY_VISIT_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics, "RENDER.STRESS.METRIC.EXTRACTION_COMPONENT_VISIT_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics, "RENDER.STRESS.METRIC.EXTRACTION_PROVIDER_VISIT_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.ACCEPTED_EXTRACTION_PUBLICATION_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.ACCEPTED_EXTRACTION_CUMULATIVE_CHANGE_FEED_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.ACCEPTED_EXTRACTION_CUMULATIVE_ACTOR_REBUILD_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.ACCEPTED_EXTRACTION_CUMULATIVE_PROXY_VISIT_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.ACCEPTED_EXTRACTION_CONTINUITY_LOSS_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.MUTATION_ACTION_TARGET_SCENE_REVISION"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.MUTATION_COMPLETED_FRAME_SEQUENCE"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.MUTATION_SCENE_REMOVED_OBJECT_DELTA"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.DIRECT_RASTER_INSTANCE_UPLOAD_BYTES"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.DIRECT_RASTER_INSTANCE_INDEX_UPLOAD_BYTES"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.DIRECT_RASTER_INSTANCE_PATCHED_ROW_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.DIRECT_RASTER_INDEX_PATCHED_ROW_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.DIRECT_RASTER_ACTIVE_INSTANCE_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.DIRECT_RASTER_ACTIVE_INSTANCE_CAPACITY"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.DIRECT_RASTER_INSTANCE_FULL_MATERIALIZATION_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.DIRECT_RASTER_INDEX_FULL_MATERIALIZATION_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.GPU_DRIVEN_ACTIVE_ROW_UPLOAD_BYTES"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.GPU_DRIVEN_ACTIVE_ROW_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.GPU_DRIVEN_ACTIVE_ROW_HIGH_WATERMARK"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.GPU_DRIVEN_INSTANCE_PATCHED_ROW_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.GPU_DRIVEN_CANDIDATE_PATCHED_ROW_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.GPU_DRIVEN_ACTIVE_ROW_PATCHED_ROW_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.GPU_DRIVEN_INSTANCE_FULL_MATERIALIZATION_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.GPU_DRIVEN_CANDIDATE_FULL_MATERIALIZATION_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.GPU_DRIVEN_ACTIVE_ROW_FULL_MATERIALIZATION_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.GPU_DRIVEN_CONTINUITY_FULL_MATERIALIZATION_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.GPU_DRIVEN_CAPACITY_FULL_MATERIALIZATION_COUNT"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.CHURN_DESTROYED_TOTAL"));
        EXPECT_TRUE(ContainsCode(
            contract.metrics,
            "RENDER.STRESS.METRIC.CHURN_RECREATED_TOTAL"));
    }

    TEST(RenderingStressSampleValidation, StaticWindowRejectsAnyRetainedOrGPUWork)
    {
        const RenderingStressDiagnosticsSnapshot before = MakeBaseline();
        RenderingStressDiagnosticsSnapshot after = before;
        ++after.sceneStaticReuseCount;

        EXPECT_TRUE(RenderingStressSample::EvaluateStaticWindow(before, after)
                        .passed);

        ++after.drawPacketBuildCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateStaticWindow(before, after)
                         .passed);
        after.drawPacketBuildCount = before.drawPacketBuildCount;
        after.gpuSceneFrameUploadBytes = 64u;
        EXPECT_FALSE(RenderingStressSample::EvaluateStaticWindow(before, after)
                         .passed);
        after.gpuSceneFrameUploadBytes = 0;
        after.gpuSceneNoOpCount = 1u;
        EXPECT_FALSE(RenderingStressSample::EvaluateStaticWindow(before, after)
                         .passed);
        after.gpuSceneNoOpCount = 0u;
        after.directRasterInstanceUploadBytes = 4096u;
        EXPECT_FALSE(RenderingStressSample::EvaluateStaticWindow(before, after)
                         .passed);
        after.directRasterInstanceUploadBytes = 0;
        after.gpuDrivenCandidateUploadBytes = 4096u;
        EXPECT_FALSE(RenderingStressSample::EvaluateStaticWindow(before, after)
                         .passed);
        after.gpuDrivenCandidateUploadBytes = 0;
        after.gpuDrivenActiveRowUploadBytes = 4096u;
        EXPECT_FALSE(RenderingStressSample::EvaluateStaticWindow(before, after)
                         .passed);
        after.gpuDrivenActiveRowUploadBytes = 0;
        after.gpuDrivenInstancePatchedRowCount = 1u;
        EXPECT_FALSE(RenderingStressSample::EvaluateStaticWindow(before, after)
                         .passed);
        after.gpuDrivenInstancePatchedRowCount = 0;
        ++after.gpuDrivenCapacityFullMaterializationCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateStaticWindow(before, after)
                         .passed);
        after.gpuDrivenCapacityFullMaterializationCount =
            before.gpuDrivenCapacityFullMaterializationCount;
        after.directRasterIndexPatchedRowCount = 1u;
        EXPECT_FALSE(RenderingStressSample::EvaluateStaticWindow(before, after)
                         .passed);
        after.directRasterIndexPatchedRowCount = 0u;
        ++after.directRasterInstanceFullMaterializationCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateStaticWindow(before, after)
                         .passed);
        after.directRasterInstanceFullMaterializationCount = 0u;
        after.directRasterActiveInstanceCount = 1u;
        EXPECT_FALSE(RenderingStressSample::EvaluateStaticWindow(before, after)
                         .passed);
        after.directRasterActiveInstanceCount = 0u;
        ++after.acceptedExtractionCumulativeProxyVisitCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateStaticWindow(before, after)
                         .passed);
    }

    TEST(RenderingStressSampleValidation,
         InitialStaticWindowAllowsOneExactTemporalSettleThenStrictReuse)
    {
        constexpr uint32 Population = 100u;
        RenderingStressDiagnosticsSnapshot baseline = MakeBaseline();
        baseline.renderSceneObjectCount = Population;
        baseline.gpuScenePublishedObjectCount = Population;
        baseline.drawPacketEntryCount = Population;
        baseline.sceneStaticReuseCount = 1u;
        baseline.mutationEvidence.gpuSceneUpdateCount = Population;

        RenderingStressDiagnosticsSnapshot settle = baseline;
        ++settle.sourceFrameSequence;
        settle.mutationEvidence.completedFrameSequence =
            settle.sourceFrameSequence;
        ++settle.mutationEvidence.completedPresentationCount;
        settle.gpuSceneUpdateCount = Population;
        settle.gpuSceneFrameUploadBytes = 64u;
        settle.directRasterInstanceUploadBytes = 4096u;
        ++settle.mutationEvidence.gpuSceneIncrementalPublicationCount;
        settle.mutationEvidence.gpuSceneUpdateCount += Population;
        settle.mutationEvidence.gpuSceneMaterializedObjectCount += Population;
        ++settle.mutationEvidence.gpuSceneSubmittedUploadCount;
        settle.mutationEvidence.gpuSceneUploadBytes += 64u;
        ++settle.mutationEvidence.gpuSceneUploadRangeCount;
        AdvanceGPUSceneSubmittedUploadedRows(settle, Population);

        const RenderingStressWindowEvaluation accepted =
            RenderingStressSample::EvaluateInitialStaticWindow(
                baseline, settle, Population, false);
        EXPECT_FALSE(accepted.passed);
        EXPECT_TRUE(accepted.pending);
        EXPECT_FALSE(RenderingStressSample::EvaluateStaticWindow(baseline, settle)
                         .passed);

        RenderingStressDiagnosticsSnapshot strict = settle;
        ++strict.sourceFrameSequence;
        strict.mutationEvidence.completedFrameSequence =
            strict.sourceFrameSequence;
        ++strict.mutationEvidence.completedPresentationCount;
        ++strict.sceneStaticReuseCount;
        strict.gpuSceneUpdateCount = 0u;
        strict.gpuSceneFrameUploadBytes = 0u;
        strict.directRasterInstanceUploadBytes = 0u;
        EXPECT_TRUE(RenderingStressSample::EvaluateInitialStaticWindow(
                        settle, strict, Population, true)
                        .passed);
        EXPECT_TRUE(RenderingStressSample::EvaluateStaticWindow(settle, strict)
                        .passed);
    }

    TEST(RenderingStressSampleValidation,
         InitialStaticWindowRejectsRepeatedOrInexactTemporalSettle)
    {
        constexpr uint32 Population = 100u;
        RenderingStressDiagnosticsSnapshot baseline = MakeBaseline();
        baseline.renderSceneObjectCount = Population;
        baseline.gpuScenePublishedObjectCount = Population;
        baseline.drawPacketEntryCount = Population;
        baseline.sceneStaticReuseCount = 1u;
        baseline.mutationEvidence.gpuSceneUpdateCount = Population;

        RenderingStressDiagnosticsSnapshot settle = baseline;
        ++settle.sourceFrameSequence;
        settle.mutationEvidence.completedFrameSequence =
            settle.sourceFrameSequence;
        ++settle.mutationEvidence.completedPresentationCount;
        settle.gpuSceneUpdateCount = Population;
        settle.gpuSceneFrameUploadBytes = 64u;
        ++settle.mutationEvidence.gpuSceneIncrementalPublicationCount;
        settle.mutationEvidence.gpuSceneUpdateCount += Population;
        settle.mutationEvidence.gpuSceneMaterializedObjectCount += Population;
        ++settle.mutationEvidence.gpuSceneSubmittedUploadCount;
        settle.mutationEvidence.gpuSceneUploadBytes += 64u;
        ++settle.mutationEvidence.gpuSceneUploadRangeCount;
        AdvanceGPUSceneSubmittedUploadedRows(settle, Population);
        EXPECT_FALSE(RenderingStressSample::EvaluateInitialStaticWindow(
                         baseline, settle, Population, true)
                         .passed);

        const auto expectRejected = [&](const auto& mutate)
        {
            RenderingStressDiagnosticsSnapshot invalid = settle;
            mutate(invalid);
            const RenderingStressWindowEvaluation evaluation =
                RenderingStressSample::EvaluateInitialStaticWindow(
                    baseline, invalid, Population, false);
            EXPECT_FALSE(evaluation.passed);
            EXPECT_FALSE(evaluation.pending);
        };
        expectRejected([&](auto& invalid)
                       { invalid.gpuSceneUpdateCount = Population - 1u; });
        expectRejected([&](auto& invalid) { invalid.gpuSceneFullUpload = true; });
        expectRejected([&](auto& invalid) { invalid.gpuSceneAddCount = 1u; });
        expectRejected([&](auto& invalid) { invalid.gpuSceneRemoveCount = 1u; });
        expectRejected([&](auto& invalid) { invalid.gpuSceneNoOpCount = 1u; });
        expectRejected([&](auto& invalid) { ++invalid.sceneStaticReuseCount; });
        expectRejected([&](auto& invalid) { ++invalid.drawPacketBuildCount; });
        expectRejected([&](auto& invalid)
                       { ++invalid.acceptedExtractionCumulativeProxyVisitCount; });
        expectRejected([&](auto& invalid) { invalid.mutationEvidence.saturated = true; });
        expectRejected([&](auto& invalid) { ++invalid.mutationEvidence.evidenceEpoch; });
        expectRejected([&](auto& invalid)
                       { ++invalid.mutationEvidence.sceneIncrementalCommitCount; });
        expectRejected([&](auto& invalid)
                       { ++invalid.mutationEvidence.gpuSceneFullPublicationCount; });
        expectRejected([&](auto& invalid)
                       { ++invalid.mutationEvidence.gpuSceneIdentityOnlyPublicationCount; });
        expectRejected([&](auto& invalid)
                       { ++invalid.mutationEvidence.gpuSceneSubmittedUploadCount; });
        expectRejected([&](auto& invalid)
                       { ++invalid.mutationEvidence.gpuSceneSubmittedUploadedRowCount[0]; });
        expectRejected([&](auto& invalid)
                       { invalid.mutationEvidence.gpuSceneUpdateCount =
                             baseline.mutationEvidence.gpuSceneUpdateCount; });
    }

    TEST(RenderingStressSampleValidation,
         CompletedFrameIdentityAndExecutionStreamQuiescenceAreExplicit)
    {
        SampleRenderDiagnostics diagnostics;
        diagnostics.renderSceneFrameSequence = 42u;
        diagnostics.renderSceneAppliedRevision = 23u;
        diagnostics.renderSceneRequiredRevision = 23u;
        diagnostics.mutationEvidence.available = true;
        diagnostics.mutationEvidence.evidenceEpoch = 7u;
        diagnostics.mutationEvidence.completedPresentationCount = 11u;
        diagnostics.mutationEvidence.completedFrameSequence = 42u;
        diagnostics.mutationEvidence.requiredSceneRevision = 23u;
        diagnostics.mutationEvidence.appliedSceneRevision = 23u;
        diagnostics.mutationEvidence.sceneRemovedObjectCount = 256u;
        diagnostics.gpuDrivenActiveRowUploadBytes = 24u;
        diagnostics.gpuDrivenActiveRowCount = 1000u;
        diagnostics.gpuDrivenActiveRowHighWatermark = 1024u;
        diagnostics.gpuDrivenInstancePatchedRowCount = 10u;
        diagnostics.gpuDrivenCandidatePatchedRowCount = 10u;
        diagnostics.gpuDrivenActiveRowPatchedRowCount = 0u;
        diagnostics.gpuDrivenInstanceFullMaterializationCount = 1u;
        diagnostics.gpuDrivenCandidateFullMaterializationCount = 2u;
        diagnostics.gpuDrivenActiveRowFullMaterializationCount = 3u;
        diagnostics.gpuDrivenContinuityFullMaterializationCount = 4u;
        diagnostics.gpuDrivenCapacityFullMaterializationCount = 5u;
        diagnostics.acceptedExtractionDiagnosticsAvailable = true;
        diagnostics.acceptedExtractionPublicationCount = 6u;
        diagnostics.acceptedExtractionLastSourceFrameSequence = 17u;
        diagnostics.acceptedExtractionLastSceneRevision = 23u;
        diagnostics.acceptedExtractionCumulativeFullScanCount = 1u;
        diagnostics.acceptedExtractionCumulativeChangeFeedChangeCount = 18u;
        diagnostics.acceptedExtractionCumulativeActorRebuildCount = 9u;
        diagnostics.acceptedExtractionCumulativeProxyVisitCount = 9u;
        diagnostics.acceptedExtractionCumulativeComponentVisitCount = 2u;
        diagnostics.acceptedExtractionCumulativeFeatureProviderVisitCount = 3u;
        diagnostics.acceptedExtractionContinuityLossCount = 4u;
        RenderingStressDiagnosticsSnapshot snapshot =
            RenderingStressSample::CaptureDiagnostics(diagnostics);
        EXPECT_EQ(snapshot.sourceFrameSequence, 42u);
        EXPECT_EQ(snapshot.renderSceneAppliedRevision, 23u);
        EXPECT_EQ(snapshot.renderSceneRequiredRevision, 23u);
        EXPECT_TRUE(snapshot.mutationEvidence.available);
        EXPECT_EQ(snapshot.mutationEvidence.evidenceEpoch, 7u);
        EXPECT_EQ(snapshot.mutationEvidence.completedPresentationCount, 11u);
        EXPECT_EQ(snapshot.mutationEvidence.sceneRemovedObjectCount, 256u);
        EXPECT_EQ(snapshot.gpuDrivenActiveRowUploadBytes, 24u);
        EXPECT_EQ(snapshot.gpuDrivenActiveRowCount, 1000u);
        EXPECT_EQ(snapshot.gpuDrivenActiveRowHighWatermark, 1024u);
        EXPECT_EQ(snapshot.gpuDrivenInstancePatchedRowCount, 10u);
        EXPECT_EQ(snapshot.gpuDrivenCandidatePatchedRowCount, 10u);
        EXPECT_EQ(snapshot.gpuDrivenActiveRowPatchedRowCount, 0u);
        EXPECT_EQ(snapshot.gpuDrivenInstanceFullMaterializationCount, 1u);
        EXPECT_EQ(snapshot.gpuDrivenCandidateFullMaterializationCount, 2u);
        EXPECT_EQ(snapshot.gpuDrivenActiveRowFullMaterializationCount, 3u);
        EXPECT_EQ(snapshot.gpuDrivenContinuityFullMaterializationCount, 4u);
        EXPECT_EQ(snapshot.gpuDrivenCapacityFullMaterializationCount, 5u);
        EXPECT_TRUE(snapshot.acceptedExtractionDiagnosticsAvailable);
        EXPECT_EQ(snapshot.acceptedExtractionPublicationCount, 6u);
        EXPECT_EQ(snapshot.acceptedExtractionLastSourceFrameSequence, 17u);
        EXPECT_EQ(snapshot.acceptedExtractionLastSceneRevision, 23u);
        EXPECT_EQ(snapshot.acceptedExtractionCumulativeFullScanCount, 1u);
        EXPECT_EQ(snapshot.acceptedExtractionCumulativeChangeFeedChangeCount, 18u);
        EXPECT_EQ(snapshot.acceptedExtractionCumulativeActorRebuildCount, 9u);
        EXPECT_EQ(snapshot.acceptedExtractionCumulativeProxyVisitCount, 9u);
        EXPECT_EQ(snapshot.acceptedExtractionCumulativeComponentVisitCount, 2u);
        EXPECT_EQ(snapshot.acceptedExtractionCumulativeFeatureProviderVisitCount, 3u);
        EXPECT_EQ(snapshot.acceptedExtractionContinuityLossCount, 4u);
        EXPECT_FALSE(
            RenderingStressSample::AreExecutionStreamsQuiescent(snapshot));

        snapshot.gpuDrivenActiveRowUploadBytes = 0;
        EXPECT_FALSE(
            RenderingStressSample::AreExecutionStreamsQuiescent(snapshot));
        snapshot.gpuDrivenInstancePatchedRowCount = 0;
        snapshot.gpuDrivenCandidatePatchedRowCount = 0;
        snapshot.gpuDrivenActiveRowPatchedRowCount = 0;
        // Full-materialization counters are cumulative; unchanged historical
        // values do not mean that the current frame performed work.
        EXPECT_TRUE(
            RenderingStressSample::AreExecutionStreamsQuiescent(snapshot));
        snapshot.gpuDrivenActiveRowUploadBytes = 48u;
        EXPECT_FALSE(
            RenderingStressSample::AreExecutionStreamsQuiescent(snapshot));
        snapshot.gpuDrivenActiveRowUploadBytes = 0;
        snapshot.gpuDrivenCandidateUploadBytes = 48u;
        EXPECT_FALSE(
            RenderingStressSample::AreExecutionStreamsQuiescent(snapshot));
        snapshot.gpuDrivenCandidateUploadBytes = 0;
        snapshot.directRasterInstanceUploadBytes = 224u;
        EXPECT_FALSE(
            RenderingStressSample::AreExecutionStreamsQuiescent(snapshot));
        snapshot.directRasterInstanceUploadBytes = 0;
        snapshot.directRasterIndexPatchedRowCount = 1u;
        EXPECT_FALSE(
            RenderingStressSample::AreExecutionStreamsQuiescent(snapshot));
        snapshot.directRasterIndexPatchedRowCount = 0;
        snapshot.directRasterIndexFullMaterializationCount = 1u;
        EXPECT_FALSE(
            RenderingStressSample::AreExecutionStreamsQuiescent(snapshot));

        const std::string source = ReadSource(
            "Samples/RenderVerseSamples/Scenes/RenderingStressSample.cpp");
        EXPECT_NE(source.find("MaxStaticStreamWarmupFrames"),
                  std::string::npos);
        EXPECT_NE(source.find("ShouldConsumeDiagnosticObservation"),
                  std::string::npos);
    }

    TEST(RenderingStressSampleValidation,
         MapsPublicRenderPathsAndRejectsUnavailableSelectedExecutionStreams)
    {
        EXPECT_EQ(RenderingStressSample::MapRenderPath(SampleRenderPath::Auto),
                  RenderGPUDrivenMode::Auto);
        EXPECT_EQ(RenderingStressSample::MapRenderPath(SampleRenderPath::Direct),
                  RenderGPUDrivenMode::ForceDisabled);
        EXPECT_EQ(RenderingStressSample::MapRenderPath(
                      SampleRenderPath::GPUDriven),
                  RenderGPUDrivenMode::ForceEnabled);

        SampleRenderDiagnostics diagnostics;
        diagnostics.available = true;
        diagnostics.sceneWorkAvailable = true;
        diagnostics.gpuSceneAvailable = true;
        diagnostics.extractionAvailable = true;
        diagnostics.engineRenderRuntimeAvailable = true;
        diagnostics.acceptedExtractionDiagnosticsAvailable = true;
        diagnostics.renderSceneValuesAvailable = true;
        diagnostics.renderSceneFrameSequence = 1u;
        diagnostics.mutationEvidence.available = true;
        diagnostics.mutationEvidence.evidenceEpoch = 1u;
        diagnostics.mutationEvidence.completedPresentationCount = 1u;
        diagnostics.mutationEvidence.completedFrameSequence = 1u;
        diagnostics.mutationEvidence.appliedSceneRevision = 1u;
        diagnostics.gpuDrivenPolicyDecisionAvailable = true;
        diagnostics.gpuDrivenRequestedMode = "ForceDisabled";
        diagnostics.opaqueExecutionCompleted = true;
        diagnostics.opaqueExecutedDrawCountAvailable = true;
        diagnostics.opaqueExecutedDrawCount = 1u;
        diagnostics.directRasterActiveInstanceCount = 1000u;
        diagnostics.directRasterActiveInstanceCapacity = 1000u;
        EXPECT_TRUE(
            RenderingStressSample::HasBaseInstrumentation(diagnostics));
        EXPECT_TRUE(RenderingStressSample::HasRequiredInstrumentation(
            diagnostics, SampleRenderPath::Direct));
        diagnostics.extractionAvailable = false;
        EXPECT_FALSE(
            RenderingStressSample::HasBaseInstrumentation(diagnostics));
        diagnostics.extractionAvailable = true;
        diagnostics.acceptedExtractionDiagnosticsAvailable = false;
        EXPECT_FALSE(
            RenderingStressSample::HasBaseInstrumentation(diagnostics));
        diagnostics.acceptedExtractionDiagnosticsAvailable = true;

        diagnostics.directRasterActiveInstanceCount = 0u;
        // A completed pre-workload frame is valid base evidence.  The sample
        // must wait for persistent population/slot propagation before treating
        // the still-empty selected execution stream as a permanent gap.
        EXPECT_TRUE(
            RenderingStressSample::HasBaseInstrumentation(diagnostics));
        diagnostics.engineRenderRuntimeAvailable = true;
        diagnostics.renderSceneAppliedRevision = 20u;
        diagnostics.renderSceneRequiredRevision = 9020u;
        diagnostics.engineRequiredSceneRevision = 12000u;
        EXPECT_TRUE(RenderingStressSample::IsWaitingForRequiredSceneRevision(
            diagnostics));
        diagnostics.renderSceneAppliedRevision = 9020u;
        // A newer update-thread snapshot must not invalidate the completed
        // frame's own applied/required provenance pair.
        EXPECT_FALSE(RenderingStressSample::IsWaitingForRequiredSceneRevision(
            diagnostics));
        EXPECT_FALSE(RenderingStressSample::HasRequiredInstrumentation(
            diagnostics, SampleRenderPath::Direct));
        diagnostics.directRasterActiveInstanceCount = 1000u;
        diagnostics.gpuDrivenRequestedMode = "ForceEnabled";
        diagnostics.gpuDrivenEnabled = true;
        diagnostics.gpuDrivenGraphPassAdded = true;
        diagnostics.gpuDrivenGraphPassRecorded = true;
        diagnostics.gpuDrivenExecutionRecorded = true;
        diagnostics.gpuDrivenActiveRowCount = 1000u;
        diagnostics.gpuDrivenActiveRowHighWatermark = 1000u;
        EXPECT_TRUE(RenderingStressSample::HasRequiredInstrumentation(
            diagnostics, SampleRenderPath::GPUDriven));

        diagnostics.gpuDrivenExecutionRecorded = false;
        EXPECT_FALSE(RenderingStressSample::HasRequiredInstrumentation(
            diagnostics, SampleRenderPath::GPUDriven));
    }

    TEST(RenderingStressSampleValidation,
         InitialPopulationDoesNotRequireCoincidentDeltaCounters)
    {
        RenderingStressDiagnosticsSnapshot snapshot = MakeBaseline();
        snapshot.renderSceneObjectCount = 1000u;
        snapshot.gpuScenePublishedObjectCount = 1000u;
        snapshot.drawPacketEntryCount = 1000u;
        snapshot.sceneLastRebuiltObjectCount = 0u;
        snapshot.gpuSceneAddCount = 0u;

        EXPECT_TRUE(RenderingStressSample::HasCompleteInitialPopulation(
            snapshot, 1000u));
        snapshot.gpuScenePublishedObjectCount = 999u;
        EXPECT_FALSE(RenderingStressSample::HasCompleteInitialPopulation(
            snapshot, 1000u));
    }

    TEST(RenderingStressSampleValidation,
         AcceptedOnlySameCompletedFrameDoesNotConsumeStaticObservation)
    {
        const RenderingStressDiagnosticsSnapshot previous = MakeBaseline();
        RenderingStressDiagnosticsSnapshot acceptedOnly = previous;
        ++acceptedOnly.acceptedExtractionPublicationCount;
        ++acceptedOnly.acceptedExtractionLastSourceFrameSequence;
        ++acceptedOnly.acceptedExtractionLastSceneRevision;

        EXPECT_FALSE(RenderingStressSample::ShouldConsumeDiagnosticObservation(
            previous, acceptedOnly, false));
        EXPECT_TRUE(RenderingStressSample::ShouldConsumeDiagnosticObservation(
            previous, acceptedOnly, true));

        ++acceptedOnly.sourceFrameSequence;
        EXPECT_TRUE(RenderingStressSample::ShouldConsumeDiagnosticObservation(
            previous, acceptedOnly, false));
    }

    TEST(RenderingStressSampleValidation,
         AcceptedExtractionCumulativeEvidenceClosesPendingMutationWindows)
    {
        RenderingStressDiagnosticsSnapshot baseline = MakeBaseline();
        baseline.gpuDrivenActiveRowCount = 10u;
        baseline.gpuDrivenActiveRowHighWatermark = 10u;
        baseline.directRasterActiveInstanceCount = 10u;
        baseline.directRasterActiveInstanceCapacity = 10u;

        RenderingStressDiagnosticsSnapshot renderEvidence = baseline;
        ++renderEvidence.sceneIncrementalUpdateCount;
        renderEvidence.sceneLastRebuiltObjectCount = 10u;
        renderEvidence.gpuSceneUpdateCount = 10u;
        renderEvidence.gpuSceneFrameUploadBytes = 64u;
        renderEvidence.gpuDrivenInstancePatchedRowCount = 10u;
        renderEvidence.gpuDrivenCandidatePatchedRowCount = 0u;
        renderEvidence.directRasterInstancePatchedRowCount = 10u;

        const RenderingStressWindowEvaluation pending =
            RenderingStressSample::EvaluateDirtyWindow(
                baseline, renderEvidence, 10u, 10u, 1u, 1u);
        EXPECT_FALSE(pending.passed);
        EXPECT_TRUE(pending.pending);

        RenderingStressDiagnosticsSnapshot actorUnder = renderEvidence;
        AdvanceAcceptedExtraction(actorUnder, 40u, 9u, 10u);
        EXPECT_TRUE(RenderingStressSample::EvaluateDirtyWindow(
                        baseline, actorUnder, 10u, 10u, 1u, 1u)
                        .pending);

        RenderingStressDiagnosticsSnapshot proxyUnder = renderEvidence;
        AdvanceAcceptedExtraction(proxyUnder, 40u, 10u, 9u);
        EXPECT_TRUE(RenderingStressSample::EvaluateDirtyWindow(
                        baseline, proxyUnder, 10u, 10u, 1u, 1u)
                        .pending);

        // Producer/transport acceptance arrives after the completed-frame
        // render evidence. The cumulative proof, rather than a coincident
        // latest-frame extraction sample, closes the window.
        AdvanceAcceptedExtraction(renderEvidence, 40u, 10u, 10u);
        EXPECT_TRUE(RenderingStressSample::EvaluateDirtyWindow(
                        baseline, renderEvidence, 10u, 10u, 1u, 1u)
                        .passed);

        RenderingStressDiagnosticsSnapshot overBudget = renderEvidence;
        ++overBudget.acceptedExtractionCumulativeActorRebuildCount;
        const RenderingStressWindowEvaluation over =
            RenderingStressSample::EvaluateDirtyWindow(
                baseline, overBudget, 10u, 10u, 1u, 1u);
        EXPECT_FALSE(over.passed);
        EXPECT_FALSE(over.pending);

        RenderingStressDiagnosticsSnapshot regressed = renderEvidence;
        --regressed.acceptedExtractionCumulativeProxyVisitCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyWindow(
                         baseline, regressed, 10u, 10u, 1u, 1u)
                         .passed);

        RenderingStressDiagnosticsSnapshot fullScan = renderEvidence;
        ++fullScan.acceptedExtractionCumulativeFullScanCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyWindow(
                         baseline, fullScan, 10u, 10u, 1u, 1u)
                         .passed);

        RenderingStressDiagnosticsSnapshot continuityLoss = renderEvidence;
        ++continuityLoss.acceptedExtractionContinuityLossCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyWindow(
                         baseline, continuityLoss, 10u, 10u, 1u, 1u)
                         .passed);

        RenderingStressDiagnosticsSnapshot componentWork = renderEvidence;
        ++componentWork.acceptedExtractionCumulativeComponentVisitCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyWindow(
                         baseline, componentWork, 10u, 10u, 1u, 1u)
                         .passed);

        RenderingStressDiagnosticsSnapshot providerWork = renderEvidence;
        ++providerWork.acceptedExtractionCumulativeFeatureProviderVisitCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyWindow(
                         baseline, providerWork, 10u, 10u, 1u, 1u)
                         .passed);
    }

    TEST(RenderingStressSampleValidation, DirtyAndChurnWindowsRequireExactDeltas)
    {
        RenderingStressDiagnosticsSnapshot baseline = MakeBaseline();
        baseline.gpuDrivenActiveRowCount = 100000u;
        baseline.gpuDrivenActiveRowHighWatermark = 100000u;
        baseline.directRasterActiveInstanceCount = 100000u;
        baseline.directRasterActiveInstanceCapacity = 100000u;
        RenderingStressDiagnosticsSnapshot dirty = baseline;
        ++dirty.sceneIncrementalUpdateCount;
        dirty.extractionActorRebuildCount = 1000u;
        dirty.extractionProxyVisitCount = 1000u;
        dirty.sceneLastRebuiltObjectCount = 1000u;
        dirty.gpuSceneUpdateCount = 1000u;
        dirty.gpuSceneFrameUploadBytes = 4096u;
        dirty.gpuDrivenInstancePatchedRowCount = 1000u;
        dirty.gpuDrivenCandidatePatchedRowCount = 0u;
        dirty.directRasterInstancePatchedRowCount = 1000u;
        AdvanceAcceptedExtraction(dirty, 4000u, 1000u, 1000u);
        EXPECT_TRUE(RenderingStressSample::EvaluateDirtyWindow(
                        baseline, dirty, 1000u, 100000u, 1u, 1u)
                        .passed);
        dirty.directRasterIndexPatchedRowCount = 100000u;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyWindow(
                         baseline, dirty, 1000u, 100000u, 1u, 1u)
                         .passed);
        dirty.directRasterIndexPatchedRowCount = 0u;
        dirty.directRasterInstancePatchedRowCount = 999u;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyWindow(
                         baseline, dirty, 1000u, 100000u, 1u, 1u)
                         .passed);
        dirty.directRasterInstancePatchedRowCount = 1000u;
        dirty.directRasterActiveInstanceCount = 100001u;
        dirty.directRasterActiveInstanceCapacity = 100001u;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyWindow(
                         baseline, dirty, 1000u, 100000u, 1u, 1u)
                         .passed);
        dirty.directRasterActiveInstanceCount = 100000u;
        dirty.directRasterActiveInstanceCapacity = 99999u;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyWindow(
                         baseline, dirty, 1000u, 100000u, 1u, 1u)
                         .passed);
        dirty.directRasterActiveInstanceCapacity = 100000u;
        dirty.directRasterInstanceFullMaterializationCount = 2u;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyWindow(
                         baseline, dirty, 1000u, 100000u, 1u, 1u)
                         .passed);
        dirty.directRasterInstanceFullMaterializationCount = 0u;
        dirty.directRasterActiveInstanceCount = 0u;
        dirty.directRasterActiveInstanceCapacity = 0u;
        dirty.directRasterInstancePatchedRowCount = 0u;
        dirty.directRasterInstanceUploadBytes = 1u;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyWindow(
                         baseline, dirty, 1000u, 100000u, 1u, 1u)
                         .passed);
        dirty.directRasterInstanceUploadBytes = 0u;
        dirty.gpuDrivenInstancePatchedRowCount = 1001u;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyWindow(
                         baseline, dirty, 1000u, 100000u, 1u, 1u)
                         .passed);
        dirty.gpuDrivenInstancePatchedRowCount = 2000u;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyWindow(
                         baseline, dirty, 1000u, 100000u, 1u, 1u)
                         .passed);
        dirty.gpuDrivenInstancePatchedRowCount = 1000u;
        dirty.gpuDrivenCandidatePatchedRowCount = 1u;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyWindow(
                         baseline, dirty, 1000u, 100000u, 1u, 1u)
                         .passed);
        dirty.gpuDrivenCandidatePatchedRowCount = 0u;
        dirty.gpuDrivenActiveRowPatchedRowCount = 1u;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyWindow(
                         baseline, dirty, 1000u, 100000u, 1u, 1u)
                         .passed);
        dirty.gpuDrivenActiveRowPatchedRowCount = 0u;
        dirty.gpuDrivenContinuityFullMaterializationCount = 2u;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyWindow(
                         baseline, dirty, 1000u, 100000u, 1u, 1u)
                         .passed);
        dirty.gpuDrivenContinuityFullMaterializationCount =
            baseline.gpuDrivenContinuityFullMaterializationCount;
        dirty.gpuSceneUpdateCount = 1001u;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyWindow(
                         baseline, dirty, 1000u, 100000u, 1u, 1u)
                         .passed);
        dirty.gpuSceneUpdateCount = 1000u;
        dirty.gpuSceneNoOpCount = 1u;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyWindow(
                         baseline, dirty, 1000u, 100000u, 1u, 1u)
                         .passed);
        dirty.gpuSceneNoOpCount = 0u;
        ++dirty.acceptedExtractionCumulativeFullScanCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyWindow(
                         baseline, dirty, 1000u, 100000u, 1u, 1u)
                         .passed);

        RenderingStressDiagnosticsSnapshot removed = baseline;
        ++removed.sceneIncrementalUpdateCount;
        removed.extractionActorRebuildCount = 10000u;
        removed.sceneLastRemovedObjectCount = 10000u;
        removed.gpuSceneRemoveCount = 10000u;
        removed.gpuSceneFrameUploadBytes = 8192u;
        removed.gpuDrivenActiveRowCount = 90000u;
        removed.directRasterActiveInstanceCount = 90000u;
        AdvanceAcceptedExtraction(removed, 20000u, 10000u, 0u);
        EXPECT_TRUE(RenderingStressSample::EvaluateChurnRemoveWindow(
                        baseline, removed, 10000u, 100000u, 90000u, 1u, 1u)
                        .passed);
        ++removed.acceptedExtractionCumulativeFullScanCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnRemoveWindow(
                         baseline, removed, 10000u, 100000u, 90000u, 1u, 1u)
                         .passed);
        --removed.acceptedExtractionCumulativeFullScanCount;
        ++removed.acceptedExtractionCumulativeActorRebuildCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnRemoveWindow(
                         baseline, removed, 10000u, 100000u, 90000u, 1u, 1u)
                         .passed);
        --removed.acceptedExtractionCumulativeActorRebuildCount;
        removed.gpuSceneNoOpCount = 1u;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnRemoveWindow(
                         baseline, removed, 10000u, 100000u, 90000u, 1u, 1u)
                         .passed);
        removed.gpuSceneNoOpCount = 0u;

        RenderingStressDiagnosticsSnapshot beforeAdd = removed;
        beforeAdd.extractionActorRebuildCount = 0u;
        beforeAdd.sceneLastRemovedObjectCount = 0u;
        beforeAdd.gpuSceneRemoveCount = 0u;
        beforeAdd.gpuSceneFrameUploadBytes = 0u;
        beforeAdd.gpuDrivenInstanceUploadBytes = 0u;
        beforeAdd.gpuDrivenCandidateUploadBytes = 0u;
        beforeAdd.gpuDrivenActiveRowUploadBytes = 0u;
        beforeAdd.gpuDrivenInstancePatchedRowCount = 0u;
        beforeAdd.gpuDrivenCandidatePatchedRowCount = 0u;
        beforeAdd.gpuDrivenActiveRowPatchedRowCount = 0u;
        beforeAdd.directRasterInstancePatchedRowCount = 0u;
        beforeAdd.directRasterIndexPatchedRowCount = 0u;
        beforeAdd.directRasterInstanceUploadBytes = 0u;
        beforeAdd.directRasterInstanceIndexUploadBytes = 0u;
        beforeAdd.directRasterInstanceFullMaterializationCount = 0u;
        beforeAdd.directRasterIndexFullMaterializationCount = 0u;
        RenderingStressDiagnosticsSnapshot added = beforeAdd;
        ++added.sceneIncrementalUpdateCount;
        added.extractionActorRebuildCount = 10000u;
        added.extractionProxyVisitCount = 10000u;
        added.sceneLastRemovedObjectCount = 0;
        added.sceneLastRebuiltObjectCount = 10000u;
        added.gpuSceneRemoveCount = 0;
        added.gpuSceneAddCount = 10000u;
        added.gpuSceneFrameUploadBytes = 8192u;
        added.gpuDrivenActiveRowCount = 100000u;
        added.directRasterActiveInstanceCount = 100000u;
        AdvanceAcceptedExtraction(added, 80000u, 10000u, 10000u);
        EXPECT_TRUE(RenderingStressSample::EvaluateChurnAddWindow(
                        beforeAdd,
                        added,
                        10000u,
                        90000u,
                        100000u,
                        1u,
                        1u)
                        .passed);
        added.sceneLastRebuiltObjectCount = 10001u;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnAddWindow(
                         beforeAdd,
                         added,
                         10000u,
                         90000u,
                         100000u,
                         1u,
                         1u)
                         .passed);
        added.sceneLastRebuiltObjectCount = 10000u;
        added.gpuSceneNoOpCount = 1u;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnAddWindow(
                         beforeAdd,
                         added,
                         10000u,
                         90000u,
                         100000u,
                         1u,
                         1u)
                         .passed);
        added.gpuSceneNoOpCount = 0u;
        ++added.acceptedExtractionContinuityLossCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnAddWindow(
                         beforeAdd,
                         added,
                         10000u,
                         90000u,
                         100000u,
                         1u,
                         1u)
                         .passed);
    }

    TEST(RenderingStressSampleValidation,
         BoundedChurnWindowsRejectFullScansAndAnyPerBatchCountMismatch)
    {
        constexpr uint32 BatchCount =
            RenderingStressSample::GetDeterministicChurnBatchSize();
        RenderingStressDiagnosticsSnapshot baseline = MakeBaseline();
        baseline.directRasterActiveInstanceCount = 1000u;
        baseline.directRasterActiveInstanceCapacity = 1000u;

        RenderingStressDiagnosticsSnapshot removed = baseline;
        ++removed.sceneIncrementalUpdateCount;
        removed.extractionActorRebuildCount = BatchCount;
        removed.sceneLastRemovedObjectCount = BatchCount;
        removed.gpuSceneRemoveCount = BatchCount;
        removed.gpuSceneFrameUploadBytes = 1024u;
        removed.directRasterActiveInstanceCount = 1000u - BatchCount;
        removed.directRasterActiveInstanceCapacity = 1000u;
        removed.directRasterInstancePatchedRowCount = BatchCount;
        removed.directRasterIndexPatchedRowCount = BatchCount;
        AdvanceAcceptedExtraction(removed, BatchCount * 2u, BatchCount, 0u);
        EXPECT_TRUE(RenderingStressSample::EvaluateChurnRemoveWindow(
                        baseline,
                        removed,
                        BatchCount,
                        1000u,
                        1000u - BatchCount,
                        0u,
                        1u)
                        .passed);

        ++removed.acceptedExtractionCumulativeFullScanCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnRemoveWindow(
                         baseline,
                         removed,
                         BatchCount,
                         1000u,
                         1000u - BatchCount,
                         0u,
                         1u)
                         .passed);
        --removed.acceptedExtractionCumulativeFullScanCount;
        --removed.sceneLastRemovedObjectCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnRemoveWindow(
                         baseline,
                         removed,
                         BatchCount,
                         1000u,
                         1000u - BatchCount,
                         0u,
                         1u)
                         .passed);
        ++removed.sceneLastRemovedObjectCount;
        removed.directRasterIndexPatchedRowCount = 1000u;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnRemoveWindow(
                         baseline,
                         removed,
                         BatchCount,
                         1000u,
                         1000u - BatchCount,
                         0u,
                         1u)
                         .passed);
        removed.directRasterIndexPatchedRowCount = BatchCount;
        removed.gpuDrivenActiveRowCount = 1000u;
        removed.gpuDrivenInstancePatchedRowCount = BatchCount + 1u;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnRemoveWindow(
                         baseline,
                         removed,
                         BatchCount,
                         1000u,
                         1000u - BatchCount,
                         0u,
                         1u)
                         .passed);
        removed.gpuDrivenInstancePatchedRowCount = 0u;
        removed.gpuDrivenActiveRowCount = 0u;

        RenderingStressDiagnosticsSnapshot beforeAdd = removed;
        beforeAdd.extractionActorRebuildCount = 0u;
        beforeAdd.sceneLastRemovedObjectCount = 0u;
        beforeAdd.gpuSceneRemoveCount = 0u;
        beforeAdd.gpuSceneFrameUploadBytes = 0u;
        beforeAdd.directRasterInstancePatchedRowCount = 0u;
        beforeAdd.directRasterIndexPatchedRowCount = 0u;
        RenderingStressDiagnosticsSnapshot added = beforeAdd;
        ++added.sceneIncrementalUpdateCount;
        added.extractionActorRebuildCount = BatchCount;
        added.extractionProxyVisitCount = BatchCount;
        added.sceneLastRemovedObjectCount = 0u;
        added.sceneLastRebuiltObjectCount = BatchCount;
        added.gpuSceneRemoveCount = 0u;
        added.gpuSceneAddCount = BatchCount;
        added.gpuSceneFrameUploadBytes = 1024u;
        added.directRasterActiveInstanceCount = 1000u;
        AdvanceAcceptedExtraction(added, BatchCount * 8u, BatchCount, BatchCount);
        EXPECT_TRUE(RenderingStressSample::EvaluateChurnAddWindow(
                        beforeAdd,
                        added,
                        BatchCount,
                        1000u - BatchCount,
                        1000u,
                        0u,
                        1u)
                        .passed);

        ++added.gpuSceneAddCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnAddWindow(
                         beforeAdd,
                         added,
                         BatchCount,
                         1000u - BatchCount,
                         1000u,
                         0u,
                         1u)
                         .passed);
        --added.gpuSceneAddCount;
        ++added.gpuDrivenActiveRowFullMaterializationCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnAddWindow(
                         beforeAdd,
                         added,
                         BatchCount,
                         1000u - BatchCount,
                         1000u,
                         0u,
                         1u)
                         .passed);
        --added.gpuDrivenActiveRowFullMaterializationCount;
        added.directRasterIndexFullMaterializationCount = 2u;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnAddWindow(
                         beforeAdd,
                         added,
                         BatchCount,
                         1000u - BatchCount,
                         1000u,
                         0u,
                         1u)
                         .passed);
    }

    TEST(RenderingStressSampleValidation,
         SecondChurnBatchRequiresItsExactBeforeAndAfterPopulations)
    {
        constexpr uint32 BatchCount =
            RenderingStressSample::GetDeterministicChurnBatchSize();
        constexpr uint32 InitialPopulation = 1000u;
        constexpr uint32 AfterFirstBatch = InitialPopulation - BatchCount;
        constexpr uint32 AfterSecondBatch = AfterFirstBatch - BatchCount;

        RenderingStressDiagnosticsSnapshot firstBatch = MakeBaseline();
        firstBatch.directRasterActiveInstanceCount = AfterFirstBatch;
        firstBatch.directRasterActiveInstanceCapacity = InitialPopulation;
        AdvanceAcceptedExtraction(firstBatch, BatchCount * 2u, BatchCount, 0u);

        RenderingStressDiagnosticsSnapshot secondBatch = firstBatch;
        ++secondBatch.sceneIncrementalUpdateCount;
        secondBatch.extractionActorRebuildCount = BatchCount;
        secondBatch.sceneLastRemovedObjectCount = BatchCount;
        secondBatch.gpuSceneRemoveCount = BatchCount;
        secondBatch.gpuSceneFrameUploadBytes = 1024u;
        secondBatch.directRasterActiveInstanceCount = AfterSecondBatch;
        secondBatch.directRasterInstancePatchedRowCount = BatchCount;
        secondBatch.directRasterIndexPatchedRowCount = BatchCount;
        AdvanceAcceptedExtraction(secondBatch, BatchCount * 2u, BatchCount, 0u);

        EXPECT_TRUE(RenderingStressSample::EvaluateChurnRemoveWindow(
                        firstBatch,
                        secondBatch,
                        BatchCount,
                        AfterFirstBatch,
                        AfterSecondBatch,
                        0u,
                        1u)
                        .passed);
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnRemoveWindow(
                         firstBatch,
                         secondBatch,
                         BatchCount,
                         InitialPopulation,
                         AfterSecondBatch,
                         0u,
                         1u)
                         .passed);
    }

    TEST(RenderingStressSampleValidation,
         ChurnDrainAllowsBoundedTwoAndThreeSlotPropagationThenRequiresQuiet)
    {
        constexpr uint32 BatchCount =
            RenderingStressSample::GetDeterministicChurnBatchSize();
        constexpr uint32 Population = 1000u - BatchCount;

        RenderingStressDiagnosticsSnapshot action = MakeBaseline();
        action.renderSceneObjectCount = Population;
        action.gpuScenePublishedObjectCount = Population;
        action.directRasterActiveInstanceCount = Population;
        action.directRasterActiveInstanceCapacity = 1000u;
        action.gpuDrivenInstanceFullMaterializationCount = 2u;

        RenderingStressDiagnosticsSnapshot drain = action;
        ++drain.sceneStaticReuseCount;
        drain.directRasterInstancePatchedRowCount = BatchCount;
        drain.directRasterIndexPatchedRowCount = BatchCount;
        EXPECT_TRUE(RenderingStressSample::EvaluateChurnDrainWindow(
                        action, drain, Population, BatchCount, 0u, 1u)
                        .passed);

        RenderingStressDiagnosticsSnapshot unexpectedFull = drain;
        unexpectedFull.directRasterInstanceFullMaterializationCount = 1u;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnDrainWindow(
                         action,
                         unexpectedFull,
                         Population,
                         BatchCount,
                         0u,
                         1u)
                         .passed);
        unexpectedFull = drain;
        ++unexpectedFull.gpuDrivenInstanceFullMaterializationCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnDrainWindow(
                         action,
                         unexpectedFull,
                         Population,
                         BatchCount,
                         0u,
                         1u)
                         .passed);

        RenderingStressDiagnosticsSnapshot thirdSlotDrain = drain;
        ++thirdSlotDrain.sceneStaticReuseCount;
        EXPECT_TRUE(RenderingStressSample::EvaluateChurnDrainWindow(
                        action,
                        thirdSlotDrain,
                        Population,
                        BatchCount,
                        0u,
                        1u)
                        .passed);
        EXPECT_EQ(RenderingStressSample::GetChurnDrainCompletedFrameLimit(),
                  RVX_MAX_FRAME_COUNT);

        RenderingStressDiagnosticsSnapshot quiet = action;
        ++quiet.sceneStaticReuseCount;
        EXPECT_TRUE(RenderingStressSample::EvaluateChurnDrainWindow(
                        action, quiet, Population, BatchCount, 0u, 1u)
                        .passed);
        EXPECT_TRUE(RenderingStressSample::AreExecutionStreamsQuiescent(quiet));

        quiet.directRasterActiveInstanceCount = Population - 1u;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnDrainWindow(
                         action, quiet, Population, BatchCount, 0u, 1u)
                         .passed);
    }

    TEST(RenderingStressSampleValidation,
         DirtyDrainRequiresOneExactTemporalSettleBeforeStrictStaticReuse)
    {
        constexpr uint32 Population = 100u;
        constexpr uint32 DirtyCount = 10u;

        RenderingStressDiagnosticsSnapshot action = MakeBaseline();
        action.renderSceneObjectCount = Population;
        action.gpuScenePublishedObjectCount = Population;
        action.sceneLastRebuiltObjectCount = DirtyCount;
        action.gpuSceneUpdateCount = DirtyCount;
        action.gpuSceneFrameUploadBytes = 64u;
        action.gpuDrivenActiveRowCount = Population;
        action.gpuDrivenActiveRowHighWatermark = Population;
        action.gpuDrivenInstancePatchedRowCount = DirtyCount;
        action.gpuDrivenInstanceUploadBytes = 640u;
        action.directRasterActiveInstanceCount = Population;
        action.directRasterActiveInstanceCapacity = Population;
        action.directRasterInstancePatchedRowCount = DirtyCount;
        action.directRasterInstanceUploadBytes = 640u;

        RenderingStressDiagnosticsSnapshot settle = action;
        settle.sourceFrameSequence = action.sourceFrameSequence + 1u;
        settle.sceneLastRebuiltObjectCount = 0u;
        settle.gpuDrivenInstancePatchedRowCount = 0u;
        settle.directRasterInstancePatchedRowCount = DirtyCount;
        EXPECT_TRUE(RenderingStressSample::EvaluateDirtyDrainWindow(
                        action, settle, DirtyCount, Population, 1u, 1u, false)
                        .passed);

        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyDrainWindow(
                         action, settle, DirtyCount, Population, 1u, 1u, true)
                         .passed);
        settle.directRasterIndexPatchedRowCount = 1u;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyDrainWindow(
                         action, settle, DirtyCount, Population, 1u, 1u, false)
                         .passed);
        settle.directRasterIndexPatchedRowCount = 0u;
        settle.gpuDrivenCandidatePatchedRowCount = 1u;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyDrainWindow(
                         action, settle, DirtyCount, Population, 1u, 1u, false)
                         .passed);
        settle.gpuDrivenCandidatePatchedRowCount = 0u;
        settle.directRasterInstancePatchedRowCount = DirtyCount + 1u;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyDrainWindow(
                         action, settle, DirtyCount, Population, 1u, 1u, false)
                         .passed);
        settle.directRasterInstancePatchedRowCount = DirtyCount;
        settle.directRasterInstanceUploadBytes =
            action.directRasterInstanceUploadBytes + 1u;
        EXPECT_TRUE(RenderingStressSample::EvaluateDirtyDrainWindow(
                        action, settle, DirtyCount, Population, 1u, 1u, false)
                        .passed);
        settle.directRasterInstanceUploadBytes =
            action.directRasterInstanceUploadBytes;
        settle.gpuSceneFrameUploadBytes =
            action.gpuSceneFrameUploadBytes + 1u;
        EXPECT_TRUE(RenderingStressSample::EvaluateDirtyDrainWindow(
                        action, settle, DirtyCount, Population, 1u, 1u, false)
                        .passed);

        RenderingStressDiagnosticsSnapshot latestStaticAction = action;
        latestStaticAction.sourceFrameSequence = settle.sourceFrameSequence + 1u;
        latestStaticAction.sceneLastRebuiltObjectCount = 0u;
        latestStaticAction.gpuSceneUpdateCount = 0u;
        latestStaticAction.gpuSceneFrameUploadBytes = 0u;
        latestStaticAction.gpuDrivenInstancePatchedRowCount = 0u;
        latestStaticAction.gpuDrivenInstanceUploadBytes = 0u;
        latestStaticAction.directRasterInstancePatchedRowCount = 0u;
        latestStaticAction.directRasterInstanceUploadBytes = 0u;
        RenderingStressDiagnosticsSnapshot lateSettle = latestStaticAction;
        lateSettle.sourceFrameSequence =
            latestStaticAction.sourceFrameSequence + 1u;
        lateSettle.gpuSceneUpdateCount = DirtyCount;
        lateSettle.gpuSceneFrameUploadBytes = 64u;
        lateSettle.directRasterInstancePatchedRowCount = DirtyCount;
        lateSettle.directRasterInstanceUploadBytes = 640u;
        EXPECT_TRUE(RenderingStressSample::EvaluateDirtyDrainWindow(
                        latestStaticAction,
                        lateSettle,
                        DirtyCount,
                        Population,
                        1u,
                        1u,
                        false)
                        .passed);

        RenderingStressDiagnosticsSnapshot strict = action;
        strict.sourceFrameSequence = settle.sourceFrameSequence + 1u;
        ++strict.sceneStaticReuseCount;
        strict.sceneLastRebuiltObjectCount = 0u;
        strict.gpuSceneUpdateCount = 0u;
        strict.gpuSceneFrameUploadBytes = 0u;
        strict.gpuDrivenInstancePatchedRowCount = 0u;
        strict.gpuDrivenInstanceUploadBytes = 0u;
        strict.directRasterInstancePatchedRowCount = 0u;
        strict.directRasterInstanceUploadBytes = 0u;
        EXPECT_TRUE(RenderingStressSample::EvaluateDirtyDrainWindow(
                        action, strict, DirtyCount, Population, 1u, 1u, true)
                        .passed);
        EXPECT_TRUE(RenderingStressSample::EvaluateStaticWindow(action, strict)
                        .passed);
        EXPECT_EQ(RenderingStressSample::GetDirtyDrainCompletedFrameLimit(),
                  RVX_MAX_FRAME_COUNT - 1u);
    }

    TEST(RenderingStressSampleValidation,
         ChurnRecreateDrainAllowsOnlyOneExactTemporalSettle)
    {
        constexpr uint32 Population = 1000u;
        constexpr uint32 BatchCount =
            RenderingStressSample::GetDeterministicChurnBatchSize();

        RenderingStressDiagnosticsSnapshot action = MakeBaseline();
        action.renderSceneObjectCount = Population;
        action.gpuScenePublishedObjectCount = Population;
        action.sceneLastRebuiltObjectCount = BatchCount;
        action.gpuSceneAddCount = BatchCount;
        action.gpuSceneFrameUploadBytes = 512u;
        action.directRasterActiveInstanceCount = Population;
        action.directRasterActiveInstanceCapacity = Population;
        action.directRasterInstancePatchedRowCount = BatchCount;
        action.directRasterIndexPatchedRowCount = BatchCount;
        action.directRasterInstanceUploadBytes = 4096u;
        action.directRasterInstanceIndexUploadBytes = 1024u;

        RenderingStressDiagnosticsSnapshot settle = action;
        settle.sourceFrameSequence = action.sourceFrameSequence + 1u;
        settle.sceneLastRebuiltObjectCount = 0u;
        settle.gpuSceneAddCount = 0u;
        settle.gpuSceneUpdateCount = BatchCount;
        settle.directRasterInstancePatchedRowCount = BatchCount;
        settle.directRasterIndexPatchedRowCount = BatchCount;
        EXPECT_TRUE(RenderingStressSample::EvaluateChurnDrainWindow(
                        action,
                        settle,
                        Population,
                        BatchCount,
                        0u,
                        1u,
                        true,
                        false)
                        .passed);
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnDrainWindow(
                         action,
                         settle,
                         Population,
                         BatchCount,
                         0u,
                         1u,
                         false,
                         false)
                         .passed);
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnDrainWindow(
                         action,
                         settle,
                         Population,
                         BatchCount,
                         0u,
                         1u,
                         true,
                         true)
                         .passed);
        settle.gpuSceneNoOpCount = 1u;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnDrainWindow(
                         action,
                         settle,
                         Population,
                         BatchCount,
                         0u,
                         1u,
                         true,
                         false)
                         .passed);

        RenderingStressDiagnosticsSnapshot strictBeforeSettle = action;
        strictBeforeSettle.sourceFrameSequence =
            settle.sourceFrameSequence + 1u;
        ++strictBeforeSettle.sceneStaticReuseCount;
        strictBeforeSettle.sceneLastRebuiltObjectCount = 0u;
        strictBeforeSettle.gpuSceneAddCount = 0u;
        strictBeforeSettle.gpuSceneFrameUploadBytes = 0u;
        strictBeforeSettle.directRasterInstancePatchedRowCount = 0u;
        strictBeforeSettle.directRasterIndexPatchedRowCount = 0u;
        strictBeforeSettle.directRasterInstanceUploadBytes = 0u;
        strictBeforeSettle.directRasterInstanceIndexUploadBytes = 0u;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnDrainWindow(
                         action,
                         strictBeforeSettle,
                         Population,
                         BatchCount,
                         0u,
                         1u,
                         true,
                         false)
                         .passed);
    }

    TEST(RenderingStressSampleValidation,
         PendingActionDrainEvidenceReplaysEveryLaterCompletedFrameInOrder)
    {
        constexpr uint32 Population = 100u;
        constexpr uint32 DirtyCount = 10u;

        RenderingStressDiagnosticsSnapshot finalizedAction = MakeBaseline();
        finalizedAction.sourceFrameSequence = 100u;
        finalizedAction.mutationEvidence.completedFrameSequence = 100u;
        finalizedAction.mutationEvidence.completedPresentationCount = 100u;
        finalizedAction.renderSceneObjectCount = Population;
        finalizedAction.gpuScenePublishedObjectCount = Population;
        finalizedAction.sceneLastRebuiltObjectCount = DirtyCount;
        finalizedAction.gpuSceneUpdateCount = DirtyCount;
        finalizedAction.gpuSceneFrameUploadBytes = 64u;
        finalizedAction.directRasterActiveInstanceCount = Population;
        finalizedAction.directRasterActiveInstanceCapacity = Population;
        finalizedAction.directRasterInstancePatchedRowCount = DirtyCount;
        finalizedAction.directRasterInstanceUploadBytes = 640u;
        finalizedAction.acceptedExtractionPublicationCount = 2u;
        finalizedAction.acceptedExtractionLastSourceFrameSequence = 102u;
        finalizedAction.acceptedExtractionLastSceneRevision = 2u;
        finalizedAction.acceptedExtractionCumulativeChangeFeedChangeCount =
            DirtyCount;
        finalizedAction.acceptedExtractionCumulativeActorRebuildCount =
            DirtyCount;
        finalizedAction.acceptedExtractionCumulativeProxyVisitCount =
            DirtyCount;

        RenderingStressDiagnosticsSnapshot settle = finalizedAction;
        settle.sourceFrameSequence = 101u;
        settle.mutationEvidence.completedFrameSequence = 101u;
        settle.mutationEvidence.completedPresentationCount = 101u;
        settle.sceneLastRebuiltObjectCount = 0u;
        // Queued frames retain their own accepted-extraction provenance. The
        // completed action watermark is authoritative; replay must not graft
        // later accepted totals onto an older completed-frame observation.

        RenderingStressDiagnosticsSnapshot strict = settle;
        strict.sourceFrameSequence = 102u;
        strict.mutationEvidence.completedFrameSequence = 102u;
        strict.mutationEvidence.completedPresentationCount = 102u;
        ++strict.sceneStaticReuseCount;
        strict.gpuSceneUpdateCount = 0u;
        strict.gpuSceneFrameUploadBytes = 0u;
        strict.directRasterInstancePatchedRowCount = 0u;
        strict.directRasterInstanceUploadBytes = 0u;

        std::vector<RenderingStressDiagnosticsSnapshot> pending;
        EXPECT_TRUE(RenderingStressSample::AppendPendingActionDrainObservation(
                        finalizedAction, pending, settle)
                        .passed);
        EXPECT_TRUE(RenderingStressSample::AppendPendingActionDrainObservation(
                        finalizedAction, pending, strict)
                        .passed);
        ASSERT_EQ(pending.size(), 2u);
        EXPECT_EQ(pending[0].sourceFrameSequence, 101u);
        EXPECT_EQ(pending[1].sourceFrameSequence, 102u);

        RenderingStressDiagnosticsSnapshot normalizedSettle;
        EXPECT_TRUE(RenderingStressSample::NormalizeActionDrainObservation(
                        finalizedAction, pending[0], normalizedSettle)
                        .passed);
        EXPECT_EQ(normalizedSettle.acceptedExtractionCumulativeActorRebuildCount,
                  DirtyCount);
        EXPECT_TRUE(RenderingStressSample::EvaluateDirtyDrainWindow(
                        finalizedAction,
                        normalizedSettle,
                        DirtyCount,
                        Population,
                        0u,
                        1u,
                        false)
                        .passed);

        RenderingStressDiagnosticsSnapshot normalizedStrict;
        EXPECT_TRUE(RenderingStressSample::NormalizeActionDrainObservation(
                        finalizedAction, pending[1], normalizedStrict)
                        .passed);
        EXPECT_TRUE(RenderingStressSample::EvaluateDirtyDrainWindow(
                        finalizedAction,
                        normalizedStrict,
                        DirtyCount,
                        Population,
                        0u,
                        1u,
                        true)
                        .passed);
        EXPECT_TRUE(RenderingStressSample::EvaluateStaticWindow(
                        finalizedAction, normalizedStrict)
                        .passed);
    }

    TEST(RenderingStressSampleValidation,
         PendingActionDrainUsesCompletedPresentationBudgetNotSourceSequence)
    {
        RenderingStressDiagnosticsSnapshot action = MakeBaseline();
        action.sourceFrameSequence = 100u;
        action.mutationEvidence.completedFrameSequence = 100u;
        action.mutationEvidence.completedPresentationCount = 100u;

        std::vector<RenderingStressDiagnosticsSnapshot> continuous;
        for (uint64 delta = 1u;
             delta <= RenderingStressSample::GetPendingActionReplayFrameLimit() +
                          1u;
             ++delta)
        {
            RenderingStressDiagnosticsSnapshot current = action;
            current.sourceFrameSequence += delta;
            current.mutationEvidence.completedFrameSequence += delta;
            current.mutationEvidence.completedPresentationCount += delta;
            EXPECT_TRUE(RenderingStressSample::AppendPendingActionDrainObservation(
                            action, continuous, current)
                            .passed);
        }

        RenderingStressDiagnosticsSnapshot sourceJump = action;
        sourceJump.sourceFrameSequence = 1000u;
        sourceJump.mutationEvidence.completedFrameSequence = 1000u;
        sourceJump.mutationEvidence.completedPresentationCount +=
            RenderingStressSample::GetPendingActionReplayFrameLimit() + 1u;
        std::vector<RenderingStressDiagnosticsSnapshot> jumped;
        EXPECT_TRUE(RenderingStressSample::AppendPendingActionDrainObservation(
                        action, jumped, sourceJump)
                        .passed);

        RenderingStressDiagnosticsSnapshot overBudget = sourceJump;
        ++overBudget.sourceFrameSequence;
        ++overBudget.mutationEvidence.completedFrameSequence;
        ++overBudget.mutationEvidence.completedPresentationCount;
        EXPECT_FALSE(RenderingStressSample::AppendPendingActionDrainObservation(
                         action, jumped, overBudget)
                         .passed);
        RenderingStressDiagnosticsSnapshot normalized;
        EXPECT_FALSE(RenderingStressSample::NormalizeActionDrainObservation(
                         action, overBudget, normalized)
                         .passed);
    }

    TEST(RenderingStressSampleValidation,
         PendingActionDrainEvidenceRejectsHiddenWorkAndCapacityOverflow)
    {
        constexpr uint32 Population = 10u;
        constexpr uint32 DirtyCount = 1u;
        RenderingStressDiagnosticsSnapshot finalizedAction = MakeBaseline();
        finalizedAction.sourceFrameSequence = 20u;
        finalizedAction.mutationEvidence.completedFrameSequence = 20u;
        finalizedAction.mutationEvidence.completedPresentationCount = 20u;
        finalizedAction.renderSceneObjectCount = Population;
        finalizedAction.gpuScenePublishedObjectCount = Population;
        finalizedAction.sceneLastRebuiltObjectCount = DirtyCount;
        finalizedAction.gpuSceneUpdateCount = DirtyCount;
        finalizedAction.gpuSceneFrameUploadBytes = 64u;
        finalizedAction.directRasterActiveInstanceCount = Population;
        finalizedAction.directRasterActiveInstanceCapacity = Population;
        finalizedAction.directRasterInstancePatchedRowCount = DirtyCount;
        finalizedAction.directRasterInstanceUploadBytes = 64u;
        finalizedAction.acceptedExtractionCumulativeActorRebuildCount =
            DirtyCount;

        RenderingStressDiagnosticsSnapshot hiddenFull = finalizedAction;
        hiddenFull.sourceFrameSequence = 21u;
        hiddenFull.mutationEvidence.completedFrameSequence = 21u;
        hiddenFull.mutationEvidence.completedPresentationCount = 21u;
        hiddenFull.sceneLastRebuiltObjectCount = 0u;
        hiddenFull.gpuSceneFullUpload = true;
        RenderingStressDiagnosticsSnapshot normalized;
        EXPECT_TRUE(RenderingStressSample::NormalizeActionDrainObservation(
                        finalizedAction, hiddenFull, normalized)
                        .passed);
        EXPECT_TRUE(normalized.gpuSceneFullUpload);
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyDrainWindow(
                         finalizedAction,
                         normalized,
                         DirtyCount,
                         Population,
                         0u,
                         1u,
                         false)
                         .passed);

        RenderingStressDiagnosticsSnapshot hiddenOverBudget = hiddenFull;
        hiddenOverBudget.gpuSceneFullUpload = false;
        hiddenOverBudget.directRasterInstancePatchedRowCount = DirtyCount + 1u;
        EXPECT_TRUE(RenderingStressSample::NormalizeActionDrainObservation(
                        finalizedAction, hiddenOverBudget, normalized)
                        .passed);
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyDrainWindow(
                         finalizedAction,
                         normalized,
                         DirtyCount,
                         Population,
                         0u,
                         1u,
                         false)
                         .passed);

        RenderingStressDiagnosticsSnapshot laterAcceptedWork = hiddenFull;
        laterAcceptedWork.acceptedExtractionCumulativeActorRebuildCount =
            DirtyCount + 1u;
        EXPECT_TRUE(RenderingStressSample::NormalizeActionDrainObservation(
                        finalizedAction, laterAcceptedWork, normalized)
                        .passed);
        EXPECT_EQ(normalized.acceptedExtractionCumulativeActorRebuildCount,
                  DirtyCount + 1u);

        std::vector<RenderingStressDiagnosticsSnapshot> pending;
        for (uint32 offset = 1u;
             offset <= RenderingStressSample::GetPendingActionReplayFrameLimit() +
                           1u;
             ++offset)
        {
            RenderingStressDiagnosticsSnapshot queued = finalizedAction;
            queued.sourceFrameSequence =
                finalizedAction.sourceFrameSequence + offset;
            queued.mutationEvidence.completedFrameSequence =
                finalizedAction.mutationEvidence.completedFrameSequence + offset;
            queued.mutationEvidence.completedPresentationCount =
                finalizedAction.mutationEvidence.completedPresentationCount + offset;
            EXPECT_TRUE(RenderingStressSample::AppendPendingActionDrainObservation(
                            finalizedAction, pending, queued)
                            .passed);
        }
        RenderingStressDiagnosticsSnapshot overflow = finalizedAction;
        overflow.sourceFrameSequence +=
            RenderingStressSample::GetPendingActionReplayFrameLimit() + 2u;
        overflow.mutationEvidence.completedFrameSequence +=
            RenderingStressSample::GetPendingActionReplayFrameLimit() + 2u;
        overflow.mutationEvidence.completedPresentationCount +=
            RenderingStressSample::GetPendingActionReplayFrameLimit() + 2u;
        EXPECT_FALSE(RenderingStressSample::AppendPendingActionDrainObservation(
                         finalizedAction, pending, overflow)
                         .passed);
        EXPECT_EQ(pending.size(),
                  RenderingStressSample::GetPendingActionReplayFrameLimit() +
                      1u);
    }

    TEST(RenderingStressSampleValidation,
         PendingChurnDrainEvidenceSurvivesActionToReceiptCompletionGap)
    {
        constexpr uint32 BeforePopulation = 100u;
        constexpr uint32 BatchCount = 10u;
        constexpr uint32 Population = BeforePopulation - BatchCount;

        RenderingStressDiagnosticsSnapshot finalizedDestroy = MakeBaseline();
        finalizedDestroy.sourceFrameSequence = 40u;
        finalizedDestroy.mutationEvidence.completedFrameSequence = 40u;
        finalizedDestroy.mutationEvidence.completedPresentationCount = 40u;
        finalizedDestroy.renderSceneObjectCount = Population;
        finalizedDestroy.gpuScenePublishedObjectCount = Population;
        finalizedDestroy.sceneLastRemovedObjectCount = BatchCount;
        finalizedDestroy.gpuSceneRemoveCount = BatchCount;
        finalizedDestroy.gpuSceneFrameUploadBytes = 64u;
        finalizedDestroy.directRasterActiveInstanceCount = Population;
        finalizedDestroy.directRasterActiveInstanceCapacity = BeforePopulation;
        finalizedDestroy.directRasterInstancePatchedRowCount = BatchCount;
        finalizedDestroy.directRasterIndexPatchedRowCount = BatchCount;
        finalizedDestroy.directRasterInstanceUploadBytes = 640u;
        finalizedDestroy.directRasterInstanceIndexUploadBytes = 64u;

        RenderingStressDiagnosticsSnapshot gapSlot = finalizedDestroy;
        gapSlot.sourceFrameSequence = 41u;
        gapSlot.mutationEvidence.completedFrameSequence = 41u;
        gapSlot.mutationEvidence.completedPresentationCount = 41u;
        ++gapSlot.sceneStaticReuseCount;
        gapSlot.sceneLastRemovedObjectCount = 0u;
        gapSlot.gpuSceneRemoveCount = 0u;
        gapSlot.gpuSceneFrameUploadBytes = 0u;

        RenderingStressDiagnosticsSnapshot gapStrict = gapSlot;
        gapStrict.sourceFrameSequence = 42u;
        gapStrict.mutationEvidence.completedFrameSequence = 42u;
        gapStrict.mutationEvidence.completedPresentationCount = 42u;
        ++gapStrict.sceneStaticReuseCount;
        gapStrict.directRasterInstancePatchedRowCount = 0u;
        gapStrict.directRasterIndexPatchedRowCount = 0u;
        gapStrict.directRasterInstanceUploadBytes = 0u;
        gapStrict.directRasterInstanceIndexUploadBytes = 0u;

        std::vector<RenderingStressDiagnosticsSnapshot> pending;
        EXPECT_TRUE(RenderingStressSample::AppendPendingActionDrainObservation(
                        finalizedDestroy, pending, gapSlot)
                        .passed);
        EXPECT_TRUE(RenderingStressSample::AppendPendingActionDrainObservation(
                        finalizedDestroy, pending, gapStrict)
                        .passed);
        ASSERT_EQ(pending.size(), 2u);

        RenderingStressDiagnosticsSnapshot normalizedSlot;
        RenderingStressDiagnosticsSnapshot normalizedStrict;
        EXPECT_TRUE(RenderingStressSample::NormalizeActionDrainObservation(
                        finalizedDestroy, pending[0], normalizedSlot)
                        .passed);
        EXPECT_TRUE(RenderingStressSample::NormalizeActionDrainObservation(
                        finalizedDestroy, pending[1], normalizedStrict)
                        .passed);
        EXPECT_TRUE(RenderingStressSample::EvaluateChurnDrainWindow(
                        finalizedDestroy,
                        normalizedSlot,
                        Population,
                        BatchCount,
                        0u,
                        1u)
                        .passed);
        EXPECT_TRUE(RenderingStressSample::EvaluateChurnDrainWindow(
                        finalizedDestroy,
                        normalizedStrict,
                        Population,
                        BatchCount,
                        0u,
                        1u)
                        .passed);
        EXPECT_TRUE(RenderingStressSample::EvaluateStaticWindow(
                        finalizedDestroy, normalizedStrict)
                        .passed);
    }

    TEST(RenderingStressSampleValidation,
         PinsCompletedActionEvidenceBeforeConsumingLaterDrainFrames)
    {
        const std::string source = ReadSource(
            "Samples/RenderVerseSamples/Scenes/RenderingStressSample.cpp");
        ASSERT_FALSE(source.empty());
        EXPECT_NE(source.find("m_churnBatchActionSnapshot = current"),
                  std::string::npos);
        EXPECT_NE(source.find("EvaluateChurnRemoveMutationEvidenceWindow"),
                  std::string::npos);
        EXPECT_NE(source.find("EvaluateChurnAddMutationEvidenceWindow"),
                  std::string::npos);
        EXPECT_NE(source.find("ReplayPendingChurnDrain"),
                  std::string::npos);
        EXPECT_NE(source.find("QueuePendingActionDrainObservation"),
                  std::string::npos);
    }

    TEST(RenderingStressSampleValidation,
         InitialSlotWarmupRollsOnlyItsInitialBaseline)
    {
        const std::string source = ReadSource(
            "Samples/RenderVerseSamples/Scenes/RenderingStressSample.cpp");
        ASSERT_FALSE(source.empty());
        EXPECT_NE(source.find("allowInitialSlotWarmup"), std::string::npos);
        EXPECT_NE(source.find("m_windowBaseline = current"),
                  std::string::npos);
        EXPECT_NE(source.find("Every later static window is strict."),
                  std::string::npos);
    }

    TEST(RenderingStressSampleValidation, MissingDiagnosticsFailClosed)
    {
        SampleRenderDiagnostics diagnostics;
        EXPECT_FALSE(
            RenderingStressSample::HasRequiredInstrumentation(diagnostics));

        const RenderingStressDiagnosticsSnapshot unavailable{};
        EXPECT_FALSE(RenderingStressSample::EvaluateStaticWindow(
                         unavailable, unavailable)
                         .passed);
    }

    TEST(RenderingStressSampleValidation,
         WorkloadBuildCannotPrecedeFirstPresentationOrSharedAssetReadiness)
    {
        EXPECT_FALSE(RenderingStressSample::CanBuildWorkload(
            RenderingStressPhase::InitialBuild, false, true));
        EXPECT_FALSE(RenderingStressSample::CanBuildWorkload(
            RenderingStressPhase::InitialBuild, true, false));
        EXPECT_FALSE(RenderingStressSample::CanBuildWorkload(
            RenderingStressPhase::StableBaseline, true, true));
        EXPECT_TRUE(RenderingStressSample::CanBuildWorkload(
            RenderingStressPhase::InitialBuild, true, true));
    }

    TEST(RenderingStressSampleValidation,
         UsesOnlyPureEcsSampleServicesWithoutBackendOrGraphBranches)
    {
        const std::string source = ReadSource(
            "Samples/RenderVerseSamples/Scenes/RenderingStressSample.cpp");
        ASSERT_FALSE(source.empty());
        EXPECT_EQ(source.find("#include \"RHI"), std::string::npos);
        EXPECT_EQ(source.find("RHIBackendType"), std::string::npos);
        EXPECT_EQ(source.find("RenderGraph"), std::string::npos);
        EXPECT_NE(source.find("context.models.Request("), std::string::npos);
        EXPECT_GT(source.find("context.models.Request("),
                  source.find("void RenderingStressSample::Update"));
        EXPECT_NE(source.find("context.options.workloadProfile"),
                  std::string::npos);
        EXPECT_NE(source.find("gpuCulling.maxVisibleObjects"),
                  std::string::npos);
        EXPECT_NE(source.find("context.cameras.SetPerspective"),
                  std::string::npos);
        EXPECT_NE(source.find("context.sceneLifetime.CreateAndAdoptWithFragments"),
                  std::string::npos);
        EXPECT_NE(source.find("context.scene.BeginSpawnTransaction"),
                  std::string::npos);
        EXPECT_NE(source.find("SceneECS::SceneCommandBuffer"),
                  std::string::npos);
        EXPECT_NE(source.find("commands.RequestDestroy"),
                  std::string::npos);
        EXPECT_NE(source.find("lastPresentedFrameSequence"), std::string::npos);
        EXPECT_NE(source.find("CanBuildWorkload"), std::string::npos);
    }

    TEST(RenderingStressSampleValidation,
         EcsSpawnTransactionPublishesTransformLightAndMaterialValues)
    {
        SceneECS::SceneEcsRuntime runtime;
        SceneECS::RuntimeEntityDesc desc;
        desc.localTransform.translation = Vec3(2.0f, 3.0f, 4.0f);

        SceneECS::SceneSpawnTransaction transaction =
            runtime.BeginSpawnTransaction();
        const SceneECS::SceneSpawnEntityId pending = transaction.Create(desc);
        ASSERT_TRUE(pending.IsValid());

        SceneECS::Light light;
        light.type = SceneECS::LightType::Point;
        light.color = Vec3(1.0f, 0.78f, 0.55f);
        light.intensity = 64.0f;
        light.range = 32.0f;
        light.castsShadows = false;
        SceneECS::MaterialSlots materialSlots;
        materialSlots.count = 1u;

        ASSERT_TRUE(transaction.Add<SceneECS::Light>(pending, light));
        ASSERT_TRUE(
            transaction.Add<SceneECS::MaterialSlots>(pending, materialSlots));

        const SceneECS::SceneSpawnCommitResult committed = transaction.Commit();
        ASSERT_TRUE(committed.IsApplied());
        const SceneECS::SceneEntityRef entityRef = committed.GetEntityRef(pending);
        ASSERT_TRUE(entityRef.IsValid());
        EXPECT_EQ(entityRef.sceneRuntimeId, runtime.GetSceneRuntimeId());

        const SceneECS::LocalTransform* transform =
            runtime.GetRegistry().TryGet<SceneECS::LocalTransform>(entityRef.entity);
        const SceneECS::Light* storedLight =
            runtime.GetRegistry().TryGet<SceneECS::Light>(entityRef.entity);
        const SceneECS::MaterialSlots* storedSlots =
            runtime.GetRegistry().TryGet<SceneECS::MaterialSlots>(entityRef.entity);
        ASSERT_NE(transform, nullptr);
        ASSERT_NE(storedLight, nullptr);
        ASSERT_NE(storedSlots, nullptr);
        EXPECT_EQ(transform->translation, desc.localTransform.translation);
        EXPECT_EQ(storedLight->type, SceneECS::LightType::Point);
        EXPECT_EQ(storedLight->intensity, light.intensity);
        EXPECT_EQ(storedSlots->count, materialSlots.count);

        const uint64 writeVersionBefore =
            runtime.GetRegistry().GetFragmentWriteVersion<SceneECS::Light>(
                entityRef.entity);
        light.intensity = 96.0f;
        ASSERT_TRUE(runtime.SetFragment<SceneECS::Light>(entityRef.entity, light));
        EXPECT_GT(runtime.GetRegistry().GetFragmentWriteVersion<SceneECS::Light>(
                      entityRef.entity),
                  writeVersionBefore);
        EXPECT_EQ(runtime.GetRegistry()
                      .TryGet<SceneECS::Light>(entityRef.entity)
                      ->intensity,
                  light.intensity);
    }

    TEST(RenderingStressSampleValidation,
         EcsCommandReceiptsProveBoundedChurnAndCleanup)
    {
        SceneECS::SceneEcsRuntime runtime;
        SceneECS::SceneCommandBuffer commands = runtime.CreateCommandBuffer();
        SceneECS::RuntimeEntityDesc desc;
        desc.localTransform.translation = Vec3(-2.0f, 0.0f, 1.0f);
        const SceneECS::SceneEntityReceipt created = commands.Create(desc);
        ASSERT_TRUE(created.IsQueued());

        SceneECS::Light light;
        light.type = SceneECS::LightType::Point;
        light.intensity = 48.0f;
        const SceneECS::SceneCommandReceipt added =
            commands.Add(created, light);
        ASSERT_TRUE(added.IsQueued());

        const SceneECS::SceneCommandBufferReceipt submitted =
            runtime.SubmitCommandBuffer(std::move(commands));
        ASSERT_TRUE(submitted.IsQueued());
        const SceneECS::SceneEcsTickResult createTick = runtime.Tick();
        ASSERT_TRUE(createTick.succeeded);
        EXPECT_TRUE(submitted.IsApplied());
        EXPECT_TRUE(created.IsResolved());
        EXPECT_TRUE(added.IsApplied());

        const ECS::EntityHandle entity = created.GetEntity();
        ASSERT_TRUE(entity.IsValid());
        ASSERT_TRUE(runtime.GetEntityRef(entity).IsValid());
        ASSERT_NE(runtime.GetRegistry().TryGet<SceneECS::LocalTransform>(entity),
                  nullptr);
        ASSERT_NE(runtime.GetRegistry().TryGet<SceneECS::Light>(entity), nullptr);

        SceneECS::SceneCommandBuffer destroyCommands = runtime.CreateCommandBuffer();
        const SceneECS::SceneCommandReceipt destroy =
            destroyCommands.RequestDestroy(entity);
        ASSERT_TRUE(destroy.IsQueued());
        const SceneECS::SceneCommandBufferReceipt destroySubmission =
            runtime.SubmitCommandBuffer(std::move(destroyCommands));
        ASSERT_TRUE(destroySubmission.IsQueued());
        const SceneECS::SceneEcsTickResult destroyTick = runtime.Tick();
        ASSERT_TRUE(destroyTick.succeeded);
        EXPECT_TRUE(destroySubmission.IsApplied());
        EXPECT_TRUE(destroy.IsApplied());
        EXPECT_EQ(destroyTick.publishedCleanupRecordCount, 1u);
        const SceneECS::EntityLifecycleState* lifecycle =
            runtime.GetRegistry().TryGet<SceneECS::EntityLifecycleState>(entity);
        ASSERT_NE(lifecycle, nullptr);
        EXPECT_EQ(lifecycle->phase, SceneECS::EntityLifecyclePhase::CleanupRequired);

        ASSERT_TRUE(runtime.AcknowledgeCleanup(
            entity, SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::All)));
        EXPECT_EQ(runtime.AdvanceRetirements(), 1u);
        EXPECT_EQ(runtime.RecycleRecyclableEntities(), 1u);
        EXPECT_FALSE(runtime.GetEntityRef(entity).IsValid());
        EXPECT_FALSE(runtime.GetRegistry().IsAlive(entity));
    }

    TEST(RenderingStressSampleValidation,
         ReportsPureEcsReadinessAndDeterministicFragmentBasedLighting)
    {
        RenderingStressSample sample;
        const SampleReadiness readiness = sample.GetReadiness({});
        EXPECT_EQ(readiness.state, SampleReadinessState::Pending);

        const std::string source = ReadSource(
            "Samples/RenderVerseSamples/Scenes/RenderingStressSample.cpp");
        ASSERT_FALSE(source.empty());
        EXPECT_NE(source.find("reporter.Enable(\"ModelResourceLoad\")"),
                  std::string::npos);
        EXPECT_NE(source.find("reporter.Enable(\"EcsPreparedModelAdoption\")"),
                  std::string::npos);
        EXPECT_NE(source.find("SceneECS::LightType::Point"),
                  std::string::npos);
        EXPECT_NE(source.find("castsShadows = false"), std::string::npos);
        EXPECT_NE(source.find("SceneECS::MaterialSlots"),
                  std::string::npos);
        EXPECT_NE(source.find("GetReadiness"), std::string::npos);
    }

    TEST(RenderingStressSampleValidation,
         CompletionWatermarksQualifyStaticLatestFrameAfterDestroy)
    {
        constexpr uint32 BeforePopulation = 1000u;
        constexpr uint32 RemovedCount = 256u;
        RenderingStressDiagnosticsSnapshot baseline = MakeBaseline();
        baseline.renderSceneObjectCount = BeforePopulation;
        baseline.gpuScenePublishedObjectCount = BeforePopulation;
        baseline.directRasterActiveInstanceCount = BeforePopulation;
        baseline.directRasterActiveInstanceCapacity = BeforePopulation;
        baseline.mutationEvidence.directRasterOwnerCount = 1u;

        RenderingStressDiagnosticsSnapshot latestStatic = baseline;
        latestStatic.sourceFrameSequence = 17u;
        latestStatic.renderSceneAppliedRevision = 1u;
        latestStatic.renderSceneObjectCount = BeforePopulation - RemovedCount;
        latestStatic.gpuScenePublishedObjectCount =
            BeforePopulation - RemovedCount;
        latestStatic.directRasterActiveInstanceCount =
            BeforePopulation - RemovedCount;
        latestStatic.mutationEvidence.completedFrameSequence = 17u;
        latestStatic.mutationEvidence.completedPresentationCount = 2u;
        latestStatic.mutationEvidence.appliedSceneRevision = 1u;
        ++latestStatic.mutationEvidence.sceneIncrementalCommitCount;
        latestStatic.mutationEvidence.sceneRemovedObjectCount += RemovedCount;
        latestStatic.mutationEvidence.gpuSceneIncrementalPublicationCount += 2u;
        ++latestStatic.mutationEvidence.gpuSceneIdentityOnlyPublicationCount;
        latestStatic.mutationEvidence.gpuSceneRemoveCount += RemovedCount;
        ++latestStatic.mutationEvidence.gpuSceneSubmittedUploadCount;
        latestStatic.mutationEvidence.gpuSceneUploadBytes += 64u;
        ++latestStatic.mutationEvidence.gpuSceneUploadRangeCount;
        AdvanceGPUSceneSubmittedUploadedRows(latestStatic, RemovedCount);
        latestStatic.mutationEvidence.directRasterIndexPatchedRowCount +=
            RemovedCount;
        latestStatic.mutationEvidence.directRasterIndexUploadBytes += 64u;
        AdvanceAcceptedExtraction(latestStatic,
                                  RemovedCount * 2u,
                                  RemovedCount,
                                  0u);

        const RenderingStressWindowEvaluation pending =
            RenderingStressSample::EvaluateChurnRemoveMutationEvidenceWindow(
                baseline,
                latestStatic,
                2u,
                RemovedCount,
                BeforePopulation,
                BeforePopulation - RemovedCount,
                0u,
                1u);
        EXPECT_FALSE(pending.passed);
        EXPECT_TRUE(pending.pending);

        latestStatic.mutationEvidence.appliedSceneRevision = 2u;
        latestStatic.renderSceneAppliedRevision = 2u;
        latestStatic.acceptedExtractionLastSceneRevision = 1u;
        EXPECT_TRUE(
            RenderingStressSample::EvaluateChurnRemoveMutationEvidenceWindow(
                baseline,
                latestStatic,
                2u,
                RemovedCount,
                BeforePopulation,
                BeforePopulation - RemovedCount,
                0u,
                1u)
                .pending);
        RenderingStressDiagnosticsSnapshot acceptedLate = latestStatic;
        acceptedLate.acceptedExtractionLastSceneRevision = 2u;
        EXPECT_FALSE(RenderingStressSample::ShouldConsumeDiagnosticObservation(
            latestStatic, acceptedLate, false));
        EXPECT_TRUE(RenderingStressSample::ShouldConsumeDiagnosticObservation(
            latestStatic, acceptedLate, true));
        latestStatic = acceptedLate;
        const RenderingStressWindowEvaluation completed =
            RenderingStressSample::EvaluateChurnRemoveMutationEvidenceWindow(
                baseline,
                latestStatic,
                2u,
                RemovedCount,
                BeforePopulation,
                BeforePopulation - RemovedCount,
                0u,
                1u);
        EXPECT_TRUE(completed.passed) << completed.detail;
        EXPECT_EQ(latestStatic.sceneLastRemovedObjectCount, 0u);
        EXPECT_EQ(latestStatic.gpuSceneRemoveCount, 0u);
        EXPECT_EQ(latestStatic.mutationEvidence.directRasterInstancePatchedRowCount,
                  0u);
        EXPECT_EQ(latestStatic.mutationEvidence.directRasterInstanceUploadBytes,
                  0u);
        EXPECT_TRUE(RenderingStressSample::AreExecutionStreamsQuiescent(
            latestStatic));

        const std::string source = ReadSource(
            "Samples/RenderVerseSamples/Scenes/RenderingStressSample.cpp");
        EXPECT_NE(source.find("m_actionTargetSceneRevision = std::max"),
                  std::string::npos);
    }

    TEST(RenderingStressSampleValidation,
         CompletionWatermarksRejectHiddenCanonicalAndDirectFullWork)
    {
        constexpr uint32 Population = 1000u;
        constexpr uint32 DirtyCount = 10u;
        RenderingStressDiagnosticsSnapshot gpuBaseline = MakeBaseline();
        gpuBaseline.renderSceneObjectCount = Population;
        gpuBaseline.gpuScenePublishedObjectCount = Population;
        gpuBaseline.gpuDrivenActiveRowCount = Population;
        gpuBaseline.gpuDrivenActiveRowHighWatermark = Population;
        gpuBaseline.mutationEvidence.gpuCullingOwnerCount = 2u;

        RenderingStressDiagnosticsSnapshot gpuAfter = gpuBaseline;
        gpuAfter.sourceFrameSequence = 2u;
        gpuAfter.renderSceneAppliedRevision = 2u;
        gpuAfter.renderSceneRequiredRevision = 2u;
        gpuAfter.mutationEvidence.completedFrameSequence = 2u;
        gpuAfter.mutationEvidence.completedPresentationCount = 2u;
        gpuAfter.mutationEvidence.requiredSceneRevision = 2u;
        gpuAfter.mutationEvidence.appliedSceneRevision = 2u;
        ++gpuAfter.mutationEvidence.sceneIncrementalCommitCount;
        gpuAfter.mutationEvidence.sceneRebuiltObjectCount += DirtyCount;
        ++gpuAfter.mutationEvidence.gpuSceneIncrementalPublicationCount;
        gpuAfter.mutationEvidence.gpuSceneUpdateCount += DirtyCount;
        gpuAfter.mutationEvidence.gpuSceneMaterializedObjectCount += DirtyCount;
        ++gpuAfter.mutationEvidence.gpuSceneSubmittedUploadCount;
        gpuAfter.mutationEvidence.gpuSceneUploadBytes += 64u;
        ++gpuAfter.mutationEvidence.gpuSceneUploadRangeCount;
        AdvanceGPUSceneSubmittedUploadedRows(gpuAfter, DirtyCount);
        gpuAfter.mutationEvidence.gpuCullingInstancePatchedRowCount += DirtyCount;
        gpuAfter.mutationEvidence.gpuCullingInstanceUploadedRowCount += DirtyCount;
        gpuAfter.mutationEvidence.gpuCullingInstanceUploadBytes += 64u;
        AdvanceAcceptedExtraction(gpuAfter,
                                  DirtyCount * 4u,
                                  DirtyCount,
                                  DirtyCount);
        EXPECT_TRUE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                        gpuBaseline,
                        gpuAfter,
                        2u,
                        DirtyCount,
                        Population,
                        1u,
                        0u)
                        .passed);
        ++gpuAfter.mutationEvidence.gpuCullingOwnerCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                         gpuBaseline,
                         gpuAfter,
                         2u,
                         DirtyCount,
                         Population,
                         1u,
                         0u)
                         .passed);
        gpuAfter.mutationEvidence.gpuCullingOwnerCount -= 2u;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                         gpuBaseline,
                         gpuAfter,
                         2u,
                         DirtyCount,
                         Population,
                         1u,
                         0u)
                         .passed);
        ++gpuAfter.mutationEvidence.gpuCullingOwnerCount;
        ++gpuAfter.mutationEvidence.gpuCullingInstancePatchedRowCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                         gpuBaseline,
                         gpuAfter,
                         2u,
                         DirtyCount,
                         Population,
                         1u,
                         0u)
                         .passed);
        --gpuAfter.mutationEvidence.gpuCullingInstancePatchedRowCount;
        --gpuAfter.acceptedExtractionCumulativeChangeFeedChangeCount;
        EXPECT_TRUE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                        gpuBaseline,
                        gpuAfter,
                        2u,
                        DirtyCount,
                        Population,
                        1u,
                        0u)
                        .pending);
        gpuAfter.acceptedExtractionCumulativeChangeFeedChangeCount += 2u;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                         gpuBaseline,
                         gpuAfter,
                         2u,
                         DirtyCount,
                         Population,
                         1u,
                         0u)
                         .passed);
        --gpuAfter.acceptedExtractionCumulativeChangeFeedChangeCount;
        --gpuAfter.mutationEvidence.gpuSceneSubmittedUploadCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                         gpuBaseline,
                         gpuAfter,
                         2u,
                         DirtyCount,
                         Population,
                         1u,
                         0u)
                         .passed);
        gpuAfter.mutationEvidence.gpuSceneSubmittedUploadCount += 2u;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                         gpuBaseline,
                         gpuAfter,
                         2u,
                         DirtyCount,
                         Population,
                         1u,
                         0u)
                         .passed);
        --gpuAfter.mutationEvidence.gpuSceneSubmittedUploadCount;
        ++gpuAfter.mutationEvidence.gpuSceneMaterializedObjectCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                         gpuBaseline,
                         gpuAfter,
                         2u,
                         DirtyCount,
                         Population,
                         1u,
                         0u)
                         .passed);
        --gpuAfter.mutationEvidence.gpuSceneMaterializedObjectCount;
        gpuAfter.mutationEvidence.gpuSceneSubmittedUploadedRowCount[0] +=
            Population;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                         gpuBaseline,
                         gpuAfter,
                         2u,
                         DirtyCount,
                         Population,
                         1u,
                         0u)
                         .passed);
        gpuAfter.mutationEvidence.gpuSceneSubmittedUploadedRowCount[0] -=
            Population;
        gpuAfter.mutationEvidence.gpuCullingInstancePatchedRowCount +=
            Population;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                         gpuBaseline,
                         gpuAfter,
                         2u,
                         DirtyCount,
                         Population,
                         1u,
                         0u)
                         .passed);
        gpuAfter.mutationEvidence.gpuCullingInstancePatchedRowCount -=
            Population;
        ++gpuAfter.mutationEvidence.gpuCullingCandidateUploadedRowCount;
        ++gpuAfter.mutationEvidence.gpuCullingCandidateUploadBytes;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                         gpuBaseline,
                         gpuAfter,
                         2u,
                         DirtyCount,
                         Population,
                         1u,
                         0u)
                         .passed);
        --gpuAfter.mutationEvidence.gpuCullingCandidateUploadedRowCount;
        --gpuAfter.mutationEvidence.gpuCullingCandidateUploadBytes;
        ++gpuAfter.mutationEvidence.gpuCullingCandidateUploadBytes;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                         gpuBaseline,
                         gpuAfter,
                         2u,
                         DirtyCount,
                         Population,
                         1u,
                         0u)
                         .passed);
        --gpuAfter.mutationEvidence.gpuCullingCandidateUploadBytes;
        ++gpuAfter.mutationEvidence.gpuCullingContinuityFullMaterializationCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                         gpuBaseline,
                         gpuAfter,
                         2u,
                         DirtyCount,
                         Population,
                         1u,
                         0u)
                         .passed);

        RenderingStressDiagnosticsSnapshot directBaseline = MakeBaseline();
        directBaseline.renderSceneObjectCount = Population;
        directBaseline.gpuScenePublishedObjectCount = Population;
        directBaseline.directRasterActiveInstanceCount = Population * 2u;
        directBaseline.directRasterActiveInstanceCapacity = Population * 2u;
        directBaseline.mutationEvidence.directRasterOwnerCount = 3u;
        RenderingStressDiagnosticsSnapshot directAfter = directBaseline;
        directAfter.sourceFrameSequence = 2u;
        directAfter.renderSceneAppliedRevision = 2u;
        directAfter.renderSceneRequiredRevision = 2u;
        directAfter.mutationEvidence.completedFrameSequence = 2u;
        directAfter.mutationEvidence.completedPresentationCount = 2u;
        directAfter.mutationEvidence.requiredSceneRevision = 2u;
        directAfter.mutationEvidence.appliedSceneRevision = 2u;
        ++directAfter.mutationEvidence.sceneIncrementalCommitCount;
        directAfter.mutationEvidence.sceneRebuiltObjectCount += DirtyCount;
        ++directAfter.mutationEvidence.gpuSceneIncrementalPublicationCount;
        directAfter.mutationEvidence.gpuSceneUpdateCount += DirtyCount;
        directAfter.mutationEvidence.gpuSceneMaterializedObjectCount += DirtyCount;
        ++directAfter.mutationEvidence.gpuSceneSubmittedUploadCount;
        directAfter.mutationEvidence.gpuSceneUploadBytes += 64u;
        ++directAfter.mutationEvidence.gpuSceneUploadRangeCount;
        AdvanceGPUSceneSubmittedUploadedRows(directAfter, DirtyCount);
        directAfter.mutationEvidence.directRasterInstancePatchedRowCount +=
            DirtyCount;
        directAfter.mutationEvidence.directRasterInstanceUploadBytes += 64u;
        AdvanceAcceptedExtraction(directAfter,
                                  DirtyCount * 4u,
                                  DirtyCount,
                                  DirtyCount);
        EXPECT_TRUE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                        directBaseline,
                        directAfter,
                        2u,
                        DirtyCount,
                        Population,
                        0u,
                        2u)
                        .passed);
        ++directAfter.mutationEvidence.directRasterOwnerCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                         directBaseline,
                         directAfter,
                         2u,
                         DirtyCount,
                         Population,
                         0u,
                         2u)
                         .passed);
        directAfter.mutationEvidence.directRasterOwnerCount -= 2u;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                         directBaseline,
                         directAfter,
                         2u,
                         DirtyCount,
                         Population,
                         0u,
                         2u)
                         .passed);
        ++directAfter.mutationEvidence.directRasterOwnerCount;
        directAfter.mutationEvidence.directRasterInstancePatchedRowCount +=
            static_cast<uint64>(2u) * DirtyCount * RVX_MAX_FRAME_COUNT;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                         directBaseline,
                         directAfter,
                         2u,
                         DirtyCount,
                         Population,
                         0u,
                         2u)
                         .passed);
        directAfter.mutationEvidence.directRasterInstancePatchedRowCount -=
            static_cast<uint64>(2u) * DirtyCount * RVX_MAX_FRAME_COUNT;
        ++directAfter.mutationEvidence.directRasterIndexFullMaterializationCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                         directBaseline,
                         directAfter,
                         2u,
                         DirtyCount,
                         Population,
                         0u,
                         2u)
                         .passed);

        const std::string source = ReadSource(
            "Samples/RenderVerseSamples/Scenes/RenderingStressSample.cpp");
        EXPECT_NE(source.find("SaturatingMetricSum"), std::string::npos);
        EXPECT_NE(source.find("after.gpuCullingCandidatePatchedRowCount"),
                  std::string::npos);
        EXPECT_NE(source.find("after.gpuCullingActiveRowPatchedRowCount"),
                  std::string::npos);
        EXPECT_NE(source.find("after.directRasterIndexPatchedRowCount"),
                  std::string::npos);
    }

    TEST(RenderingStressSampleValidation,
         GPUCullingActionMatrixDistinguishesTierOneAndWarmCandidateAdd)
    {
        constexpr uint32 Population = 100u;
        constexpr uint32 ChurnCount = 10u;
        constexpr uint32 BeforeAddPopulation = Population - ChurnCount;

        const auto makeGPUCullingBaseline = [=](uint32 population)
        {
            RenderingStressDiagnosticsSnapshot snapshot = MakeBaseline();
            snapshot.renderSceneObjectCount = population;
            snapshot.gpuScenePublishedObjectCount = population;
            snapshot.gpuDrivenActiveRowCount = population;
            snapshot.gpuDrivenActiveRowHighWatermark = Population;
            return snapshot;
        };
        const auto makeCompletedChurnAction = [=](
                                                   const RenderingStressDiagnosticsSnapshot& baseline,
                                                   uint32 completedPopulation,
                                                   bool isAdd)
        {
            RenderingStressDiagnosticsSnapshot snapshot = baseline;
            snapshot.sourceFrameSequence = 2u;
            snapshot.renderSceneAppliedRevision = 2u;
            snapshot.renderSceneRequiredRevision = 2u;
            snapshot.renderSceneObjectCount = completedPopulation;
            snapshot.gpuScenePublishedObjectCount = completedPopulation;
            snapshot.gpuDrivenActiveRowCount = completedPopulation;
            snapshot.gpuDrivenActiveRowHighWatermark = Population;
            snapshot.mutationEvidence.completedFrameSequence = 2u;
            snapshot.mutationEvidence.completedPresentationCount = 2u;
            snapshot.mutationEvidence.requiredSceneRevision = 2u;
            snapshot.mutationEvidence.appliedSceneRevision = 2u;
            ++snapshot.mutationEvidence.sceneIncrementalCommitCount;
            if (isAdd)
            {
                snapshot.mutationEvidence.sceneRebuiltObjectCount += ChurnCount;
                snapshot.mutationEvidence.gpuSceneAddCount += ChurnCount;
                snapshot.mutationEvidence.gpuSceneMaterializedObjectCount +=
                    ChurnCount;
            }
            else
            {
                snapshot.mutationEvidence.sceneRemovedObjectCount += ChurnCount;
                snapshot.mutationEvidence.gpuSceneRemoveCount += ChurnCount;
            }
            ++snapshot.mutationEvidence.gpuSceneIncrementalPublicationCount;
            ++snapshot.mutationEvidence.gpuSceneSubmittedUploadCount;
            snapshot.mutationEvidence.gpuSceneUploadBytes += 64u;
            ++snapshot.mutationEvidence.gpuSceneUploadRangeCount;
            AdvanceGPUSceneSubmittedUploadedRows(snapshot, ChurnCount);
            AdvanceAcceptedExtraction(snapshot,
                                      ChurnCount * (isAdd ? 8u : 2u),
                                      ChurnCount,
                                      isAdd ? ChurnCount : 0u);
            return snapshot;
        };
        const auto evaluateRemove = [=](const RenderingStressDiagnosticsSnapshot& before,
                                       const RenderingStressDiagnosticsSnapshot& after)
        {
            return RenderingStressSample::EvaluateChurnRemoveMutationEvidenceWindow(
                before,
                after,
                2u,
                ChurnCount,
                Population,
                BeforeAddPopulation,
                1u,
                0u);
        };
        const auto evaluateAdd = [=](const RenderingStressDiagnosticsSnapshot& before,
                                    const RenderingStressDiagnosticsSnapshot& after)
        {
            return RenderingStressSample::EvaluateChurnAddMutationEvidenceWindow(
                before,
                after,
                2u,
                ChurnCount,
                BeforeAddPopulation,
                Population,
                1u,
                0u);
        };

        RenderingStressDiagnosticsSnapshot removeBaseline =
            makeGPUCullingBaseline(Population);
        RenderingStressDiagnosticsSnapshot removed = makeCompletedChurnAction(
            removeBaseline, BeforeAddPopulation, false);
        removed.mutationEvidence.gpuCullingActiveRowPatchedRowCount +=
            ChurnCount;
        removed.mutationEvidence.gpuCullingActiveRowUploadedRowCount +=
            ChurnCount;
        removed.mutationEvidence.gpuCullingActiveRowUploadBytes += 64u;
        const RenderingStressWindowEvaluation removedEvaluation =
            evaluateRemove(removeBaseline, removed);
        EXPECT_TRUE(removedEvaluation.passed) << removedEvaluation.detail;

        RenderingStressDiagnosticsSnapshot missingActive = removed;
        missingActive.mutationEvidence.gpuCullingActiveRowPatchedRowCount -=
            ChurnCount;
        missingActive.mutationEvidence.gpuCullingActiveRowUploadedRowCount -=
            ChurnCount;
        missingActive.mutationEvidence.gpuCullingActiveRowUploadBytes -= 64u;
        EXPECT_FALSE(evaluateRemove(removeBaseline, missingActive).passed);

        RenderingStressDiagnosticsSnapshot removeInstanceWork = removed;
        ++removeInstanceWork.mutationEvidence.gpuCullingInstancePatchedRowCount;
        ++removeInstanceWork.mutationEvidence.gpuCullingInstanceUploadedRowCount;
        ++removeInstanceWork.mutationEvidence.gpuCullingInstanceUploadBytes;
        EXPECT_FALSE(evaluateRemove(removeBaseline, removeInstanceWork).passed);

        RenderingStressDiagnosticsSnapshot removeCandidateWork = removed;
        ++removeCandidateWork.mutationEvidence.gpuCullingCandidatePatchedRowCount;
        ++removeCandidateWork.mutationEvidence.gpuCullingCandidateUploadedRowCount;
        ++removeCandidateWork.mutationEvidence.gpuCullingCandidateUploadBytes;
        EXPECT_FALSE(evaluateRemove(removeBaseline, removeCandidateWork).passed);

        RenderingStressDiagnosticsSnapshot warmAddBaseline =
            makeGPUCullingBaseline(BeforeAddPopulation);
        warmAddBaseline.mutationEvidence.gpuCullingCandidateFullMaterializationCount =
            1u;
        RenderingStressDiagnosticsSnapshot warmTierOne = makeCompletedChurnAction(
            warmAddBaseline, Population, true);
        warmTierOne.mutationEvidence.gpuCullingInstancePatchedRowCount +=
            ChurnCount;
        warmTierOne.mutationEvidence.gpuCullingActiveRowPatchedRowCount +=
            ChurnCount;
        warmTierOne.mutationEvidence.gpuCullingInstanceUploadedRowCount +=
            ChurnCount;
        warmTierOne.mutationEvidence.gpuCullingActiveRowUploadedRowCount +=
            ChurnCount;
        warmTierOne.mutationEvidence.gpuCullingInstanceUploadBytes += 64u;
        warmTierOne.mutationEvidence.gpuCullingActiveRowUploadBytes += 64u;
        const RenderingStressWindowEvaluation warmPending =
            evaluateAdd(warmAddBaseline, warmTierOne);
        EXPECT_FALSE(warmPending.passed);
        EXPECT_TRUE(warmPending.pending);

        RenderingStressDiagnosticsSnapshot warmStillPending = warmTierOne;
        warmStillPending.sourceFrameSequence = 3u;
        warmStillPending.mutationEvidence.completedFrameSequence = 3u;
        warmStillPending.mutationEvidence.completedPresentationCount = 3u;
        const RenderingStressWindowEvaluation warmMissingAtLaterPresentation =
            evaluateAdd(warmAddBaseline, warmStillPending);
        EXPECT_FALSE(warmMissingAtLaterPresentation.passed);
        EXPECT_TRUE(warmMissingAtLaterPresentation.pending);

        RenderingStressDiagnosticsSnapshot warmCandidateReady = warmStillPending;
        warmCandidateReady.mutationEvidence.gpuCullingCandidatePatchedRowCount +=
            ChurnCount;
        warmCandidateReady.mutationEvidence.gpuCullingCandidateUploadedRowCount +=
            ChurnCount;
        warmCandidateReady.mutationEvidence.gpuCullingCandidateUploadBytes += 64u;
        const RenderingStressWindowEvaluation warmCandidateReadyEvaluation =
            evaluateAdd(warmAddBaseline, warmCandidateReady);
        EXPECT_TRUE(warmCandidateReadyEvaluation.passed)
            << warmCandidateReadyEvaluation.detail;

        RenderingStressDiagnosticsSnapshot tierOneBaseline =
            makeGPUCullingBaseline(BeforeAddPopulation);
        RenderingStressDiagnosticsSnapshot tierOneAdd = makeCompletedChurnAction(
            tierOneBaseline, Population, true);
        tierOneAdd.mutationEvidence.gpuCullingInstancePatchedRowCount +=
            ChurnCount;
        tierOneAdd.mutationEvidence.gpuCullingActiveRowPatchedRowCount +=
            ChurnCount;
        tierOneAdd.mutationEvidence.gpuCullingInstanceUploadedRowCount +=
            ChurnCount;
        tierOneAdd.mutationEvidence.gpuCullingActiveRowUploadedRowCount +=
            ChurnCount;
        tierOneAdd.mutationEvidence.gpuCullingInstanceUploadBytes += 64u;
        tierOneAdd.mutationEvidence.gpuCullingActiveRowUploadBytes += 64u;
        const RenderingStressWindowEvaluation tierOneEvaluation =
            evaluateAdd(tierOneBaseline, tierOneAdd);
        EXPECT_TRUE(tierOneEvaluation.passed) << tierOneEvaluation.detail;

        const std::string source = ReadSource(
            "Samples/RenderVerseSamples/Scenes/RenderingStressSample.cpp");
        EXPECT_NE(source.find("Waiting for warm candidate-row evidence"),
                  std::string::npos);
        EXPECT_NE(source.find(
                      "Churn recreation action evidence exceeded its completed-presentation budget."),
                  std::string::npos);
    }

    TEST(RenderingStressSampleValidation,
         CompletionWatermarksAllowOnlyOneBoundedDirtyOrAddTemporalSettle)
    {
        constexpr uint32 Population = 100u;
        constexpr uint32 Count = 10u;
        RenderingStressDiagnosticsSnapshot dirtyBaseline = MakeBaseline();
        dirtyBaseline.renderSceneObjectCount = Population;
        dirtyBaseline.gpuScenePublishedObjectCount = Population;
        dirtyBaseline.gpuDrivenActiveRowCount = Population;
        dirtyBaseline.gpuDrivenActiveRowHighWatermark = Population;
        dirtyBaseline.mutationEvidence.gpuCullingOwnerCount = 2u;
        RenderingStressDiagnosticsSnapshot dirty = dirtyBaseline;
        dirty.sourceFrameSequence = 5u;
        dirty.renderSceneAppliedRevision = 2u;
        dirty.renderSceneRequiredRevision = 2u;
        dirty.mutationEvidence.completedFrameSequence = 5u;
        dirty.mutationEvidence.completedPresentationCount = 2u;
        dirty.mutationEvidence.requiredSceneRevision = 2u;
        dirty.mutationEvidence.appliedSceneRevision = 2u;
        ++dirty.mutationEvidence.sceneIncrementalCommitCount;
        dirty.mutationEvidence.sceneRebuiltObjectCount += Count;
        dirty.mutationEvidence.gpuSceneIncrementalPublicationCount += 2u;
        dirty.mutationEvidence.gpuSceneUpdateCount += Count * 2u;
        dirty.mutationEvidence.gpuSceneMaterializedObjectCount += Count * 2u;
        dirty.mutationEvidence.gpuSceneSubmittedUploadCount += 2u;
        dirty.mutationEvidence.gpuSceneUploadBytes += 64u;
        ++dirty.mutationEvidence.gpuSceneUploadRangeCount;
        AdvanceGPUSceneSubmittedUploadedRows(dirty, Count * 2u);
        dirty.mutationEvidence.gpuCullingInstancePatchedRowCount += Count;
        dirty.mutationEvidence.gpuCullingInstanceUploadedRowCount += Count;
        dirty.mutationEvidence.gpuCullingInstanceUploadBytes += 64u;
        AdvanceAcceptedExtraction(dirty, Count * 4u, Count, Count);
        EXPECT_TRUE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                        dirtyBaseline, dirty, 2u, Count, Population, 1u, 0u)
                        .passed);
        --dirty.mutationEvidence.gpuSceneSubmittedUploadCount;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                         dirtyBaseline, dirty, 2u, Count, Population, 1u, 0u)
                         .passed);
        dirty.mutationEvidence.gpuSceneSubmittedUploadCount += 2u;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                         dirtyBaseline, dirty, 2u, Count, Population, 1u, 0u)
                         .passed);
        --dirty.mutationEvidence.gpuSceneSubmittedUploadCount;
        dirty.mutationEvidence.gpuSceneUpdateCount += Count;
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                         dirtyBaseline, dirty, 2u, Count, Population, 1u, 0u)
                         .passed);
        dirty.mutationEvidence.gpuSceneUpdateCount -= Count;

        RenderingStressDiagnosticsSnapshot skippedSettleStrict = dirty;
        skippedSettleStrict.sourceFrameSequence = 6u;
        skippedSettleStrict.mutationEvidence.completedFrameSequence = 6u;
        skippedSettleStrict.mutationEvidence.completedPresentationCount = 3u;
        ++skippedSettleStrict.sceneStaticReuseCount;
        EXPECT_TRUE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                        dirtyBaseline,
                        skippedSettleStrict,
                        2u,
                        Count,
                        Population,
                        1u,
                        0u)
                        .passed);
        EXPECT_TRUE(RenderingStressSample::EvaluateDirtyDrainWindow(
                        dirty,
                        skippedSettleStrict,
                        Count,
                        Population,
                        1u,
                        0u,
                        true)
                        .passed);

        RenderingStressDiagnosticsSnapshot withoutCumulativeSettle = dirty;
        withoutCumulativeSettle.sourceFrameSequence = 7u;
        withoutCumulativeSettle.mutationEvidence.completedFrameSequence = 7u;
        withoutCumulativeSettle.mutationEvidence.completedPresentationCount = 2u;
        withoutCumulativeSettle.mutationEvidence.gpuSceneIncrementalPublicationCount =
            dirtyBaseline.mutationEvidence.gpuSceneIncrementalPublicationCount +
            1u;
        withoutCumulativeSettle.mutationEvidence.gpuSceneUpdateCount =
            dirtyBaseline.mutationEvidence.gpuSceneUpdateCount + Count;
        withoutCumulativeSettle.mutationEvidence.gpuSceneMaterializedObjectCount =
            dirtyBaseline.mutationEvidence.gpuSceneMaterializedObjectCount + Count;
        withoutCumulativeSettle.mutationEvidence.gpuSceneSubmittedUploadCount =
            dirtyBaseline.mutationEvidence.gpuSceneSubmittedUploadCount + 1u;
        for (uint64& tableRows :
             withoutCumulativeSettle.mutationEvidence
                 .gpuSceneSubmittedUploadedRowCount)
        {
            tableRows = Count;
        }
        RenderingStressDiagnosticsSnapshot noSettleStrict =
            withoutCumulativeSettle;
        noSettleStrict.sourceFrameSequence = 8u;
        noSettleStrict.mutationEvidence.completedFrameSequence = 8u;
        noSettleStrict.mutationEvidence.completedPresentationCount = 3u;
        ++noSettleStrict.sceneStaticReuseCount;
        EXPECT_TRUE(RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
                        dirtyBaseline,
                        noSettleStrict,
                        2u,
                        Count,
                        Population,
                        1u,
                        0u)
                        .passed);
        EXPECT_FALSE(RenderingStressSample::EvaluateDirtyDrainWindow(
                         withoutCumulativeSettle,
                         noSettleStrict,
                         Count,
                         Population,
                         1u,
                         0u,
                         false)
                         .passed);

        RenderingStressDiagnosticsSnapshot addBaseline = MakeBaseline();
        addBaseline.renderSceneObjectCount = Population - Count;
        addBaseline.gpuScenePublishedObjectCount = Population - Count;
        addBaseline.directRasterActiveInstanceCount = Population - Count;
        addBaseline.directRasterActiveInstanceCapacity = Population;
        addBaseline.mutationEvidence.directRasterOwnerCount = 1u;
        RenderingStressDiagnosticsSnapshot add = addBaseline;
        add.sourceFrameSequence = 8u;
        add.renderSceneAppliedRevision = 2u;
        add.renderSceneRequiredRevision = 2u;
        add.renderSceneObjectCount = Population;
        add.gpuScenePublishedObjectCount = Population;
        add.directRasterActiveInstanceCount = Population;
        add.mutationEvidence.completedFrameSequence = 8u;
        add.mutationEvidence.completedPresentationCount = 2u;
        add.mutationEvidence.requiredSceneRevision = 2u;
        add.mutationEvidence.appliedSceneRevision = 2u;
        ++add.mutationEvidence.sceneIncrementalCommitCount;
        add.mutationEvidence.sceneRebuiltObjectCount += Count;
        add.mutationEvidence.gpuSceneIncrementalPublicationCount += 2u;
        add.mutationEvidence.gpuSceneAddCount += Count;
        add.mutationEvidence.gpuSceneUpdateCount += Count;
        add.mutationEvidence.gpuSceneMaterializedObjectCount += Count * 2u;
        add.mutationEvidence.gpuSceneSubmittedUploadCount += 2u;
        add.mutationEvidence.gpuSceneUploadBytes += 64u;
        ++add.mutationEvidence.gpuSceneUploadRangeCount;
        AdvanceGPUSceneSubmittedUploadedRows(add, Count * 2u);
        add.mutationEvidence.directRasterInstancePatchedRowCount += Count;
        add.mutationEvidence.directRasterIndexPatchedRowCount += Count;
        add.mutationEvidence.directRasterInstanceUploadBytes += 64u;
        add.mutationEvidence.directRasterIndexUploadBytes += 64u;
        AdvanceAcceptedExtraction(add, Count * 8u, Count, Count);
        EXPECT_TRUE(RenderingStressSample::EvaluateChurnAddMutationEvidenceWindow(
                        addBaseline,
                        add,
                        2u,
                        Count,
                        Population - Count,
                        Population,
                        0u,
                        1u)
                        .passed);
        add.mutationEvidence.gpuSceneUpdateCount += Count;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnAddMutationEvidenceWindow(
                         addBaseline,
                         add,
                         2u,
                         Count,
                         Population - Count,
                         Population,
                         0u,
                         1u)
                         .passed);
    }

    TEST(RenderingStressSampleValidation,
         CompletionWatermarksFailClosedForEpochSaturationAndRegression)
    {
        RenderingStressDiagnosticsSnapshot baseline = MakeBaseline();
        baseline.renderSceneObjectCount = 10u;
        baseline.gpuScenePublishedObjectCount = 10u;
        baseline.directRasterActiveInstanceCount = 10u;
        baseline.directRasterActiveInstanceCapacity = 10u;
        baseline.mutationEvidence.directRasterOwnerCount = 1u;
        RenderingStressDiagnosticsSnapshot after = baseline;
        after.sourceFrameSequence = 2u;
        after.renderSceneAppliedRevision = 2u;
        after.renderSceneRequiredRevision = 2u;
        after.mutationEvidence.completedFrameSequence = 2u;
        after.mutationEvidence.completedPresentationCount = 2u;
        after.mutationEvidence.requiredSceneRevision = 2u;
        after.mutationEvidence.appliedSceneRevision = 2u;
        ++after.mutationEvidence.sceneIncrementalCommitCount;
        ++after.mutationEvidence.sceneRemovedObjectCount;
        ++after.mutationEvidence.gpuSceneIncrementalPublicationCount;
        ++after.mutationEvidence.gpuSceneRemoveCount;
        ++after.mutationEvidence.gpuSceneSubmittedUploadCount;
        ++after.mutationEvidence.gpuSceneUploadBytes;
        ++after.mutationEvidence.gpuSceneUploadRangeCount;
        AdvanceGPUSceneSubmittedUploadedRows(after, 1u);
        ++after.mutationEvidence.directRasterIndexPatchedRowCount;
        ++after.mutationEvidence.directRasterIndexUploadBytes;
        after.renderSceneObjectCount = 9u;
        after.gpuScenePublishedObjectCount = 9u;
        after.directRasterActiveInstanceCount = 9u;
        AdvanceAcceptedExtraction(after, 2u, 1u, 0u);
        EXPECT_TRUE(RenderingStressSample::EvaluateChurnRemoveMutationEvidenceWindow(
                        baseline, after, 2u, 1u, 10u, 9u, 0u, 1u)
                        .passed);
        ++after.mutationEvidence.directRasterInstanceUploadBytes;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnRemoveMutationEvidenceWindow(
                         baseline, after, 2u, 1u, 10u, 9u, 0u, 1u)
                         .passed);
        --after.mutationEvidence.directRasterInstanceUploadBytes;
        ++after.mutationEvidence.evidenceEpoch;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnRemoveMutationEvidenceWindow(
                         baseline, after, 2u, 1u, 10u, 9u, 0u, 1u)
                         .passed);
        --after.mutationEvidence.evidenceEpoch;
        after.mutationEvidence.saturated = true;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnRemoveMutationEvidenceWindow(
                         baseline, after, 2u, 1u, 10u, 9u, 0u, 1u)
                         .passed);
        after.mutationEvidence.saturated = false;
        after.mutationEvidence.sceneRemovedObjectCount = 0u;
        EXPECT_FALSE(RenderingStressSample::EvaluateChurnRemoveMutationEvidenceWindow(
                         baseline, after, 2u, 1u, 10u, 9u, 0u, 1u)
                         .passed);
    }
} // namespace RVX
