#include "Core/Core.h"
#include "Render/PipelineCache.h"
#include "TestFramework/TestRunner.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// =============================================================================
// SOURCE-PATTERN LINT - NOT A BEHAVIORAL TEST.
// These checks grep source files for expected binding patterns. They prove that
// strings are present in source, not that rendering behavior is correct.
// CTest label: "lint".
// =============================================================================

using namespace RVX::Test;

namespace
{
    std::string LoadSourceFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return {};

        std::ostringstream stream;
        stream << file.rdbuf();
        return stream.str();
    }

    bool Test_TransparentPassUsesSplitDescriptorSets()
    {
        const std::filesystem::path sourcePath =
            std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "Passes" / "TransparentPass.cpp";
        const std::string source = LoadSourceFile(sourcePath);

        TEST_ASSERT_TRUE(!source.empty());
        TEST_ASSERT_TRUE(source.find("m_pipelineCache->GetMaterialDescriptorSet") == std::string::npos);
        TEST_ASSERT_TRUE(source.find("m_pipelineCache->GetCurrentConstantDynamicOffsets") == std::string::npos);

        const size_t updateObjectPos = source.find("m_pipelineCache->UpdateObjectConstants");
        const size_t objectSetPos = source.find("m_pipelineCache->GetObjectDescriptorSet", updateObjectPos);
        const size_t objectOffsetPos = source.find("m_pipelineCache->GetCurrentObjectDynamicOffset", objectSetPos);
        const size_t objectBindPos = source.find("ctx.SetDescriptorSet(1, objectSet, objectDynamicOffsets)", objectOffsetPos);
        const size_t materialUpdatePos = source.find("m_materialSystem->UpdateMaterialConstants", objectBindPos);
        const size_t materialSetPos = source.find("m_materialSystem->GetOrCreateMaterialSet", materialUpdatePos);
        const size_t materialOffsetPos = source.find("m_materialSystem->GetCurrentMaterialDynamicOffset", materialSetPos);
        const size_t materialBindPos = source.find("ctx.SetDescriptorSet(2, materialSet, materialDynamicOffsets)", materialOffsetPos);
        const size_t drawPos = source.find("ctx.DrawIndexed", materialBindPos);

        TEST_ASSERT_TRUE(updateObjectPos != std::string::npos);
        TEST_ASSERT_TRUE(objectSetPos != std::string::npos);
        TEST_ASSERT_TRUE(objectOffsetPos != std::string::npos);
        TEST_ASSERT_TRUE(objectBindPos != std::string::npos);
        TEST_ASSERT_TRUE(materialUpdatePos != std::string::npos);
        TEST_ASSERT_TRUE(materialSetPos != std::string::npos);
        TEST_ASSERT_TRUE(materialOffsetPos != std::string::npos);
        TEST_ASSERT_TRUE(materialBindPos != std::string::npos);
        TEST_ASSERT_TRUE(drawPos != std::string::npos);
        TEST_ASSERT_TRUE(updateObjectPos < objectSetPos);
        TEST_ASSERT_TRUE(objectSetPos < objectOffsetPos);
        TEST_ASSERT_TRUE(objectOffsetPos < objectBindPos);
        TEST_ASSERT_TRUE(objectBindPos < materialUpdatePos);
        TEST_ASSERT_TRUE(materialUpdatePos < materialSetPos);
        TEST_ASSERT_TRUE(materialSetPos < materialOffsetPos);
        TEST_ASSERT_TRUE(materialOffsetPos < materialBindPos);
        TEST_ASSERT_TRUE(materialBindPos < drawPos);

        return true;
    }

    bool Test_PipelineCacheUsesSplitDescriptorSetLayouts()
    {
        const std::filesystem::path sourcePath =
            std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "PipelineCache.cpp";
        const std::filesystem::path headerPath =
            std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Include" / "Render" / "PipelineCache.h";

        const std::string source = LoadSourceFile(sourcePath);
        const std::string header = LoadSourceFile(headerPath);

        TEST_ASSERT_TRUE(!source.empty());
        TEST_ASSERT_TRUE(!header.empty());
        TEST_ASSERT_TRUE(header.find("GetFrameDescriptorSet") != std::string::npos);
        TEST_ASSERT_TRUE(header.find("GetObjectDescriptorSet") != std::string::npos);
        TEST_ASSERT_TRUE(header.find("GetMaterialSetLayout") != std::string::npos);
        TEST_ASSERT_TRUE(header.find("GetMaterialDescriptorSet") == std::string::npos);
        TEST_ASSERT_TRUE(header.find("m_materialDescriptorSets") == std::string::npos);
        TEST_ASSERT_TRUE(header.find("m_materialConstantBuffer") == std::string::npos);
        TEST_ASSERT_TRUE(header.find("GetCurrentConstantDynamicOffsets") == std::string::npos);
        TEST_ASSERT_TRUE(source.find("CreateFrameDescriptorSet") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("CreateObjectDescriptorSet") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("m_setLayouts.resize(3)") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("m_setLayouts[0] = m_device->CreateDescriptorSetLayout(frameLayoutDesc)") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("m_setLayouts[1] = m_device->CreateDescriptorSetLayout(objectLayoutDesc)") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("m_setLayouts[2] = m_device->CreateDescriptorSetLayout(materialLayoutDesc)") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("objectEntry.type = RHIBindingType::DynamicUniformBuffer") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("materialEntry.type = RHIBindingType::DynamicUniformBuffer") != std::string::npos);

        return true;
    }

    bool Test_OpaquePassUsesSplitDescriptorSets()
    {
        const std::filesystem::path sourcePath =
            std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "Passes" / "OpaquePass.cpp";
        const std::string source = LoadSourceFile(sourcePath);

        TEST_ASSERT_TRUE(!source.empty());
        TEST_ASSERT_TRUE(source.find("m_pipelineCache->GetMaterialDescriptorSet") == std::string::npos);
        TEST_ASSERT_TRUE(source.find("m_pipelineCache->GetCurrentConstantDynamicOffsets") == std::string::npos);

        const size_t frameSetPos = source.find("m_pipelineCache->GetFrameDescriptorSet");
        const size_t frameBindPos = source.find("ctx.SetDescriptorSet(0, frameSet)", frameSetPos);
        const size_t updateObjectPos = source.find("m_pipelineCache->UpdateObjectConstants", frameBindPos);
        const size_t objectSetPos = source.find("m_pipelineCache->GetObjectDescriptorSet", updateObjectPos);
        const size_t objectOffsetPos = source.find("m_pipelineCache->GetCurrentObjectDynamicOffset", objectSetPos);
        const size_t objectBindPos = source.find("ctx.SetDescriptorSet(1, objectSet, objectDynamicOffsets)", objectOffsetPos);
        const size_t materialUpdatePos = source.find("m_materialSystem->UpdateMaterialConstants", objectBindPos);
        const size_t materialSetPos = source.find("m_materialSystem->GetOrCreateMaterialSet", materialUpdatePos);
        const size_t materialOffsetPos = source.find("m_materialSystem->GetCurrentMaterialDynamicOffset", materialSetPos);
        const size_t materialBindPos = source.find("ctx.SetDescriptorSet(2, materialSet, materialDynamicOffsets)", materialOffsetPos);

        TEST_ASSERT_TRUE(frameSetPos != std::string::npos);
        TEST_ASSERT_TRUE(frameBindPos != std::string::npos);
        TEST_ASSERT_TRUE(updateObjectPos != std::string::npos);
        TEST_ASSERT_TRUE(objectSetPos != std::string::npos);
        TEST_ASSERT_TRUE(objectOffsetPos != std::string::npos);
        TEST_ASSERT_TRUE(objectBindPos != std::string::npos);
        TEST_ASSERT_TRUE(materialUpdatePos != std::string::npos);
        TEST_ASSERT_TRUE(materialSetPos != std::string::npos);
        TEST_ASSERT_TRUE(materialOffsetPos != std::string::npos);
        TEST_ASSERT_TRUE(materialBindPos != std::string::npos);
        TEST_ASSERT_TRUE(frameSetPos < frameBindPos);
        TEST_ASSERT_TRUE(frameBindPos < updateObjectPos);
        TEST_ASSERT_TRUE(updateObjectPos < objectSetPos);
        TEST_ASSERT_TRUE(objectSetPos < objectOffsetPos);
        TEST_ASSERT_TRUE(objectOffsetPos < objectBindPos);
        TEST_ASSERT_TRUE(objectBindPos < materialUpdatePos);
        TEST_ASSERT_TRUE(materialUpdatePos < materialSetPos);
        TEST_ASSERT_TRUE(materialSetPos < materialOffsetPos);
        TEST_ASSERT_TRUE(materialOffsetPos < materialBindPos);

        return true;
    }

    bool Test_DefaultLitUsesDescriptorSetSpaces()
    {
        const std::filesystem::path sourcePath =
            std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Shaders" / "DefaultLit.hlsl";
        const std::string source = LoadSourceFile(sourcePath);

        TEST_ASSERT_TRUE(!source.empty());
        TEST_ASSERT_TRUE(source.find("cbuffer ViewConstants : register(b0, space0)") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("cbuffer ObjectConstants : register(b0, space1)") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("cbuffer MaterialConstants : register(b0, space2)") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("Texture2D BaseColorTexture : register(t1, space2)") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("Texture2D NormalTexture : register(t2, space2)") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("Texture2D MetallicRoughnessTexture : register(t3, space2)") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("Texture2D OcclusionTexture : register(t4, space2)") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("Texture2D EmissiveTexture : register(t5, space2)") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("SamplerState MaterialSampler : register(s6, space2)") != std::string::npos);

        return true;
    }

    bool Test_DynamicConstantSlotsDoNotWrapWithinFrame()
    {
        const std::filesystem::path pipelineSourcePath =
            std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "PipelineCache.cpp";
        const std::filesystem::path materialSourcePath =
            std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "Material" / "MaterialSystem.cpp";

        const std::string pipelineSource = LoadSourceFile(pipelineSourcePath);
        const std::string materialSource = LoadSourceFile(materialSourcePath);

        TEST_ASSERT_TRUE(!pipelineSource.empty());
        TEST_ASSERT_TRUE(!materialSource.empty());
        TEST_ASSERT_TRUE(pipelineSource.find("% RVX_MAX_DRAW_CONSTANTS_PER_FRAME") == std::string::npos);
        TEST_ASSERT_TRUE(materialSource.find("% RVX_MAX_MATERIAL_CONSTANTS_PER_FRAME") == std::string::npos);
        TEST_ASSERT_TRUE(pipelineSource.find("Object constant buffer exhausted") != std::string::npos);
        TEST_ASSERT_TRUE(materialSource.find("Material constant buffer exhausted") != std::string::npos);

        return true;
    }
} // namespace

int main()
{
    RVX::Log::Initialize();
    RVX_CORE_INFO("Render Pass Binding Validation Tests");

    TestSuite suite;
    suite.AddTest("TransparentPassUsesSplitDescriptorSets",
                  Test_TransparentPassUsesSplitDescriptorSets);
    suite.AddTest("PipelineCacheUsesSplitDescriptorSetLayouts",
                  Test_PipelineCacheUsesSplitDescriptorSetLayouts);
    suite.AddTest("OpaquePassUsesSplitDescriptorSets",
                  Test_OpaquePassUsesSplitDescriptorSets);
    suite.AddTest("DefaultLitUsesDescriptorSetSpaces",
                  Test_DefaultLitUsesDescriptorSetSpaces);
    suite.AddTest("DynamicConstantSlotsDoNotWrapWithinFrame",
                  Test_DynamicConstantSlotsDoNotWrapWithinFrame);

    auto results = suite.Run();
    suite.PrintResults(results);

    RVX::Log::Shutdown();

    for (const auto& result : results)
    {
        if (!result.passed)
            return 1;
    }

    return 0;
}
