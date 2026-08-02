#include "Render/Passes/MeshPassProcessor.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace RVX;

namespace
{
    MeshBatch MakeBatch(MaterialRenderMode mode = MaterialRenderMode::Opaque,
                        RenderBatchFlags extraFlags = RenderBatchFlags::None)
    {
        MeshBatch batch;
        batch.objectId = 71;
        batch.mesh = {11, 3};
        batch.material = {19, 5};
        batch.submeshIndex = 2;
        batch.primitiveData = 7;
        batch.indexType = MeshUploadIndexType::UInt32;
        batch.geometry = {12, 36, -2,
                          MeshUploadPrimitiveTopology::Triangles};
        batch.materialMode = mode;
        batch.flags = extraFlags;
        if (mode == MaterialRenderMode::Masked)
        {
            batch.flags |= RenderBatchFlags::Masked;
        }
        if (mode == MaterialRenderMode::Transparent)
        {
            batch.flags |= RenderBatchFlags::Transparent;
        }
        return batch;
    }

    MeshPassProcessorInput MakeInput(
        MaterialRenderMode mode = MaterialRenderMode::Opaque,
        RenderBatchFlags flags = RenderBatchFlags::None)
    {
        MeshPassProcessorInput input;
        input.packet = BuildLegacyMaterialDrawPacket(MakeBatch(mode, flags));
        input.sourceOrdinal = 9;
        input.viewDepth = 42.0f;
        return input;
    }

    bool HasBinding(const MeshPassProcessorResult& result,
                    MeshPassBindingRequirements requirement)
    {
        return HasMeshPassBindingRequirement(
            result.groupKey.layout.bindings, requirement);
    }

    bool HasStream(const MeshPassProcessorResult& result,
                   MeshPassVertexStreams stream)
    {
        return (static_cast<uint32>(result.groupKey.layout.vertexStreams) &
                static_cast<uint32>(stream)) != 0;
    }
} // namespace

TEST(MeshPassProcessorValidation, EnumerationsHaveStableValuesAndNames)
{
    EXPECT_EQ(static_cast<uint8>(MeshPassDisposition::GPUCandidate), 0U);
    EXPECT_EQ(static_cast<uint8>(MeshPassDisposition::Direct), 1U);
    EXPECT_EQ(static_cast<uint8>(MeshPassDisposition::Skip), 2U);
    EXPECT_STREQ(ToName(MeshPassDisposition::GPUCandidate), "GPUCandidate");
    EXPECT_STREQ(GetName(MeshPassDisposition::Direct), "Direct");
    EXPECT_STREQ(ToName(MeshPassDisposition::Skip), "Skip");
    EXPECT_STREQ(ToName(static_cast<MeshPassDisposition>(255)), "Unknown");

    for (uint8 value = 0;
         value <= static_cast<uint8>(MeshPassEligibilityReason::Count);
         ++value)
    {
        EXPECT_STRNE(ToName(static_cast<MeshPassEligibilityReason>(value)),
                     "Unknown");
    }
    EXPECT_STREQ(ToName(MeshPassEligibilityReason::UnsupportedTopology),
                 "UnsupportedTopology");
    EXPECT_STREQ(ToName(MeshPassEligibilityReason::UnsupportedIndexType),
                 "UnsupportedIndexType");
    EXPECT_STREQ(ToName(MeshPassEligibilityReason::ResourcePending),
                 "ResourcePending");
    EXPECT_STREQ(ToName(static_cast<MeshPassEligibilityReason>(255)),
                 "Unknown");
}

