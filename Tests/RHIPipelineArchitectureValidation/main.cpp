#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace RVX::Tests
{
    namespace
    {
        std::string ReadTextFile(
            const std::filesystem::path& path)
        {
            std::ifstream file(path);
            std::ostringstream stream;
            stream << file.rdbuf();
            return stream.str();
        }
    } // namespace

    TEST(RHIPipelineValidation,
         PrimaryBackendsConsumeSharedVertexTranslation)
    {
        const std::filesystem::path sourceRoot = RVX_SOURCE_DIR;
        const std::array<std::filesystem::path, 3> backendSources = {
            sourceRoot / "RHI_DX12" / "Private" / "DX12Pipeline.cpp",
            sourceRoot / "RHI_Vulkan" / "Private" /
                "VulkanPipeline.cpp",
            sourceRoot / "RHI_Metal" / "Private" / "MetalPipeline.mm",
        };

        for (const std::filesystem::path& path : backendSources)
        {
            const std::string source = ReadTextFile(path);
            ASSERT_FALSE(source.empty()) << path.string();
            EXPECT_NE(
                source.find("BuildRHIVertexInputTranslation"),
                std::string::npos)
                << path.string();
        }
    }

    TEST(RHIPipelineValidation,
         MetalVertexStreamsUseLegalDescendingBufferIndices)
    {
        const std::filesystem::path sourceRoot = RVX_SOURCE_DIR;
        const std::string commonSource = ReadTextFile(
            sourceRoot / "RHI_Metal" / "Private" / "MetalCommon.h");
        const std::string pipelineSource = ReadTextFile(
            sourceRoot / "RHI_Metal" / "Private" / "MetalPipeline.mm");
        const std::string commandSource = ReadTextFile(
            sourceRoot / "RHI_Metal" / "Private" /
                "MetalCommandContext.mm");

        ASSERT_FALSE(commonSource.empty());
        ASSERT_FALSE(pipelineSource.empty());
        ASSERT_FALSE(commandSource.empty());
        EXPECT_NE(commonSource.find("kMetalHighestBufferIndex = 30"),
                  std::string::npos);
        EXPECT_NE(commonSource.find(
                      "kMetalHighestBufferIndex - inputSlot"),
                  std::string::npos);
        EXPECT_NE(pipelineSource.find(
                      "MetalVertexBufferIndex(attribute.inputSlot)"),
                  std::string::npos);
        EXPECT_NE(pipelineSource.find(
                      "MetalVertexBufferIndex(binding.inputSlot)"),
                  std::string::npos);
        EXPECT_NE(commandSource.find("MetalVertexBufferIndex(slot)"),
                  std::string::npos);
        EXPECT_EQ(pipelineSource.find("30 + attribute.inputSlot"),
                  std::string::npos);
        EXPECT_EQ(commandSource.find("30 + slot"), std::string::npos);
    }
} // namespace RVX::Tests
