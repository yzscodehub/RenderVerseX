#pragma once

/**
 * @file WaterSimulation.h
 * @brief Water wave simulation systems
 * 
 * Provides multiple wave simulation methods from simple sine waves
 * to full FFT-based ocean simulation.
 */

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include "RHI/RHITexture.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHIPipeline.h"

#include <memory>
#include <vector>

namespace RVX
{
    class IRHIDevice;
    class RHICommandContext;

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
        float wavelength = 10.0f;        ///< Wavelength in meters
        float amplitude = 0.5f;          ///< Wave amplitude
        float speed = 1.0f;              ///< Wave speed multiplier
        float steepness = 0.5f;          ///< Wave steepness (0-1)
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
     * @brief Water wave simulation system
     * 
     * Simulates water surface waves using different methods based on
     * quality/performance requirements.
     * 
     * Simulation Types:
     * - Simple: Basic sine waves, very fast
     * - Gerstner: Sum of Gerstner waves, good visual quality
     * - FFT: Phillips spectrum ocean simulation, most realistic
     * 
     * Features:
     * - GPU-accelerated simulation
     * - Displacement and normal map generation
     * - Multiple wave layers/cascades
     * - Time-based animation
     * 
     * Usage:
     * @code
     * WaterSimulationDesc desc;
     * desc.type = WaterSimulationType::FFT;
     * desc.resolution = 512;
     * desc.oceanParams.windSpeed = 15.0f;
     * 
     * auto simulation = std::make_unique<WaterSimulation>();
     * simulation->Initialize(desc);
     * simulation->InitializeGPU(device);
     * 
     * // Per frame
     * simulation->Update(deltaTime);
     * simulation->Dispatch(commandContext);
     * @endcode
     */
    class WaterSimulation
    {
    public:
        using Ptr = std::unique_ptr<WaterSimulation>;

        WaterSimulation() = default;
        ~WaterSimulation() = default;

        // Non-copyable
        WaterSimulation(const WaterSimulation&) = delete;
        WaterSimulation& operator=(const WaterSimulation&) = delete;

        // =====================================================================
        // Initialization
        // =====================================================================

        /**
         * @brief Initialize the simulation
         * @param desc Simulation descriptor
         * @return true if initialization succeeded
         */
        bool Initialize(const WaterSimulationDesc& desc);

        /**
         * @brief Initialize GPU resources
         * @param device RHI device
         * @return true if initialization succeeded
         */
        bool InitializeGPU(IRHIDevice* device);

        // =====================================================================
        // Simulation Control
        // =====================================================================

        /**
         * @brief Update simulation time
         * @param deltaTime Frame delta time
         */
        void Update(float deltaTime);

        /**
         * @brief Dispatch GPU simulation
         * @param ctx Command context
         */
        void Dispatch(RHICommandContext& ctx);

        /**
         * @brief Reset simulation to initial state
         */
        void Reset();

        /**
         * @brief Pause/resume simulation
         */
        void SetPaused(bool paused) { m_paused = paused; }
        bool IsPaused() const { return m_paused; }

        /**
         * @brief Set time scale
         */
        void SetTimeScale(float scale) { m_timeScale = scale; }
        float GetTimeScale() const { return m_timeScale; }

        // =====================================================================
        // Parameters
        // =====================================================================

        /**
         * @brief Get simulation type
         */
        WaterSimulationType GetType() const { return m_type; }

        /**
         * @brief Set wind parameters
         * @param direction Wind direction (normalized)
         * @param speed Wind speed (m/s)
         */
        void SetWind(const Vec2& direction, float speed);

        /**
         * @brief Add a Gerstner wave
         * @param wave Wave parameters
         */
        void AddGerstnerWave(const GerstnerWave& wave);

        /**
         * @brief Clear all Gerstner waves
         */
        void ClearGerstnerWaves();

        /**
         * @brief Set FFT ocean parameters
         */
        void SetOceanParams(const OceanSpectrumParams& params);
        const OceanSpectrumParams& GetOceanParams() const { return m_oceanParams; }

        // =====================================================================
        // Wave Queries (CPU fallback)
        // =====================================================================

        /**
         * @brief Sample wave height at position (CPU)
         * @param x Local X coordinate
         * @param z Local Z coordinate
         * @return Wave height
         */
        float SampleHeight(float x, float z) const;

        /**
         * @brief Sample displacement at position (CPU)
         * @param x Local X coordinate
         * @param z Local Z coordinate
         * @return 3D displacement
         */
        Vec3 SampleDisplacement(float x, float z) const;

        /**
         * @brief Sample normal at position (CPU)
         * @param x Local X coordinate
         * @param z Local Z coordinate
         * @return Surface normal
         */
        Vec3 SampleNormal(float x, float z) const;

        // =====================================================================
        // GPU Resources
        // =====================================================================

        /**
         * @brief Get displacement map texture
         */
        RHITexture* GetDisplacementMap() const { return m_displacementMap.Get(); }

        /**
         * @brief Get normal map texture
         */
        RHITexture* GetNormalMap() const { return m_normalMap.Get(); }

        /**
         * @brief Get foam map texture
         */
        RHITexture* GetFoamMap() const { return m_foamMap.Get(); }

        /**
         * @brief Check if GPU initialized
         */
        bool IsGPUInitialized() const { return m_gpuInitialized; }

    private:
        // Simulation methods
        void UpdateSimple(float time);
        void UpdateGerstner(float time);
        void UpdateFFT(float time);

        // FFT helpers
        void GenerateSpectrum();
        void PerformFFT(RHICommandContext& ctx);

        WaterSimulationType m_type = WaterSimulationType::Gerstner;
        uint32 m_resolution = 256;
        float m_domainSize = 100.0f;
        float m_time = 0.0f;
        float m_timeScale = 1.0f;
        bool m_paused = false;

        // Gerstner waves
        std::vector<GerstnerWave> m_gerstnerWaves;

        // FFT ocean
        OceanSpectrumParams m_oceanParams;

        // GPU resources
        RHITextureRef m_displacementMap;
        RHITextureRef m_normalMap;
        RHITextureRef m_foamMap;
        RHITextureRef m_spectrumTexture;
        RHITextureRef m_fftTempTexture;
        RHIBufferRef m_paramBuffer;
        RHIPipelineRef m_spectrumPipeline;
        RHIPipelineRef m_fftPipeline;
        RHIPipelineRef m_normalPipeline;

        bool m_gpuInitialized = false;
        bool m_spectrumDirty = true;
    };

} // namespace RVX
