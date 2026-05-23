#include "Core/Core.h"
#include "TestFramework/TestRunner.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// =============================================================================
// SOURCE-PATTERN LINT — NOT A BEHAVIORAL TEST.
// These checks grep the backend .cpp text for expected substrings. They prove
// a pattern is *present in source*, not that the runtime behavior is correct,
// and they go stale silently when a refactor preserves behavior but renames a
// symbol. CTest label: "lint" (see Tests/CMakeLists.txt). Converting these into
// real device/behavioral tests is tracked as a Phase 2 follow-on plan.
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

    bool Test_DX12CommandContextUsesAllocatorPool()
    {
        const std::filesystem::path sourcePath =
            std::filesystem::path(RVX_SOURCE_DIR) / "RHI_DX12" / "Private" / "DX12CommandContext.cpp";
        const std::string source = LoadSourceFile(sourcePath);

        TEST_ASSERT_TRUE(!source.empty());
        TEST_ASSERT_TRUE(source.find("GetAllocatorPool().Acquire") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("GetAllocatorPool().Release") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("CreateCommandAllocator") == std::string::npos);

        return true;
    }

    bool Test_DX12MultiSubmitRejectsMixedQueues()
    {
        const std::filesystem::path sourcePath =
            std::filesystem::path(RVX_SOURCE_DIR) / "RHI_DX12" / "Private" / "DX12CommandContext.cpp";
        const std::string source = LoadSourceFile(sourcePath);

        TEST_ASSERT_TRUE(!source.empty());
        TEST_ASSERT_TRUE(source.find("contexts.front()") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("GetQueueType() != queueType") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("requires all contexts to use the same command queue type") != std::string::npos);

        return true;
    }

    bool Test_DX12AllocatorPoolOwnsQueueFenceRecycling()
    {
        const std::filesystem::path sourcePath =
            std::filesystem::path(RVX_SOURCE_DIR) / "RHI_DX12" / "Private" / "DX12CommandAllocatorPool.cpp";
        const std::string source = LoadSourceFile(sourcePath);

        TEST_ASSERT_TRUE(!source.empty());
        TEST_ASSERT_TRUE(source.find("CreateFence") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("queue->Signal") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("return;") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("GetCompletedValue") != std::string::npos);

        return true;
    }

    bool Test_DX12SkipsOversizedConstantBufferDescriptors()
    {
        const std::filesystem::path sourcePath =
            std::filesystem::path(RVX_SOURCE_DIR) / "RHI_DX12" / "Private" / "DX12Resources.cpp";
        const std::string source = LoadSourceFile(sourcePath);

        TEST_ASSERT_TRUE(!source.empty());
        TEST_ASSERT_TRUE(source.find("RVX_DX12_MAX_CBV_SIZE") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("m_desc.size <= RVX_DX12_MAX_CBV_SIZE") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("Skipping CBV descriptor") != std::string::npos);

        return true;
    }
} // namespace

int main()
{
    RVX::Log::Initialize();
    RVX_CORE_INFO("RHI DX12 Source Validation Tests");

    TestSuite suite;
    suite.AddTest("DX12CommandContextUsesAllocatorPool", Test_DX12CommandContextUsesAllocatorPool);
    suite.AddTest("DX12MultiSubmitRejectsMixedQueues", Test_DX12MultiSubmitRejectsMixedQueues);
    suite.AddTest("DX12AllocatorPoolOwnsQueueFenceRecycling", Test_DX12AllocatorPoolOwnsQueueFenceRecycling);
    suite.AddTest("DX12SkipsOversizedConstantBufferDescriptors", Test_DX12SkipsOversizedConstantBufferDescriptors);

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
