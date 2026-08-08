#include "Render/Passes/MeshPassProcessor.h"
#include "Render/Submission/RenderInstanceBatchPlan.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace
{
    RVX::DirectDrawPacket MakePacket(RVX::uint32 ordinal,
                                     RVX::uint32 materialSlot = 7,
                                     RVX::RenderPassKind pass =
                                         RVX::RenderPassKind::Opaque)
    {
        RVX::DirectDrawPacket direct;
        direct.sourcePacketIndex = ordinal;
        direct.sourceOrdinal = ordinal;
        direct.packet.objectId = 1000 + ordinal;
        direct.packet.primitiveData = ordinal;
        direct.packet.submeshIndex = 0;
        direct.packet.pass = pass;
        direct.packet.pipelineKey.materialVariant =
            RVX::MaterialPipelineVariant::Opaque;
        direct.packet.pipelineKey.topology =
            RVX::MeshUploadPrimitiveTopology::Triangles;
        direct.packet.geometryKey.mesh = {3, 1};
        direct.packet.geometryKey.submeshIndex = 0;
        direct.packet.geometryKey.indexType =
            RVX::MeshUploadIndexType::UInt32;
        direct.packet.materialKey.material = {materialSlot, 1};
        direct.packet.materialKey.materialMode =
            RVX::MaterialRenderMode::Opaque;
        direct.packet.arguments = {240, 1, 12, -3, 0};
        direct.packet.flags = RVX::RenderDrawFlags::CastsShadow |
                              RVX::RenderDrawFlags::ReceivesShadow;
        direct.layout.vertexStreams = RVX::MeshPassVertexStreams::Position |
                                      RVX::MeshPassVertexStreams::Normal |
                                      RVX::MeshPassVertexStreams::TexCoord |
                                      RVX::MeshPassVertexStreams::Tangent;
        direct.layout.bindings = RVX::MeshPassBindingRequirements::Frame |
                                 RVX::MeshPassBindingRequirements::Object |
                                 RVX::MeshPassBindingRequirements::Geometry |
                                 RVX::MeshPassBindingRequirements::Material;
        direct.layout.primitiveDataBinding =
            RVX::PrimitiveDataBinding::PerDrawConstants;
        direct.packetId.frameSequence = 9;
        direct.packetId.viewOrdinal = 0;
        direct.packetId.pass = pass;
        direct.packetId.objectId = direct.packet.objectId;
        direct.packetId.primitiveData = ordinal;
        direct.packetId.mesh = direct.packet.geometryKey.mesh;
        direct.packetId.logicalSubmeshIndex = 0;
        direct.packetId.geometrySubmeshIndex = 0;
        direct.packetId.sourcePacketIndex = ordinal;
        direct.packetId.sourceOrdinal = ordinal;
        return direct;
    }

    std::vector<RVX::RenderDrawPacketId> PacketIds(
        const RVX::RenderInstanceBatchPlan& plan)
    {
        std::vector<RVX::RenderDrawPacketId> ids;
        for (const RVX::RenderInstanceBatch& batch : plan.batches)
        {
            for (const RVX::RenderInstanceBatchMember& member : batch.members)
            {
                ids.push_back(member.packetId);
            }
        }
        return ids;
    }
} // namespace

TEST(RenderInstanceBatchPlanValidation, AutoMerges125ExactRigidOpaquePackets)
{
    RVX::DirectDrawPacketBatch input;
    input.pass = RVX::RenderPassKind::Opaque;
    for (RVX::uint32 index = 0; index < 125; ++index)
    {
        input.packets.push_back(MakePacket(index));
    }

    const RVX::RenderInstanceBatchPlan plan =
        RVX::BuildRenderInstanceBatchPlan(
            input, RVX::RenderInstancingMode::Auto);
    ASSERT_TRUE(plan.IsComplete());
    ASSERT_EQ(plan.batches.size(), 1u);
    EXPECT_TRUE(plan.batches[0].instanced);
    EXPECT_EQ(plan.batches[0].members.size(), 125u);
    EXPECT_EQ(plan.executedPacketCount, 125u);
    EXPECT_EQ(plan.submittedDrawCount, 1u);
    EXPECT_EQ(plan.submittedInstanceCount, 125u);
    EXPECT_EQ(plan.instancedBatchCount, 1u);
    EXPECT_EQ(plan.batches[0].key.layout.primitiveDataBinding,
              RVX::PrimitiveDataBinding::InstanceBuffer);
}

