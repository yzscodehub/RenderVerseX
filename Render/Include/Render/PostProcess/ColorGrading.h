#pragma once

/**
 * @file ColorGrading.h
 * @brief Color grading post-process effect
 * 
 * Implements color grading with LUT support and manual adjustments.
 */

#include "Core/MathTypes.h"
#include "Render/PostProcess/PostProcessStack.h"

namespace RVX
{
    class RHITexture;

    /**
     * @brief Color grading mode
     */
    enum class ColorGradingMode : uint8
    {
        None,           ///< No color grading
        LDR,            ///< LDR color grading (after tone mapping)
        HDR             ///< HDR color grading (before tone mapping)
    };

    /**
     * @brief Color grading configuration
     */
    struct ColorGradingConfig
    {
        // Mode
        ColorGradingMode mode = ColorGradingMode::HDR;
        
        // White balance
        float temperature = 0.0f;           ///< Color temperature offset (-100 to 100)
        float tint = 0.0f;                  ///< Green-magenta tint (-100 to 100)
        
        // Global adjustments
        float exposure = 0.0f;              ///< Exposure offset in EV
        float contrast = 1.0f;              ///< Contrast (0 to 2)
        float saturation = 1.0f;            ///< Saturation (0 to 2)
        float hueShift = 0.0f;              ///< Hue shift in degrees (-180 to 180)
        
        // Lift/Gamma/Gain (color wheels)
        Vec4 lift = Vec4(1.0f, 1.0f, 1.0f, 0.0f);     ///< Shadows RGB + offset
        Vec4 gamma = Vec4(1.0f, 1.0f, 1.0f, 0.0f);    ///< Midtones RGB + offset
        Vec4 gain = Vec4(1.0f, 1.0f, 1.0f, 0.0f);     ///< Highlights RGB + offset
        
        // Channel mixer
        Vec3 redChannel = Vec3(1.0f, 0.0f, 0.0f);     ///< Red channel contribution
        Vec3 greenChannel = Vec3(0.0f, 1.0f, 0.0f);   ///< Green channel contribution
        Vec3 blueChannel = Vec3(0.0f, 0.0f, 1.0f);    ///< Blue channel contribution
        
        // Color curves (simplified)
        float shadowsIntensity = 1.0f;
        float midtonesIntensity = 1.0f;
        float highlightsIntensity = 1.0f;
        
        // LUT
        bool useLUT = false;
        float lutContribution = 1.0f;       ///< LUT blend strength (0 to 1)
        
        // Split toning
        Vec3 shadowsTint = Vec3(0.5f, 0.5f, 0.5f);
        Vec3 highlightsTint = Vec3(0.5f, 0.5f, 0.5f);
        float splitToningBalance = 0.0f;    ///< Balance between shadows and highlights (-1 to 1)
    };

    /**
     * @brief Color grading post-process pass
     * 
     * Implements comprehensive color grading:
     * - White balance (temperature/tint)
     * - Lift/Gamma/Gain (color wheels)
     * - HSV adjustments
     * - Channel mixing
     * - 3D LUT support
     * - Split toning
     * 
     * Can operate in LDR (after tone mapping) or HDR (before tone mapping) mode.
     */
    class ColorGradingPass : public IPostProcessPass
    {
    public:
        ColorGradingPass();
        ~ColorGradingPass() override = default;

        const char* GetName() const override { return "ColorGrading"; }
        int32 GetPriority() const override { return 800; }  // Just before tone mapping

        void Configure(const PostProcessSettings& settings) override;
        void AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output) override;

        // =========================================================================
        // Configuration
        // =========================================================================

        void SetConfig(const ColorGradingConfig& config) { m_config = config; }
        const ColorGradingConfig& GetConfig() const { return m_config; }

        // White balance
        void SetTemperature(float temp) { m_config.temperature = temp; }
        float GetTemperature() const { return m_config.temperature; }

        void SetTint(float tint) { m_config.tint = tint; }
        float GetTint() const { return m_config.tint; }

        // Global adjustments
        void SetExposure(float ev) { m_config.exposure = ev; }
        float GetExposure() const { return m_config.exposure; }

        void SetContrast(float contrast) { m_config.contrast = contrast; }
        float GetContrast() const { return m_config.contrast; }

        void SetSaturation(float saturation) { m_config.saturation = saturation; }
        float GetSaturation() const { return m_config.saturation; }

        void SetHueShift(float degrees) { m_config.hueShift = degrees; }
        float GetHueShift() const { return m_config.hueShift; }

        // Lift/Gamma/Gain
        void SetLift(const Vec4& lift) { m_config.lift = lift; }
        const Vec4& GetLift() const { return m_config.lift; }

        void SetGamma(const Vec4& gamma) { m_config.gamma = gamma; }
        const Vec4& GetGamma() const { return m_config.gamma; }

        void SetGain(const Vec4& gain) { m_config.gain = gain; }
        const Vec4& GetGain() const { return m_config.gain; }

        // LUT
        void SetLUT(RHITexture* lut);
        RHITexture* GetLUT() const { return m_lutTexture; }

        void SetLUTContribution(float contribution) { m_config.lutContribution = contribution; }
        float GetLUTContribution() const { return m_config.lutContribution; }

        void SetUseLUT(bool enable) { m_config.useLUT = enable; }
        bool IsUsingLUT() const { return m_config.useLUT; }

        // Split toning
        void SetShadowsTint(const Vec3& color) { m_config.shadowsTint = color; }
        const Vec3& GetShadowsTint() const { return m_config.shadowsTint; }

        void SetHighlightsTint(const Vec3& color) { m_config.highlightsTint = color; }
        const Vec3& GetHighlightsTint() const { return m_config.highlightsTint; }

        void SetSplitToningBalance(float balance) { m_config.splitToningBalance = balance; }
        float GetSplitToningBalance() const { return m_config.splitToningBalance; }

        // Mode
        void SetMode(ColorGradingMode mode) { m_config.mode = mode; }
        ColorGradingMode GetMode() const { return m_config.mode; }

        // =========================================================================
        // LUT generation
        // =========================================================================

        /**
         * @brief Bake current settings into a LUT texture
         * @param device RHI device
         * @param size LUT size (typically 32 or 64)
         * @return Generated 3D LUT texture
         */
        RHITextureRef BakeToLUT(IRHIDevice* device, uint32 size = 32);

    private:
        ColorGradingConfig m_config;
        RHITexture* m_lutTexture = nullptr;
    };

} // namespace RVX