TEST(MeshPassProcessorValidation, PassMatrixIsExactAndValueOnly)
{
    DepthMeshPassProcessor depth;
    OpaqueMeshPassProcessor opaque;
    TransparentMeshPassProcessor transparent;

    const MeshPassProcessorInput opaqueInput = MakeInput();
    const MeshPassProcessorInput maskedInput =
        MakeInput(MaterialRenderMode::Masked);
    const MeshPassProcessorInput transparentInput =
        MakeInput(MaterialRenderMode::Transparent);

    EXPECT_TRUE(depth.Process(opaqueInput).IsGPUCandidate());
    EXPECT_FALSE(depth.Process(maskedInput).IsGPUCandidate());
    EXPECT_EQ(depth.Process(maskedInput).reason,
              MeshPassEligibilityReason::PassRequiresDirect);
    EXPECT_FALSE(depth.Process(transparentInput).IsRelevant());
    EXPECT_EQ(depth.Process(opaqueInput).packet.pass, RenderPassKind::Depth);

    EXPECT_TRUE(opaque.Process(opaqueInput).IsGPUCandidate());
    EXPECT_TRUE(opaque.Process(maskedInput).IsGPUCandidate());
    EXPECT_FALSE(opaque.Process(transparentInput).IsRelevant());
    EXPECT_EQ(opaque.Process(maskedInput).packet.pass,
              RenderPassKind::Opaque);

    EXPECT_FALSE(transparent.Process(opaqueInput).IsRelevant());
    EXPECT_FALSE(transparent.Process(maskedInput).IsRelevant());
    const MeshPassProcessorResult transparentResult =
        transparent.Process(transparentInput);
    EXPECT_TRUE(transparentResult.IsRelevant());
    EXPECT_EQ(transparentResult.disposition, MeshPassDisposition::Direct);
    EXPECT_EQ(transparentResult.reason,
              MeshPassEligibilityReason::Transparent);
    EXPECT_EQ(transparentResult.packet.pass, RenderPassKind::Transparent);
}

TEST(MeshPassProcessorValidation,
     PreservesArgumentsAndSelectsPassSpecificBindings)
{
    const MeshPassProcessorInput input = MakeInput(MaterialRenderMode::Masked);
    DepthMeshPassProcessor depth;
    OpaqueMeshPassProcessor opaque;

    const MeshPassProcessorResult depthResult = depth.Process(input);
    EXPECT_EQ(depthResult.packet.arguments, input.packet.arguments);
    EXPECT_EQ(depthResult.packet.geometryKey, input.packet.geometryKey);
    EXPECT_TRUE(HasBinding(depthResult, MeshPassBindingRequirements::Frame));
    EXPECT_TRUE(HasBinding(depthResult, MeshPassBindingRequirements::Object));
    EXPECT_TRUE(HasBinding(depthResult, MeshPassBindingRequirements::Geometry));
    EXPECT_TRUE(HasBinding(depthResult, MeshPassBindingRequirements::Material));
    EXPECT_TRUE(HasStream(depthResult, MeshPassVertexStreams::Position));
    EXPECT_TRUE(HasStream(depthResult, MeshPassVertexStreams::TexCoord));
    EXPECT_FALSE(HasStream(depthResult, MeshPassVertexStreams::InstanceIndex));
    EXPECT_EQ(depthResult.groupKey.layout.primitiveDataBinding,
              PrimitiveDataBinding::PerDrawConstants);
    EXPECT_EQ(depthResult.directLayout.vertexStreams,
              depthResult.groupKey.layout.vertexStreams);
    EXPECT_EQ(depthResult.directLayout.bindings,
              depthResult.groupKey.layout.bindings);

    const MeshPassProcessorResult opaqueResult = opaque.Process(input);
    EXPECT_TRUE(HasStream(opaqueResult, MeshPassVertexStreams::Normal));
    EXPECT_TRUE(HasStream(opaqueResult, MeshPassVertexStreams::Tangent));
    EXPECT_EQ(opaqueResult.groupKey.pipeline,
              opaqueResult.packet.pipelineKey);
    EXPECT_EQ(opaqueResult.groupKey.geometry,
              opaqueResult.packet.geometryKey);
    EXPECT_EQ(opaqueResult.groupKey.material,
              opaqueResult.packet.materialKey);
}

TEST(MeshPassProcessorValidation, ShadowCasterMatrixPreservesCurrentCoverage)
{
    ShadowMeshPassProcessor shadow;
    EXPECT_FALSE(shadow.Process(MakeInput()).IsRelevant());

    const RenderBatchFlags casts = RenderBatchFlags::CastsShadow;
    const MeshPassProcessorResult opaque =
        shadow.Process(MakeInput(MaterialRenderMode::Opaque, casts));
    const MeshPassProcessorResult masked =
        shadow.Process(MakeInput(MaterialRenderMode::Masked, casts));
    const MeshPassProcessorResult transparent =
        shadow.Process(MakeInput(MaterialRenderMode::Transparent, casts));
    const MeshPassProcessorResult skinned = shadow.Process(
        MakeInput(MaterialRenderMode::Opaque,
                  casts | RenderBatchFlags::Skinned));

    EXPECT_EQ(opaque.packet.pass, RenderPassKind::Shadow);
    EXPECT_EQ(opaque.reason, MeshPassEligibilityReason::PassRequiresDirect);
    EXPECT_EQ(masked.reason, MeshPassEligibilityReason::PassRequiresDirect);
    EXPECT_EQ(transparent.reason, MeshPassEligibilityReason::Transparent);
    EXPECT_EQ(skinned.reason, MeshPassEligibilityReason::Skinned);
    EXPECT_TRUE(HasBinding(masked, MeshPassBindingRequirements::Material));
    EXPECT_TRUE(HasBinding(skinned, MeshPassBindingRequirements::Skinning));
}

