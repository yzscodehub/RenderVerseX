/**
 * @file SIMDTypes.h
 * @brief SIMD vector types for batch processing
 */

#pragma once

#include "Geometry/Batch/SIMDPlatform.h"
#include "Core/MathTypes.h"
#include <cmath>

namespace RVX::Geometry::SIMD
{

// ============================================================================
// Float4 - 4-wide SIMD float vector
// ============================================================================

#if defined(RVX_SIMD_X86)

/**
 * @brief 4-wide float vector using SSE
 */
struct Float4
{
    __m128 data;

    Float4() : data(_mm_setzero_ps()) {}
    Float4(__m128 v) : data(v) {}
    Float4(float x, float y, float z, float w) : data(_mm_set_ps(w, z, y, x)) {}

    // Factory methods
    static Float4 Zero() { return Float4(_mm_setzero_ps()); }
    static Float4 Splat(float v) { return Float4(_mm_set1_ps(v)); }
    static Float4 Set(float x, float y, float z, float w) { return Float4(_mm_set_ps(w, z, y, x)); }
    static Float4 Load(const float* ptr) { return Float4(_mm_loadu_ps(ptr)); }
    static Float4 LoadAligned(const float* ptr) { return Float4(_mm_load_ps(ptr)); }

    // Store
    void Store(float* ptr) const { _mm_storeu_ps(ptr, data); }
    void StoreAligned(float* ptr) const { _mm_store_ps(ptr, data); }

    // Accessors
    float operator[](int i) const
    {
        RVX_SIMD_ALIGN float arr[4];
        _mm_store_ps(arr, data);
        return arr[i];
    }

    // Arithmetic
    Float4 operator+(Float4 b) const { return Float4(_mm_add_ps(data, b.data)); }
    Float4 operator-(Float4 b) const { return Float4(_mm_sub_ps(data, b.data)); }
    Float4 operator*(Float4 b) const { return Float4(_mm_mul_ps(data, b.data)); }
    Float4 operator/(Float4 b) const { return Float4(_mm_div_ps(data, b.data)); }

    Float4 operator-() const { return Float4(_mm_sub_ps(_mm_setzero_ps(), data)); }

    Float4& operator+=(Float4 b) { data = _mm_add_ps(data, b.data); return *this; }
    Float4& operator-=(Float4 b) { data = _mm_sub_ps(data, b.data); return *this; }
    Float4& operator*=(Float4 b) { data = _mm_mul_ps(data, b.data); return *this; }

    // Comparison (returns mask)
    Float4 operator<(Float4 b) const { return Float4(_mm_cmplt_ps(data, b.data)); }
    Float4 operator<=(Float4 b) const { return Float4(_mm_cmple_ps(data, b.data)); }
    Float4 operator>(Float4 b) const { return Float4(_mm_cmpgt_ps(data, b.data)); }
    Float4 operator>=(Float4 b) const { return Float4(_mm_cmpge_ps(data, b.data)); }

    // Min/Max
    Float4 Min(Float4 b) const { return Float4(_mm_min_ps(data, b.data)); }
    Float4 Max(Float4 b) const { return Float4(_mm_max_ps(data, b.data)); }

    // Logical (for masks)
    Float4 And(Float4 b) const { return Float4(_mm_and_ps(data, b.data)); }
    Float4 Or(Float4 b) const { return Float4(_mm_or_ps(data, b.data)); }
    Float4 AndNot(Float4 b) const { return Float4(_mm_andnot_ps(data, b.data)); }

    // Select: (mask ? a : b)
    Float4 Select(Float4 a, Float4 b) const
    {
        return Float4(_mm_blendv_ps(b.data, a.data, data));
    }

    // Convert mask to int bitmask
    int MoveMask() const { return _mm_movemask_ps(data); }

    // Math
    Float4 Sqrt() const { return Float4(_mm_sqrt_ps(data)); }
    Float4 Reciprocal() const { return Float4(_mm_rcp_ps(data)); }
    Float4 ReciprocalSqrt() const { return Float4(_mm_rsqrt_ps(data)); }

    Float4 Abs() const
    {
        __m128 signMask = _mm_set1_ps(-0.0f);
        return Float4(_mm_andnot_ps(signMask, data));
    }

