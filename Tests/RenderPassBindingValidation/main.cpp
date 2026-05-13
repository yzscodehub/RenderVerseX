#include "Core/Core.h"
#include "Render/PipelineCache.h"
#include "TestFramework/TestRunner.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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

    bool Test_TransparentPassRebindsDescriptorAfterObjectConstants()
    {
        const std::filesystem::path sourcePath =
            std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "Passes" / "TransparentPass.cpp";
        const std::string source = LoadSourceFile(sourcePath);

        TEST_ASSERT_TRUE(!source.empty());

        const size_t updateObjectPos = source.find("m_pipelineCache->UpdateObjectConstants");
        const size_t updateMaterialPos = source.find("m_pipelineCache->UpdateMaterialConstants", updateObjectPos);
        const size_t descriptorPos = source.find("m_pipelineCache->GetMaterialDescriptorSet", updateObjectPos);
        const size_t offsetsPos = source.find("m_pipelineCache->GetCurrentConstantDynamicOffsets", descriptorPos);
        const size_t bindPos = source.find("ctx.SetDescriptorSet(0, materialSet, dynamicOffsets)", offsetsPos);
        const size_t drawPos = source.find("ctx.DrawIndexed", bindPos);

        TEST_ASSERT_TRUE(updateObjectPos != std::string::npos);
        TEST_ASSERT_TRUE(updateMaterialPos != std::string::npos);
        TEST_ASSERT_TRUE(descriptorPos != std::string::npos);
        TEST_ASSERT_TRUE(offsetsPos != std::string::npos);
        TEST_ASSERT_TRUE(bindPos != std::string::npos);
        TEST_ASSERT_TRUE(drawPos != std::string::npos);
        TEST_ASSERT_TRUE(updateObjectPos < updateMaterialPos);
        TEST_ASSERT_TRUE(updateMaterialPos < descriptorPos);
        TEST_ASSERT_TRUE(descriptorPos < offsetsPos);
        TEST_ASSERT_TRUE(offsetsPos < bindPos);
        TEST_ASSERT_TRUE(bindPos < drawPos);

        return true;
    }

    bool Test_PipelineCacheUsesDynamicConstantOffsets()
    {
        const std::filesystem::path sourcePath =
            std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "PipelineCache.cpp";
        const std::filesystem::path headerPath =
            std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Include" / "Render" / "PipelineCache.h";

        const std::string source = LoadSourceFile(sourcePath);
        const std::string header = LoadSourceFile(headerPath);

        TEST_ASSERT_TRUE(!source.empty());
        TEST_ASSERT_TRUE(!header.empty());
        TEST_ASSERT_TRUE(source.find("entry.type = RHIBindingType::DynamicUniformBuffer") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("entry.isDynamic = true") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("ClearMaterialDescriptorSets();\n}") == std::string::npos);
        TEST_ASSERT_TRUE(source.find("GetMaterialDescriptorSet(RHITextureView* baseColorView, uint64 textureViewGeneration)") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("descSetDesc.BindBuffer(1, m_objectConstantBuffer.Get(), 0, m_objectConstantStride)") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("descSetDesc.BindBuffer(4, m_materialConstantBuffer.Get(), 0, m_materialConstantStride)") != std::string::npos);

        const size_t keyStart = header.find("struct MaterialDescriptorKey");
        const size_t keyEnd = header.find("struct MaterialDescriptorKeyHash", keyStart);
        TEST_ASSERT_TRUE(keyStart != std::string::npos);
        TEST_ASSERT_TRUE(keyEnd != std::string::npos);
        const std::string keyBlock = header.substr(keyStart, keyEnd - keyStart);
        TEST_ASSERT_TRUE(keyBlock.find("objectOffset") == std::string::npos);
        TEST_ASSERT_TRUE(keyBlock.find("materialOffset") == std::string::npos);

        TEST_ASSERT_TRUE(header.find("m_materialDescriptorCacheGeneration") != std::string::npos);
        TEST_ASSERT_TRUE(header.find("GetCurrentConstantDynamicOffsets") != std::string::npos);

        return true;
    }

    bool Test_OpaquePassSynchronizesMaterialDescriptorGeneration()
    {
        const std::filesystem::path sourcePath =
            std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "Passes" / "OpaquePass.cpp";
        const std::string source = LoadSourceFile(sourcePath);

        TEST_ASSERT_TRUE(!source.empty());
        TEST_ASSERT_TRUE(source.find("m_pipelineCache->ClearMaterialDescriptorSets") == std::string::npos);

        const size_t baseColorPos = source.find("ResolveBaseColorTextureView");
        const size_t descriptorPos = source.find("m_pipelineCache->GetMaterialDescriptorSet(baseColorView, view.viewCache->GetGeneration())",
                                                 baseColorPos);
        const size_t fallbackDescriptorPos = source.find("m_pipelineCache->GetMaterialDescriptorSet(baseColorView)",
                                                         descriptorPos);
        const size_t offsetsPos = source.find("m_pipelineCache->GetCurrentConstantDynamicOffsets", descriptorPos);
        const size_t bindPos = source.find("ctx.SetDescriptorSet(0, materialSet, dynamicOffsets)", offsetsPos);

        TEST_ASSERT_TRUE(baseColorPos != std::string::npos);
        TEST_ASSERT_TRUE(descriptorPos != std::string::npos);
        TEST_ASSERT_TRUE(fallbackDescriptorPos != std::string::npos);
        TEST_ASSERT_TRUE(offsetsPos != std::string::npos);
        TEST_ASSERT_TRUE(bindPos != std::string::npos);
        TEST_ASSERT_TRUE(baseColorPos < descriptorPos);
        TEST_ASSERT_TRUE(descriptorPos < offsetsPos);
        TEST_ASSERT_TRUE(offsetsPos < bindPos);

        return true;
    }

    bool Test_PipelineCacheDynamicOffsetsRuntime()
    {
        auto offsets = RVX::PipelineCache::BuildConstantDynamicOffsets(256, 512);
        TEST_ASSERT_EQ(offsets[0], 256u);
        TEST_ASSERT_EQ(offsets[1], 512u);

        return true;
    }
} // namespace

int main()
{
    RVX::Log::Initialize();
    RVX_CORE_INFO("Render Pass Binding Validation Tests");

    TestSuite suite;
    suite.AddTest("TransparentPassRebindsDescriptorAfterObjectConstants",
                  Test_TransparentPassRebindsDescriptorAfterObjectConstants);
    suite.AddTest("PipelineCacheUsesDynamicConstantOffsets",
                  Test_PipelineCacheUsesDynamicConstantOffsets);
    suite.AddTest("PipelineCacheDynamicOffsetsRuntime",
                  Test_PipelineCacheDynamicOffsetsRuntime);
    suite.AddTest("OpaquePassSynchronizesMaterialDescriptorGeneration",
                  Test_OpaquePassSynchronizesMaterialDescriptorGeneration);

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
