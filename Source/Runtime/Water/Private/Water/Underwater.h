#pragma once

/**
 * @file Underwater.h
 * @brief Underwater post-processing state.
 *
 * Stores visual effect settings for Render-owned underwater passes.
 */

#include "Water/WaterTypes.h"

#include <memory>

namespace RVX
{
    /**
     * @brief Underwater post-processing state.
     */
    class Underwater
    {
    public:
        using Ptr = std::unique_ptr<Underwater>;

        Underwater() = default;
        ~Underwater() = default;

        Underwater(const Underwater&) = delete;
        Underwater& operator=(const Underwater&) = delete;

        bool Initialize(const UnderwaterDesc& desc);
        void Update(float deltaTime);

        UnderwaterProperties& GetProperties() { return m_properties; }
        const UnderwaterProperties& GetProperties() const { return m_properties; }
        void SetProperties(const UnderwaterProperties& props);

        UnderwaterQuality GetQuality() const { return m_quality; }
        void SetQuality(UnderwaterQuality quality);

        void SetUnderwater(bool underwater) { m_isUnderwater = underwater; }
        bool IsUnderwater() const { return m_isUnderwater; }

        bool IsRenderPathAvailable() const { return false; }
        const char* GetRenderPathReason() const
        {
            return "Underwater effects are exported as snapshot state; Render-owned underwater pass is not implemented yet";
        }

    private:
        UnderwaterQuality m_quality = UnderwaterQuality::High;
        UnderwaterProperties m_properties;
        bool m_isUnderwater = false;
        float m_time = 0.0f;
    };

} // namespace RVX
