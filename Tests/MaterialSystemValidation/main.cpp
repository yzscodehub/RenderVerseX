#include "Core/Core.h"
#include "Render/Material/MaterialClassification.h"
#include "Scene/Material.h"
#include "TestFramework/TestRunner.h"

#include <filesystem>
#include <fstream>
#include <memory>
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

    bool Test_MaterialClassificationMatchesAlphaMode()
    {
        auto opaque = std::make_shared<RVX::Material>();
        opaque->SetAlphaMode(RVX::Material::AlphaMode::Opaque);
        TEST_ASSERT_EQ(RVX::MaterialRenderMode::Opaque,
                       RVX::ClassifyMaterialRenderMode(opaque.get()));

        auto masked = std::make_shared<RVX::Material>();
        masked->SetAlphaMode(RVX::Material::AlphaMode::Mask);
        TEST_ASSERT_EQ(RVX::MaterialRenderMode::Masked,
                       RVX::ClassifyMaterialRenderMode(masked.get()));

        auto transparent = std::make_shared<RVX::Material>();
        transparent->SetAlphaMode(RVX::Material::AlphaMode::Blend);
        TEST_ASSERT_EQ(RVX::MaterialRenderMode::Transparent,
                       RVX::ClassifyMaterialRenderMode(transparent.get()));

        TEST_ASSERT_EQ(RVX::MaterialPipelineVariant::Opaque,
                       RVX::GetPipelineVariantForRenderMode(RVX::MaterialRenderMode::Opaque));
        TEST_ASSERT_EQ(RVX::MaterialPipelineVariant::Masked,
                       RVX::GetPipelineVariantForRenderMode(RVX::MaterialRenderMode::Masked));
        TEST_ASSERT_EQ(RVX::MaterialPipelineVariant::Transparent,
                       RVX::GetPipelineVariantForRenderMode(RVX::MaterialRenderMode::Transparent));

        return true;
    }

    bool Test_SceneRendererOwnsMaterialDrawLists()
    {
        const std::filesystem::path headerPath =
            std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Include" / "Render" / "Renderer" / "SceneRenderer.h";
        const std::filesystem::path sourcePath =
            std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "Renderer" / "SceneRenderer.cpp";

        const std::string header = LoadSourceFile(headerPath);
        const std::string source = LoadSourceFile(sourcePath);

        TEST_ASSERT_TRUE(!header.empty());
        TEST_ASSERT_TRUE(!source.empty());
        TEST_ASSERT_TRUE(header.find("std::vector<RenderDrawItem> m_opaqueDrawItems") != std::string::npos);
        TEST_ASSERT_TRUE(header.find("std::vector<RenderDrawItem> m_maskedDrawItems") != std::string::npos);
        TEST_ASSERT_TRUE(header.find("std::vector<RenderDrawItem> m_transparentDrawItems") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("BuildMaterialDrawLists") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("ClassifyMaterialRenderMode") != std::string::npos);

        return true;
    }

    bool Test_PipelineCacheCreatesMaterialPipelineVariants()
    {
        const std::filesystem::path headerPath =
            std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Include" / "Render" / "PipelineCache.h";
        const std::filesystem::path sourcePath =
            std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "PipelineCache.cpp";

        const std::string header = LoadSourceFile(headerPath);
        const std::string source = LoadSourceFile(sourcePath);

        TEST_ASSERT_TRUE(!header.empty());
        TEST_ASSERT_TRUE(!source.empty());
        TEST_ASSERT_TRUE(header.find("GetMaskedPipeline") != std::string::npos);
        TEST_ASSERT_TRUE(header.find("GetTransparentPipeline") != std::string::npos);
        TEST_ASSERT_TRUE(header.find("GetPipelineForVariant") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("DefaultMaskedPipeline") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("DefaultTransparentPipeline") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("RHIDepthStencilState::ReadOnly()") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("RHIRenderTargetBlendState::AlphaBlend()") != std::string::npos);

        return true;
    }

    bool Test_DefaultRendererWiresTransparentPass()
    {
        const std::string sceneRendererSource =
            LoadSourceFile(std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "Renderer" / "SceneRenderer.cpp");
        const std::string binderHeader =
            LoadSourceFile(std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "Renderer" / "RenderFrameResourceBinder.h");
        const std::string binderSource =
            LoadSourceFile(std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "Renderer" / "RenderFrameResourceBinder.cpp");
        const std::string transparentSource =
            LoadSourceFile(std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "Passes" / "TransparentPass.cpp");

        TEST_ASSERT_TRUE(!sceneRendererSource.empty());
        TEST_ASSERT_TRUE(!binderHeader.empty());
        TEST_ASSERT_TRUE(!binderSource.empty());
        TEST_ASSERT_TRUE(!transparentSource.empty());
        TEST_ASSERT_TRUE(sceneRendererSource.find("std::make_unique<TransparentPass>()") != std::string::npos);
        TEST_ASSERT_TRUE(sceneRendererSource.find("m_transparentPass = transparentPass.get()") != std::string::npos);
        TEST_ASSERT_TRUE(binderHeader.find("TransparentPass* transparentPass") != std::string::npos);
        TEST_ASSERT_TRUE(binderSource.find("transparentPass->SetRenderScene") != std::string::npos);
        TEST_ASSERT_TRUE(transparentSource.find("GetTransparentPipeline()") != std::string::npos);

        return true;
    }

    bool Test_NormalMappingUsesTangentSpace()
    {
        const std::string shader =
            LoadSourceFile(std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Shaders" / "DefaultLit.hlsl");
        const std::string pipeline =
            LoadSourceFile(std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "PipelineCache.cpp");
        const std::string opaquePass =
            LoadSourceFile(std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "Passes" / "OpaquePass.cpp");
        const std::string transparentPass =
            LoadSourceFile(std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Private" / "Passes" / "TransparentPass.cpp");

        TEST_ASSERT_TRUE(!shader.empty());
        TEST_ASSERT_TRUE(!pipeline.empty());
        TEST_ASSERT_TRUE(!opaquePass.empty());
        TEST_ASSERT_TRUE(!transparentPass.empty());
        TEST_ASSERT_TRUE(shader.find("float4 Tangent") != std::string::npos);
        TEST_ASSERT_TRUE(shader.find("WorldTangent") != std::string::npos);
        TEST_ASSERT_TRUE(shader.find("SampleNormalMap") != std::string::npos);
        TEST_ASSERT_TRUE(shader.find("NormalTexture.Sample") != std::string::npos);
        TEST_ASSERT_TRUE(pipeline.find("pipelineDesc.inputLayout.AddElement(\"TANGENT\", RHIFormat::RGBA32_FLOAT, 3)") !=
                         std::string::npos);
        TEST_ASSERT_TRUE(opaquePass.find("ctx.SetVertexBuffer(3, buffers.tangentBuffer)") != std::string::npos);
        TEST_ASSERT_TRUE(transparentPass.find("ctx.SetVertexBuffer(3, buffers.tangentBuffer)") != std::string::npos);

        return true;
    }

    bool Test_DefaultLitUsesCookTorranceBRDF()
    {
        const std::string shader =
            LoadSourceFile(std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Shaders" / "DefaultLit.hlsl");
        const std::string brdf =
            LoadSourceFile(std::filesystem::path(RVX_SOURCE_DIR) / "Render" / "Shaders" / "Include" / "BRDF.hlsli");

        TEST_ASSERT_TRUE(!shader.empty());
        TEST_ASSERT_TRUE(!brdf.empty());
        TEST_ASSERT_TRUE(brdf.find("D_GGX") != std::string::npos);
        TEST_ASSERT_TRUE(brdf.find("G_SmithGGXCorrelated") != std::string::npos);
        TEST_ASSERT_TRUE(brdf.find("F_Schlick") != std::string::npos);
        TEST_ASSERT_TRUE(shader.find("#include \"Include/BRDF.hlsli\"") != std::string::npos);
        TEST_ASSERT_TRUE(shader.find("EvaluatePBR(") != std::string::npos);
        TEST_ASSERT_TRUE(shader.find("ComputeF0") != std::string::npos);
        TEST_ASSERT_TRUE(shader.find("shininess = lerp") == std::string::npos);

        return true;
    }
} // namespace

int main()
{
    RVX::Log::Initialize();
    RVX_CORE_INFO("Material System Validation Tests");

    TestSuite suite;
    suite.AddTest("MaterialClassificationMatchesAlphaMode",
                  Test_MaterialClassificationMatchesAlphaMode);
    suite.AddTest("SceneRendererOwnsMaterialDrawLists",
                  Test_SceneRendererOwnsMaterialDrawLists);
    suite.AddTest("PipelineCacheCreatesMaterialPipelineVariants",
                  Test_PipelineCacheCreatesMaterialPipelineVariants);
    suite.AddTest("DefaultRendererWiresTransparentPass",
                  Test_DefaultRendererWiresTransparentPass);
    suite.AddTest("NormalMappingUsesTangentSpace",
                  Test_NormalMappingUsesTangentSpace);
    suite.AddTest("DefaultLitUsesCookTorranceBRDF",
                  Test_DefaultLitUsesCookTorranceBRDF);

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