TEST(RenderInstanceBatchPlanValidation,
     OneHundredThousandRigidInstancesProduceOneStableSubmissionBatch)
{
    constexpr RVX::uint32 instanceCount = 100'000;
    RVX::DirectDrawPacketBatch input;
    input.pass = RVX::RenderPassKind::Opaque;
    input.packets.reserve(instanceCount);
    for (RVX::uint32 index = 0; index < instanceCount; ++index)
        input.packets.push_back(MakePacket(index));

    const auto begin = std::chrono::steady_clock::now();
    const RVX::RenderInstanceBatchPlan plan =
        RVX::BuildRenderInstanceBatchPlan(
            input, RVX::RenderInstancingMode::Auto);
    const auto end = std::chrono::steady_clock::now();

    ASSERT_TRUE(plan.IsComplete());
    ASSERT_EQ(plan.batches.size(), 1U);
    EXPECT_TRUE(plan.batches.front().instanced);
    EXPECT_EQ(plan.batches.front().members.size(), instanceCount);
    EXPECT_EQ(plan.executedPacketCount, instanceCount);
    EXPECT_EQ(plan.submittedDrawCount, 1U);
    EXPECT_EQ(plan.submittedInstanceCount, instanceCount);
    EXPECT_EQ(plan.instancedBatchCount, 1U);
    RecordProperty(
        "instance_batch_100000_us",
        std::chrono::duration_cast<std::chrono::microseconds>(end - begin)
            .count());
}

TEST(RenderInstanceBatchPlanValidation, DisabledPreservesOneDrawPerPacket)
{
    RVX::DirectDrawPacketBatch input;
    input.pass = RVX::RenderPassKind::Opaque;
    for (RVX::uint32 index = 0; index < 5; ++index)
    {
        input.packets.push_back(MakePacket(index));
    }

    const RVX::RenderInstanceBatchPlan plan =
        RVX::BuildRenderInstanceBatchPlan(
            input, RVX::RenderInstancingMode::Disabled);
    ASSERT_EQ(plan.batches.size(), 5u);
    EXPECT_EQ(plan.submittedDrawCount, 5u);
    EXPECT_EQ(plan.submittedInstanceCount, 5u);
    EXPECT_EQ(plan.instancedBatchCount, 0u);
    for (const RVX::RenderInstanceBatch& batch : plan.batches)
    {
        EXPECT_FALSE(batch.instanced);
        EXPECT_EQ(batch.reason, RVX::RenderInstanceBatchReason::ModeDisabled);
    }
}

TEST(RenderInstanceBatchPlanValidation, AnyRenderAffectingKeyDifferenceSplits)
{
    RVX::DirectDrawPacketBatch input;
    input.pass = RVX::RenderPassKind::Opaque;
    input.packets.push_back(MakePacket(0));

    RVX::DirectDrawPacket material = MakePacket(1);
    material.packet.materialKey.material.slot++;
    input.packets.push_back(material);

    RVX::DirectDrawPacket lod = MakePacket(2);
    lod.packet.geometryKey.submeshIndex = 1;
    lod.packet.submeshIndex = 1;
    input.packets.push_back(lod);

    RVX::DirectDrawPacket flags = MakePacket(3);
    flags.packet.flags = RVX::RenderDrawFlags::CastsShadow;
    input.packets.push_back(flags);

    RVX::DirectDrawPacket indices = MakePacket(4);
    indices.packet.arguments.firstIndex++;
    input.packets.push_back(indices);

    RVX::DirectDrawPacket layout = MakePacket(5);
    layout.layout.bindings = RVX::MeshPassBindingRequirements::Frame |
                             RVX::MeshPassBindingRequirements::Geometry;
    input.packets.push_back(layout);

    const RVX::RenderInstanceBatchPlan plan =
        RVX::BuildRenderInstanceBatchPlan(
            input, RVX::RenderInstancingMode::Auto);
    EXPECT_EQ(plan.batches.size(), 6u);
    EXPECT_EQ(plan.instancedBatchCount, 0u);
    EXPECT_EQ(plan.submittedDrawCount, 6u);
}

