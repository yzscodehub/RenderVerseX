#include "Render/RayTracing/RayTracingScene.h"
#include "Render/RayTracing/RayTracingSceneManager.h"

#include <gtest/gtest.h>

namespace RVX
{
namespace
{
    RayTracingSceneBuildPlan MakePlan()
    {
        RayTracingSceneBuildPlan plan;
        RayTracingBLASBuild build;
        build.key.mesh = {7u, 3u};
        build.key.meshId = (static_cast<uint64>(7u) << 32u) | 3u;
        build.key.meshContentRevision = 41u;
        build.key.submeshIndex = 2u;
        plan.blasBuilds.push_back(build);

        RayTracingTLASInstance instance;
        instance.blasIndex = 0u;
        instance.entityId = 101u;
        instance.materialId = 303u;
        instance.desc.instanceMask = 0x7Fu;
        instance.desc.flags = RHIRayTracingInstanceFlags::ForceNoOpaque;
        instance.alphaTest.enabled = true;
        instance.alphaTest.status = RayTracingMaskedMetadataStatus::Complete;
        instance.alphaTest.baseColorUVSet = 1u;
        instance.alphaTest.baseColorTextureId = 909u;
        plan.instances.push_back(instance);
        return plan;
    }

    TEST(RayTracingSceneLifecycleValidation,
         StructuralSignatureIgnoresTransformOnlyChanges)
    {
        RayTracingSceneBuildPlan plan = MakePlan();
        const RayTracingTLASStructuralSignature initial =
            BuildRayTracingTLASStructuralSignature(plan);

        plan.instances.front().desc.transform[0] = 42.0f;
        const RayTracingTLASStructuralSignature moved =
            BuildRayTracingTLASStructuralSignature(plan);

        EXPECT_EQ(initial, moved);
    }

    TEST(RayTracingSceneLifecycleValidation,
         StructuralSignatureChangesForMeshContentRevision)
    {
        RayTracingSceneBuildPlan plan = MakePlan();
        const RayTracingTLASStructuralSignature initial =
            BuildRayTracingTLASStructuralSignature(plan);

        ++plan.blasBuilds.front().key.meshContentRevision;
        const RayTracingTLASStructuralSignature changed =
            BuildRayTracingTLASStructuralSignature(plan);

        EXPECT_NE(initial, changed);
    }

    TEST(RayTracingSceneLifecycleValidation,
         StructuralSignatureChangesForMaskedMaterialLayout)
    {
        RayTracingSceneBuildPlan plan = MakePlan();
        const RayTracingTLASStructuralSignature initial =
            BuildRayTracingTLASStructuralSignature(plan);

        plan.instances.front().alphaTest.baseColorUVSet = 0u;
        const RayTracingTLASStructuralSignature changed =
            BuildRayTracingTLASStructuralSignature(plan);

        EXPECT_NE(initial, changed);
    }

    TEST(RayTracingSceneLifecycleValidation,
         CompactionCannotPublishBeforeCopyCompletion)
    {
        EXPECT_FALSE(IsRayTracingBLASCompactionPublished(
            RayTracingBLASCompactionStatus::QueryRecorded));
        EXPECT_FALSE(IsRayTracingBLASCompactionPublished(
            RayTracingBLASCompactionStatus::AwaitingCopyCompletion));
        EXPECT_TRUE(IsRayTracingBLASCompactionPublished(
            RayTracingBLASCompactionStatus::Compacted));
    }

    TEST(RayTracingSceneLifecycleValidation,
         CompactionAbortReturnsOnlyRecordedWorkToRetryableStages)
    {
        EXPECT_EQ(RayTracingBLASCompactionStatus::AwaitingQueryRecord,
                  ReleaseUnsubmittedRayTracingBLASCompaction(
                      RayTracingBLASCompactionStatus::QueryRecorded));
        EXPECT_EQ(RayTracingBLASCompactionStatus::ReadyToRecordCopy,
                  ReleaseUnsubmittedRayTracingBLASCompaction(
                      RayTracingBLASCompactionStatus::CopyRecorded));
        EXPECT_EQ(RayTracingBLASCompactionStatus::AwaitingCopyCompletion,
                  ReleaseUnsubmittedRayTracingBLASCompaction(
                      RayTracingBLASCompactionStatus::AwaitingCopyCompletion));
    }

    TEST(RayTracingSceneLifecycleValidation,
         CompactionOnlyTreatsAcceptedSubmissionStagesAsBound)
    {
        EXPECT_FALSE(IsRayTracingBLASCompactionSubmissionBound(
            RayTracingBLASCompactionStatus::QueryRecorded));
        EXPECT_TRUE(IsRayTracingBLASCompactionSubmissionBound(
            RayTracingBLASCompactionStatus::AwaitingQueryCompletion));
        EXPECT_FALSE(IsRayTracingBLASCompactionSubmissionBound(
            RayTracingBLASCompactionStatus::CopyRecorded));
        EXPECT_TRUE(IsRayTracingBLASCompactionSubmissionBound(
            RayTracingBLASCompactionStatus::AwaitingCopyCompletion));
    }

    TEST(RayTracingSceneLifecycleValidation,
         CompactionRejectsZeroOversizedAndUnalignedReadbackSizes)
    {
        EXPECT_EQ(RayTracingBLASCompactedSizeDisposition::Invalid,
                  ClassifyRayTracingBLASCompactedSize(4096u, 0u));
        EXPECT_EQ(RayTracingBLASCompactedSizeDisposition::Invalid,
                  ClassifyRayTracingBLASCompactedSize(4096u, 4352u));
        EXPECT_EQ(RayTracingBLASCompactedSizeDisposition::Invalid,
                  ClassifyRayTracingBLASCompactedSize(4096u, 1025u));
        EXPECT_EQ(RayTracingBLASCompactedSizeDisposition::Unchanged,
                  ClassifyRayTracingBLASCompactedSize(4096u, 4096u));
        EXPECT_EQ(RayTracingBLASCompactedSizeDisposition::Compactable,
                  ClassifyRayTracingBLASCompactedSize(4096u, 2048u));
    }
} // namespace
} // namespace RVX