TEST(MeshPassProcessorValidation,
     EligibilityUsesFixedPrecedenceAndExactlyOneReason)
{
    OpaqueMeshPassProcessor processor;
    MeshPassProcessorStats stats;

    MeshPassProcessorInput skinned =
        MakeInput(MaterialRenderMode::Opaque, RenderBatchFlags::Skinned);
    skinned.availability.specialMaterial = true;
    skinned.availability.pipeline = MeshPassResourceAvailability::Unavailable;
    MeshPassProcessorResult result = processor.Process(skinned);
    EXPECT_EQ(result.reason, MeshPassEligibilityReason::Skinned);
    stats.Record(result);

    MeshPassProcessorInput special = MakeInput();
    special.availability.specialMaterial = true;
    special.packet.pipelineKey.topology = MeshUploadPrimitiveTopology::Lines;
    result = processor.Process(special);
    EXPECT_EQ(result.reason, MeshPassEligibilityReason::SpecialMaterial);
    stats.Record(result);

    MeshPassProcessorInput topology = MakeInput();
    topology.packet.pipelineKey.topology = MeshUploadPrimitiveTopology::Lines;
    topology.packet.geometryKey.indexType = MeshUploadIndexType::UInt16;
    result = processor.Process(topology);
    EXPECT_EQ(result.reason, MeshPassEligibilityReason::UnsupportedTopology);
    stats.Record(result);

    MeshPassProcessorInput indexType = MakeInput();
    indexType.packet.geometryKey.indexType = MeshUploadIndexType::UInt16;
    indexType.availability.pipeline = MeshPassResourceAvailability::Unavailable;
    result = processor.Process(indexType);
    EXPECT_EQ(result.reason, MeshPassEligibilityReason::UnsupportedIndexType);
    stats.Record(result);

    MeshPassProcessorInput pipeline = MakeInput();
    pipeline.availability.pipeline = MeshPassResourceAvailability::Unavailable;
    pipeline.availability.geometry = MeshPassResourceAvailability::Unavailable;
    result = processor.Process(pipeline);
    EXPECT_EQ(result.reason, MeshPassEligibilityReason::PipelineUnavailable);
    stats.Record(result);

    MeshPassProcessorInput geometry = MakeInput();
    geometry.availability.geometry = MeshPassResourceAvailability::Unavailable;
    geometry.availability.material = MeshPassResourceAvailability::Pending;
    result = processor.Process(geometry);
    EXPECT_EQ(result.reason, MeshPassEligibilityReason::GeometryUnavailable);
    stats.Record(result);

    MeshPassProcessorInput pending = MakeInput();
    pending.availability.geometry = MeshPassResourceAvailability::Pending;
    result = processor.Process(pending);
    EXPECT_EQ(result.reason, MeshPassEligibilityReason::ResourcePending);
    stats.Record(result);

    MeshPassProcessorInput unavailable = MakeInput();
    unavailable.availability.material =
        MeshPassResourceAvailability::Unavailable;
    result = processor.Process(unavailable);
    EXPECT_EQ(result.reason, MeshPassEligibilityReason::ResourceUnavailable);
    stats.Record(result);

    result = processor.Process(MakeInput());
    EXPECT_TRUE(result.IsGPUCandidate());
    EXPECT_EQ(result.reason, MeshPassEligibilityReason::None);
    stats.Record(result);

    EXPECT_EQ(stats.relevantPacketCount, 9U);
    EXPECT_EQ(stats.gpuCandidatePacketCount, 1U);
    EXPECT_EQ(stats.directPacketCount, 8U);
    EXPECT_TRUE(stats.HasCompleteRelevantOutcome());
    EXPECT_TRUE(stats.HasExactlyOneReasonPerRejectedPacket());
}

