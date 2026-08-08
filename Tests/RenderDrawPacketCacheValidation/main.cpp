#include "Render/Renderer/RenderDrawPacketCache.h"

#include <gtest/gtest.h>

using namespace RVX;

namespace
{
    MeshBatch MakeBatch(uint32 submeshIndex = 0)
    {
        MeshBatch batch;
        batch.objectId = 91;
        batch.mesh = RenderResourceHandle{14, 3};
        batch.material = RenderResourceHandle{22 + submeshIndex, 7};
        batch.submeshIndex = submeshIndex;
        batch.primitiveData = 5 + submeshIndex;
        batch.indexType = MeshUploadIndexType::UInt16;
        batch.geometry = {submeshIndex * 3,
                          3,
                          -static_cast<int32>(submeshIndex),
                          MeshUploadPrimitiveTopology::Triangles};
        batch.materialMode = RenderMaterialMode::Opaque;
        batch.flags = RenderBatchFlags::Skinned |
                      RenderBatchFlags::CastsShadow |
                      RenderBatchFlags::ReceivesShadow;
        return batch;
    }

    RenderDrawPacketCacheVersions MakeVersions()
    {
        return {RVX_LEGACY_MATERIAL_PASS_CONTRACT_VERSION,
                RVX_LEGACY_SHADER_LAYOUT_VERSION_NOT_APPLICABLE};
    }

    void RebindTemplate(RenderDrawPacket& packet, const MeshBatch& batch)
    {
        packet.objectId = batch.objectId;
        packet.primitiveData = batch.primitiveData;
        packet.submeshIndex = batch.submeshIndex;
    }

    void ExpectStatsEqual(const RenderDrawPacketCacheStats& expected,
                          const RenderDrawPacketCacheStats& actual)
    {
        EXPECT_EQ(actual.resolveCount, expected.resolveCount);
        EXPECT_EQ(actual.hitCount, expected.hitCount);
        EXPECT_EQ(actual.missCount, expected.missCount);
        EXPECT_EQ(actual.dynamicBypassCount, expected.dynamicBypassCount);
        EXPECT_EQ(actual.packetBuildCount, expected.packetBuildCount);
        EXPECT_EQ(actual.entryCreationCount, expected.entryCreationCount);
        EXPECT_EQ(actual.clearCount, expected.clearCount);
        EXPECT_EQ(actual.entryCount, expected.entryCount);
        EXPECT_EQ(actual.invalidationCounts, expected.invalidationCounts);
    }
} // namespace

TEST(RenderDrawPacketCacheValidation, EnumerationsHaveStableNames)
{
    EXPECT_STREQ(ToName(RenderDrawPacketCacheResolveCode::Hit), "Hit");
    EXPECT_STREQ(GetName(RenderDrawPacketCacheResolveCode::Miss), "Miss");
    EXPECT_STREQ(ToName(RenderDrawPacketCacheResolveCode::DynamicBypass),
                 "DynamicBypass");
    EXPECT_STREQ(ToName(RenderDrawPacketCacheResolveCode::PublicationInactive),
                 "PublicationInactive");
    EXPECT_STREQ(ToName(RenderDrawPacketCacheInvalidationReason::None), "None");
    EXPECT_STREQ(ToName(
                     RenderDrawPacketCacheInvalidationReason::MeshGenerationChanged),
                 "MeshGenerationChanged");
    EXPECT_STREQ(ToName(
                     RenderDrawPacketCacheInvalidationReason::MaterialGenerationChanged),
                 "MaterialGenerationChanged");
    EXPECT_STREQ(ToName(
                     RenderDrawPacketCacheInvalidationReason::PassContractVersionChanged),
                 "PassContractVersionChanged");
    EXPECT_STREQ(ToName(
                     RenderDrawPacketCacheInvalidationReason::ShaderLayoutVersionChanged),
                 "ShaderLayoutVersionChanged");
    EXPECT_STREQ(ToName(
                     RenderDrawPacketCacheInvalidationReason::StaticStateChanged),
                 "StaticStateChanged");
    EXPECT_STREQ(ToName(
                     RenderDrawPacketCacheInvalidationReason::ObjectRevisionChanged),
                 "ObjectRevisionChanged");
    EXPECT_STREQ(ToName(RenderDrawPacketCacheInvalidationReason::ObjectRemoved),
                 "ObjectRemoved");
    EXPECT_STREQ(ToName(RenderDrawPacketCacheInvalidationReason::DynamicBypass),
                 "DynamicBypass");
    EXPECT_STREQ(GetName(RenderDrawPacketCacheInvalidationReason::Count), "Count");
    EXPECT_STREQ(ToName(static_cast<RenderDrawPacketCacheResolveCode>(255)),
                 "Unknown");
    EXPECT_STREQ(ToName(
                     static_cast<RenderDrawPacketCacheInvalidationReason>(255)),
                 "Unknown");
}

