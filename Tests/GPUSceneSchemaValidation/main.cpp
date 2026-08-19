#include "Render/GPUDriven/GPUCulling.h"
#include "Render/GPUScene/GPUSceneSchema.h"
#include "Render/Submission/RasterInstanceStream.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    std::string ReadSource(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        EXPECT_TRUE(stream.is_open()) << path.string();
        return {std::istreambuf_iterator<char>(stream),
                std::istreambuf_iterator<char>()};
    }

    void ExpectTokensInOrder(
        std::string_view source,
        const std::vector<std::string_view>& tokens)
    {
        size_t position = 0;
        for (const std::string_view token : tokens)
        {
            position = source.find(token, position);
            ASSERT_NE(position, std::string_view::npos) << token;
            position += token.size();
        }
    }
} // namespace

TEST(GPUSceneSchemaValidation, CpuRowsHaveVersionedStableLayout)
{
    EXPECT_EQ(RVX::RVX_GPU_SCENE_SCHEMA_VERSION, 1u);
    EXPECT_EQ(sizeof(RVX::GPUSceneRowHeader), 32u);
    EXPECT_EQ(sizeof(RVX::GPUScenePrimitiveRow), 80u);
    EXPECT_EQ(sizeof(RVX::GPUSceneBoundsRow), 96u);
    EXPECT_EQ(sizeof(RVX::GPUSceneTransformRow), 192u);
    EXPECT_EQ(sizeof(RVX::GPUSceneMaterialRow), 96u);
    EXPECT_EQ(sizeof(RVX::GPUSceneGeometryRow), 80u);
    EXPECT_EQ(sizeof(RVX::GPUSceneDrawMetadataRow), 112u);
    EXPECT_EQ(alignof(RVX::GPUScenePrimitiveRow), 16u);
    EXPECT_EQ(offsetof(RVX::GPUScenePrimitiveRow, bounds), 32u);
    EXPECT_EQ(offsetof(RVX::GPUScenePrimitiveRow, sortKey), 72u);
    EXPECT_EQ(offsetof(RVX::GPUSceneTransformRow, normalFromLocal), 128u);
    EXPECT_EQ(offsetof(RVX::GPUSceneDrawMetadataRow, pipelineKey), 88u);
}

TEST(GPUSceneSchemaValidation, SubmissionAndCullingAbiStayStable)
{
    EXPECT_EQ(sizeof(RVX::GPUSceneCullingCandidate), 48u);
    EXPECT_EQ(offsetof(RVX::GPUSceneCullingCandidate, objectIdLow), 16u);
    EXPECT_EQ(offsetof(RVX::GPUSceneCullingCandidate, rasterInstanceIndex),
              36u);
    EXPECT_EQ(sizeof(RVX::GPUInstanceData), 224u);
    EXPECT_EQ(alignof(RVX::GPUInstanceData), 16u);
    EXPECT_EQ(offsetof(RVX::GPUInstanceData, meshId), 176u);
    EXPECT_EQ(offsetof(RVX::GPUInstanceData, forceVisible), 212u);
}

TEST(GPUSceneSchemaValidation, ShaderDeclarationsMirrorCpuFieldOrder)
{
    const std::filesystem::path sourceRoot(RVX_SOURCE_DIR);
    const std::string gpuScene = ReadSource(
        sourceRoot / "Render/Shaders/GPUDriven/GPUSceneCulling.hlsli");
    const std::string instance = ReadSource(
        sourceRoot / "Render/Shaders/Include/GPUInstanceData.hlsli");

    EXPECT_NE(gpuScene.find("#define RVX_GPU_SCENE_SCHEMA_VERSION 1u"),
              std::string::npos);
    ExpectTokensInOrder(
        gpuScene,
        {"struct GPUSceneCullingCandidate",
         "uint primitiveSlot;",
         "uint primitiveGeneration;",
         "uint drawSlot;",
         "uint drawGeneration;",
         "uint objectIdLow;",
         "uint objectIdHigh;",
         "uint requiredPassMask;",
         "uint drawGroupIndex;",
         "uint drawGroupVisibleOffset;",
         "uint rasterInstanceIndex;",
         "uint materialParameterSlot;",
         "uint padding0;"});
    ExpectTokensInOrder(
        gpuScene,
        {"struct GPUScenePrimitiveRow",
         "GPUSceneRowHeader header;",
         "uint2 bounds;",
         "uint2 transform;",
         "uint2 firstDraw;",
         "uint drawCount;",
         "uint primitiveFlags;",
         "uint layerMask;",
         "uint padding0;",
         "uint2 sortKey;"});
    ExpectTokensInOrder(
        gpuScene,
        {"struct GPUSceneDrawMetadataRow",
         "GPUSceneRowHeader header;",
         "uint2 primitive;",
         "uint2 material;",
         "uint2 geometry;",
         "uint indexCount;",
         "uint instanceCount;",
         "uint firstIndex;",
         "int vertexOffset;",
         "uint firstInstance;",
         "uint passMask;",
         "uint materialVariant;",
         "uint padding0;",
         "uint2 pipelineKey;",
         "uint2 sortKey;"});
    ExpectTokensInOrder(
        instance,
        {"struct GPUInstanceData",
         "float4x4 worldMatrix;",
         "float4x4 normalMatrix;",
         "float4 boundingSphere;",
         "float4 aabbMin;",
         "float4 aabbMax;",
         "uint meshId;",
         "uint materialId;",
         "uint indexCount;",
         "uint firstIndex;",
         "int vertexOffset;",
         "uint sourceIndex;",
         "uint drawGroupIndex;",
         "uint drawGroupVisibleOffset;",
         "uint candidateIndex;",
         "uint forceVisible;",
         "uint2 padding;"});
}

TEST(GPUSceneSchemaValidation, ExplicitPackingIsRoundTripStable)
{
    constexpr RVX::uint64 identity = 0xF0E1D2C3B4A59687ull;
    constexpr auto packedIdentity = RVX::PackGPUSceneUint64(identity);
    static_assert(RVX::UnpackGPUSceneUint64(packedIdentity) == identity);
    EXPECT_EQ(packedIdentity.low, 0xB4A59687u);
    EXPECT_EQ(packedIdentity.high, 0xF0E1D2C3u);

    RVX::GPUSceneMatrix4x4 matrix{};
    matrix.rows[0] = {1.0f, 2.0f, 3.0f, 4.0f};
    matrix.rows[1] = {5.0f, 6.0f, 7.0f, 8.0f};
    matrix.rows[2] = {9.0f, 10.0f, 11.0f, 12.0f};
    matrix.rows[3] = {13.0f, 14.0f, 15.0f, 16.0f};
    const auto expanded = RVX::UnpackGPUSceneAffineMatrix(
        RVX::PackGPUSceneAffineMatrix(matrix));
    EXPECT_EQ(expanded.rows[0], matrix.rows[0]);
    EXPECT_EQ(expanded.rows[1], matrix.rows[1]);
    EXPECT_EQ(expanded.rows[2], matrix.rows[2]);
    EXPECT_EQ(expanded.rows[3], (RVX::GPUSceneFloat4{0.0f, 0.0f, 0.0f, 1.0f}));
}
