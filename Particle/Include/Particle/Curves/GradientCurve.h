#pragma once

/**
 * @file GradientCurve.h
 * @brief Color gradient curve for particle color modulation over lifetime
 */

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include <vector>
#include <algorithm>

namespace RVX::Particle
{
    /**
     * @brief Gradient keyframe (color stop)
     */
    struct GradientKey
    {
        float time = 0.0f;      ///< Normalized time (0-1)
        Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};  ///< RGBA color

        GradientKey() = default;
        GradientKey(float t, const Vec4& c) : time(t), color(c) {}
        GradientKey(float t, float r, float g, float b, float a = 1.0f)
            : time(t), color(r, g, b, a) {}

        bool operator<(const GradientKey& other) const { return time < other.time; }
    };

    /**
     * @brief Color gradient curve for value modulation over normalized time (0-1)
     * 
     * Used for Color over Lifetime effects. Supports linear interpolation
     * between color stops.
     */
    class GradientCurve
    {
    public:
        // =====================================================================
        // Construction
        // =====================================================================

        GradientCurve() = default;

        GradientCurve(std::initializer_list<GradientKey> keys)
            : m_keys(keys)
        {
            Sort();
        }

        // =====================================================================
        // Key Management
        // =====================================================================

        /// Add a color stop
        void AddKey(const GradientKey& key)
        {
            m_keys.push_back(key);
            Sort();
        }

        /// Add a color stop with time and color
        void AddKey(float time, const Vec4& color)
        {
            AddKey(GradientKey(time, color));
        }

        /// Remove key at index
        void RemoveKey(size_t index)
        {
            if (index < m_keys.size())
            {
                m_keys.erase(m_keys.begin() + index);
            }
        }

        /// Clear all keys
        void Clear() { m_keys.clear(); }

        /// Get number of keys
        size_t GetKeyCount() const { return m_keys.size(); }

        /// Get key at index
        GradientKey& GetKey(size_t index) { return m_keys[index]; }
        const GradientKey& GetKey(size_t index) const { return m_keys[index]; }

        /// Get all keys
        std::vector<GradientKey>& GetKeys() { return m_keys; }
        const std::vector<GradientKey>& GetKeys() const { return m_keys; }

        // =====================================================================
        // Evaluation
        // =====================================================================

        /**
         * @brief Evaluate the gradient at normalized time t (0-1)
         * @param t Normalized time
         * @return Interpolated color
         */
        Vec4 Evaluate(float t) const
        {
            if (m_keys.empty())
                return Vec4(1.0f, 1.0f, 1.0f, 1.0f);

            if (m_keys.size() == 1)
                return m_keys[0].color;

            // Clamp t to valid range
            t = std::clamp(t, 0.0f, 1.0f);

            // Find surrounding keys
            size_t i = 0;
            for (; i < m_keys.size() - 1; ++i)
            {
                if (t < m_keys[i + 1].time)
                    break;
            }

            if (i >= m_keys.size() - 1)
                return m_keys.back().color;

            const auto& k0 = m_keys[i];
            const auto& k1 = m_keys[i + 1];

            // Normalize t between keys
            float duration = k1.time - k0.time;
            if (duration <= 0.0f)
                return k0.color;

            float localT = (t - k0.time) / duration;

            // Linear interpolation
            return mix(k0.color, k1.color, localT);
        }

        // =====================================================================
        // GPU Export
        // =====================================================================

        /**
         * @brief Bake gradient to a lookup table for GPU sampling
         * @param outData Output array
         * @param samples Number of samples (LUT size)
         */
        void BakeToLUT(Vec4* outData, uint32 samples) const
        {
            for (uint32 i = 0; i < samples; ++i)
            {
                float t = static_cast<float>(i) / static_cast<float>(samples - 1);
                outData[i] = Evaluate(t);
            }
        }

        // =====================================================================
        // Presets
        // =====================================================================

        /// Solid white
        static GradientCurve White()
        {
            return GradientCurve({
                GradientKey(0.0f, Vec4(1.0f, 1.0f, 1.0f, 1.0f)),
                GradientKey(1.0f, Vec4(1.0f, 1.0f, 1.0f, 1.0f))
            });
        }

        /// Solid color
        static GradientCurve Solid(const Vec4& color)
        {
            return GradientCurve({
                GradientKey(0.0f, color),
                GradientKey(1.0f, color)
            });
        }

        /// White to transparent (fade out alpha)
        static GradientCurve FadeOut()
        {
            return GradientCurve({
                GradientKey(0.0f, Vec4(1.0f, 1.0f, 1.0f, 1.0f)),
                GradientKey(1.0f, Vec4(1.0f, 1.0f, 1.0f, 0.0f))
            });
        }

        /// Transparent to white (fade in alpha)
        static GradientCurve FadeIn()
        {
            return GradientCurve({
                GradientKey(0.0f, Vec4(1.0f, 1.0f, 1.0f, 0.0f)),
                GradientKey(1.0f, Vec4(1.0f, 1.0f, 1.0f, 1.0f))
            });
        }

        /// Fire gradient (yellow -> orange -> red -> dark)
        static GradientCurve Fire()
        {
            return GradientCurve({
                GradientKey(0.0f, Vec4(1.0f, 1.0f, 0.0f, 1.0f)),   // Yellow
                GradientKey(0.3f, Vec4(1.0f, 0.5f, 0.0f, 1.0f)),   // Orange
                GradientKey(0.6f, Vec4(1.0f, 0.0f, 0.0f, 0.8f)),   // Red
                GradientKey(1.0f, Vec4(0.2f, 0.0f, 0.0f, 0.0f))    // Dark/transparent
            });
        }

        /// Smoke gradient (white -> gray -> transparent)
        static GradientCurve Smoke()
        {
            return GradientCurve({
                GradientKey(0.0f, Vec4(1.0f, 1.0f, 1.0f, 0.5f)),
                GradientKey(0.5f, Vec4(0.5f, 0.5f, 0.5f, 0.3f)),
                GradientKey(1.0f, Vec4(0.3f, 0.3f, 0.3f, 0.0f))
            });
        }

        /// Spark gradient (bright -> dark)
        static GradientCurve Spark()
        {
            return GradientCurve({
                GradientKey(0.0f, Vec4(1.0f, 1.0f, 0.8f, 1.0f)),
                GradientKey(0.2f, Vec4(1.0f, 0.8f, 0.0f, 1.0f)),
                GradientKey(1.0f, Vec4(0.5f, 0.2f, 0.0f, 0.0f))
            });
        }

        /// Rainbow gradient
        static GradientCurve Rainbow()
        {
            return GradientCurve({
                GradientKey(0.0f, Vec4(1.0f, 0.0f, 0.0f, 1.0f)),    // Red
                GradientKey(0.17f, Vec4(1.0f, 0.5f, 0.0f, 1.0f)),   // Orange
                GradientKey(0.33f, Vec4(1.0f, 1.0f, 0.0f, 1.0f)),   // Yellow
                GradientKey(0.5f, Vec4(0.0f, 1.0f, 0.0f, 1.0f)),    // Green
                GradientKey(0.67f, Vec4(0.0f, 0.0f, 1.0f, 1.0f)),   // Blue
                GradientKey(0.83f, Vec4(0.5f, 0.0f, 1.0f, 1.0f)),   // Indigo
                GradientKey(1.0f, Vec4(1.0f, 0.0f, 1.0f, 1.0f))     // Violet
            });
        }

    private:
        void Sort()
        {
            std::sort(m_keys.begin(), m_keys.end());
        }

        std::vector<GradientKey> m_keys;
    };

} // namespace RVX::Particle