TEST(RenderDrawPacketCacheValidation,
     InactivePublicationCannotMutateEntriesStatisticsOrTemplates)
{
    RenderDrawPacketCache cache;
    const RenderDrawPacketCacheVersions versions = MakeVersions();
    const MeshBatch batch = MakeBatch();
    RenderDrawPacket packetTemplate;

    cache.BeginAcceptedPublication();
    static_cast<void>(cache.Resolve(batch, versions, false, packetTemplate));
    cache.EndAcceptedPublication();
    const RenderDrawPacketCacheStats before = cache.GetStats();

    RenderDrawPacket untouchedTemplate;
    untouchedTemplate.objectId = 700;
    untouchedTemplate.primitiveData = 701;
    untouchedTemplate.submeshIndex = 702;
    const RenderDrawPacket expectedTemplate = untouchedTemplate;
    EXPECT_EQ(cache.Resolve(batch, versions, false, untouchedTemplate).code,
              RenderDrawPacketCacheResolveCode::PublicationInactive);
    EXPECT_EQ(untouchedTemplate, expectedTemplate);
    ExpectStatsEqual(before, cache.GetStats());

    EXPECT_EQ(cache.Resolve(batch, versions, true, untouchedTemplate).code,
              RenderDrawPacketCacheResolveCode::PublicationInactive);
    EXPECT_EQ(untouchedTemplate, expectedTemplate);
    ExpectStatsEqual(before, cache.GetStats());
}

TEST(RenderDrawPacketCacheValidation,
     FirstMissBuildsOnceAndWarmHitRebindsCurrentPrimitiveData)
{
    RenderDrawPacketCache cache;
    const RenderDrawPacketCacheVersions versions = MakeVersions();
    const MeshBatch first = MakeBatch();

    cache.BeginAcceptedPublication();
    RenderDrawPacket packetTemplate;
    EXPECT_EQ(cache.Resolve(first, versions, false, packetTemplate).code,
              RenderDrawPacketCacheResolveCode::Miss);
    EXPECT_EQ(packetTemplate.primitiveData, RVX_INVALID_PRIMITIVE_DATA_INDEX);
    cache.EndAcceptedPublication();

    const RenderDrawPacketCacheStats afterFirst = cache.GetStats();
    EXPECT_EQ(afterFirst.entryCount, 1U);
    EXPECT_EQ(afterFirst.packetBuildCount, 1U);
    EXPECT_EQ(afterFirst.entryCreationCount, 1U);

    MeshBatch current = first;
    current.primitiveData = 73;
    cache.BeginAcceptedPublication();
    const RenderDrawPacketCacheResolveResult hit =
        cache.Resolve(current, versions, false, packetTemplate);
    cache.EndAcceptedPublication();

    EXPECT_TRUE(hit.IsHit());
    RebindTemplate(packetTemplate, current);
    EXPECT_EQ(packetTemplate, BuildLegacyMaterialDrawPacket(current));

    const RenderDrawPacketCacheStats afterHit = cache.GetStats();
    EXPECT_EQ(afterHit.packetBuildCount, afterFirst.packetBuildCount);
    EXPECT_EQ(afterHit.entryCreationCount, afterFirst.entryCreationCount);
    EXPECT_EQ(afterHit.hitCount, 1U);
}

