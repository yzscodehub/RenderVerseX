#pragma once

/**
 * @file WaterSimulation.h
 * @brief Water wave simulation systems
 * 
 * Provides CPU wave queries used by gameplay and Render snapshot extraction.
 */

#include "Water/WaterTypes.h"

#include <memory>
#include <vector>

namespace RVX
{
    /**
     * @brief Water wave simulation system
     * 
     * Simulates water surface waves on the CPU. Render-owned water passes are
     * responsible for any GPU simulation or textures.
     * 
     * Simulation Types:
     * - Simple: Basic sine waves, very fast
     * - Gerstner: Sum of Gerstner waves, good visual quality
     * - FFT: Currently uses a deterministic CPU fallback profile
     * 
     * Features:
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
     * 
     * // Per frame
     * simulation->Update(deltaTime);
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

        // =====================================================================
        // Simulation Control
        // =====================================================================

        /**
         * @brief Update simulation time
         * @param deltaTime Frame delta time
         */
        void Update(float deltaTime);

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

    private:
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
        bool m_spectrumDirty = true;
    };

} // namespace RVX