    // Horizontal operations
    float HorizontalMin() const
    {
        __m128 t = _mm_min_ps(data, _mm_shuffle_ps(data, data, _MM_SHUFFLE(2, 3, 0, 1)));
        t = _mm_min_ps(t, _mm_shuffle_ps(t, t, _MM_SHUFFLE(1, 0, 3, 2)));
        return _mm_cvtss_f32(t);
    }

    float HorizontalMax() const
    {
        __m128 t = _mm_max_ps(data, _mm_shuffle_ps(data, data, _MM_SHUFFLE(2, 3, 0, 1)));
        t = _mm_max_ps(t, _mm_shuffle_ps(t, t, _MM_SHUFFLE(1, 0, 3, 2)));
        return _mm_cvtss_f32(t);
    }
};

#elif defined(RVX_SIMD_NEON)

/**
 * @brief 4-wide float vector using ARM NEON
 */
struct Float4
{
    float32x4_t data;

    Float4() : data(vdupq_n_f32(0.0f)) {}
    Float4(float32x4_t v) : data(v) {}
    Float4(float x, float y, float z, float w)
    {
        float arr[4] = {x, y, z, w};
        data = vld1q_f32(arr);
    }

    // Factory methods
    static Float4 Zero() { return Float4(vdupq_n_f32(0.0f)); }
    static Float4 Splat(float v) { return Float4(vdupq_n_f32(v)); }
    static Float4 Set(float x, float y, float z, float w) { return Float4(x, y, z, w); }
    static Float4 Load(const float* ptr) { return Float4(vld1q_f32(ptr)); }
    static Float4 LoadAligned(const float* ptr) { return Float4(vld1q_f32(ptr)); }

    // Store
    void Store(float* ptr) const { vst1q_f32(ptr, data); }
    void StoreAligned(float* ptr) const { vst1q_f32(ptr, data); }

    // Accessors
    float operator[](int i) const
    {
        float arr[4];
        vst1q_f32(arr, data);
        return arr[i];
    }

    // Arithmetic
    Float4 operator+(Float4 b) const { return Float4(vaddq_f32(data, b.data)); }
    Float4 operator-(Float4 b) const { return Float4(vsubq_f32(data, b.data)); }
    Float4 operator*(Float4 b) const { return Float4(vmulq_f32(data, b.data)); }
    Float4 operator/(Float4 b) const
    {
        // NEON doesn't have direct divide, use reciprocal estimate
        float32x4_t recip = vrecpeq_f32(b.data);
        recip = vmulq_f32(recip, vrecpsq_f32(b.data, recip));  // Newton-Raphson
        return Float4(vmulq_f32(data, recip));
    }

    Float4 operator-() const { return Float4(vnegq_f32(data)); }

    Float4& operator+=(Float4 b) { data = vaddq_f32(data, b.data); return *this; }
    Float4& operator-=(Float4 b) { data = vsubq_f32(data, b.data); return *this; }
    Float4& operator*=(Float4 b) { data = vmulq_f32(data, b.data); return *this; }

    // Comparison (returns mask)
    Float4 operator<(Float4 b) const { return Float4(vreinterpretq_f32_u32(vcltq_f32(data, b.data))); }
    Float4 operator<=(Float4 b) const { return Float4(vreinterpretq_f32_u32(vcleq_f32(data, b.data))); }
    Float4 operator>(Float4 b) const { return Float4(vreinterpretq_f32_u32(vcgtq_f32(data, b.data))); }
    Float4 operator>=(Float4 b) const { return Float4(vreinterpretq_f32_u32(vcgeq_f32(data, b.data))); }

    // Min/Max
    Float4 Min(Float4 b) const { return Float4(vminq_f32(data, b.data)); }
    Float4 Max(Float4 b) const { return Float4(vmaxq_f32(data, b.data)); }

    // Logical (for masks)
    Float4 And(Float4 b) const
    {
        return Float4(vreinterpretq_f32_u32(
            vandq_u32(vreinterpretq_u32_f32(data), vreinterpretq_u32_f32(b.data))));
    }
    Float4 Or(Float4 b) const
    {
        return Float4(vreinterpretq_f32_u32(
            vorrq_u32(vreinterpretq_u32_f32(data), vreinterpretq_u32_f32(b.data))));
    }
    Float4 AndNot(Float4 b) const
    {
        return Float4(vreinterpretq_f32_u32(
            vbicq_u32(vreinterpretq_u32_f32(b.data), vreinterpretq_u32_f32(data))));
    }

