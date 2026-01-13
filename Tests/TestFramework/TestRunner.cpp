#include "TestFramework/TestRunner.h"
#include <chrono>

namespace RVX::Test
{
    std::vector<TestResult> TestSuite::Run()
    {
        std::vector<TestResult> results;

        for (const auto& test : m_tests)
        {
            TestResult result;
            result.name = test.name;

            RVX_CORE_INFO("Running test: {}", test.name);

            auto start = std::chrono::high_resolution_clock::now();

            try
            {
                result.passed = test.func();
                result.message = result.passed ? "PASSED" : "FAILED";
            }
            catch (const std::exception& e)
            {
                result.passed = false;
                result.message = std::string("EXCEPTION: ") + e.what();
            }
            catch (...)
            {
                result.passed = false;
                result.message = "UNKNOWN EXCEPTION";
            }

            auto end = std::chrono::high_resolution_clock::now();
            result.durationMs = std::chrono::duration<double, std::milli>(end - start).count();

            if (result.passed)
                RVX_CORE_INFO("  [PASSED] {} ({:.2f}ms)", test.name, result.durationMs);
            else
                RVX_CORE_ERROR("  [FAILED] {} - {} ({:.2f}ms)", test.name, result.message, result.durationMs);

            results.push_back(result);
        }

        return results;
    }

    void TestSuite::PrintResults(const std::vector<TestResult>& results)
    {
        uint32 passed = 0;
        uint32 failed = 0;

        for (const auto& result : results)
        {
            if (result.passed)
                passed++;
            else
                failed++;
        }

        RVX_CORE_INFO("");
        RVX_CORE_INFO("=== Test Results ===");
        RVX_CORE_INFO("  Passed: {}", passed);
        RVX_CORE_INFO("  Failed: {}", failed);
        RVX_CORE_INFO("  Total:  {}", results.size());

        if (failed > 0)
        {
            RVX_CORE_ERROR("");
            RVX_CORE_ERROR("Failed tests:");
            for (const auto& result : results)
            {
                if (!result.passed)
                    RVX_CORE_ERROR("  - {}: {}", result.name, result.message);
            }
        }
    }

} // namespace RVX::Test