TEST(RenderInstanceBatchPlanValidation,
     DifferentMaterialHandlesMergeOnlyWithSameInstanceBindingKey)
{
    RVX::DirectDrawPacketBatch compatible;
    compatible.pass = RVX::RenderPassKind::Opaque;
    for (RVX::uint32 index = 0; index < 5; ++index)
    {
        RVX::DirectDrawPacket packet = MakePacket(index, 20 + index);
        packet.packet.materialInstanceKey.textureBindingHash = 0xA55A1234u;
        packet.packet.materialInstanceKey.parameterTableCompatible = true;
        compatible.packets.push_back(packet);
    }

    const RVX::RenderInstanceBatchPlan merged =
        RVX::BuildRenderInstanceBatchPlan(
            compatible, RVX::RenderInstancingMode::Auto);
    ASSERT_EQ(merged.batches.size(), 1u);
    EXPECT_TRUE(merged.batches[0].instanced);
    EXPECT_TRUE(merged.batches[0].key.usesMaterialParameterTable);
    EXPECT_EQ(merged.batches[0].members.size(), 5u);

    RVX::DirectDrawPacketBatch incompatible = compatible;
    incompatible.packets.back().packet.materialInstanceKey.textureBindingHash++;
    const RVX::RenderInstanceBatchPlan split =
        RVX::BuildRenderInstanceBatchPlan(
            incompatible, RVX::RenderInstancingMode::Auto);
    ASSERT_EQ(split.batches.size(), 2u);
    EXPECT_EQ(split.instancedBatchCount, 1u);
    EXPECT_EQ(split.submittedDrawCount, 2u);
}

TEST(RenderInstanceBatchPlanValidation, TransparentAndSkinnedRemainDirect)
{
    RVX::DirectDrawPacketBatch input;
    input.pass = RVX::RenderPassKind::Opaque;
    RVX::DirectDrawPacket transparent = MakePacket(0);
    transparent.packet.flags = RVX::RenderDrawFlags::Transparent;
    transparent.packet.materialKey.materialMode =
        RVX::MaterialRenderMode::Transparent;
    transparent.packet.pipelineKey.materialVariant =
        RVX::MaterialPipelineVariant::Transparent;
    input.packets.push_back(transparent);
    RVX::DirectDrawPacket skinned = MakePacket(1);
    skinned.packet.flags = RVX::RenderDrawFlags::Skinned;
    skinned.packet.pipelineKey.skinned = true;
    input.packets.push_back(skinned);

    const RVX::RenderInstanceBatchPlan plan =
        RVX::BuildRenderInstanceBatchPlan(
            input, RVX::RenderInstancingMode::Auto);
    ASSERT_EQ(plan.batches.size(), 2u);
    EXPECT_EQ(plan.instancedBatchCount, 0u);
    EXPECT_EQ(plan.submittedDrawCount, 2u);
}

TEST(RenderInstanceBatchPlanValidation, PacketIdentityOrderIsDeterministic)
{
    RVX::DirectDrawPacketBatch forward;
    forward.pass = RVX::RenderPassKind::Opaque;
    for (RVX::uint32 index = 0; index < 6; ++index)
    {
        forward.packets.push_back(MakePacket(index, index % 2));
    }
    RVX::DirectDrawPacketBatch reverse = forward;
    std::reverse(reverse.packets.begin(), reverse.packets.end());

    const RVX::RenderInstanceBatchPlan first =
        RVX::BuildRenderInstanceBatchPlan(
            forward, RVX::RenderInstancingMode::Auto);
    const RVX::RenderInstanceBatchPlan second =
        RVX::BuildRenderInstanceBatchPlan(
            reverse, RVX::RenderInstancingMode::Auto);
    ASSERT_EQ(first.batches.size(), second.batches.size());
    EXPECT_EQ(PacketIds(first), PacketIds(second));
    for (size_t index = 0; index < first.batches.size(); ++index)
    {
        EXPECT_EQ(first.batches[index].key, second.batches[index].key);
    }
}

TEST(RenderInstanceBatchPlanValidation,
     DirectAndGPUDrivenUseTheSameCanonicalInstanceBatchKey)
{
    RVX::DirectDrawPacket direct = MakePacket(0, 41);
    direct.packet.materialInstanceKey.textureBindingHash = 0x4512A55Au;
    direct.packet.materialInstanceKey.parameterTableCompatible = true;

    RVX::MeshPassProcessorInput processorInput;
    processorInput.packet = direct.packet;
    processorInput.sourceOrdinal = direct.sourceOrdinal;
    RVX::OpaqueMeshPassProcessor processor;
    const RVX::MeshPassProcessorResult gpuCandidate =
        processor.Process(processorInput);
    ASSERT_EQ(RVX::MeshPassDisposition::GPUCandidate,
              gpuCandidate.disposition);

    RVX::DirectDrawPacketBatch input;
    input.pass = RVX::RenderPassKind::Opaque;
    input.packets.push_back(direct);
    const RVX::RenderInstanceBatchPlan directPlan =
        RVX::BuildRenderInstanceBatchPlan(
            input, RVX::RenderInstancingMode::Auto);
    ASSERT_EQ(1u, directPlan.batches.size());
    EXPECT_EQ(gpuCandidate.groupKey, directPlan.batches.front().key);
}
