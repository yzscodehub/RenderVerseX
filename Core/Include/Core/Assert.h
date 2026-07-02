#pragma once

#include "Core/Log.h"

#include <csignal>
#include <cstdlib>

// =============================================================================
// Assertion Macros
// =============================================================================

#ifdef RVX_DEBUG
    #if defined(_MSC_VER)
        #define RVX_DEBUGBREAK() __debugbreak()
    #elif defined(__clang__) || defined(__GNUC__)
        #define RVX_DEBUGBREAK() __builtin_trap()
    #else
        #define RVX_DEBUGBREAK() std::raise(SIGTRAP)
    #endif
#else
    #define RVX_DEBUGBREAK() ((void)0)
#endif

#define RVX_ASSERT(condition)                                               \
    do {                                                                    \
        if (!(condition)) {                                                 \
            RVX_CORE_CRITICAL("Assertion Failed: {}", #condition);          \
            RVX_DEBUGBREAK();                                               \
            std::abort();                                                   \
        }                                                                   \
    } while (false)

#define RVX_ASSERT_MSG(condition, ...)                                      \
    do {                                                                    \
        if (!(condition)) {                                                 \
            RVX_CORE_CRITICAL("Assertion Failed: {}", #condition);          \
            RVX_CORE_CRITICAL(__VA_ARGS__);                                 \
            RVX_DEBUGBREAK();                                               \
            std::abort();                                                   \
        }                                                                   \
    } while (false)

#define RVX_VERIFY(condition, ...)                                          \
    do {                                                                    \
        if (!(condition)) {                                                 \
            RVX_CORE_ERROR("Verification Failed: {}", #condition);          \
            RVX_CORE_ERROR(__VA_ARGS__);                                    \
        }                                                                   \
    } while (false)

// Debug-only assertions
#ifdef RVX_DEBUG
    #define RVX_DEBUG_ASSERT(condition) RVX_ASSERT(condition)
    #define RVX_DEBUG_ASSERT_MSG(condition, ...) RVX_ASSERT_MSG(condition, __VA_ARGS__)
#else
    #define RVX_DEBUG_ASSERT(condition) ((void)0)
    #define RVX_DEBUG_ASSERT_MSG(condition, ...) ((void)0)
#endif

// Unreachable code marker
#define RVX_UNREACHABLE()                                                   \
    do {                                                                    \
        RVX_CORE_CRITICAL("Unreachable code reached at {}:{}", __FILE__, __LINE__); \
        RVX_DEBUGBREAK();                                                   \
        std::abort();                                                       \
    } while (false)
