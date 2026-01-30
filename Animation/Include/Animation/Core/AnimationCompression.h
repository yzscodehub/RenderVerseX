/**
 * @file AnimationCompression.h
 * @brief Animation compression algorithms for reducing memory usage
 * 
 * Supports:
 * - Quantization compression for transforms
 * - Curve fitting/keyframe reduction
 * - Variable bit-rate encoding
 * - Lossy and lossless modes
 */

#pragma once

#include "Animation/Core/Types.h"
#include "Animation/Core/Keyframe.h"
#include "Animation/Core/TransformSample.h"
#include "Animation/Data/AnimationClip.h"
#include "Core/MathTypes.h"
#include <memory>
#include <vector>
#include <cstdint>

namespace RVX::Animation
{

// ============================================================================
// Compression Settings
// ============================================================================

/**
 * @brief Compression quality preset
 */
enum class CompressionQuality : uint8_t
{
    Lossless,       ///< No data loss (only removes redundant keyframes)
    Highest,        ///< Highest quality, minimal compression
    High,           ///< Good quality, moderate compression
    Medium,         ///< Balanced quality/compression
    Low,            ///< Lower quality, high compression
    Lowest          ///< Lowest quality, maximum compression
};

/**
 * @brief Quantization settings for a single channel
 */
struct QuantizationSettings
{
    /// Bits used for translation (per component)
    uint8_t translationBits = 16;

    /// Bits used for rotation (total for quaternion)
    uint8_t rotationBits = 48;  // 12 bits per component (smallest three)

    /// Bits used for scale (per component)
    uint8_t scaleBits = 16;

    /// Bits used for float properties
    uint8_t propertyBits = 16;

    static QuantizationSettings FromQuality(CompressionQuality quality)
    {
        QuantizationSettings settings;
        switch (quality)
        {
            case CompressionQuality::Lossless:
                settings.translationBits = 32;
                settings.rotationBits = 64;
                settings.scaleBits = 32;
                settings.propertyBits = 32;
                break;

            case CompressionQuality::Highest:
                settings.translationBits = 24;
                settings.rotationBits = 60;
                settings.scaleBits = 24;
                settings.propertyBits = 24;
                break;

            case CompressionQuality::High:
                settings.translationBits = 16;
                settings.rotationBits = 48;
                settings.scaleBits = 16;
                settings.propertyBits = 16;
                break;

            case CompressionQuality::Medium:
                settings.translationBits = 14;
                settings.rotationBits = 42;
                settings.scaleBits = 14;
                settings.propertyBits = 14;
                break;

            case CompressionQuality::Low:
                settings.translationBits = 12;
                settings.rotationBits = 36;
                settings.scaleBits = 12;
                settings.propertyBits = 12;
                break;

            case CompressionQuality::Lowest:
                settings.translationBits = 10;
                settings.rotationBits = 30;
                settings.scaleBits = 10;
                settings.propertyBits = 10;
                break;
        }
        return settings;
    }
};

/**
 * @brief Keyframe reduction settings
 */
struct KeyframeReductionSettings
{
    /// Enable keyframe reduction
    bool enabled = true;

    /// Maximum allowed error for translation (world units)
    float maxTranslationError = 0.001f;

    /// Maximum allowed error for rotation (radians)
    float maxRotationError = 0.001f;

    /// Maximum allowed error for scale (factor)
    float maxScaleError = 0.0001f;

    /// Maximum allowed error for properties
    float maxPropertyError = 0.001f;

    /// Minimum keyframes to keep per track
    size_t minKeyframes = 2;

    static KeyframeReductionSettings FromQuality(CompressionQuality quality)
    {
        KeyframeReductionSettings settings;
        switch (quality)
        {
            case CompressionQuality::Lossless:
                settings.enabled = false;
                break;

            case CompressionQuality::Highest:
                settings.maxTranslationError = 0.0001f;
                settings.maxRotationError = 0.0001f;
                settings.maxScaleError = 0.00001f;
                break;

            case CompressionQuality::High:
                settings.maxTranslationError = 0.001f;
                settings.maxRotationError = 0.001f;
                settings.maxScaleError = 0.0001f;
                break;

            case CompressionQuality::Medium:
                settings.maxTranslationError = 0.005f;
                settings.maxRotationError = 0.005f;
                settings.maxScaleError = 0.001f;
                break;

            case CompressionQuality::Low:
                settings.maxTranslationError = 0.01f;
                settings.maxRotationError = 0.01f;
                settings.maxScaleError = 0.005f;
                break;

            case CompressionQuality::Lowest:
                settings.maxTranslationError = 0.05f;
                settings.maxRotationError = 0.05f;
                settings.maxScaleError = 0.01f;
                break;
        }
        return settings;
    }
};

/**
 * @brief Complete compression configuration
 */
struct CompressionConfig
{
    /// Overall quality preset
    CompressionQuality quality = CompressionQuality::High;