    // Select: (mask ? a : b)
    Float4 Select(Float4 a, Float4 b) const
    {
        return Float4(vbslq_f32(vreinterpretq_u32_f32(data), a.data, b.data));
    }

    // Convert mask to int bitmask
    int MoveMask() const
    {
        uint32x4_t shifted = vshrq_n_u32(vreinterpretq_u32_f32(data), 31);
        uint32_t arr[4];
        vst1q_u32(arr, shifted);
        return static_cast<int>(arr[0] | (arr[1] << 1) | (arr[2] << 2) | (arr[3] << 3));
    }

    // Math
    Float4 Sqrt() const { return Float4(vsqrtq_f32(data)); }
    Float4 Reciprocal() const
    {
        float32x4_t recip = vrecpeq_f32(data);
        return Float4(vmulq_f32(recip, vrecpsq_f32(data, recip)));
    }
    Float4 ReciprocalSqrt() const
    {
        float32x4_t rsqrt = vrsqrteq_f32(data);
        return Float4(vmulq_f32(rsqrt, vrsqrtsq_f32(vmulq_f32(data, rsqrt), rsqrt)));
    }
    Float4 Abs() const { return Float4(vabsq_f32(data)); }

    // Horizontal operations
    float HorizontalMin() const
    {
        float32x2_t low = vget_low_f32(data);
        float32x2_t high = vget_high_f32(data);
        float32x2_t m = vpmin_f32(low, high);
        m = vpmin_f32(m, m);
        return vget_lane_f32(m, 0);
    }

    float HorizontalMax() const
    {
        float32x2_t low = vget_low_f32(data);
        float32x2_t high = vget_high_f32(data);
        float32x2_t m = vpmax_f32(low, high);
        m = vpmax_f32(m, m);
        return vget_lane_f32(m, 0);
    }
};

#else

/**
 * @brief Scalar fallback implementation
 */
struct Float4
{
    float x, y, z, w;

    Float4() : x(0), y(0), z(0), w(0) {}
    Float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}

    // Factory methods
    static Float4 Zero() { return Float4(); }
    static Float4 Splat(float v) { return Float4(v, v, v, v); }
    static Float4 Set(float a, float b, float c, float d) { return Float4(a, b, c, d); }
    static Float4 Load(const float* ptr) { return Float4(ptr[0], ptr[1], ptr[2], ptr[3]); }
    static Float4 LoadAligned(const float* ptr) { return Load(ptr); }

    // Store
    void Store(float* ptr) const { ptr[0] = x; ptr[1] = y; ptr[2] = z; ptr[3] = w; }
    void StoreAligned(float* ptr) const { Store(ptr); }

    // Accessors
    float operator[](int i) const { return (&x)[i]; }

    // Arithmetic
    Float4 operator+(Float4 b) const { return Float4(x + b.x, y + b.y, z + b.z, w + b.w); }
    Float4 operator-(Float4 b) const { return Float4(x - b.x, y - b.y, z - b.z, w - b.w); }
    Float4 operator*(Float4 b) const { return Float4(x * b.x, y * b.y, z * b.z, w * b.w); }
    Float4 operator/(Float4 b) const { return Float4(x / b.x, y / b.y, z / b.z, w / b.w); }

    Float4 operator-() const { return Float4(-x, -y, -z, -w); }

    Float4& operator+=(Float4 b) { x += b.x; y += b.y; z += b.z; w += b.w; return *this; }
    Float4& operator-=(Float4 b) { x -= b.x; y -= b.y; z -= b.z; w -= b.w; return *this; }
    Float4& operator*=(Float4 b) { x *= b.x; y *= b.y; z *= b.z; w *= b.w; return *this; }