TEST(RenderDrawPacketCacheValidation,
     ExactGenerationIdentityInvalidatesOnlyChangedSubmesh)
{
    RenderDrawPacketCache cache;
    const RenderDrawPacketCacheVersions versions = MakeVersions();
    const MeshBatch original = MakeBatch(0);
    const MeshBatch sibling = MakeBatch(1);
    RenderDrawPacket packetTemplate;

    cache.BeginAcceptedPublication();
    EXPECT_EQ(cache.Resolve(original, versions, false, packetTemplate).code,
              RenderDrawPacketCacheResolveCode::Miss);
    EXPECT_EQ(cache.Resolve(sibling, versions, false, packetTemplate).code,
              RenderDrawPacketCacheResolveCode::Miss);
    cache.EndAcceptedPublication();

    MeshBatch changedMesh = original;
    ++changedMesh.mesh.generation;
    cache.BeginAcceptedPublication();
    const RenderDrawPacketCacheResolveResult meshMiss =
        cache.Resolve(changedMesh, versions, false, packetTemplate);
    const RenderDrawPacketCacheResolveResult siblingHit =
        cache.Resolve(sibling, versions, false, packetTemplate);
    cache.EndAcceptedPublication();
    EXPECT_EQ(meshMiss.invalidationReason,
              RenderDrawPacketCacheInvalidationReason::MeshGenerationChanged);
    EXPECT_TRUE(siblingHit.IsHit());

    MeshBatch changedMaterial = sibling;
    ++changedMaterial.material.generation;
    cache.BeginAcceptedPublication();
    EXPECT_TRUE(cache.Resolve(changedMesh, versions, false, packetTemplate)
                    .IsHit());
    const RenderDrawPacketCacheResolveResult materialMiss =
        cache.Resolve(changedMaterial, versions, false, packetTemplate);
    cache.EndAcceptedPublication();
    EXPECT_EQ(materialMiss.invalidationReason,
              RenderDrawPacketCacheInvalidationReason::MaterialGenerationChanged);
    EXPECT_EQ(cache.GetStats().entryCount, 2U);
}

TEST(RenderDrawPacketCacheValidation,
     ContractLayoutAndStaticStateChangesHaveStableReasons)
{
    RenderDrawPacketCache cache;
    const MeshBatch batch = MakeBatch();
    const RenderDrawPacketCacheVersions baseVersions = MakeVersions();
    RenderDrawPacket packetTemplate;

    cache.BeginAcceptedPublication();
    static_cast<void>(cache.Resolve(batch, baseVersions, false, packetTemplate));
    cache.EndAcceptedPublication();

    RenderDrawPacketCacheVersions nextPassVersion = baseVersions;
    ++nextPassVersion.passContractVersion;
    cache.BeginAcceptedPublication();
    const RenderDrawPacketCacheResolveResult contractMiss =
        cache.Resolve(batch, nextPassVersion, false, packetTemplate);
    cache.EndAcceptedPublication();
    EXPECT_EQ(contractMiss.invalidationReason,
              RenderDrawPacketCacheInvalidationReason::PassContractVersionChanged);

    RenderDrawPacketCacheVersions nextLayoutVersion = nextPassVersion;
    ++nextLayoutVersion.shaderLayoutVersion;
    cache.BeginAcceptedPublication();
    const RenderDrawPacketCacheResolveResult layoutMiss =
        cache.Resolve(batch, nextLayoutVersion, false, packetTemplate);
    cache.EndAcceptedPublication();
    EXPECT_EQ(layoutMiss.invalidationReason,
              RenderDrawPacketCacheInvalidationReason::ShaderLayoutVersionChanged);

    MeshBatch changedGeometry = batch;
    changedGeometry.geometry.indexCount = 6;
    cache.BeginAcceptedPublication();
    const RenderDrawPacketCacheResolveResult staticMiss =
        cache.Resolve(changedGeometry, nextLayoutVersion, false, packetTemplate);
    cache.EndAcceptedPublication();
    EXPECT_EQ(staticMiss.invalidationReason,
              RenderDrawPacketCacheInvalidationReason::StaticStateChanged);

    MeshBatch changedFlags = changedGeometry;
    changedFlags.flags = RenderBatchFlags::Skinned;
    cache.BeginAcceptedPublication();
    const RenderDrawPacketCacheResolveResult flagsMiss =
        cache.Resolve(changedFlags, nextLayoutVersion, false, packetTemplate);
    cache.EndAcceptedPublication();
    EXPECT_EQ(flagsMiss.invalidationReason,
              RenderDrawPacketCacheInvalidationReason::StaticStateChanged);
}

