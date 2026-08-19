#include "RenderContracts/FeatureRenderSnapshot.h"
#include "RenderContracts/RenderFramePacketV5.h"
#include "RenderContracts/RenderFrameValidation.h"
#include "RenderContracts/RenderIdentity.h"
#include "RenderContracts/RenderSceneUpdate.h"
#include "RenderContracts/ResourceUploadRequest.h"
#include "ResourceUploadRequestInternal.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>

namespace RVX
{
namespace
{
    static_assert(!std::is_default_constructible_v<ResourceUploadRequest>);
    static_assert(!std::is_copy_constructible_v<ResourceUploadRequest>);
    static_assert(!std::is_move_constructible_v<ResourceUploadRequest>);
    ResourceUploadRequestCreateInfo MakeValidMeshRequestInfo()
    {
        MeshUploadPayload payload;
        payload.createInfo.vertexCount = 3;
        payload.createInfo.boundsMin = Vec3(-1.0f);
        payload.createInfo.boundsMax = Vec3(1.0f);
        payload.bytes.resize(36, 7U);
        payload.positionRange = UploadByteRange{0, 36, 12};

        ResourceUploadRequestCreateInfo info;
        info.sequence = 1;
        info.assetId = AssetId{1};
        info.handle = RenderResourceHandle{1, 1};
        info.kind = RenderResourceKind::Mesh;
        info.payload = std::move(payload);
        info.declaredPayloadBytes = 36;
        return info;
    }

    ResourceUploadRequestCreateInfo MakeValidTextureRequestInfo()
    {
        TextureUploadPayload payload;
        payload.createInfo.width = 1;
        payload.createInfo.height = 1;
        payload.createInfo.depth = 1;
        payload.createInfo.mipLevels = 1;
        payload.createInfo.arrayLayers = 1;
        payload.createInfo.format = TextureUploadFormat::RGBA8;
        payload.bytes = {11U, 22U, 33U, 44U};
        payload.subresources.push_back(
            TextureUploadSubresource{UploadByteRange{0, 4, 0}, 0, 0, 4, 4});

        ResourceUploadRequestCreateInfo info;
        info.sequence = 1;
        info.assetId = AssetId{1};
        info.handle = RenderResourceHandle{1, 1};
        info.kind = RenderResourceKind::Texture;
        info.payload = std::move(payload);
        info.declaredPayloadBytes =
            4 + static_cast<uint64>(sizeof(TextureUploadSubresource));
        return info;
    }

    ResourceUploadRequestCreateInfo MakeValidMaterialRequestInfo()
    {
        MaterialUploadPayload payload;

        ResourceUploadRequestCreateInfo info;
        info.sequence = 1;
        info.assetId = AssetId{1};
        info.handle = RenderResourceHandle{1, 1};
        info.kind = RenderResourceKind::Material;
        info.payload = std::move(payload);
        info.declaredPayloadBytes = 0;
        return info;
    }

    RenderViewSnapshot MakeValidView()
    {
        RenderViewSnapshot view;
        view.viewportWidth = 1;
        view.viewportHeight = 1;
        return view;
    }

    RenderExtractionDiagnostics MakeCompleteDiagnostics()
    {
        RenderExtractionDiagnostics diagnostics;
        diagnostics.complete = true;
        return diagnostics;
    }

    void ExpectCreationCode(const ResourceUploadRequestCreateInfo& info,
                            ResourceUploadRequestCreateCode expected)
    {
        const ResourceUploadRequestCreateResult result =
            ResourceUploadRequest::Create(info);
        EXPECT_EQ(result.code, expected);
        EXPECT_EQ(result.request != nullptr,
                  expected == ResourceUploadRequestCreateCode::Created);
    }

    TEST(RenderContractsValidation, ZeroIdentityValuesAreInvalid)
    {
        EXPECT_FALSE(AssetId{}.IsValid());
        EXPECT_FALSE(RenderResourceHandle{}.IsValid());
        EXPECT_FALSE((RenderResourceHandle{1, 0}.IsValid()));
        EXPECT_TRUE((RenderResourceHandle{1, 1}.IsValid()));
    }

    TEST(RenderContractsValidation, IdentityHashersAreDeterministic)
    {
        const AssetId asset{42};
        const RenderResourceHandle handle{0x10203040U, 0x50607080U};
        const uint64 packed =
            (static_cast<uint64>(handle.slot) << 32U) |
            static_cast<uint64>(handle.generation);

        EXPECT_EQ(AssetIdHash{}(asset), AssetIdHash{}(asset));
        EXPECT_EQ(RenderResourceHandleHash{}(handle),
                  RenderResourceHandleHash{}(handle));
        EXPECT_EQ(RenderResourceHandleHash{}(handle),
                  std::hash<uint64>{}(packed));
    }

    TEST(RenderContractsValidation, CreatesMesh)
    {
        const auto info = MakeValidMeshRequestInfo();
        const auto result = ResourceUploadRequest::Create(info);

        ASSERT_EQ(result.code, ResourceUploadRequestCreateCode::Created);
        ASSERT_TRUE(result.request);
        EXPECT_EQ(result.request->GetSchemaId(), RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_ID);
        EXPECT_EQ(result.request->GetSchemaVersion(), RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_VERSION);
        EXPECT_EQ(result.request->GetSequence(), 1U);
        EXPECT_EQ(result.request->GetAssetId(), (AssetId{1}));
        EXPECT_EQ(result.request->GetHandle(), (RenderResourceHandle{1, 1}));
        EXPECT_EQ(result.request->GetKind(), RenderResourceKind::Mesh);
        EXPECT_TRUE(std::holds_alternative<MeshUploadPayload>(result.request->GetPayload()));
        EXPECT_EQ(result.request->GetDerivedPayloadBytes(), 36U);
        EXPECT_EQ(result.request->GetDeclaredPayloadBytes(), 36U);
        EXPECT_TRUE(result.request->GetDependencies().empty());
        EXPECT_EQ(result.request->GetDependencyReadiness(),
                  RenderDependencyReadiness::RequireAll);
        EXPECT_EQ(result.request->GetPriority(), RenderUploadPriority::Normal);
        EXPECT_EQ(result.request->GetProvenance().sourceRevision, 0U);
    }