    // Comparison (returns mask using IEEE float representation)
    Float4 operator<(Float4 b) const
    {
        union { float f; uint32_t u; } cx, cy, cz, cw;
        cx.u = x < b.x ? 0xFFFFFFFF : 0;
        cy.u = y < b.y ? 0xFFFFFFFF : 0;
        cz.u = z < b.z ? 0xFFFFFFFF : 0;
        cw.u = w < b.w ? 0xFFFFFFFF : 0;
        return Float4(cx.f, cy.f, cz.f, cw.f);
    }
    Float4 operator<=(Float4 b) const
    {
        union { float f; uint32_t u; } cx, cy, cz, cw;
        cx.u = x <= b.x ? 0xFFFFFFFF : 0;
        cy.u = y <= b.y ? 0xFFFFFFFF : 0;
        cz.u = z <= b.z ? 0xFFFFFFFF : 0;
        cw.u = w <= b.w ? 0xFFFFFFFF : 0;
        return Float4(cx.f, cy.f, cz.f, cw.f);
    }
    Float4 operator>(Float4 b) const
    {
        union { float f; uint32_t u; } cx, cy, cz, cw;
        cx.u = x > b.x ? 0xFFFFFFFF : 0;
        cy.u = y > b.y ? 0xFFFFFFFF : 0;
        cz.u = z > b.z ? 0xFFFFFFFF : 0;
        cw.u = w > b.w ? 0xFFFFFFFF : 0;
        return Float4(cx.f, cy.f, cz.f, cw.f);
    }
    Float4 operator>=(Float4 b) const
    {
        union { float f; uint32_t u; } cx, cy, cz, cw;
        cx.u = x >= b.x ? 0xFFFFFFFF : 0;
        cy.u = y >= b.y ? 0xFFFFFFFF : 0;
        cz.u = z >= b.z ? 0xFFFFFFFF : 0;
        cw.u = w >= b.w ? 0xFFFFFFFF : 0;
        return Float4(cx.f, cy.f, cz.f, cw.f);
    }

    // Min/Max
    Float4 Min(Float4 b) const
    {
        return Float4(
            x < b.x ? x : b.x,
            y < b.y ? y : b.y,
            z < b.z ? z : b.z,
            w < b.w ? w : b.w);
    }

    Float4 Max(Float4 b) const
    {
        return Float4(
            x > b.x ? x : b.x,
            y > b.y ? y : b.y,
            z > b.z ? z : b.z,
            w > b.w ? w : b.w);
    }

    // Logical (for masks)
    Float4 And(Float4 b) const
    {
        union { float f; uint32_t u; } ax, ay, az, aw, bx, by, bz, bw, rx, ry, rz, rw;
        ax.f = x; ay.f = y; az.f = z; aw.f = w;
        bx.f = b.x; by.f = b.y; bz.f = b.z; bw.f = b.w;
        rx.u = ax.u & bx.u; ry.u = ay.u & by.u; rz.u = az.u & bz.u; rw.u = aw.u & bw.u;
        return Float4(rx.f, ry.f, rz.f, rw.f);
    }
    Float4 Or(Float4 b) const
    {
        union { float f; uint32_t u; } ax, ay, az, aw, bx, by, bz, bw, rx, ry, rz, rw;
        ax.f = x; ay.f = y; az.f = z; aw.f = w;
        bx.f = b.x; by.f = b.y; bz.f = b.z; bw.f = b.w;
        rx.u = ax.u | bx.u; ry.u = ay.u | by.u; rz.u = az.u | bz.u; rw.u = aw.u | bw.u;
        return Float4(rx.f, ry.f, rz.f, rw.f);
    }
    Float4 AndNot(Float4 b) const
    {
        union { float f; uint32_t u; } ax, ay, az, aw, bx, by, bz, bw, rx, ry, rz, rw;
        ax.f = x; ay.f = y; az.f = z; aw.f = w;
        bx.f = b.x; by.f = b.y; bz.f = b.z; bw.f = b.w;
        rx.u = (~ax.u) & bx.u; ry.u = (~ay.u) & by.u; rz.u = (~az.u) & bz.u; rw.u = (~aw.u) & bw.u;
        return Float4(rx.f, ry.f, rz.f, rw.f);
    }

    // Select: (mask ? a : b)
    Float4 Select(Float4 a, Float4 b) const
    {
        union { float f; uint32_t u; } mx, my, mz, mw;
        mx.f = x; my.f = y; mz.f = z; mw.f = w;
        return Float4(
            mx.u ? a.x : b.x,
            my.u ? a.y : b.y,
            mz.u ? a.z : b.z,
            mw.u ? a.w : b.w);
    }