    /// Quantization settings
    QuantizationSettings quantization;

    /// Keyframe reduction settings
    KeyframeReductionSettings keyframeReduction;

    /// Remove static tracks (tracks with no change)
    bool removeStaticTracks = true;

    /// Static threshold for detecting unchanging values
    float staticThreshold = 1e-6f;

    static CompressionConfig FromQuality(CompressionQuality quality)
    {
        CompressionConfig config;
        config.quality = quality;
        config.quantization = QuantizationSettings::FromQuality(quality);
        config.keyframeReduction = KeyframeReductionSettings::FromQuality(quality);
        return config;
    }
};

// ============================================================================
// Compression Statistics
// ============================================================================

/**
 * @brief Statistics from compression
 */
struct CompressionStats
{
    /// Original size in bytes (estimated)
    size_t originalSize = 0;

    /// Compressed size in bytes (estimated)
    size_t compressedSize = 0;

    /// Compression ratio (original / compressed)
    float compressionRatio = 1.0f;

    /// Number of keyframes removed
    size_t keyframesRemoved = 0;

    /// Number of tracks removed (static)
    size_t tracksRemoved = 0;

    /// Maximum error introduced
    float maxError = 0.0f;

    /// Average error
    float averageError = 0.0f;

    void Calculate()
    {
        if (compressedSize > 0)
        {
            compressionRatio = static_cast<float>(originalSize) / 
                               static_cast<float>(compressedSize);
        }
    }
};

// ============================================================================
// Quantized Data Types
// ============================================================================

/**
 * @brief Quantized vector (16-bit per component)
 */
struct QuantizedVec3_16
{
    int16_t x, y, z;

    QuantizedVec3_16() : x(0), y(0), z(0) {}

    QuantizedVec3_16(const Vec3& v, const Vec3& minBound, const Vec3& maxBound)
    {
        Vec3 range = maxBound - minBound;
        Vec3 normalized = (v - minBound) / range;
        normalized = clamp(normalized, Vec3(0.0f), Vec3(1.0f));

        x = static_cast<int16_t>(normalized.x * 32767.0f);
        y = static_cast<int16_t>(normalized.y * 32767.0f);
        z = static_cast<int16_t>(normalized.z * 32767.0f);
    }

    Vec3 Decompress(const Vec3& minBound, const Vec3& maxBound) const
    {
        Vec3 normalized(
            static_cast<float>(x) / 32767.0f,
            static_cast<float>(y) / 32767.0f,
            static_cast<float>(z) / 32767.0f
        );
        return minBound + normalized * (maxBound - minBound);
    }
};

/**
 * @brief Quantized quaternion using smallest-three encoding
 */
struct QuantizedQuat
{
    /// Index of the dropped (largest) component (0-3)
    uint8_t droppedIndex;
    
    /// Quantized values for the 3 smallest components
    int16_t a, b, c;

    QuantizedQuat() : droppedIndex(0), a(0), b(0), c(0) {}