TEST(MeshPassProcessorValidation,
     MissingMaterialUsesDefaultBindingAndNeverInfersPending)
{
    MeshBatch batch = MakeBatch();
    batch.material = {};
    batch.flags |= RenderBatchFlags::MissingMaterial;
    MeshPassProcessorInput input;
    input.packet = BuildLegacyMaterialDrawPacket(batch);
    input.availability.material = MeshPassResourceAvailability::Unavailable;

    OpaqueMeshPassProcessor processor;
    const MeshPassProcessorResult result = processor.Process(input);
    EXPECT_TRUE(result.IsGPUCandidate());
    EXPECT_EQ(result.reason, MeshPassEligibilityReason::None);
    EXPECT_TRUE(HasBinding(result,
                           MeshPassBindingRequirements::DefaultMaterial));
    EXPECT_FALSE(HasBinding(result, MeshPassBindingRequirements::Material));
    EXPECT_FALSE(result.groupKey.material.material.IsValid());
}

TEST(MeshPassProcessorValidation,
     DrawGroupKeyCoversExactStateAndHasCollisionSafeOrdering)
{
    OpaqueMeshPassProcessor processor;
    const RenderDrawGroupKey base = processor.Process(MakeInput()).groupKey;
    const uint64 baseHash = GetStableHash(base);
    const RenderDrawGroupKeyLess less;

    std::vector<RenderDrawGroupKey> changed;
    changed.push_back(base);
    changed.back().pass = RenderPassKind::Depth;
    changed.push_back(base);
    changed.back().pipeline.topology = MeshUploadPrimitiveTopology::Lines;
    changed.push_back(base);
    ++changed.back().geometry.mesh.generation;
    changed.push_back(base);
    ++changed.back().geometry.submeshIndex;
    changed.push_back(base);
    changed.back().geometry.indexType = MeshUploadIndexType::UInt16;
    changed.push_back(base);
    ++changed.back().material.material.generation;
    changed.push_back(base);
    changed.back().layout.vertexStreams |= MeshPassVertexStreams::BoneIndices;
    changed.push_back(base);
    changed.back().layout.bindings |= MeshPassBindingRequirements::Skinning;
    changed.push_back(base);
    changed.back().layout.primitiveDataBinding =
        PrimitiveDataBinding::PerDrawConstants;

    for (const RenderDrawGroupKey& key : changed)
    {
        EXPECT_NE(key, base);
        EXPECT_NE(GetStableHash(key), baseHash);
        EXPECT_NE(less(key, base), less(base, key));
    }
}

TEST(MeshPassProcessorValidation,
     GroupingIsDeterministicAcrossInputPermutationAndOmitsDirectPackets)
{
    OpaqueMeshPassProcessor opaque;
    TransparentMeshPassProcessor transparent;
    std::vector<MeshPassProcessorResult> first;
    for (uint32 ordinal : {4U, 1U, 3U, 2U})
    {
        MeshPassProcessorInput input = MakeInput();
        input.sourceOrdinal = ordinal;
        if (ordinal >= 3U)
        {
            ++input.packet.materialKey.material.slot;
        }
        first.push_back(opaque.Process(input));
    }
    MeshPassProcessorInput transparentInput =
        MakeInput(MaterialRenderMode::Transparent);
    transparentInput.sourceOrdinal = 0;
    first.push_back(transparent.Process(transparentInput));

    std::vector<MeshPassProcessorResult> second = first;
    std::reverse(second.begin(), second.end());
    std::vector<MeshPassProcessorResult> sortedFirst;
    std::vector<MeshPassProcessorResult> sortedSecond;
    std::vector<RenderDrawGroupRange> groupsFirst;
    std::vector<RenderDrawGroupRange> groupsSecond;
    BuildDeterministicRenderDrawGroups(
        first, sortedFirst, groupsFirst);
    BuildDeterministicRenderDrawGroups(
        second, sortedSecond, groupsSecond);

    ASSERT_EQ(sortedFirst.size(), 4U);
    ASSERT_EQ(sortedSecond.size(), sortedFirst.size());
    ASSERT_EQ(groupsFirst.size(), 2U);
    ASSERT_EQ(groupsSecond.size(), groupsFirst.size());
    for (size_t index = 0; index < sortedFirst.size(); ++index)
    {
        EXPECT_EQ(sortedFirst[index].groupKey, sortedSecond[index].groupKey);
        EXPECT_EQ(sortedFirst[index].sourceOrdinal,
                  sortedSecond[index].sourceOrdinal);
    }
    for (size_t index = 0; index < groupsFirst.size(); ++index)
    {
        EXPECT_EQ(groupsFirst[index].key, groupsSecond[index].key);
        EXPECT_EQ(groupsFirst[index].first, groupsSecond[index].first);
        EXPECT_EQ(groupsFirst[index].count, groupsSecond[index].count);
    }
}

