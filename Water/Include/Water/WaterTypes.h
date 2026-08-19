#pragma once

/**
 * @file WaterTypes.h
 * @brief Runtime-facing water configuration contracts without renderer backend types
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"

#include <vector>

namespace RVX
{
    /**
     * @brief Water surface type
     */
    enum class WaterSurfaceType : uint8
    {
        Ocean,      ///< Large-scale ocean with FFT waves
        Lake,       ///< Calm lake with subtle waves
        River,      ///< Flowing water with directional flow
        Pool        ///< Still water with small ripples
    };

    /**
     * @brief Water surface visual properties
     */
    struct WaterVisualProperties
    {
        Vec3 shallowColor{0.0f, 0.4f, 0.5f};    ///< Shallow water color
        Vec3 deepColor{0.0f, 0.1f, 0.2f};       ///< Deep water color
        Vec3 foamColor{1.0f, 1.0f, 1.0f};       ///< Foam/whitecap color

        float transparency = 0.8f;              ///< Water transparency [0-1]
        float refractionStrength = 0.1f;        ///< Refraction distortion strength
        float reflectionStrength = 0.5f;        ///< Reflection intensity
        float fresnelPower = 5.0f;              ///< Fresnel effect power
        float fresnelBias = 0.02f;              ///< Fresnel bias

        float specularPower = 256.0f;           ///< Specular highlight power
        float specularIntensity = 1.0f;         ///< Specular intensity
        float roughness = 0.1f;                 ///< Surface roughness

        float depthFalloff = 0.5f;              ///< Depth color falloff
        float maxVisibleDepth = 50.0f;          ///< Maximum visible depth

        float foamThreshold = 0.5f;             ///< Foam generation threshold
        float foamIntensity = 1.0f;             ///< Foam intensity
        float foamFalloff = 2.0f;               ///< Foam edge falloff
    };

    /**
     * @brief Water surface mesh descriptor
     */
    struct WaterSurfaceDesc
    {
        Vec2 size{100.0f, 100.0f};              ///< Surface size in world units
        uint32 resolution = 128;                ///< Mesh resolution (vertices per side)
        WaterSurfaceType type = WaterSurfaceType::Ocean;
        WaterVisualProperties visual;
    };

    /**
     * @brief Water simulation type
     */
    enum class WaterSimulationType : uint8
    {
        Simple,     ///< Simple sine wave (fastest)
        Gerstner,   ///< Gerstner waves (good balance)
        FFT         ///< FFT-based ocean simulation (most realistic)
    };

    /**
     * @brief Gerstner wave parameters
     */
    struct GerstnerWave
    {
        Vec2 direction{1.0f, 0.0f};     ///< Wave direction (normalized)
        float wavelength = 10.0f;       ///< Wavelength in meters
        float amplitude = 0.5f;         ///< Wave amplitude
        float speed = 1.0f;             ///< Wave speed multiplier
        float steepness = 0.5f;         ///< Wave steepness (0-1)
    };

    /**
     * @brief FFT ocean spectrum parameters
     */
    struct OceanSpectrumParams
    {
        float windSpeed = 10.0f;         ///< Wind speed (m/s)
        Vec2 windDirection{1.0f, 0.0f};  ///< Wind direction
        float fetch = 1000.0f;           ///< Fetch distance (wind travel distance)
        float spectrumScale = 1.0f;      ///< Spectrum amplitude scale
        float choppiness = 1.0f;         ///< Horizontal displacement scale
        float depth = 100.0f;            ///< Water depth (affects wave speed)
    };

    /**
     * @brief Water simulation configuration
     */
    struct WaterSimulationDesc
    {
        WaterSimulationType type = WaterSimulationType::Gerstner;
        uint32 resolution = 256;                     ///< Simulation resolution (power of 2)
        float domainSize = 100.0f;                   ///< Simulation domain size in meters
        std::vector<GerstnerWave> gerstnerWaves;     ///< Gerstner wave parameters
        OceanSpectrumParams oceanParams;             ///< FFT ocean parameters
    };

    /**
     * @brief Caustics rendering quality
     */
    enum class CausticsQuality : uint8
    {
        Off,        ///< Disabled
        Low,        ///< Simple projected texture
        Medium,     ///< Animated caustics
        High        ///< Ray-traced caustics
    };

    /**
     * @brief Caustics configuration
     */
    struct CausticsDesc
    {
        CausticsQuality quality = CausticsQuality::Medium;
        uint32 textureSize = 512;           ///< Caustics texture resolution
        float intensity = 1.0f;             ///< Caustics brightness
        float scale = 5.0f;                 ///< UV scale for caustics pattern
        float speed = 1.0f;                 ///< Animation speed
        float maxDepth = 20.0f;             ///< Maximum depth for caustics
        float focusFalloff = 0.5f;          ///< How fast caustics fade with depth
    };

    /**
     * @brief Underwater effect quality
     */
    enum class UnderwaterQuality : uint8
    {
        Off,        ///< Disabled
        Low,        ///< Simple tint
        Medium,     ///< Tint + blur
        High        ///< Full effects (god rays, distortion)
    };

    /**
     * @brief Underwater visual properties
     */
    struct UnderwaterProperties
    {
        Vec3 fogColor{0.0f, 0.15f, 0.25f};      ///< Underwater fog color
        float fogDensity = 0.05f;                ///< Fog density
        float fogStart = 0.0f;                   ///< Fog start distance
        float fogEnd = 100.0f;                   ///< Fog end distance

        Vec3 absorptionColor{1.0f, 0.5f, 0.2f};  ///< Color absorption rates (RGB)
        float absorptionScale = 0.1f;            ///< Absorption intensity

        float distortionStrength = 0.02f;        ///< Screen distortion amount
        float distortionSpeed = 1.0f;            ///< Distortion animation speed

        float blurAmount = 0.5f;                 ///< Blur intensity
        float blurFalloff = 0.1f;                ///< Blur distance falloff

        bool enableGodRays = true;               ///< Enable underwater god rays
        float godRayIntensity = 0.5f;            ///< God ray brightness
        float godRayDecay = 0.95f;               ///< God ray decay
        int godRaySamples = 64;                  ///< God ray sample count

        bool enableParticles = true;             ///< Enable floating particles
        float particleDensity = 100.0f;          ///< Particles per cubic meter
        float particleSize = 0.01f;              ///< Particle size
    };

    /**
     * @brief Underwater configuration
     */
    struct UnderwaterDesc
    {
        UnderwaterQuality quality = UnderwaterQuality::High;
        UnderwaterProperties properties;
    };

} // namespace RVX