    TEST(RenderContractsValidation, CreatesTexture)
    {
        const auto info = MakeValidTextureRequestInfo();
        const auto result = ResourceUploadRequest::Create(info);

        ASSERT_EQ(result.code, ResourceUploadRequestCreateCode::Created);
        ASSERT_TRUE(result.request);
        EXPECT_EQ(result.request->GetKind(), RenderResourceKind::Texture);
        EXPECT_TRUE(std::holds_alternative<TextureUploadPayload>(result.request->GetPayload()));
        EXPECT_EQ(result.request->GetDerivedPayloadBytes(), info.declaredPayloadBytes);
        EXPECT_EQ(result.request->GetDeclaredPayloadBytes(), info.declaredPayloadBytes);
    }

    TEST(RenderContractsValidation, CreatesMaterial)
    {
        const auto info = MakeValidMaterialRequestInfo();
        const auto result = ResourceUploadRequest::Create(info);

        ASSERT_EQ(result.code, ResourceUploadRequestCreateCode::Created);
        ASSERT_TRUE(result.request);
        EXPECT_EQ(result.request->GetKind(), RenderResourceKind::Material);
        EXPECT_TRUE(std::holds_alternative<MaterialUploadPayload>(result.request->GetPayload()));
        EXPECT_EQ(result.request->GetDerivedPayloadBytes(), 0U);
        EXPECT_EQ(result.request->GetDeclaredPayloadBytes(), 0U);
    }

    TEST(RenderContractsValidation, RejectsSchema)
    {
        auto info = MakeValidTextureRequestInfo();
        info.schemaVersion = RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_VERSION + 1U;
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidSchema);
    }