    // Convert mask to int bitmask
    int MoveMask() const
    {
        union { float f; uint32_t u; } mx, my, mz, mw;
        mx.f = x; my.f = y; mz.f = z; mw.f = w;
        return static_cast<int>(
            ((mx.u >> 31) & 1) |
            ((my.u >> 31) & 1) << 1 |
            ((mz.u >> 31) & 1) << 2 |
            ((mw.u >> 31) & 1) << 3);
    }

    // Math
    Float4 Sqrt() const
    {
        return Float4(std::sqrt(x), std::sqrt(y), std::sqrt(z), std::sqrt(w));
    }
    Float4 Reciprocal() const
    {
        return Float4(1.0f / x, 1.0f / y, 1.0f / z, 1.0f / w);
    }
    Float4 ReciprocalSqrt() const
    {
        return Float4(1.0f / std::sqrt(x), 1.0f / std::sqrt(y), 1.0f / std::sqrt(z), 1.0f / std::sqrt(w));
    }
    Float4 Abs() const
    {
        return Float4(std::abs(x), std::abs(y), std::abs(z), std::abs(w));
    }

    // Horizontal operations
    float HorizontalMin() const
    {
        return std::min({x, y, z, w});
    }

    float HorizontalMax() const
    {
        return std::max({x, y, z, w});
    }
};

#endif

// ============================================================================
// Vec3x4 - 4 Vec3s in SOA layout
// ============================================================================

/**
 * @brief 4 Vec3 vectors stored in Structure-of-Arrays layout
 * 
 * Enables parallel processing of 4 vectors at once.
 */
struct Vec3x4
{
    Float4 x, y, z;

    Vec3x4() = default;
    Vec3x4(Float4 xx, Float4 yy, Float4 zz) : x(xx), y(yy), z(zz) {}

    /// Create from 4 separate Vec3s
    static Vec3x4 Load(const Vec3& v0, const Vec3& v1, const Vec3& v2, const Vec3& v3)
    {
        return Vec3x4(
            Float4(v0.x, v1.x, v2.x, v3.x),
            Float4(v0.y, v1.y, v2.y, v3.y),
            Float4(v0.z, v1.z, v2.z, v3.z)
        );
    }

    /// Splat a single Vec3 to all 4 slots
    static Vec3x4 Splat(const Vec3& v)
    {
        return Vec3x4(
            Float4::Splat(v.x),
            Float4::Splat(v.y),
            Float4::Splat(v.z)
        );
    }

    // Arithmetic
    Vec3x4 operator+(const Vec3x4& b) const { return Vec3x4(x + b.x, y + b.y, z + b.z); }
    Vec3x4 operator-(const Vec3x4& b) const { return Vec3x4(x - b.x, y - b.y, z - b.z); }
    Vec3x4 operator*(Float4 s) const { return Vec3x4(x * s, y * s, z * s); }
    Vec3x4 operator*(const Vec3x4& b) const { return Vec3x4(x * b.x, y * b.y, z * b.z); }

    Vec3x4 operator-() const { return Vec3x4(-x, -y, -z); }

    /// Dot product (returns 4 scalars)
    Float4 Dot(const Vec3x4& b) const
    {
        return x * b.x + y * b.y + z * b.z;
    }

    /// Cross product
    Vec3x4 Cross(const Vec3x4& b) const
    {
        return Vec3x4(
            y * b.z - z * b.y,
            z * b.x - x * b.z,
            x * b.y - y * b.x
        );
    }

    /// Length squared
    Float4 LengthSquared() const
    {
        return Dot(*this);
    }

    /// Length
    Float4 Length() const
    {
        return LengthSquared().Sqrt();
    }

    /// Normalize
    Vec3x4 Normalize() const
    {
        Float4 len = Length();
        Float4 invLen = Float4::Splat(1.0f) / len;
        return *this * invLen;
    }

    /// Component-wise min
    Vec3x4 Min(const Vec3x4& b) const
    {
        return Vec3x4(x.Min(b.x), y.Min(b.y), z.Min(b.z));
    }

    /// Component-wise max
    Vec3x4 Max(const Vec3x4& b) const
    {
        return Vec3x4(x.Max(b.x), y.Max(b.y), z.Max(b.z));
    }

    /// Extract single Vec3
    Vec3 Extract(int i) const
    {
        return Vec3(x[i], y[i], z[i]);
    }
};

} // namespace RVX::Geometry::SIMD