    explicit QuantizedQuat(const Quat& q)
    {
        // Find largest component
        float absW = std::abs(q.w);
        float absX = std::abs(q.x);
        float absY = std::abs(q.y);
        float absZ = std::abs(q.z);

        float maxAbs = absW;
        droppedIndex = 0;

        if (absX > maxAbs) { maxAbs = absX; droppedIndex = 1; }
        if (absY > maxAbs) { maxAbs = absY; droppedIndex = 2; }
        if (absZ > maxAbs) { maxAbs = absZ; droppedIndex = 3; }

        // Get the three remaining components
        float sign = 1.0f;
        float v0, v1, v2;

        switch (droppedIndex)
        {
            case 0: // W dropped
                sign = q.w >= 0 ? 1.0f : -1.0f;
                v0 = q.x * sign; v1 = q.y * sign; v2 = q.z * sign;
                break;
            case 1: // X dropped
                sign = q.x >= 0 ? 1.0f : -1.0f;
                v0 = q.w * sign; v1 = q.y * sign; v2 = q.z * sign;
                break;
            case 2: // Y dropped
                sign = q.y >= 0 ? 1.0f : -1.0f;
                v0 = q.w * sign; v1 = q.x * sign; v2 = q.z * sign;
                break;
            case 3: // Z dropped
            default:
                sign = q.z >= 0 ? 1.0f : -1.0f;
                v0 = q.w * sign; v1 = q.x * sign; v2 = q.y * sign;
                break;
        }

        // Quantize to 15-bit range [-1/sqrt(2), 1/sqrt(2)] -> [-1, 1]
        constexpr float kScale = 1.41421356f; // sqrt(2)
        a = static_cast<int16_t>(clamp(v0 * kScale, -1.0f, 1.0f) * 16383.0f);
        b = static_cast<int16_t>(clamp(v1 * kScale, -1.0f, 1.0f) * 16383.0f);
        c = static_cast<int16_t>(clamp(v2 * kScale, -1.0f, 1.0f) * 16383.0f);
    }

    Quat Decompress() const
    {
        constexpr float kInvScale = 0.70710678f; // 1/sqrt(2)

        float v0 = (static_cast<float>(a) / 16383.0f) * kInvScale;
        float v1 = (static_cast<float>(b) / 16383.0f) * kInvScale;
        float v2 = (static_cast<float>(c) / 16383.0f) * kInvScale;

        // Reconstruct the dropped component
        float sumSq = v0 * v0 + v1 * v1 + v2 * v2;
        float dropped = std::sqrt(std::max(0.0f, 1.0f - sumSq));

        Quat result;
        switch (droppedIndex)
        {
            case 0:
                result = Quat(dropped, v0, v1, v2);
                break;
            case 1:
                result = Quat(v0, dropped, v1, v2);
                break;
            case 2:
                result = Quat(v0, v1, dropped, v2);
                break;
            case 3:
            default:
                result = Quat(v0, v1, v2, dropped);
                break;
        }

        return normalize(result);
    }
};

/**
 * @brief Quantized float (16-bit)
 */
struct QuantizedFloat
{
    int16_t value;
    float minVal, maxVal;

    QuantizedFloat() : value(0), minVal(0.0f), maxVal(1.0f) {}

    QuantizedFloat(float v, float min, float max)
        : minVal(min), maxVal(max)
    {
        float range = max - min;
        if (range > 0.0f)
        {
            float normalized = (v - min) / range;
            normalized = clamp(normalized, 0.0f, 1.0f);
            value = static_cast<int16_t>(normalized * 32767.0f);
        }
        else
        {
            value = 0;
        }
    }

    float Decompress() const
    {
        float normalized = static_cast<float>(value) / 32767.0f;
        return minVal + normalized * (maxVal - minVal);
    }
};

// ============================================================================
// Animation Compressor
// ============================================================================

/**
 * @brief Compresses animation clips to reduce memory usage
 * 
 * Usage:
 * @code
 * AnimationCompressor compressor;
 * compressor.SetConfig(CompressionConfig::FromQuality(CompressionQuality::High));
 * 
 * // Compress in-place
 * CompressionStats stats = compressor.Compress(clip);
 * 
 * // Or create compressed copy
 * auto compressedClip = compressor.CompressCopy(clip);
 * @endcode
 */
class AnimationCompressor
{
public:
    using Ptr = std::shared_ptr<AnimationCompressor>;

    AnimationCompressor() = default;

    static Ptr Create()
    {
        return std::make_shared<AnimationCompressor>();
    }

    // =========================================================================
    // Configuration
    // =========================================================================

    void SetConfig(const CompressionConfig& config) { m_config = config; }
    const CompressionConfig& GetConfig() const { return m_config; }

    void SetQuality(CompressionQuality quality)
    {
        m_config = CompressionConfig::FromQuality(quality);
    }

    // =========================================================================
    // Compression
    // =========================================================================

