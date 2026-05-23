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

    bool Test_VulkanCrossQueueSemaphoresAreDeferredDestroyed()
    {
        const std::filesystem::path sourcePath =
            std::filesystem::path(RVX_SOURCE_DIR) / "RHI_Vulkan" / "Private" / "VulkanCommandContext.cpp";
        const std::string source = LoadSourceFile(sourcePath);

        TEST_ASSERT_TRUE(!source.empty());
        TEST_ASSERT_TRUE(source.find("EnqueueDeferredSemaphoreDestroy") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("device->WaitIdle()") == std::string::npos);
        TEST_ASSERT_TRUE(source.find("needCopyToGraphicsSync = !copyCmdBuffers.empty() && !graphicsCmdBuffers.empty() && computeCmdBuffers.empty()") != std::string::npos);

        return true;
    }

    bool Test_VulkanDeferredSemaphoreDestroyUsesFence()
    {
        const std::filesystem::path sourcePath =
            std::filesystem::path(RVX_SOURCE_DIR) / "RHI_Vulkan" / "Private" / "VulkanDevice.cpp";
        const std::string source = LoadSourceFile(sourcePath);

        TEST_ASSERT_TRUE(!source.empty());
        TEST_ASSERT_TRUE(source.find("vkCreateFence") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("vkQueueSubmit(signalQueue") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("vkGetFenceStatus") != std::string::npos);
        TEST_ASSERT_TRUE(source.find("ProcessDeferredSemaphoreDestroys(true)") != std::string::npos);

        return true;
    }

    bool Test_VulkanFenceSignalValuesAreSubmissionMonotonic()
    {
        const std::filesystem::path contextPath =
            std::filesystem::path(RVX_SOURCE_DIR) / "RHI_Vulkan" / "Private" / "VulkanCommandContext.cpp";
        const std::filesystem::path resourcePath =
            std::filesystem::path(RVX_SOURCE_DIR) / "RHI_Vulkan" / "Private" / "VulkanResources.cpp";
        const std::filesystem::path resourceHeaderPath =
            std::filesystem::path(RVX_SOURCE_DIR) / "RHI_Vulkan" / "Private" / "VulkanResources.h";

        const std::string contextSource = LoadSourceFile(contextPath);
        const std::string resourceSource = LoadSourceFile(resourcePath);
        const std::string resourceHeader = LoadSourceFile(resourceHeaderPath);

        TEST_ASSERT_TRUE(!contextSource.empty());
        TEST_ASSERT_TRUE(!resourceSource.empty());
        TEST_ASSERT_TRUE(!resourceHeader.empty());
        TEST_ASSERT_TRUE(contextSource.find("GetCompletedValue() + 1") == std::string::npos);
        TEST_ASSERT_TRUE(contextSource.find("AllocateSignalValue()") != std::string::npos);
        TEST_ASSERT_TRUE(resourceHeader.find("std::atomic<uint64> m_nextSignalValue") != std::string::npos);
        TEST_ASSERT_TRUE(resourceSource.find("fetch_add(1") != std::string::npos);
        TEST_ASSERT_TRUE(resourceSource.find("TrackSubmittedValue(value)") != std::string::npos);

        return true;
    }
} // namespace

int main()
{
    RVX::Log::Initialize();
    RVX_CORE_INFO("RHI Vulkan Source Validation Tests");

    TestSuite suite;
    suite.AddTest("VulkanCrossQueueSemaphoresAreDeferredDestroyed",
                  Test_VulkanCrossQueueSemaphoresAreDeferredDestroyed);
    suite.AddTest("VulkanDeferredSemaphoreDestroyUsesFence",
                  Test_VulkanDeferredSemaphoreDestroyUsesFence);
    suite.AddTest("VulkanFenceSignalValuesAreSubmissionMonotonic",
                  Test_VulkanFenceSignalValuesAreSubmissionMonotonic);

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
