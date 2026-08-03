#include "RenderContracts/FeatureRenderSnapshot.h"
#include "RenderContracts/RenderFramePacket.h"
#include "RenderContracts/RenderIdentity.h"
#include "RenderContracts/ResourceUploadRequest.h"
#include "RenderExtraction/RenderFramePacketBuilder.h"
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
    static_assert(!std::is_default_constructible_v<RenderFramePacket>);
    static_assert(!std::is_copy_constructible_v<RenderFramePacket>);
    static_assert(!std::is_move_constructible_v<RenderFramePacket>);
    static_assert(!std::is_default_constructible_v<ResourceUploadRequest>);
    static_assert(!std::is_copy_constructible_v<ResourceUploadRequest>);
    static_assert(!std::is_move_constructible_v<ResourceUploadRequest>);
    static_assert(!std::is_copy_constructible_v<RenderFramePacketBuilder>);
    static_assert(!std::is_move_constructible_v<RenderFramePacketBuilder>);
    static_assert(std::is_same_v<
                  decltype(std::declval<RenderFramePacketBuilder>().Seal()),
                  std::unique_ptr<const RenderFramePacket>>);

    constexpr uint32 RVX_OMIT_HEADER = 1U << 0U;
    constexpr uint32 RVX_OMIT_VIEW = 1U << 1U;
    constexpr uint32 RVX_OMIT_SKY = 1U << 2U;
    constexpr uint32 RVX_OMIT_ENVIRONMENT = 1U << 3U;
    constexpr uint32 RVX_OMIT_SETTINGS = 1U << 4U;
    constexpr uint32 RVX_OMIT_CAPTURE = 1U << 5U;
    constexpr uint32 RVX_OMIT_FEATURES = 1U << 6U;
    constexpr uint32 RVX_OMIT_DIAGNOSTICS = 1U << 7U;

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

    RenderFrameHeader MakeValidHeader()
    {
        RenderFrameHeader header;
        header.sequence = 1;
        return header;
    }

    RenderViewSnapshot MakeValidView()
    {
        RenderViewSnapshot view;
        view.viewportWidth = 1;
        view.viewportHeight = 1;
        return view;
    }

    RenderFeatureSnapshot MakeCompleteFeatures()
    {
        RenderFeatureSnapshot features;
        features.BeginBuild(1);
        features.MarkComplete();
        return features;
    }

    RenderExtractionDiagnostics MakeCompleteDiagnostics()
    {
        RenderExtractionDiagnostics diagnostics;
        diagnostics.complete = true;
        return diagnostics;
    }

    void PopulateCompletePacketBuilder(RenderFramePacketBuilder& builder,
                                       uint32 omittedSingletonMask = 0)
    {
        if ((omittedSingletonMask & RVX_OMIT_HEADER) == 0)
        {
            EXPECT_TRUE(builder.SetHeader(MakeValidHeader()));
        }
        if ((omittedSingletonMask & RVX_OMIT_VIEW) == 0)
        {
            EXPECT_TRUE(builder.SetView(MakeValidView()));
        }
        if ((omittedSingletonMask & RVX_OMIT_SKY) == 0)
        {
            EXPECT_TRUE(builder.SetSky(RenderSkySnapshot{}));
        }
        if ((omittedSingletonMask & RVX_OMIT_ENVIRONMENT) == 0)
        {
            EXPECT_TRUE(builder.SetEnvironment(RenderEnvironmentSnapshot{}));
        }
        if ((omittedSingletonMask & RVX_OMIT_SETTINGS) == 0)
        {
            EXPECT_TRUE(builder.SetSettings(RenderFrameSettings{}));
        }
        if ((omittedSingletonMask & RVX_OMIT_CAPTURE) == 0)
        {
            EXPECT_TRUE(builder.SetCaptureRequest(RenderFrameCaptureRequest{}));
        }
        if ((omittedSingletonMask & RVX_OMIT_FEATURES) == 0)
        {
            EXPECT_TRUE(builder.SetFeatures(MakeCompleteFeatures()));
        }
        if ((omittedSingletonMask & RVX_OMIT_DIAGNOSTICS) == 0)
        {
            EXPECT_TRUE(builder.SetExtractionDiagnostics(MakeCompleteDiagnostics()));
        }
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
        info.schemaVersion = 2;
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
        info.schemaVersion = 2;
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

    class RenderContractsValidationMissingSingletons : public testing::TestWithParam<uint32>
    {
    };

    TEST_P(RenderContractsValidationMissingSingletons, MissingEachSingletonIsRepairable)
    {
        RenderFramePacketBuilder builder;
        const uint32 omitted = GetParam();
        PopulateCompletePacketBuilder(builder, omitted);

        EXPECT_FALSE(builder.Seal());
        EXPECT_EQ(builder.GetLastSealCode(), RenderFrameSealCode::MissingValue);
        EXPECT_EQ(builder.GetState(), RenderFramePacketBuilderState::Building);

        switch (omitted)
        {
            case RVX_OMIT_HEADER: EXPECT_TRUE(builder.SetHeader(MakeValidHeader())); break;
            case RVX_OMIT_VIEW: EXPECT_TRUE(builder.SetView(MakeValidView())); break;
            case RVX_OMIT_SKY: EXPECT_TRUE(builder.SetSky(RenderSkySnapshot{})); break;
            case RVX_OMIT_ENVIRONMENT:
                EXPECT_TRUE(builder.SetEnvironment(RenderEnvironmentSnapshot{}));
                break;
            case RVX_OMIT_SETTINGS:
                EXPECT_TRUE(builder.SetSettings(RenderFrameSettings{}));
                break;
            case RVX_OMIT_CAPTURE:
                EXPECT_TRUE(builder.SetCaptureRequest(RenderFrameCaptureRequest{}));
                break;
            case RVX_OMIT_FEATURES:
                EXPECT_TRUE(builder.SetFeatures(MakeCompleteFeatures()));
                break;
            case RVX_OMIT_DIAGNOSTICS:
                EXPECT_TRUE(builder.SetExtractionDiagnostics(MakeCompleteDiagnostics()));
                break;
            default: FAIL() << "unexpected singleton bit"; break;
        }

        const auto packet = builder.Seal();
        EXPECT_TRUE(packet);
        EXPECT_EQ(builder.GetState(), RenderFramePacketBuilderState::Sealed);
        EXPECT_EQ(builder.GetLastSealCode(), RenderFrameSealCode::Sealed);
    }

    INSTANTIATE_TEST_SUITE_P(
        AllSingletons,
        RenderContractsValidationMissingSingletons,
        testing::Values(
            RVX_OMIT_HEADER,
            RVX_OMIT_VIEW,
            RVX_OMIT_SKY,
            RVX_OMIT_ENVIRONMENT,
            RVX_OMIT_SETTINGS,
            RVX_OMIT_CAPTURE,
            RVX_OMIT_FEATURES,
            RVX_OMIT_DIAGNOSTICS));

    TEST(RenderContractsValidation, RejectsSchemaThenRepairs)
    {
        RenderFramePacketBuilder builder;
        PopulateCompletePacketBuilder(builder);
        RenderFrameHeader invalid = MakeValidHeader();
        invalid.schemaVersion = RVX_RENDER_FRAME_PACKET_SCHEMA_VERSION + 1;
        ASSERT_TRUE(builder.SetHeader(invalid));

        EXPECT_FALSE(builder.Seal());
        EXPECT_EQ(builder.GetLastSealCode(), RenderFrameSealCode::InvalidSchema);
        EXPECT_EQ(builder.GetState(), RenderFramePacketBuilderState::Building);

        ASSERT_TRUE(builder.SetHeader(MakeValidHeader()));
        EXPECT_TRUE(builder.Seal());
        EXPECT_EQ(builder.GetLastSealCode(), RenderFrameSealCode::Sealed);
    }

    TEST(RenderContractsValidation, BuilderRejectsZeroSequence)
    {
        RenderFramePacketBuilder builder;
        PopulateCompletePacketBuilder(builder);
        RenderFrameHeader header = MakeValidHeader();
        header.sequence = 0;
        ASSERT_TRUE(builder.SetHeader(header));

        EXPECT_FALSE(builder.Seal());
        EXPECT_EQ(builder.GetLastSealCode(), RenderFrameSealCode::InvalidSequence);
    }

    TEST(RenderContractsValidation, RejectsZeroViewport)
    {
        RenderFramePacketBuilder builder;
        PopulateCompletePacketBuilder(builder);
        RenderViewSnapshot view = MakeValidView();
        view.viewportWidth = 0;
        ASSERT_TRUE(builder.SetView(view));

        EXPECT_FALSE(builder.Seal());
        EXPECT_EQ(builder.GetLastSealCode(), RenderFrameSealCode::InvalidViewport);
    }

    enum class NumericFamilyCase : uint8
    {
        ViewMatrix,
        ViewVector,
        ViewScalar,
        Primitive,
        Light,
        Sky,
        Environment,
        Settings
    };

    class RenderContractsValidationBuilderNumericFamilies :
        public testing::TestWithParam<NumericFamilyCase>
    {
    };

    TEST_P(RenderContractsValidationBuilderNumericFamilies, RejectsEachNonFiniteNumericFamily)
    {
        RenderFramePacketBuilder builder;
        PopulateCompletePacketBuilder(builder);
        const float32 nan = std::numeric_limits<float32>::quiet_NaN();

        switch (GetParam())
        {
            case NumericFamilyCase::ViewMatrix:
            {
                auto view = MakeValidView();
                view.viewMatrix[0][0] = nan;
                ASSERT_TRUE(builder.SetView(view));
                break;
            }
            case NumericFamilyCase::ViewVector:
            {
                auto view = MakeValidView();
                view.cameraPosition.x = nan;
                ASSERT_TRUE(builder.SetView(view));
                break;
            }
            case NumericFamilyCase::ViewScalar:
            {
                auto view = MakeValidView();
                view.absoluteTime = nan;
                ASSERT_TRUE(builder.SetView(view));
                break;
            }
            case NumericFamilyCase::Primitive:
            {
                RenderPrimitiveSnapshot primitive;
                primitive.objectId = 1;
                primitive.mesh = RenderResourceHandle{1, 1};
                primitive.worldTransform[0][0] = nan;
                ASSERT_TRUE(builder.AddPrimitive(primitive));
                auto header = MakeValidHeader();
                header.expectedPrimitiveCount = 1;
                header.extractedPrimitiveCount = 1;
                ASSERT_TRUE(builder.SetHeader(header));
                break;
            }
            case NumericFamilyCase::Light:
            {
                RenderLightSnapshot light;
                light.lightId = 1;
                light.intensity = nan;
                ASSERT_TRUE(builder.AddLight(light));
                auto header = MakeValidHeader();
                header.expectedLightCount = 1;
                header.extractedLightCount = 1;
                ASSERT_TRUE(builder.SetHeader(header));
                break;
            }
            case NumericFamilyCase::Sky:
            {
                RenderSkySnapshot sky;
                sky.intensity = nan;
                ASSERT_TRUE(builder.SetSky(sky));
                break;
            }
            case NumericFamilyCase::Environment:
            {
                RenderEnvironmentSnapshot environment;
                environment.intensity = nan;
                ASSERT_TRUE(builder.SetEnvironment(environment));
                break;
            }
            case NumericFamilyCase::Settings:
            {
                RenderFrameSettings settings;
                settings.renderScale = nan;
                ASSERT_TRUE(builder.SetSettings(settings));
                break;
            }
        }

        EXPECT_FALSE(builder.Seal());
        EXPECT_EQ(builder.GetLastSealCode(), RenderFrameSealCode::InvalidNumericValue);
    }

    INSTANTIATE_TEST_SUITE_P(
        AllNumericFamilies,
        RenderContractsValidationBuilderNumericFamilies,
        testing::Values(
            NumericFamilyCase::ViewMatrix,
            NumericFamilyCase::ViewVector,
            NumericFamilyCase::ViewScalar,
            NumericFamilyCase::Primitive,
            NumericFamilyCase::Light,
            NumericFamilyCase::Sky,
            NumericFamilyCase::Environment,
            NumericFamilyCase::Settings));

    enum class SkyExtendedNumericField : uint8
    {
        SunDirection,
        SunColor,
        ZenithColor,
        HorizonColor,
        GroundColor,
        BlurLevel,
        ScatteringIntensity
    };

    class RenderContractsValidationSkyExtendedNumerics :
        public testing::TestWithParam<SkyExtendedNumericField>
    {
    };

    TEST_P(RenderContractsValidationSkyExtendedNumerics,
           RejectsNonFiniteSkySnapshotFields)
    {
        RenderFramePacketBuilder builder;
        PopulateCompletePacketBuilder(builder);
        RenderSkySnapshot sky;
        sky.mode = RenderSkyMode::Procedural;
        const float32 nan = std::numeric_limits<float32>::quiet_NaN();

        switch (GetParam())
        {
            case SkyExtendedNumericField::SunDirection:
                sky.sunDirection.x = nan;
                break;
            case SkyExtendedNumericField::SunColor:
                sky.sunColor.y = nan;
                break;
            case SkyExtendedNumericField::ZenithColor:
                sky.zenithColor.z = nan;
                break;
            case SkyExtendedNumericField::HorizonColor:
                sky.horizonColor.x = nan;
                break;
            case SkyExtendedNumericField::GroundColor:
                sky.groundColor.y = nan;
                break;
            case SkyExtendedNumericField::BlurLevel:
                sky.blurLevel = nan;
                break;
            case SkyExtendedNumericField::ScatteringIntensity:
                sky.scatteringIntensity = nan;
                break;
        }

        ASSERT_TRUE(builder.SetSky(sky));
        EXPECT_FALSE(builder.Seal());
        EXPECT_EQ(builder.GetLastSealCode(),
                  RenderFrameSealCode::InvalidNumericValue);
    }

    INSTANTIATE_TEST_SUITE_P(
        AllExtendedSkyFields,
        RenderContractsValidationSkyExtendedNumerics,
        testing::Values(SkyExtendedNumericField::SunDirection,
                        SkyExtendedNumericField::SunColor,
                        SkyExtendedNumericField::ZenithColor,
                        SkyExtendedNumericField::HorizonColor,
                        SkyExtendedNumericField::GroundColor,
                        SkyExtendedNumericField::BlurLevel,
                        SkyExtendedNumericField::ScatteringIntensity));

    TEST(RenderContractsValidation, BuilderRejectsUndeclaredSkyMode)
    {
        RenderFramePacketBuilder builder;
        PopulateCompletePacketBuilder(builder);
        RenderSkySnapshot sky;
        sky.mode = static_cast<RenderSkyMode>(255);
        ASSERT_TRUE(builder.SetSky(sky));

        EXPECT_FALSE(builder.Seal());
        EXPECT_EQ(builder.GetLastSealCode(),
                  RenderFrameSealCode::InvalidNumericValue);
    }

    enum class NumericConstraintCase : uint8
    {
        ViewBounds,
        PrimitiveBounds,
        NearPlane,
        FarPlane,
        NegativeDelta,
        Exposure,
        RenderScale,
        BloomThreshold,
        BloomIntensity,
        ShadowAtlas,
        ShadowCascades,
        ShadowDistance,
        CullingLimit,
        RayInstanceLimit,
        RayLimit,
        DisabledRayShadows,
        DisabledRayReflections
    };

    class RenderContractsValidationBuilderNumericConstraints :
        public testing::TestWithParam<NumericConstraintCase>
    {
    };

    TEST_P(RenderContractsValidationBuilderNumericConstraints, RejectsNumericConstraints)
    {
        RenderFramePacketBuilder builder;
        PopulateCompletePacketBuilder(builder);
        RenderViewSnapshot view = MakeValidView();
        RenderFrameSettings settings;

        switch (GetParam())
        {
            case NumericConstraintCase::ViewBounds:
                view.nearPlane = 2.0f;
                view.farPlane = 1.0f;
                ASSERT_TRUE(builder.SetView(view));
                break;
            case NumericConstraintCase::PrimitiveBounds:
            {
                RenderPrimitiveSnapshot primitive;
                primitive.objectId = 1;
                primitive.mesh = RenderResourceHandle{1, 1};
                primitive.boundsMin.x = 1.0f;
                primitive.boundsMax.x = 0.0f;
                ASSERT_TRUE(builder.AddPrimitive(primitive));
                auto header = MakeValidHeader();
                header.expectedPrimitiveCount = 1;
                header.extractedPrimitiveCount = 1;
                ASSERT_TRUE(builder.SetHeader(header));
                break;
            }
            case NumericConstraintCase::NearPlane:
                view.nearPlane = 0.0f;
                ASSERT_TRUE(builder.SetView(view));
                break;
            case NumericConstraintCase::FarPlane:
                view.farPlane = view.nearPlane;
                ASSERT_TRUE(builder.SetView(view));
                break;
            case NumericConstraintCase::NegativeDelta:
                view.deltaTime = -0.1f;
                ASSERT_TRUE(builder.SetView(view));
                break;
            case NumericConstraintCase::Exposure:
                view.exposure = 0.0f;
                ASSERT_TRUE(builder.SetView(view));
                break;
            case NumericConstraintCase::RenderScale: settings.renderScale = 0.0f; goto set_settings;
            case NumericConstraintCase::BloomThreshold: settings.postProcess.bloomThreshold = -0.1f; goto set_settings;
            case NumericConstraintCase::BloomIntensity: settings.postProcess.bloomIntensity = -0.1f; goto set_settings;
            case NumericConstraintCase::ShadowAtlas: settings.shadows.atlasResolution = 0; goto set_settings;
            case NumericConstraintCase::ShadowCascades: settings.shadows.cascadeCount = 0; goto set_settings;
            case NumericConstraintCase::ShadowDistance: settings.shadows.maxDistance = 0.0f; goto set_settings;
            case NumericConstraintCase::CullingLimit: settings.gpuCulling.maxVisibleObjects = 0; goto set_settings;
            case NumericConstraintCase::RayInstanceLimit:
                settings.rayTracing.enabled = true;
                settings.rayTracing.maxInstances = 0;
                goto set_settings;
            case NumericConstraintCase::RayLimit:
                settings.rayTracing.enabled = true;
                settings.rayTracing.maxRaysPerPixel = 0;
                goto set_settings;
            case NumericConstraintCase::DisabledRayShadows:
                settings.rayTracing.enableShadows = true;
                goto set_settings;
            case NumericConstraintCase::DisabledRayReflections:
                settings.rayTracing.enableReflections = true;
                goto set_settings;
            set_settings:
                ASSERT_TRUE(builder.SetSettings(settings));
                break;
        }

        EXPECT_FALSE(builder.Seal());
        EXPECT_EQ(builder.GetLastSealCode(), RenderFrameSealCode::InvalidNumericValue);
    }

    INSTANTIATE_TEST_SUITE_P(
        AllNumericConstraints,
        RenderContractsValidationBuilderNumericConstraints,
        testing::Values(
            NumericConstraintCase::ViewBounds,
            NumericConstraintCase::PrimitiveBounds,
            NumericConstraintCase::NearPlane,
            NumericConstraintCase::FarPlane,
            NumericConstraintCase::NegativeDelta,
            NumericConstraintCase::Exposure,
            NumericConstraintCase::RenderScale,
            NumericConstraintCase::BloomThreshold,
            NumericConstraintCase::BloomIntensity,
            NumericConstraintCase::ShadowAtlas,
            NumericConstraintCase::ShadowCascades,
            NumericConstraintCase::ShadowDistance,
            NumericConstraintCase::CullingLimit,
            NumericConstraintCase::RayInstanceLimit,
            NumericConstraintCase::RayLimit,
            NumericConstraintCase::DisabledRayShadows,
            NumericConstraintCase::DisabledRayReflections));

    enum class ExtractionFailureCase : uint8
    {
        Code,
        CompleteBit,
        SkippedPrimitive,
        SkippedLight,
        SkippedProvider
    };

    class RenderContractsValidationExtractionCompleteness :
        public testing::TestWithParam<ExtractionFailureCase>
    {
    };

    TEST_P(RenderContractsValidationExtractionCompleteness, RejectsIncompleteExtraction)
    {
        RenderFramePacketBuilder builder;
        PopulateCompletePacketBuilder(builder);
        auto diagnostics = MakeCompleteDiagnostics();

        switch (GetParam())
        {
            case ExtractionFailureCase::Code:
                diagnostics.code = RenderExtractionCode::MissingProvider;
                break;
            case ExtractionFailureCase::CompleteBit: diagnostics.complete = false; break;
            case ExtractionFailureCase::SkippedPrimitive:
                diagnostics.skippedPrimitiveCount = 1;
                break;
            case ExtractionFailureCase::SkippedLight:
                diagnostics.skippedLightCount = 1;
                break;
            case ExtractionFailureCase::SkippedProvider:
                diagnostics.skippedFeatureProviderCount = 1;
                break;
        }
        ASSERT_TRUE(builder.SetExtractionDiagnostics(diagnostics));

        EXPECT_FALSE(builder.Seal());
        EXPECT_EQ(builder.GetLastSealCode(), RenderFrameSealCode::IncompleteExtraction);
    }

    INSTANTIATE_TEST_SUITE_P(
        AllExtractionMarkers,
        RenderContractsValidationExtractionCompleteness,
        testing::Values(
            ExtractionFailureCase::Code,
            ExtractionFailureCase::CompleteBit,
            ExtractionFailureCase::SkippedPrimitive,
            ExtractionFailureCase::SkippedLight,
            ExtractionFailureCase::SkippedProvider));

    enum class FeatureCompletenessCase : uint8
    {
        AggregateSchema,
        AggregateSequence,
        AggregateStatus,
        AggregateComplete,
        ParticleSchema,
        ParticleSequence,
        ParticleStatus,
        ParticleComplete,
        WaterSchema,
        WaterSequence,
        WaterStatus,
        WaterComplete,
        TerrainSchema,
        TerrainSequence,
        TerrainStatus,
        TerrainComplete
    };

    class RenderContractsValidationFeatureCompleteness :
        public testing::TestWithParam<FeatureCompletenessCase>
    {
    };

    TEST_P(RenderContractsValidationFeatureCompleteness, RejectsIncompleteFeatureSnapshot)
    {
        RenderFramePacketBuilder builder;
        PopulateCompletePacketBuilder(builder);
        auto features = MakeCompleteFeatures();

        switch (GetParam())
        {
            case FeatureCompletenessCase::AggregateSchema: ++features.metadata.schemaVersion; break;
            case FeatureCompletenessCase::AggregateSequence: ++features.metadata.sequence; break;
            case FeatureCompletenessCase::AggregateStatus:
                features.metadata.status = RenderFeatureSnapshotStatus::Incomplete;
                break;
            case FeatureCompletenessCase::AggregateComplete: features.metadata.complete = false; break;
            case FeatureCompletenessCase::ParticleSchema: ++features.particles.metadata.schemaVersion; break;
            case FeatureCompletenessCase::ParticleSequence: ++features.particles.metadata.sequence; break;
            case FeatureCompletenessCase::ParticleStatus:
                features.particles.metadata.status = ParticleRenderSnapshotStatus::Incomplete;
                break;
            case FeatureCompletenessCase::ParticleComplete: features.particles.metadata.complete = false; break;
            case FeatureCompletenessCase::WaterSchema: ++features.water.metadata.schemaVersion; break;
            case FeatureCompletenessCase::WaterSequence: ++features.water.metadata.sequence; break;
            case FeatureCompletenessCase::WaterStatus:
                features.water.metadata.status = WaterRenderSnapshotStatus::Incomplete;
                break;
            case FeatureCompletenessCase::WaterComplete: features.water.metadata.complete = false; break;
            case FeatureCompletenessCase::TerrainSchema: ++features.terrain.metadata.schemaVersion; break;
            case FeatureCompletenessCase::TerrainSequence: ++features.terrain.metadata.sequence; break;
            case FeatureCompletenessCase::TerrainStatus:
                features.terrain.metadata.status = TerrainRenderSnapshotStatus::Incomplete;
                break;
            case FeatureCompletenessCase::TerrainComplete: features.terrain.metadata.complete = false; break;
        }
        ASSERT_TRUE(builder.SetFeatures(features));

        EXPECT_FALSE(builder.Seal());
        EXPECT_EQ(builder.GetLastSealCode(), RenderFrameSealCode::IncompleteFeatureSnapshot);
    }

    INSTANTIATE_TEST_SUITE_P(
        AggregateAndNestedSnapshots,
        RenderContractsValidationFeatureCompleteness,
        testing::Values(
            FeatureCompletenessCase::AggregateSchema,
            FeatureCompletenessCase::AggregateSequence,
            FeatureCompletenessCase::AggregateStatus,
            FeatureCompletenessCase::AggregateComplete,
            FeatureCompletenessCase::ParticleSchema,
            FeatureCompletenessCase::ParticleSequence,
            FeatureCompletenessCase::ParticleStatus,
            FeatureCompletenessCase::ParticleComplete,
            FeatureCompletenessCase::WaterSchema,
            FeatureCompletenessCase::WaterSequence,
            FeatureCompletenessCase::WaterStatus,
            FeatureCompletenessCase::WaterComplete,
            FeatureCompletenessCase::TerrainSchema,
            FeatureCompletenessCase::TerrainSequence,
            FeatureCompletenessCase::TerrainStatus,
            FeatureCompletenessCase::TerrainComplete));

    enum class CountMismatchCase : uint8
    {
        PrimitiveExpected,
        PrimitiveExtracted,
        LightExpected,
        LightExtracted,
        ProviderExpected,
        ProviderExtracted,
        ProviderMetadata,
        FeatureSkippedProvider,
        ParticleSkippedCount,
        ParticleSkippedReasons,
        AggregateParticleItems,
        AggregateWaterItems,
        AggregateTerrainItems,
        ParticleItems,
        WaterItems,
        TerrainItems
    };

    class RenderContractsValidationBuilderCounts : public testing::TestWithParam<CountMismatchCase>
    {
    };

    TEST_P(RenderContractsValidationBuilderCounts, RejectsEachCountMismatch)
    {
        RenderFramePacketBuilder builder;
        PopulateCompletePacketBuilder(builder);
        RenderFrameHeader header = MakeValidHeader();
        RenderFeatureSnapshot features = MakeCompleteFeatures();

        switch (GetParam())
        {
            case CountMismatchCase::PrimitiveExpected: header.expectedPrimitiveCount = 1; break;
            case CountMismatchCase::PrimitiveExtracted: header.extractedPrimitiveCount = 1; break;
            case CountMismatchCase::LightExpected: header.expectedLightCount = 1; break;
            case CountMismatchCase::LightExtracted: header.extractedLightCount = 1; break;
            case CountMismatchCase::ProviderExpected:
                header.expectedFeatureProviderCount = 1;
                break;
            case CountMismatchCase::ProviderExtracted:
                header.extractedFeatureProviderCount = 1;
                break;
            case CountMismatchCase::ProviderMetadata:
                features.metadata.providerCount = 1;
                break;
            case CountMismatchCase::FeatureSkippedProvider:
                features.metadata.skippedProviderCount = 1;
                break;
            case CountMismatchCase::ParticleSkippedCount:
                features.particles.metadata.skippedInstanceCount = 1;
                break;
            case CountMismatchCase::ParticleSkippedReasons:
                features.particles.skippedReasons.push_back("skipped");
                break;
            case CountMismatchCase::AggregateParticleItems:
                features.metadata.particleItemCount = 1;
                break;
            case CountMismatchCase::AggregateWaterItems:
                features.metadata.waterItemCount = 1;
                break;
            case CountMismatchCase::AggregateTerrainItems:
                features.metadata.terrainItemCount = 1;
                break;
            case CountMismatchCase::ParticleItems:
                features.particles.metadata.itemCount = 1;
                break;
            case CountMismatchCase::WaterItems:
                features.water.metadata.itemCount = 1;
                break;
            case CountMismatchCase::TerrainItems:
                features.terrain.metadata.itemCount = 1;
                break;
        }
        ASSERT_TRUE(builder.SetHeader(header));
        ASSERT_TRUE(builder.SetFeatures(features));

        EXPECT_FALSE(builder.Seal());
        EXPECT_EQ(builder.GetLastSealCode(), RenderFrameSealCode::CountMismatch);
    }

    INSTANTIATE_TEST_SUITE_P(
        AllCountFields,
        RenderContractsValidationBuilderCounts,
        testing::Values(
            CountMismatchCase::PrimitiveExpected,
            CountMismatchCase::PrimitiveExtracted,
            CountMismatchCase::LightExpected,
            CountMismatchCase::LightExtracted,
            CountMismatchCase::ProviderExpected,
            CountMismatchCase::ProviderExtracted,
            CountMismatchCase::ProviderMetadata,
            CountMismatchCase::FeatureSkippedProvider,
            CountMismatchCase::ParticleSkippedCount,
            CountMismatchCase::ParticleSkippedReasons,
            CountMismatchCase::AggregateParticleItems,
            CountMismatchCase::AggregateWaterItems,
            CountMismatchCase::AggregateTerrainItems,
            CountMismatchCase::ParticleItems,
            CountMismatchCase::WaterItems,
            CountMismatchCase::TerrainItems));

    enum class ResourceReferenceCase : uint8
    {
        ZeroPrimitiveObject,
        InvalidPrimitiveMesh,
        ZeroLightId,
        InvalidLightType,
        ShadowUnexpectedHandle,
        PartialEnvironmentOne,
        PartialEnvironmentTwo
    };

    class RenderContractsValidationResourceReferences :
        public testing::TestWithParam<ResourceReferenceCase>
    {
    };

    TEST_P(RenderContractsValidationResourceReferences, RejectsResourceReferenceRules)
    {
        RenderFramePacketBuilder builder;
        PopulateCompletePacketBuilder(builder);

        switch (GetParam())
        {
            case ResourceReferenceCase::ZeroPrimitiveObject:
            case ResourceReferenceCase::InvalidPrimitiveMesh:
            {
                RenderPrimitiveSnapshot primitive;
                primitive.objectId = GetParam() == ResourceReferenceCase::ZeroPrimitiveObject ? 0 : 1;
                primitive.mesh = GetParam() == ResourceReferenceCase::InvalidPrimitiveMesh
                    ? RenderResourceHandle{}
                    : RenderResourceHandle{1, 1};
                ASSERT_TRUE(builder.AddPrimitive(primitive));
                auto header = MakeValidHeader();
                header.expectedPrimitiveCount = 1;
                header.extractedPrimitiveCount = 1;
                ASSERT_TRUE(builder.SetHeader(header));
                break;
            }
            case ResourceReferenceCase::ZeroLightId:
            case ResourceReferenceCase::InvalidLightType:
            case ResourceReferenceCase::ShadowUnexpectedHandle:
            {
                RenderLightSnapshot light;
                light.lightId = GetParam() == ResourceReferenceCase::ZeroLightId ? 0 : 1;
                if (GetParam() == ResourceReferenceCase::InvalidLightType)
                {
                    light.type = static_cast<RenderLightType>(255);
                }
                if (GetParam() == ResourceReferenceCase::ShadowUnexpectedHandle)
                {
                    light.shadowResource = RenderResourceHandle{1, 1};
                }
                ASSERT_TRUE(builder.AddLight(light));
                auto header = MakeValidHeader();
                header.expectedLightCount = 1;
                header.extractedLightCount = 1;
                ASSERT_TRUE(builder.SetHeader(header));
                break;
            }
            case ResourceReferenceCase::PartialEnvironmentOne:
            case ResourceReferenceCase::PartialEnvironmentTwo:
            {
                RenderEnvironmentSnapshot environment;
                environment.irradianceTexture = RenderResourceHandle{1, 1};
                if (GetParam() == ResourceReferenceCase::PartialEnvironmentTwo)
                {
                    environment.prefilteredTexture = RenderResourceHandle{2, 1};
                }
                ASSERT_TRUE(builder.SetEnvironment(environment));
                break;
            }
        }

        EXPECT_FALSE(builder.Seal());
        EXPECT_EQ(builder.GetLastSealCode(), RenderFrameSealCode::InvalidResourceReference);
    }

    INSTANTIATE_TEST_SUITE_P(
        RequiredAndConditionalHandles,
        RenderContractsValidationResourceReferences,
        testing::Values(
            ResourceReferenceCase::ZeroPrimitiveObject,
            ResourceReferenceCase::InvalidPrimitiveMesh,
            ResourceReferenceCase::ZeroLightId,
            ResourceReferenceCase::InvalidLightType,
            ResourceReferenceCase::ShadowUnexpectedHandle,
            ResourceReferenceCase::PartialEnvironmentOne,
            ResourceReferenceCase::PartialEnvironmentTwo));

    TEST(RenderContractsValidation,
         AcceptsRenderOwnedShadowIntentWithoutExternalResource)
    {
        RenderFramePacketBuilder builder;
        PopulateCompletePacketBuilder(builder);
        RenderLightSnapshot light;
        light.lightId = 1;
        light.castsShadows = true;
        ASSERT_TRUE(builder.AddLight(light));

        auto header = MakeValidHeader();
        header.expectedLightCount = 1;
        header.extractedLightCount = 1;
        ASSERT_TRUE(builder.SetHeader(header));
        EXPECT_TRUE(builder.Seal());
    }

    TEST(RenderContractsValidation, AcceptsOptionalBindingsAndCompleteResourceTriples)
    {
        RenderFramePacketBuilder builder;
        PopulateCompletePacketBuilder(builder);
        RenderPrimitiveSnapshot primitive;
        primitive.objectId = 1;
        primitive.mesh = RenderResourceHandle{1, 1};
        primitive.material = {};
        primitive.fallbackMesh = {};
        primitive.fallbackMaterial = {};
        ASSERT_TRUE(builder.AddPrimitive(primitive));

        RenderLightSnapshot light;
        light.lightId = 1;
        light.castsShadows = true;
        ASSERT_TRUE(builder.AddLight(light));

        RenderEnvironmentSnapshot environment;
        environment.irradianceTexture = RenderResourceHandle{3, 1};
        environment.prefilteredTexture = RenderResourceHandle{4, 1};
        environment.brdfLutTexture = RenderResourceHandle{5, 1};
        ASSERT_TRUE(builder.SetEnvironment(environment));

        auto header = MakeValidHeader();
        header.expectedPrimitiveCount = 1;
        header.extractedPrimitiveCount = 1;
        header.expectedLightCount = 1;
        header.extractedLightCount = 1;
        ASSERT_TRUE(builder.SetHeader(header));
        EXPECT_TRUE(builder.Seal());
    }

    TEST(RenderContractsValidation,
         UsesSchemaFourOwnedCanonicalSubmeshMaterialBindings)
    {
        EXPECT_EQ(RVX_RENDER_FRAME_PACKET_SCHEMA_VERSION, 4U);

        RenderFramePacketBuilder builder;
        PopulateCompletePacketBuilder(builder);
        RenderPrimitiveSnapshot primitive;
        primitive.objectId = 1;
        primitive.mesh = RenderResourceHandle{1, 1};
        primitive.submeshes = {
            RenderSubmeshMaterialBinding{
                0, RenderResourceHandle{2, 4}, RenderMaterialMode::Masked},
            RenderSubmeshMaterialBinding{
                1, RenderResourceHandle{}, RenderMaterialMode::Transparent}};
        primitive.material = primitive.submeshes.front().material;
        ASSERT_TRUE(builder.AddPrimitive(std::move(primitive)));

        auto header = MakeValidHeader();
        header.expectedPrimitiveCount = 1;
        header.extractedPrimitiveCount = 1;
        ASSERT_TRUE(builder.SetHeader(header));
        const auto packet = builder.Seal();
        ASSERT_NE(packet, nullptr);
        ASSERT_EQ(packet->GetPrimitives()[0].submeshes.size(), 2U);
        EXPECT_EQ(packet->GetPrimitives()[0].submeshes[0].material,
                  (RenderResourceHandle{2, 4}));
        EXPECT_EQ(packet->GetPrimitives()[0].submeshes[1].materialMode,
                  RenderMaterialMode::Transparent);
    }

    TEST(RenderContractsValidation,
         RejectsMalformedSubmeshBindingsButAllowsMissingMaterial)
    {
        const auto verifyRejected = [](RenderSubmeshMaterialBinding binding)
        {
            RenderFramePacketBuilder builder;
            PopulateCompletePacketBuilder(builder);
            RenderPrimitiveSnapshot primitive;
            primitive.objectId = 1;
            primitive.mesh = RenderResourceHandle{1, 1};
            primitive.submeshes.push_back(binding);
            EXPECT_TRUE(builder.AddPrimitive(std::move(primitive)));
            auto header = MakeValidHeader();
            header.expectedPrimitiveCount = 1;
            header.extractedPrimitiveCount = 1;
            EXPECT_TRUE(builder.SetHeader(header));
            EXPECT_EQ(builder.Seal(), nullptr);
            EXPECT_EQ(builder.GetLastSealCode(),
                      RenderFrameSealCode::InvalidResourceReference);
        };

        verifyRejected(RenderSubmeshMaterialBinding{
            1, RenderResourceHandle{}, RenderMaterialMode::Opaque});
        verifyRejected(RenderSubmeshMaterialBinding{
            0, RenderResourceHandle{}, static_cast<RenderMaterialMode>(255)});
    }

    enum class CaptureRuleCase : uint8
    {
        NoneWithId,
        NoneWithWidth,
        NoneWithHeight,
        NoneWithAlpha,
        InvalidKind,
        ColorWithoutId,
        ColorWithoutWidth,
        ColorWithoutHeight
    };

    class RenderContractsValidationCaptureRequests : public testing::TestWithParam<CaptureRuleCase>
    {
    };

    TEST_P(RenderContractsValidationCaptureRequests, RejectsCaptureRules)
    {
        RenderFramePacketBuilder builder;
        PopulateCompletePacketBuilder(builder);
        RenderFrameCaptureRequest request;

        switch (GetParam())
        {
            case CaptureRuleCase::NoneWithId: request.requestId = 1; break;
            case CaptureRuleCase::NoneWithWidth: request.width = 1; break;
            case CaptureRuleCase::NoneWithHeight: request.height = 1; break;
            case CaptureRuleCase::NoneWithAlpha: request.includeAlpha = true; break;
            case CaptureRuleCase::InvalidKind:
                request.kind = static_cast<RenderFrameCaptureKind>(255);
                request.requestId = 1;
                request.width = 1;
                request.height = 1;
                break;
            case CaptureRuleCase::ColorWithoutId:
                request.kind = RenderFrameCaptureKind::Color;
                request.width = 1;
                request.height = 1;
                break;
            case CaptureRuleCase::ColorWithoutWidth:
                request.kind = RenderFrameCaptureKind::Color;
                request.requestId = 1;
                request.height = 1;
                break;
            case CaptureRuleCase::ColorWithoutHeight:
                request.kind = RenderFrameCaptureKind::Color;
                request.requestId = 1;
                request.width = 1;
                break;
        }
        ASSERT_TRUE(builder.SetCaptureRequest(request));

        EXPECT_FALSE(builder.Seal());
        EXPECT_EQ(builder.GetLastSealCode(), RenderFrameSealCode::InvalidCaptureRequest);
    }

    INSTANTIATE_TEST_SUITE_P(
        NoneAndDeclaredCaptureKinds,
        RenderContractsValidationCaptureRequests,
        testing::Values(
            CaptureRuleCase::NoneWithId,
            CaptureRuleCase::NoneWithWidth,
            CaptureRuleCase::NoneWithHeight,
            CaptureRuleCase::NoneWithAlpha,
            CaptureRuleCase::InvalidKind,
            CaptureRuleCase::ColorWithoutId,
            CaptureRuleCase::ColorWithoutWidth,
            CaptureRuleCase::ColorWithoutHeight));

    TEST(RenderContractsValidation, SuccessfulSealIsImmutable)
    {
        RenderFramePacketBuilder builder;
        PopulateCompletePacketBuilder(builder);
        auto header = MakeValidHeader();
        header.worldRevision = 7;
        header.temporalEpoch = 9;
        header.explicitDiscontinuity = true;
        ASSERT_TRUE(builder.SetHeader(header));
        auto view = MakeValidView();
        view.viewportX = 3;
        view.viewportY = 4;
        view.exposure = 2.0f;
        ASSERT_TRUE(builder.SetView(view));

        const std::unique_ptr<const RenderFramePacket> packet = builder.Seal();

        ASSERT_TRUE(packet);
        EXPECT_EQ(builder.GetState(), RenderFramePacketBuilderState::Sealed);
        EXPECT_EQ(builder.GetLastSealCode(), RenderFrameSealCode::Sealed);
        EXPECT_EQ(packet->GetHeader().schemaId, RVX_RENDER_FRAME_PACKET_SCHEMA_ID);
        EXPECT_EQ(packet->GetHeader().schemaVersion,
                  RVX_RENDER_FRAME_PACKET_SCHEMA_VERSION);
        EXPECT_EQ(packet->GetHeader().sequence, 1U);
        EXPECT_EQ(packet->GetHeader().worldRevision, 7U);
        EXPECT_EQ(packet->GetHeader().temporalEpoch, 9U);
        EXPECT_TRUE(packet->GetHeader().explicitDiscontinuity);
        EXPECT_EQ(packet->GetView().viewportX, 3U);
        EXPECT_EQ(packet->GetView().viewportY, 4U);
        EXPECT_FLOAT_EQ(packet->GetView().exposure, 2.0f);
        EXPECT_TRUE(packet->GetPrimitives().empty());
        EXPECT_TRUE(packet->GetLights().empty());
        EXPECT_FALSE(packet->GetSky().skyTexture.IsValid());
        EXPECT_FALSE(packet->GetEnvironment().irradianceTexture.IsValid());
        EXPECT_FLOAT_EQ(packet->GetSettings().renderScale, 1.0f);
        EXPECT_EQ(packet->GetCaptureRequest().kind, RenderFrameCaptureKind::None);
        EXPECT_TRUE(packet->GetFeatures().metadata.complete);
        EXPECT_TRUE(packet->GetExtractionDiagnostics().complete);
    }

    TEST(RenderContractsValidation, PostSealOperationsAreRejected)
    {
        RenderFramePacketBuilder builder;
        PopulateCompletePacketBuilder(builder);
        const auto packet = builder.Seal();
        ASSERT_TRUE(packet);

        EXPECT_FALSE(builder.SetHeader(RenderFrameHeader{}));
        EXPECT_FALSE(builder.SetView(RenderViewSnapshot{}));
        EXPECT_FALSE(builder.AddPrimitive(RenderPrimitiveSnapshot{}));
        EXPECT_FALSE(builder.AddLight(RenderLightSnapshot{}));
        EXPECT_FALSE(builder.SetSky(RenderSkySnapshot{}));
        EXPECT_FALSE(builder.SetEnvironment(RenderEnvironmentSnapshot{}));
        EXPECT_FALSE(builder.SetSettings(RenderFrameSettings{}));
        EXPECT_FALSE(builder.SetCaptureRequest(RenderFrameCaptureRequest{}));
        EXPECT_FALSE(builder.SetFeatures(RenderFeatureSnapshot{}));
        EXPECT_FALSE(builder.SetExtractionDiagnostics(RenderExtractionDiagnostics{}));
        EXPECT_FALSE(builder.Seal());
        EXPECT_EQ(builder.GetState(), RenderFramePacketBuilderState::Sealed);
        EXPECT_EQ(builder.GetLastSealCode(), RenderFrameSealCode::AlreadySealed);
        EXPECT_EQ(packet->GetHeader().sequence, 1U);
        EXPECT_EQ(packet->GetView().viewportWidth, 1U);
    }

    TEST(RenderContractsValidation, BuilderReturnsDocumentedFirstFailure)
    {
        RenderFramePacketBuilder builder;
        EXPECT_FALSE(builder.Seal());
        EXPECT_EQ(builder.GetLastSealCode(), RenderFrameSealCode::MissingValue);

        PopulateCompletePacketBuilder(builder);
        auto header = MakeValidHeader();
        header.schemaVersion = RVX_RENDER_FRAME_PACKET_SCHEMA_VERSION + 1;
        header.sequence = 0;
        ASSERT_TRUE(builder.SetHeader(header));
        auto view = MakeValidView();
        view.viewportWidth = 0;
        ASSERT_TRUE(builder.SetView(view));
        EXPECT_FALSE(builder.Seal());
        EXPECT_EQ(builder.GetLastSealCode(), RenderFrameSealCode::InvalidSchema);
    }
} // namespace
} // namespace RVX