TEST(RenderDrawPacketCacheValidation,
     CompleteObjectRevisionInvalidatesWithoutChangingResourceGenerations)
{
    RenderDrawPacketCache cache;
    const RenderDrawPacketCacheVersions versions = MakeVersions();
    MeshBatch original = MakeBatch();
    original.objectRevision = 41;
    RenderDrawPacket packetTemplate;

    cache.BeginAcceptedPublication();
    ASSERT_EQ(cache.Resolve(original, versions, false, packetTemplate).code,
              RenderDrawPacketCacheResolveCode::Miss);
    cache.EndAcceptedPublication();

    MeshBatch changed = original;
    changed.objectRevision = 42;
    cache.BeginAcceptedPublication();
    const RenderDrawPacketCacheResolveResult miss =
        cache.Resolve(changed, versions, false, packetTemplate);
    cache.EndAcceptedPublication();

    EXPECT_EQ(miss.code, RenderDrawPacketCacheResolveCode::Miss);
    EXPECT_EQ(miss.invalidationReason,
              RenderDrawPacketCacheInvalidationReason::ObjectRevisionChanged);
    EXPECT_EQ(cache.GetStats().GetInvalidationCount(
                  RenderDrawPacketCacheInvalidationReason::ObjectRevisionChanged),
              1U);
}

TEST(RenderDrawPacketCacheValidation,
     PrunesRemovedObjectsClearsAndHonorsDynamicBypass)
{
    RenderDrawPacketCache cache;
    const RenderDrawPacketCacheVersions versions = MakeVersions();
    const MeshBatch first = MakeBatch(0);
    MeshBatch removed = MakeBatch(1);
    removed.objectId = 92;
    RenderDrawPacket packetTemplate;

    cache.BeginAcceptedPublication();
    static_cast<void>(cache.Resolve(first, versions, false, packetTemplate));
    static_cast<void>(cache.Resolve(removed, versions, false, packetTemplate));
    cache.EndAcceptedPublication();

    cache.BeginAcceptedPublication();
    EXPECT_TRUE(cache.Resolve(first, versions, false, packetTemplate).IsHit());
    cache.EndAcceptedPublication();
    EXPECT_EQ(cache.GetStats().entryCount, 1U);
    EXPECT_EQ(cache.GetStats().GetInvalidationCount(
                  RenderDrawPacketCacheInvalidationReason::ObjectRemoved),
              1U);

    cache.Clear();
    EXPECT_EQ(cache.GetStats().entryCount, 0U);
    EXPECT_EQ(cache.GetStats().clearCount, 1U);
    EXPECT_EQ(cache.Find(first, versions, packetTemplate).code,
              RenderDrawPacketCacheResolveCode::Miss);

    cache.BeginAcceptedPublication();
    static_cast<void>(cache.Resolve(first, versions, false, packetTemplate));
    cache.EndAcceptedPublication();
    cache.BeginAcceptedPublication();
    EXPECT_EQ(cache.Resolve(first, versions, true, packetTemplate).code,
              RenderDrawPacketCacheResolveCode::DynamicBypass);
    cache.EndAcceptedPublication();
    EXPECT_EQ(cache.GetStats().entryCount, 0U);
    EXPECT_EQ(cache.GetStats().GetInvalidationCount(
                  RenderDrawPacketCacheInvalidationReason::DynamicBypass),
              1U);

    cache.BeginAcceptedPublication();
    static_cast<void>(cache.Resolve(first, versions, true, packetTemplate));
    cache.EndAcceptedPublication();
    EXPECT_EQ(cache.GetStats().GetInvalidationCount(
                  RenderDrawPacketCacheInvalidationReason::DynamicBypass),
              1U);
}