    /**
     * @brief Compress an animation clip in-place
     * @param clip Clip to compress
     * @return Compression statistics
     */
    CompressionStats Compress(AnimationClip& clip);

    /**
     * @brief Create a compressed copy of an animation clip
     * @param clip Source clip
     * @return Compressed copy
     */
    AnimationClip::Ptr CompressCopy(const AnimationClip& clip);

    /**
     * @brief Estimate compressed size without actually compressing
     * @param clip Clip to analyze
     * @return Estimated compressed size in bytes
     */
    size_t EstimateCompressedSize(const AnimationClip& clip) const;

    /**
     * @brief Calculate original size of a clip
     * @param clip Clip to analyze
     * @return Size in bytes
     */
    size_t CalculateOriginalSize(const AnimationClip& clip) const;

    // =========================================================================
    // Individual Operations
    // =========================================================================

    /**
     * @brief Remove redundant keyframes from a track
     */
    template<typename KeyframeType>
    size_t ReduceKeyframes(std::vector<KeyframeType>& keyframes, float maxError);

    /**
     * @brief Check if a track is static (no meaningful change)
     */
    bool IsTrackStatic(const TransformTrack& track) const;

    /**
     * @brief Check if float keyframes are static
     */
    bool IsTrackStatic(const std::vector<KeyframeFloat>& keyframes) const;

    // =========================================================================
    // Statistics
    // =========================================================================

    const CompressionStats& GetLastStats() const { return m_lastStats; }

private:
    // Keyframe reduction helpers
    void ReduceTransformTrack(TransformTrack& track);
    void ReduceBlendShapeTrack(BlendShapeTrack& track);
    void ReducePropertyTrack(PropertyTrack& track);

    // Static track removal
    void RemoveStaticTracks(AnimationClip& clip);

    // Error calculation
    float CalculateVec3Error(const Vec3& a, const Vec3& b) const;
    float CalculateQuatError(const Quat& a, const Quat& b) const;

    CompressionConfig m_config;
    CompressionStats m_lastStats;
};

// ============================================================================
// Template Implementation
// ============================================================================

template<typename KeyframeType>
size_t AnimationCompressor::ReduceKeyframes(
    std::vector<KeyframeType>& keyframes,
    float maxError)
{
    if (keyframes.size() <= m_config.keyframeReduction.minKeyframes)
        return 0;

    // Ramer-Douglas-Peucker style reduction
    std::vector<bool> keep(keyframes.size(), true);
    size_t removed = 0;

    // Always keep first and last
    for (size_t i = 1; i < keyframes.size() - 1; ++i)
    {
        // Check if keyframe can be removed
        // Linear interpolation between neighbors should approximate this keyframe
        const auto& prev = keyframes[i - 1];
        const auto& curr = keyframes[i];
        const auto& next = keyframes[i + 1];

        // Calculate t for interpolation
        float t = 0.0f;
        TimeUs duration = next.time - prev.time;
        if (duration > 0)
        {
            t = static_cast<float>(curr.time - prev.time) / static_cast<float>(duration);
        }

        // Interpolate and check error
        auto interpolated = InterpolateLinear(prev.value, next.value, t);
        
        // Calculate error (specialized per type)
        float error = 0.0f;
        if constexpr (std::is_same_v<typename KeyframeType::value_type, Vec3>)
        {
            error = length(curr.value - interpolated);
        }
        else if constexpr (std::is_same_v<typename KeyframeType::value_type, float>)
        {
            error = std::abs(curr.value - interpolated);
        }
        else if constexpr (std::is_same_v<typename KeyframeType::value_type, Quat>)
        {
            // Use dot product for quaternion similarity
            float d = std::abs(dot(curr.value, interpolated));
            error = 1.0f - d;
        }

        if (error <= maxError)
        {
            keep[i] = false;
            removed++;
        }
    }

    // Remove marked keyframes
    if (removed > 0)
    {
        std::vector<KeyframeType> filtered;
        filtered.reserve(keyframes.size() - removed);
        
        for (size_t i = 0; i < keyframes.size(); ++i)
        {
            if (keep[i])
            {
                filtered.push_back(std::move(keyframes[i]));
            }
        }
        
        keyframes = std::move(filtered);
    }

    return removed;
}

} // namespace RVX::Animation
