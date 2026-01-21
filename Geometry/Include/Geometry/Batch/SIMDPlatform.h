/**
 * @file SIMDPlatform.h
 * @brief Platform detection and SIMD feature configuration
 */

#pragma once

// ============================================================================
// Platform Detection
// ============================================================================

// Detect x86/x64 SIMD support
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    #define RVX_SIMD_X86 1
    
    #if defined(__AVX2__)
        #define RVX_SIMD_AVX2 1
        #define RVX_SIMD_LEVEL 2
    #elif defined(__AVX__)
        #define RVX_SIMD_AVX 1
        #define RVX_SIMD_LEVEL 1
    #elif defined(__SSE4_1__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
        #define RVX_SIMD_SSE4 1
        #define RVX_SIMD_LEVEL 0
    #endif
    
    // MSVC doesn't define __SSE4_1__ by default, but x64 always has SSE2
    #if defined(_MSC_VER) && !defined(RVX_SIMD_LEVEL)
        #define RVX_SIMD_SSE4 1
        #define RVX_SIMD_LEVEL 0
    #endif
#endif

// Detect ARM NEON
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    #define RVX_SIMD_NEON 1
    #define RVX_SIMD_LEVEL 0
#endif

// Detect WebAssembly SIMD
#if defined(__wasm_simd128__)
    #define RVX_SIMD_WASM 1
    #define RVX_SIMD_LEVEL 0
#endif

// Fallback to scalar
#ifndef RVX_SIMD_LEVEL
    #define RVX_SIMD_SCALAR 1
    #define RVX_SIMD_LEVEL -1
#endif

// ============================================================================
// SIMD Headers
// ============================================================================

#if defined(RVX_SIMD_X86)
    #include <xmmintrin.h>  // SSE
    #include <emmintrin.h>  // SSE2
    #include <smmintrin.h>  // SSE4.1
    #if defined(RVX_SIMD_AVX) || defined(RVX_SIMD_AVX2)
        #include <immintrin.h>  // AVX/AVX2
    #endif
#elif defined(RVX_SIMD_NEON)
    #include <arm_neon.h>
#elif defined(RVX_SIMD_WASM)
    #include <wasm_simd128.h>
#endif

// ============================================================================
// Common Alignment
// ============================================================================

#if defined(_MSC_VER)
    #define RVX_ALIGN(n) __declspec(align(n))
#else
    #define RVX_ALIGN(n) __attribute__((aligned(n)))
#endif

#define RVX_SIMD_ALIGN RVX_ALIGN(16)
#define RVX_SIMD_ALIGN32 RVX_ALIGN(32)

namespace RVX::Geometry::SIMD
{

/// Number of floats processed in parallel
#if defined(RVX_SIMD_AVX2) || defined(RVX_SIMD_AVX)
    constexpr int SIMD_WIDTH = 8;
#elif defined(RVX_SIMD_X86) || defined(RVX_SIMD_NEON) || defined(RVX_SIMD_WASM)
    constexpr int SIMD_WIDTH = 4;
#else
    constexpr int SIMD_WIDTH = 1;
#endif

/// Minimum SIMD width (always available)
constexpr int SIMD_WIDTH_MIN = 4;

} // namespace RVX::Geometry::SIMD