    TEST(RenderContractsValidation, RejectsZeroSequence)
    {
        auto info = MakeValidTextureRequestInfo();
        info.sequence = 0;
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidSequence);
    }

    TEST(RenderContractsValidation, RejectsZeroAsset)
    {
        auto info = MakeValidTextureRequestInfo();
        info.assetId = {};
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidAsset);
    }

    TEST(RenderContractsValidation, RejectsZeroHandlePart)
    {
        auto info = MakeValidTextureRequestInfo();
        info.handle = RenderResourceHandle{1, 0};
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidHandle);
    }

    TEST(RenderContractsValidation, RejectsInvalidKind)
    {
        auto info = MakeValidTextureRequestInfo();
        info.kind = RenderResourceKind::Invalid;
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidKind);
    }

    TEST(RenderContractsValidation, RejectsVariantKindMismatch)
    {
        auto info = MakeValidMeshRequestInfo();
        info.kind = RenderResourceKind::Texture;
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::PayloadKindMismatch);
    }

    TEST(RenderContractsValidation, RejectsMeshShape)
    {
        auto info = MakeValidMeshRequestInfo();
        std::get<MeshUploadPayload>(info.payload).bytes.clear();
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidPayload);
    }

    TEST(RenderContractsValidation, RejectsTextureShape)
    {
        auto info = MakeValidTextureRequestInfo();
        auto& payload = std::get<TextureUploadPayload>(info.payload);
        payload.createInfo.arrayLayers = 2;
        payload.createInfo.isArray = true;
        payload.subresources.push_back(payload.subresources.front());
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidPayload);
    }

    TEST(RenderContractsValidation, RejectsMaterialShape)
    {
        auto info = MakeValidMaterialRequestInfo();
        MaterialUploadTextureBinding binding;
        binding.texture = RenderResourceHandle{2, 1};
        auto& bindings = std::get<MaterialUploadPayload>(info.payload).textureBindings;
        bindings = {binding, binding};
        info.declaredPayloadBytes =
            2 * static_cast<uint64>(sizeof(MaterialUploadTextureBinding));
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidPayload);
    }

    TEST(RenderContractsValidation, RejectsRangeOverflow)
    {
        auto info = MakeValidTextureRequestInfo();
        std::get<TextureUploadPayload>(info.payload).subresources.front().bytes =
            UploadByteRange{std::numeric_limits<uint64>::max(), 2, 0};
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::PayloadRangeOverflow);
    }

    TEST(RenderContractsValidation, RejectsDeclaredBytes)
    {
        auto info = MakeValidTextureRequestInfo();
        ++info.declaredPayloadBytes;
        ExpectCreationCode(info,
                           ResourceUploadRequestCreateCode::DiagnosticByteCountMismatch);
    }

    TEST(RenderContractsValidation, RejectsInvalidDependency)
    {
        auto info = MakeValidTextureRequestInfo();
        info.dependencies.push_back(RenderResourceHandle{});
        info.declaredPayloadBytes += sizeof(RenderResourceHandle);
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidDependency);
    }

    TEST(RenderContractsValidation, RejectsSelfDependency)
    {
        auto info = MakeValidTextureRequestInfo();
        info.dependencies.push_back(info.handle);
        info.declaredPayloadBytes += sizeof(RenderResourceHandle);
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::SelfDependency);
    }

    TEST(RenderContractsValidation, RejectsDuplicateDependency)
    {
        auto info = MakeValidTextureRequestInfo();
        info.dependencies = {{2, 1}, {2, 1}};
        info.declaredPayloadBytes += 2 * sizeof(RenderResourceHandle);
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::DuplicateDependency);
    }

    TEST(RenderContractsValidation, UploadRequestOwnsCopiedPayloadBytes)
    {
        ResourceUploadRequestCreateInfo info = MakeValidTextureRequestInfo();
        const uint8 expected =
            std::get<TextureUploadPayload>(info.payload).bytes.front();
        const auto created = ResourceUploadRequest::Create(info);
        std::get<TextureUploadPayload>(info.payload).bytes.clear();
        ASSERT_EQ(created.code, ResourceUploadRequestCreateCode::Created);
        ASSERT_TRUE(created.request);
        const auto* payload =
            std::get_if<TextureUploadPayload>(&created.request->GetPayload());
        ASSERT_NE(payload, nullptr);
        EXPECT_EQ(payload->bytes.front(), expected);
    }

    TEST(RenderContractsValidation, OwnsCopiedSourceValues)
    {
        auto meshInfo = MakeValidMeshRequestInfo();
        auto textureInfo = MakeValidTextureRequestInfo();
        auto materialInfo = MakeValidMaterialRequestInfo();
        MaterialUploadTextureBinding binding;
        binding.texture = RenderResourceHandle{2, 1};
        binding.offset = Vec2(0.25f, 0.5f);
        std::get<MaterialUploadPayload>(materialInfo.payload)
            .textureBindings.push_back(binding);
        materialInfo.declaredPayloadBytes = sizeof(MaterialUploadTextureBinding);

        const auto mesh = ResourceUploadRequest::Create(meshInfo);
        const auto texture = ResourceUploadRequest::Create(textureInfo);
        const auto material = ResourceUploadRequest::Create(materialInfo);

        std::get<MeshUploadPayload>(meshInfo.payload).bytes.clear();
        std::get<TextureUploadPayload>(textureInfo.payload).subresources.clear();
        std::get<MaterialUploadPayload>(materialInfo.payload).textureBindings.clear();
        meshInfo = {};
        textureInfo = {};
        materialInfo = {};

        ASSERT_TRUE(mesh.request);
        ASSERT_TRUE(texture.request);
        ASSERT_TRUE(material.request);
        EXPECT_EQ(std::get<MeshUploadPayload>(mesh.request->GetPayload()).bytes.size(), 36U);
        EXPECT_EQ(std::get<TextureUploadPayload>(texture.request->GetPayload())
                      .subresources.size(),
                  1U);
        const auto& retainedBinding =
            std::get<MaterialUploadPayload>(material.request->GetPayload())
                .textureBindings.front();
        EXPECT_EQ(retainedBinding.texture, (RenderResourceHandle{2, 1}));
        EXPECT_FLOAT_EQ(retainedBinding.offset.x, 0.25f);
    }

    TEST(RenderContractsValidation, FactoryUsesDocumentedFirstFailureOrder)
    {
        auto info = MakeValidTextureRequestInfo();
        info.schemaVersion = RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_VERSION + 1U;
        info.sequence = 0;
        info.assetId = {};
        info.handle = {};
        info.kind = RenderResourceKind::Invalid;
        std::get<TextureUploadPayload>(info.payload).bytes.clear();
        info.declaredPayloadBytes = std::numeric_limits<uint64>::max();

        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidSchema);
        info.schemaVersion = RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_VERSION;
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidSequence);
        info.sequence = 1;
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidAsset);
        info.assetId = AssetId{1};
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidHandle);
    }

    TEST(RenderContractsValidation, AccumulateOwnedBytesChecksMultiplyAndAdd)
    {
        uint64 total = 5;
        EXPECT_EQ(Detail::AccumulateOwnedBytes(3, 4, total),
                  ResourceUploadRequestCreateCode::Created);
        EXPECT_EQ(total, 17U);

        total = 9;
        EXPECT_EQ(Detail::AccumulateOwnedBytes(
                      std::numeric_limits<uint64>::max(), 2, total),
                  ResourceUploadRequestCreateCode::PayloadByteCountOverflow);
        EXPECT_EQ(total, 9U);

        total = std::numeric_limits<uint64>::max();
        EXPECT_EQ(Detail::AccumulateOwnedBytes(1, 1, total),
                  ResourceUploadRequestCreateCode::PayloadByteCountOverflow);
        EXPECT_EQ(total, std::numeric_limits<uint64>::max());
    }

    enum class MeshRuleCase : uint8
    {
        InvalidIndexType,
        InvalidTopology,
        ZeroVertexCount,
        NonFiniteBounds,
        UnorderedBounds,
        NonCanonicalEmptyPosition,
        ZeroPositionStride,
        PositionCountOverflow,
        PositionTooSmall,
        NonCanonicalOptionalRange,
        ZeroOptionalStride,
        OptionalRangeTooSmall,
        BoneRangePairMismatch,
        ZeroIndexWithRange,
        ZeroIndexWithSubmesh,
        IndexedWithEmptyRange,
        IndexedWithWrongStride,
        IndexedCountOverflow,
        IndexedRangeTooSmall,
        SubmeshInvalidTopology,
        SubmeshZeroCount,
        SubmeshOverflow,
        SubmeshOutOfBounds,
        RangeAdditionOverflow,
        RangeOutOfBounds
    };

    class RenderContractsValidationMeshPayloadRules : public testing::TestWithParam<MeshRuleCase>
    {
    };

    void ConfigureIndexedMesh(ResourceUploadRequestCreateInfo& info)
    {
        auto& payload = std::get<MeshUploadPayload>(info.payload);
        payload.bytes.resize(48, 7U);
        payload.createInfo.indexCount = 3;
        payload.createInfo.indexType = MeshUploadIndexType::UInt32;
        payload.indexRange = UploadByteRange{36, 12, 4};
        info.declaredPayloadBytes = 48;
    }

    TEST_P(RenderContractsValidationMeshPayloadRules, RejectsInvalidShapeRangeOrEnum)
    {
        auto info = MakeValidMeshRequestInfo();
        auto& payload = std::get<MeshUploadPayload>(info.payload);
        ResourceUploadRequestCreateCode expected =
            ResourceUploadRequestCreateCode::InvalidPayload;

        switch (GetParam())
        {
            case MeshRuleCase::InvalidIndexType:
                payload.createInfo.indexType = static_cast<MeshUploadIndexType>(255);
                break;
            case MeshRuleCase::InvalidTopology:
                payload.createInfo.topology =
                    static_cast<MeshUploadPrimitiveTopology>(255);
                break;
            case MeshRuleCase::ZeroVertexCount:
                payload.createInfo.vertexCount = 0;
                break;
            case MeshRuleCase::NonFiniteBounds:
                payload.createInfo.boundsMax.x =
                    std::numeric_limits<float32>::quiet_NaN();
                break;
            case MeshRuleCase::UnorderedBounds:
                payload.createInfo.boundsMin.x = 2.0f;
                payload.createInfo.boundsMax.x = 1.0f;
                break;
            case MeshRuleCase::NonCanonicalEmptyPosition:
                payload.positionRange = UploadByteRange{1, 0, 0};
                break;
            case MeshRuleCase::ZeroPositionStride:
                payload.positionRange.stride = 0;
                break;
            case MeshRuleCase::PositionCountOverflow:
                payload.createInfo.vertexCount =
                    std::numeric_limits<uint64>::max();
                expected = ResourceUploadRequestCreateCode::PayloadRangeOverflow;
                break;
            case MeshRuleCase::PositionTooSmall:
                payload.positionRange.size = 24;
                break;
            case MeshRuleCase::NonCanonicalOptionalRange:
                payload.normalRange = UploadByteRange{0, 0, 4};
                break;
            case MeshRuleCase::ZeroOptionalStride:
                payload.normalRange = UploadByteRange{0, 12, 0};
                break;
            case MeshRuleCase::OptionalRangeTooSmall:
                payload.normalRange = UploadByteRange{0, 8, 4};
                break;
            case MeshRuleCase::BoneRangePairMismatch:
                payload.boneIndexRange = UploadByteRange{0, 12, 4};
                break;
            case MeshRuleCase::ZeroIndexWithRange:
                payload.indexRange = UploadByteRange{0, 3, 1};
                break;
            case MeshRuleCase::ZeroIndexWithSubmesh:
                payload.submeshes.push_back(MeshUploadSubmesh{0, 1, 0,
                    MeshUploadPrimitiveTopology::Triangles});
                info.declaredPayloadBytes += sizeof(MeshUploadSubmesh);
                break;
            case MeshRuleCase::IndexedWithEmptyRange:
                payload.createInfo.indexCount = 3;
                break;
            case MeshRuleCase::IndexedWithWrongStride:
                ConfigureIndexedMesh(info);
                payload.indexRange.stride = 2;
                break;
            case MeshRuleCase::IndexedCountOverflow:
                ConfigureIndexedMesh(info);
                payload.createInfo.indexCount =
                    std::numeric_limits<uint64>::max();
                expected = ResourceUploadRequestCreateCode::PayloadRangeOverflow;
                break;
            case MeshRuleCase::IndexedRangeTooSmall:
                ConfigureIndexedMesh(info);
                payload.indexRange.size = 8;
                break;
            case MeshRuleCase::SubmeshInvalidTopology:
                ConfigureIndexedMesh(info);
                payload.submeshes.push_back(MeshUploadSubmesh{0, 3, 0,
                    static_cast<MeshUploadPrimitiveTopology>(255)});
                info.declaredPayloadBytes += sizeof(MeshUploadSubmesh);
                break;
            case MeshRuleCase::SubmeshZeroCount:
                ConfigureIndexedMesh(info);
                payload.submeshes.push_back(MeshUploadSubmesh{});
                info.declaredPayloadBytes += sizeof(MeshUploadSubmesh);
                break;
            case MeshRuleCase::SubmeshOverflow:
                ConfigureIndexedMesh(info);
                payload.submeshes.push_back(MeshUploadSubmesh{
                    std::numeric_limits<uint32>::max(), 2, 0,
                    MeshUploadPrimitiveTopology::Triangles});
                info.declaredPayloadBytes += sizeof(MeshUploadSubmesh);
                expected = ResourceUploadRequestCreateCode::PayloadRangeOverflow;
                break;
            case MeshRuleCase::SubmeshOutOfBounds:
                ConfigureIndexedMesh(info);
                payload.submeshes.push_back(MeshUploadSubmesh{2, 2, 0,
                    MeshUploadPrimitiveTopology::Triangles});
                info.declaredPayloadBytes += sizeof(MeshUploadSubmesh);
                break;
            case MeshRuleCase::RangeAdditionOverflow:
                payload.normalRange = UploadByteRange{
                    std::numeric_limits<uint64>::max(), 2, 4};
                expected = ResourceUploadRequestCreateCode::PayloadRangeOverflow;
                break;
            case MeshRuleCase::RangeOutOfBounds:
                payload.normalRange = UploadByteRange{32, 8, 4};
                break;
        }

        ExpectCreationCode(info, expected);
    }

    INSTANTIATE_TEST_SUITE_P(
        AllMeshPayloadRules,
        RenderContractsValidationMeshPayloadRules,
        testing::Values(
            MeshRuleCase::InvalidIndexType,
            MeshRuleCase::InvalidTopology,
            MeshRuleCase::ZeroVertexCount,
            MeshRuleCase::NonFiniteBounds,
            MeshRuleCase::UnorderedBounds,
            MeshRuleCase::NonCanonicalEmptyPosition,
            MeshRuleCase::ZeroPositionStride,
            MeshRuleCase::PositionCountOverflow,
            MeshRuleCase::PositionTooSmall,
            MeshRuleCase::NonCanonicalOptionalRange,
            MeshRuleCase::ZeroOptionalStride,
            MeshRuleCase::OptionalRangeTooSmall,
            MeshRuleCase::BoneRangePairMismatch,
            MeshRuleCase::ZeroIndexWithRange,
            MeshRuleCase::ZeroIndexWithSubmesh,
            MeshRuleCase::IndexedWithEmptyRange,
            MeshRuleCase::IndexedWithWrongStride,
            MeshRuleCase::IndexedCountOverflow,
            MeshRuleCase::IndexedRangeTooSmall,
            MeshRuleCase::SubmeshInvalidTopology,
            MeshRuleCase::SubmeshZeroCount,
            MeshRuleCase::SubmeshOverflow,
            MeshRuleCase::SubmeshOutOfBounds,
            MeshRuleCase::RangeAdditionOverflow,
            MeshRuleCase::RangeOutOfBounds));

    enum class TextureRuleCase : uint8
    {
        ZeroWidth,
        ZeroHeight,
        ZeroDepth,
        ZeroMips,
        ZeroLayers,
        UnknownFormat,
        UndeclaredFormat,
        EmptyBytes,
        EmptySubresources,
        CubemapDepth,
        CubemapTooFewLayers,
        CubemapLayerMultiple,
        ArraySingleLayer,
        NonArrayMultipleLayers,
        SubresourceCountMultiplyOverflow,
        SubresourceCountMismatch,
        DuplicatePair,
        PairMipOutOfBounds,
        PairLayerOutOfBounds,
        EmptyRange,
        NonCanonicalEmptyRange,
        NonzeroRangeStride,
        RangeAdditionOverflow,
        RangeOutOfBounds,
        ZeroRowPitch,
        SliceBelowRow,
        RangeBelowSlice
    };

    class RenderContractsValidationTexturePayloadRules :
        public testing::TestWithParam<TextureRuleCase>
    {
    };

    void ConfigureTwoLayerTexture(ResourceUploadRequestCreateInfo& info)
    {
        auto& payload = std::get<TextureUploadPayload>(info.payload);
        payload.createInfo.arrayLayers = 2;
        payload.createInfo.isArray = true;
        payload.bytes.resize(8, 5U);
        payload.subresources = {
            TextureUploadSubresource{UploadByteRange{0, 4, 0}, 0, 0, 4, 4},
            TextureUploadSubresource{UploadByteRange{4, 4, 0}, 0, 1, 4, 4}};
        info.declaredPayloadBytes =
            8 + 2 * static_cast<uint64>(sizeof(TextureUploadSubresource));
    }

    TEST_P(RenderContractsValidationTexturePayloadRules, RejectsInvalidShapeRangeOrEnum)
    {
        auto info = MakeValidTextureRequestInfo();
        auto& payload = std::get<TextureUploadPayload>(info.payload);
        auto& createInfo = payload.createInfo;
        ResourceUploadRequestCreateCode expected =
            ResourceUploadRequestCreateCode::InvalidPayload;

        switch (GetParam())
        {
            case TextureRuleCase::ZeroWidth: createInfo.width = 0; break;
            case TextureRuleCase::ZeroHeight: createInfo.height = 0; break;
            case TextureRuleCase::ZeroDepth: createInfo.depth = 0; break;
            case TextureRuleCase::ZeroMips: createInfo.mipLevels = 0; break;
            case TextureRuleCase::ZeroLayers: createInfo.arrayLayers = 0; break;
            case TextureRuleCase::UnknownFormat:
                createInfo.format = TextureUploadFormat::Unknown;
                break;
            case TextureRuleCase::UndeclaredFormat:
                createInfo.format = static_cast<TextureUploadFormat>(255);
                break;
            case TextureRuleCase::EmptyBytes: payload.bytes.clear(); break;
            case TextureRuleCase::EmptySubresources:
                payload.subresources.clear();
                break;
            case TextureRuleCase::CubemapDepth:
                createInfo.isCubemap = true;
                createInfo.depth = 2;
                createInfo.arrayLayers = 6;
                break;
            case TextureRuleCase::CubemapTooFewLayers:
                createInfo.isCubemap = true;
                createInfo.arrayLayers = 5;
                break;
            case TextureRuleCase::CubemapLayerMultiple:
                createInfo.isCubemap = true;
                createInfo.arrayLayers = 7;
                break;
            case TextureRuleCase::ArraySingleLayer:
                createInfo.isArray = true;
                break;
            case TextureRuleCase::NonArrayMultipleLayers:
                createInfo.arrayLayers = 2;
                break;
            case TextureRuleCase::SubresourceCountMultiplyOverflow:
                createInfo.isArray = true;
                createInfo.mipLevels = std::numeric_limits<uint32>::max();
                createInfo.arrayLayers = 2;
                expected = ResourceUploadRequestCreateCode::PayloadRangeOverflow;
                break;
            case TextureRuleCase::SubresourceCountMismatch:
                ConfigureTwoLayerTexture(info);
                payload.subresources.pop_back();
                break;
            case TextureRuleCase::DuplicatePair:
                ConfigureTwoLayerTexture(info);
                payload.subresources[1].arrayLayer = 0;
                break;
            case TextureRuleCase::PairMipOutOfBounds:
                payload.subresources.front().mipLevel = 1;
                break;
            case TextureRuleCase::PairLayerOutOfBounds:
                payload.subresources.front().arrayLayer = 1;
                break;
            case TextureRuleCase::EmptyRange:
                payload.subresources.front().bytes = {};
                break;
            case TextureRuleCase::NonCanonicalEmptyRange:
                payload.subresources.front().bytes = UploadByteRange{1, 0, 0};
                break;
            case TextureRuleCase::NonzeroRangeStride:
                payload.subresources.front().bytes.stride = 1;
                break;
            case TextureRuleCase::RangeAdditionOverflow:
                payload.subresources.front().bytes = UploadByteRange{
                    std::numeric_limits<uint64>::max(), 2, 0};
                expected = ResourceUploadRequestCreateCode::PayloadRangeOverflow;
                break;
            case TextureRuleCase::RangeOutOfBounds:
                payload.subresources.front().bytes = UploadByteRange{2, 4, 0};
                break;
            case TextureRuleCase::ZeroRowPitch:
                payload.subresources.front().rowPitch = 0;
                break;
            case TextureRuleCase::SliceBelowRow:
                payload.subresources.front().rowPitch = 5;
                break;
            case TextureRuleCase::RangeBelowSlice:
                payload.subresources.front().slicePitch = 5;
                break;
        }

        ExpectCreationCode(info, expected);
    }

    INSTANTIATE_TEST_SUITE_P(
        AllTexturePayloadRules,
        RenderContractsValidationTexturePayloadRules,
        testing::Values(
            TextureRuleCase::ZeroWidth,
            TextureRuleCase::ZeroHeight,
            TextureRuleCase::ZeroDepth,
            TextureRuleCase::ZeroMips,
            TextureRuleCase::ZeroLayers,
            TextureRuleCase::UnknownFormat,
            TextureRuleCase::UndeclaredFormat,
            TextureRuleCase::EmptyBytes,
            TextureRuleCase::EmptySubresources,
            TextureRuleCase::CubemapDepth,
            TextureRuleCase::CubemapTooFewLayers,
            TextureRuleCase::CubemapLayerMultiple,
            TextureRuleCase::ArraySingleLayer,
            TextureRuleCase::NonArrayMultipleLayers,
            TextureRuleCase::SubresourceCountMultiplyOverflow,
            TextureRuleCase::SubresourceCountMismatch,
            TextureRuleCase::DuplicatePair,
            TextureRuleCase::PairMipOutOfBounds,
            TextureRuleCase::PairLayerOutOfBounds,
            TextureRuleCase::EmptyRange,
            TextureRuleCase::NonCanonicalEmptyRange,
            TextureRuleCase::NonzeroRangeStride,
            TextureRuleCase::RangeAdditionOverflow,
            TextureRuleCase::RangeOutOfBounds,
            TextureRuleCase::ZeroRowPitch,
            TextureRuleCase::SliceBelowRow,
            TextureRuleCase::RangeBelowSlice));

    TEST(RenderContractsValidation, CreatesCompleteCubemapAndArrayTextures)
    {
        auto cubemapInfo = MakeValidTextureRequestInfo();
        auto& cubemap = std::get<TextureUploadPayload>(cubemapInfo.payload);
        cubemap.createInfo.isCubemap = true;
        cubemap.createInfo.arrayLayers = 6;
        cubemap.bytes.resize(24, 1U);
        cubemap.subresources.clear();
        for (uint32 layer = 0; layer < 6; ++layer)
        {
            cubemap.subresources.push_back(TextureUploadSubresource{
                UploadByteRange{layer * 4U, 4, 0}, 0, layer, 4, 4});
        }
        cubemapInfo.declaredPayloadBytes =
            24 + 6 * static_cast<uint64>(sizeof(TextureUploadSubresource));
        ExpectCreationCode(cubemapInfo, ResourceUploadRequestCreateCode::Created);

        auto arrayInfo = MakeValidTextureRequestInfo();
        ConfigureTwoLayerTexture(arrayInfo);
        ExpectCreationCode(arrayInfo, ResourceUploadRequestCreateCode::Created);
    }

    enum class MaterialRuleCase : uint8
    {
        NonFiniteBaseColor,
        NonFiniteMetallic,
        NonFiniteRoughness,
        NonFiniteNormalScale,
        NonFiniteOcclusion,
        NonFiniteEmissiveColor,
        NonFiniteEmissiveStrength,
        NonFiniteAlphaCutoff,
        BaseColorBelowRange,
        MetallicAboveRange,
        RoughnessBelowRange,
        OcclusionAboveRange,
        AlphaCutoffAboveRange,
        NegativeNormalScale,
        NegativeEmissiveStrength,
        InvalidAlphaMode,
        InvalidWorkflow,
        InvalidBindingHandle,
        NegativeUvSet,
        ZeroScaleX,
        ZeroScaleY,
        NonFiniteOffset,
        NonFiniteScale,
        NonFiniteRotation,
        DuplicateSlot,
        InvalidSlot,
        InvalidWrapS,
        InvalidWrapT,
        InvalidMinFilter,
        InvalidMagFilter,
        UnsupportedMagFilter
    };

    class RenderContractsValidationMaterialPayloadRules :
        public testing::TestWithParam<MaterialRuleCase>
    {
    };

    TEST_P(RenderContractsValidationMaterialPayloadRules, RejectsInvalidNumericEnumOrBinding)
    {
        auto info = MakeValidMaterialRequestInfo();
        auto& payload = std::get<MaterialUploadPayload>(info.payload);
        auto& source = payload.sourceData;
        const float32 nan = std::numeric_limits<float32>::quiet_NaN();
        const MaterialRuleCase testCase = GetParam();

        if (testCase >= MaterialRuleCase::InvalidBindingHandle)
        {
            MaterialUploadTextureBinding binding;
            binding.texture = RenderResourceHandle{2, 1};
            payload.textureBindings.push_back(binding);
            info.declaredPayloadBytes = sizeof(MaterialUploadTextureBinding);
        }

        switch (testCase)
        {
            case MaterialRuleCase::NonFiniteBaseColor: source.baseColorFactor.x = nan; break;
            case MaterialRuleCase::NonFiniteMetallic: source.metallicFactor = nan; break;
            case MaterialRuleCase::NonFiniteRoughness: source.roughnessFactor = nan; break;
            case MaterialRuleCase::NonFiniteNormalScale: source.normalScale = nan; break;
            case MaterialRuleCase::NonFiniteOcclusion: source.occlusionStrength = nan; break;
            case MaterialRuleCase::NonFiniteEmissiveColor: source.emissiveColor.x = nan; break;
            case MaterialRuleCase::NonFiniteEmissiveStrength: source.emissiveStrength = nan; break;
            case MaterialRuleCase::NonFiniteAlphaCutoff: source.alphaCutoff = nan; break;
            case MaterialRuleCase::BaseColorBelowRange: source.baseColorFactor.x = -0.1f; break;
            case MaterialRuleCase::MetallicAboveRange: source.metallicFactor = 1.1f; break;
            case MaterialRuleCase::RoughnessBelowRange: source.roughnessFactor = -0.1f; break;
            case MaterialRuleCase::OcclusionAboveRange: source.occlusionStrength = 1.1f; break;
            case MaterialRuleCase::AlphaCutoffAboveRange: source.alphaCutoff = 1.1f; break;
            case MaterialRuleCase::NegativeNormalScale: source.normalScale = -0.1f; break;
            case MaterialRuleCase::NegativeEmissiveStrength: source.emissiveStrength = -0.1f; break;
            case MaterialRuleCase::InvalidAlphaMode:
                source.alphaMode = static_cast<MaterialSourceAlphaMode>(255);
                break;
            case MaterialRuleCase::InvalidWorkflow:
                source.workflow = static_cast<MaterialSourceWorkflow>(255);
                break;
            case MaterialRuleCase::InvalidBindingHandle:
                payload.textureBindings.front().texture = {};
                break;
            case MaterialRuleCase::NegativeUvSet:
                payload.textureBindings.front().uvSet = -1;
                break;
            case MaterialRuleCase::ZeroScaleX:
                payload.textureBindings.front().scale.x = 0.0f;
                break;
            case MaterialRuleCase::ZeroScaleY:
                payload.textureBindings.front().scale.y = 0.0f;
                break;
            case MaterialRuleCase::NonFiniteOffset:
                payload.textureBindings.front().offset.x = nan;
                break;
            case MaterialRuleCase::NonFiniteScale:
                payload.textureBindings.front().scale.x = nan;
                break;
            case MaterialRuleCase::NonFiniteRotation:
                payload.textureBindings.front().rotation = nan;
                break;
            case MaterialRuleCase::DuplicateSlot:
                payload.textureBindings.push_back(payload.textureBindings.front());
                info.declaredPayloadBytes += sizeof(MaterialUploadTextureBinding);
                break;
            case MaterialRuleCase::InvalidSlot:
                payload.textureBindings.front().slot =
                    static_cast<MaterialUploadTextureSlot>(255);
                break;
            case MaterialRuleCase::InvalidWrapS:
                payload.textureBindings.front().wrapS =
                    static_cast<MaterialUploadWrapMode>(255);
                break;
            case MaterialRuleCase::InvalidWrapT:
                payload.textureBindings.front().wrapT =
                    static_cast<MaterialUploadWrapMode>(255);
                break;
            case MaterialRuleCase::InvalidMinFilter:
                payload.textureBindings.front().minFilter =
                    static_cast<MaterialUploadFilterMode>(255);
                break;
            case MaterialRuleCase::InvalidMagFilter:
                payload.textureBindings.front().magFilter =
                    static_cast<MaterialUploadFilterMode>(255);
                break;
            case MaterialRuleCase::UnsupportedMagFilter:
                payload.textureBindings.front().magFilter =
                    MaterialUploadFilterMode::LinearMipmapLinear;
                break;
        }

        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidPayload);
    }

    INSTANTIATE_TEST_SUITE_P(
        AllMaterialPayloadRules,
        RenderContractsValidationMaterialPayloadRules,
        testing::Values(
            MaterialRuleCase::NonFiniteBaseColor,
            MaterialRuleCase::NonFiniteMetallic,
            MaterialRuleCase::NonFiniteRoughness,
            MaterialRuleCase::NonFiniteNormalScale,
            MaterialRuleCase::NonFiniteOcclusion,
            MaterialRuleCase::NonFiniteEmissiveColor,
            MaterialRuleCase::NonFiniteEmissiveStrength,
            MaterialRuleCase::NonFiniteAlphaCutoff,
            MaterialRuleCase::BaseColorBelowRange,
            MaterialRuleCase::MetallicAboveRange,
            MaterialRuleCase::RoughnessBelowRange,
            MaterialRuleCase::OcclusionAboveRange,
            MaterialRuleCase::AlphaCutoffAboveRange,
            MaterialRuleCase::NegativeNormalScale,
            MaterialRuleCase::NegativeEmissiveStrength,
            MaterialRuleCase::InvalidAlphaMode,
            MaterialRuleCase::InvalidWorkflow,
            MaterialRuleCase::InvalidBindingHandle,
            MaterialRuleCase::NegativeUvSet,
            MaterialRuleCase::ZeroScaleX,
            MaterialRuleCase::ZeroScaleY,
            MaterialRuleCase::NonFiniteOffset,
            MaterialRuleCase::NonFiniteScale,
            MaterialRuleCase::NonFiniteRotation,
            MaterialRuleCase::DuplicateSlot,
            MaterialRuleCase::InvalidSlot,
            MaterialRuleCase::InvalidWrapS,
            MaterialRuleCase::InvalidWrapT,
            MaterialRuleCase::InvalidMinFilter,
            MaterialRuleCase::InvalidMagFilter,
            MaterialRuleCase::UnsupportedMagFilter));

    TEST(RenderContractsValidation, RejectsUndeclaredReadinessAndPriority)
    {
        auto readinessInfo = MakeValidMaterialRequestInfo();
        readinessInfo.dependencyReadiness =
            static_cast<RenderDependencyReadiness>(255);
        ExpectCreationCode(readinessInfo,
                           ResourceUploadRequestCreateCode::InvalidPayload);

        auto priorityInfo = MakeValidMaterialRequestInfo();
        priorityInfo.priority = static_cast<RenderUploadPriority>(255);
        ExpectCreationCode(priorityInfo,
                           ResourceUploadRequestCreateCode::InvalidPayload);
    }

    TEST(RenderContractsValidation,
         ViewClearPolicyIsVersionedCopiedAndRejectsInvalidValues)
    {
        RenderFrameHeaderV5 header;
        header.sequence = 41;
        header.requiredSceneRevision = 9;
        RenderViewSnapshot view = MakeValidView();
        view.clearPolicy = RenderViewClearPolicy::SolidColor;
        view.clearColor = {0.25f, 0.5f, 0.75f, 1.0f};

        const std::unique_ptr<const RenderFramePacketV5> packet =
            RenderFramePacketV5::Create(header,
                                        view,
                                        RenderFrameSettings{},
                                        RenderFrameCaptureRequest{},
                                        MakeCompleteDiagnostics());
        ASSERT_NE(packet, nullptr);
        EXPECT_EQ(packet->GetHeader().schemaVersion,
                  RVX_RENDER_FRAME_PACKET_V5_SCHEMA_VERSION);
        EXPECT_EQ(packet->GetView().clearPolicy,
                  RenderViewClearPolicy::SolidColor);
        EXPECT_FLOAT_EQ(packet->GetView().clearColor.z, 0.75f);

        view.clearColor.x = std::numeric_limits<float32>::quiet_NaN();
        EXPECT_EQ(RenderFramePacketV5::Create(header,
                                               view,
                                               RenderFrameSettings{},
                                               RenderFrameCaptureRequest{},
                                               MakeCompleteDiagnostics()),
                  nullptr);

        view = MakeValidView();
        view.clearPolicy = static_cast<RenderViewClearPolicy>(255);
        EXPECT_FALSE(IsValidRenderViewClearValues(view));
        EXPECT_EQ(RenderFramePacketV5::Create(header,
                                               view,
                                               RenderFrameSettings{},
                                               RenderFrameCaptureRequest{},
                                               MakeCompleteDiagnostics()),
                  nullptr);
    }

    TEST(RenderContractsValidation,
         FrameV5RejectsIncompleteInputsAndOwnsAcceptedValues)
    {
        RenderFrameHeaderV5 header;
        header.sequence = 87;
        header.requiredSceneRevision = 31;
        header.worldRevision = 12;

        RenderViewSnapshot view = MakeValidView();
        view.exposure = 1.25f;

        RenderFrameSettings settings;
        settings.renderScale = 0.75f;

        RenderFrameCaptureRequest captureRequest;
        captureRequest.requestId = 9;
        captureRequest.kind = RenderFrameCaptureKind::Color;
        captureRequest.width = 64;
        captureRequest.height = 32;

        const std::unique_ptr<const RenderFramePacketV5> packet =
            RenderFramePacketV5::Create(header,
                                        view,
                                        settings,
                                        captureRequest,
                                        MakeCompleteDiagnostics());
        ASSERT_NE(packet, nullptr);

        header.sequence = 0;
        view.exposure = 4.0f;
        settings.renderScale = 0.5f;
        captureRequest.width = 1;

        EXPECT_EQ(packet->GetHeader().sequence, 87U);
        EXPECT_EQ(packet->GetHeader().requiredSceneRevision, 31U);
        EXPECT_FLOAT_EQ(packet->GetView().exposure, 1.25f);
        EXPECT_FLOAT_EQ(packet->GetSettings().renderScale, 0.75f);
        EXPECT_EQ(packet->GetCaptureRequest().width, 64U);

        RenderFrameHeaderV5 invalidHeader;
        invalidHeader.sequence = 1;
        EXPECT_EQ(RenderFramePacketV5::Create(invalidHeader,
                                               MakeValidView(),
                                               RenderFrameSettings{},
                                               RenderFrameCaptureRequest{},
                                               MakeCompleteDiagnostics()),
                  nullptr);

        RenderExtractionDiagnostics incompleteDiagnostics =
            MakeCompleteDiagnostics();
        incompleteDiagnostics.complete = false;
        EXPECT_EQ(RenderFramePacketV5::Create(packet->GetHeader(),
                                               MakeValidView(),
                                               RenderFrameSettings{},
                                               RenderFrameCaptureRequest{},
                                               incompleteDiagnostics),
                  nullptr);
    }

} // namespace
} // namespace RVX