TEST(MeshPassProcessorValidation,
     DuplicateSourceOrdinalsUseStablePacketValueTieBreakers)
{
    OpaqueMeshPassProcessor processor;
    MeshPassProcessorInput firstInput = MakeInput();
    MeshPassProcessorInput secondInput = MakeInput();
    firstInput.sourceOrdinal = 3;
    secondInput.sourceOrdinal = 3;
    firstInput.packet.objectId = 90;
    secondInput.packet.objectId = 80;

    const std::vector<MeshPassProcessorResult> forward = {
        processor.Process(firstInput), processor.Process(secondInput)};
    const std::vector<MeshPassProcessorResult> reverse = {
        processor.Process(secondInput), processor.Process(firstInput)};
    std::vector<MeshPassProcessorResult> sortedForward;
    std::vector<MeshPassProcessorResult> sortedReverse;
    std::vector<RenderDrawGroupRange> groupsForward;
    std::vector<RenderDrawGroupRange> groupsReverse;
    BuildDeterministicRenderDrawGroups(
        forward, sortedForward, groupsForward);
    BuildDeterministicRenderDrawGroups(
        reverse, sortedReverse, groupsReverse);

    ASSERT_EQ(sortedForward.size(), 2U);
    ASSERT_EQ(sortedReverse.size(), 2U);
    EXPECT_EQ(sortedForward[0].packet.objectId, 80U);
    EXPECT_EQ(sortedReverse[0].packet.objectId, 80U);
    EXPECT_EQ(sortedForward[1].packet.objectId, 90U);
    EXPECT_EQ(sortedReverse[1].packet.objectId, 90U);
}

TEST(MeshPassProcessorValidation, TransparentProcessorPreservesSourceOrderValues)
{
    TransparentMeshPassProcessor processor;
    std::vector<MeshPassProcessorResult> results;
    for (uint32 ordinal : {0U, 1U, 2U})
    {
        MeshPassProcessorInput input =
            MakeInput(MaterialRenderMode::Transparent);
        input.sourceOrdinal = ordinal;
        input.viewDepth = 30.0f - static_cast<float32>(ordinal);
        results.push_back(processor.Process(input));
    }

    ASSERT_EQ(results.size(), 3U);
    for (uint32 index = 0; index < results.size(); ++index)
    {
        EXPECT_EQ(results[index].sourceOrdinal, index);
        EXPECT_FLOAT_EQ(results[index].viewDepth,
                        30.0f - static_cast<float32>(index));
        EXPECT_EQ(results[index].disposition, MeshPassDisposition::Direct);
    }
}

TEST(MeshPassProcessorValidation,
     PacketStreamsOwnStatsAndFinalizeOnlyGPUCandidates)
{
    OpaqueMeshPassProcessor opaque;
    TransparentMeshPassProcessor transparent;
    SceneMeshPassPreparation preparation;

    MeshPassProcessorInput first = MakeInput();
    first.sourceOrdinal = 2;
    MeshPassProcessorInput second = MakeInput();
    second.sourceOrdinal = 1;
    MeshPassProcessorInput direct =
        MakeInput(MaterialRenderMode::Transparent);
    direct.sourceOrdinal = 0;

    preparation.opaque.Record(opaque.Process(first));
    preparation.opaque.Record(opaque.Process(second));
    preparation.opaque.Record(transparent.Process(direct));
    preparation.opaque.FinalizeGroups();

    EXPECT_EQ(preparation.opaque.packets.size(), 3U);
    EXPECT_EQ(preparation.opaque.stats.inputPacketCount, 3U);
    EXPECT_EQ(preparation.opaque.stats.gpuCandidatePacketCount, 2U);
    EXPECT_EQ(preparation.opaque.stats.directPacketCount, 1U);
    ASSERT_EQ(preparation.opaque.sortedGPUCandidates.size(), 2U);
    ASSERT_EQ(preparation.opaque.groups.size(), 1U);
    EXPECT_EQ(preparation.opaque.sortedGPUCandidates[0].sourceOrdinal, 1U);
    EXPECT_EQ(preparation.opaque.sortedGPUCandidates[1].sourceOrdinal, 2U);

    preparation.Clear();
    EXPECT_TRUE(preparation.opaque.packets.empty());
    EXPECT_TRUE(preparation.opaque.sortedGPUCandidates.empty());
    EXPECT_TRUE(preparation.opaque.groups.empty());
    EXPECT_EQ(preparation.opaque.stats.inputPacketCount, 0U);
}
