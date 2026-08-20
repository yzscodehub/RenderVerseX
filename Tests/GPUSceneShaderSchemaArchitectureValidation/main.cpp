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

TEST(GPUSceneSchemaValidation, ShaderDeclarationsMirrorCpuFieldOrder)
{
    const std::filesystem::path sourceRoot(RVX_SOURCE_DIR);
    const std::string gpuScene = ReadSource(
        sourceRoot / "Source/Runtime/Render/Shaders/GPUDriven/GPUSceneCulling.hlsli");
    const std::string instance = ReadSource(
        sourceRoot / "Source/Runtime/Render/Shaders/Include/GPUInstanceData.hlsli");

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
