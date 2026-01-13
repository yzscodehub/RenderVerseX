#pragma once

#include "Core/Core.h"
#include <functional>
#include <vector>
#include <string>

namespace RVX::Test
{
    // =============================================================================
    // Test Result
    // =============================================================================
    struct TestResult
    {
        std::string name;
        bool passed = false;
        std::string message;
        double durationMs = 0.0;
    };

    // =============================================================================
    // Test Case
    // =============================================================================
    struct TestCase
    {
        std::string name;
        std::function<bool()> func;
    };

    // =============================================================================
    // Test Suite
    // =============================================================================
    class TestSuite
    {
    public:
        void AddTest(const char* name, std::function<bool()> func)
        {
            m_tests.push_back({name, func});
        }

        std::vector<TestResult> Run();

        void PrintResults(const std::vector<TestResult>& results);

    private:
        std::vector<TestCase> m_tests;
    };

    // =============================================================================
    // Assertion Macros
    // =============================================================================
    #define TEST_ASSERT(condition, message) \
        do { \
            if (!(condition)) { \
                RVX_CORE_ERROR("Test assertion failed: {} - {}", #condition, message); \
                return false; \
            } \
        } while (false)

    #define TEST_ASSERT_EQ(a, b) \
        TEST_ASSERT((a) == (b), "Expected " #a " == " #b)

    #define TEST_ASSERT_NE(a, b) \
        TEST_ASSERT((a) != (b), "Expected " #a " != " #b)

    #define TEST_ASSERT_TRUE(a) \
        TEST_ASSERT((a), "Expected " #a " to be true")

    #define TEST_ASSERT_FALSE(a) \
        TEST_ASSERT(!(a), "Expected " #a " to be false")

    #define TEST_ASSERT_NOT_NULL(a) \
        TEST_ASSERT((a) != nullptr, "Expected " #a " to not be null")

} // namespace RVX::Test
