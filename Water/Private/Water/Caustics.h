#pragma once

/**
 * @file Caustics.h
 * @brief Underwater light caustics state.
 *
 * Stores caustics settings that can be exported to Render-owned passes.
 */

#include "Water/WaterTypes.h"

#include <memory>

namespace RVX
{
    class WaterSimulation;

    /**
     * @brief Underwater caustics state.
     *
     * Tracks caustics animation/configuration on the Feature side. Texture
     * generation and scene application belong to Render.
     */
    class Caustics
    {
    public:
        using Ptr = std::unique_ptr<Caustics>;

        Caustics() = default;
        ~Caustics() = default;

        Caustics(const Caustics&) = delete;
        Caustics& operator=(const Caustics&) = delete;

        bool Initialize(const CausticsDesc& desc);
        void Update(float deltaTime, const WaterSimulation* simulation);

        void SetIntensity(float intensity) { m_intensity = intensity; }
        float GetIntensity() const { return m_intensity; }

        void SetScale(float scale) { m_scale = scale; }
        float GetScale() const { return m_scale; }

        void SetSpeed(float speed) { m_speed = speed; }
        float GetSpeed() const { return m_speed; }

        void SetMaxDepth(float depth) { m_maxDepth = depth; }
        float GetMaxDepth() const { return m_maxDepth; }

        CausticsQuality GetQuality() const { return m_quality; }

        bool IsRenderPathAvailable() const { return false; }
        const char* GetRenderPathReason() const
        {
            return "Water caustics are exported as snapshot state; Render-owned caustics pass is not implemented yet";
        }

    private:
        CausticsQuality m_quality = CausticsQuality::Medium;
        uint32 m_textureSize = 512;
        float m_intensity = 1.0f;
        float m_scale = 5.0f;
        float m_speed = 1.0f;
        float m_maxDepth = 20.0f;
        float m_focusFalloff = 0.5f;
        float m_time = 0.0f;
    };

} // namespace RVX
