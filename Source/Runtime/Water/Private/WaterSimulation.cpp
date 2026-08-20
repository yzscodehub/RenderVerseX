/**
 * @file WaterSimulation.cpp
 * @brief Implementation of CPU water wave simulation
 */

#include "Water/WaterSimulation.h"
#include "Core/Log.h"

#include <algorithm>
#include <cmath>

namespace RVX
{

bool WaterSimulation::Initialize(const WaterSimulationDesc& desc)
{
    m_type = desc.type;
    m_resolution = desc.resolution;
    m_domainSize = desc.domainSize;
    m_gerstnerWaves = desc.gerstnerWaves;
    m_oceanParams = desc.oceanParams;
    m_time = 0.0f;
    m_spectrumDirty = true;

    // Add default waves if none specified
    if (m_type == WaterSimulationType::Gerstner && m_gerstnerWaves.empty())
    {
        GerstnerWave wave;
        wave.direction = Vec2(1.0f, 0.0f);
        wave.wavelength = 20.0f;
        wave.amplitude = 1.0f;
        wave.steepness = 0.5f;
        m_gerstnerWaves.push_back(wave);
    }

    RVX_CORE_INFO("WaterSimulation: Initialized CPU {} simulation at {}x{} resolution",
                  m_type == WaterSimulationType::FFT ? "FFT" :
                  m_type == WaterSimulationType::Gerstner ? "Gerstner" : "Simple",
                  m_resolution, m_resolution);
    return true;
}

void WaterSimulation::Update(float deltaTime)
{
    if (m_paused) return;

    m_time += deltaTime * m_timeScale;
}

void WaterSimulation::Reset()
{
    m_time = 0.0f;
    m_spectrumDirty = true;
}

void WaterSimulation::SetWind(const Vec2& direction, float speed)
{
    m_oceanParams.windDirection = normalize(direction);
    m_oceanParams.windSpeed = speed;
    m_spectrumDirty = true;
}

void WaterSimulation::AddGerstnerWave(const GerstnerWave& wave)
{
    m_gerstnerWaves.push_back(wave);
}

void WaterSimulation::ClearGerstnerWaves()
{
    m_gerstnerWaves.clear();
}

void WaterSimulation::SetOceanParams(const OceanSpectrumParams& params)
{
    m_oceanParams = params;
    m_spectrumDirty = true;
}

float WaterSimulation::SampleHeight(float x, float z) const
{
    return SampleDisplacement(x, z).y;
}

Vec3 WaterSimulation::SampleDisplacement(float x, float z) const
{
    Vec3 displacement(0.0f);

    switch (m_type)
    {
    case WaterSimulationType::Simple:
    {
        // Simple sine wave
        float k = 2.0f * 3.14159f / 20.0f;
        float omega = std::sqrt(9.81f * k);
        displacement.y = std::sin(k * x - omega * m_time) * 0.5f;
        displacement.y += std::sin(k * z * 0.7f - omega * m_time * 0.8f) * 0.3f;
        break;
    }

    case WaterSimulationType::Gerstner:
    {
        for (const auto& wave : m_gerstnerWaves)
        {
            Vec2 dir = normalize(wave.direction);
            float k = 2.0f * 3.14159f / wave.wavelength;
            float omega = std::sqrt(9.81f * k);
            float amplitude = std::max(std::abs(wave.amplitude), 0.001f);
            float Q = wave.steepness / (k * amplitude);

            float phase = k * (dir.x * x + dir.y * z) - omega * m_time * wave.speed;
            float s = std::sin(phase);
            float c = std::cos(phase);

            displacement.x += Q * wave.amplitude * dir.x * c;
            displacement.y += wave.amplitude * s;
            displacement.z += Q * wave.amplitude * dir.y * c;
        }
        break;
    }

    case WaterSimulationType::FFT:
    {
        Vec2 dir = normalize(m_oceanParams.windDirection);
        if (std::abs(dir.x) < 0.001f && std::abs(dir.y) < 0.001f)
        {
            dir = Vec2(1.0f, 0.0f);
        }

        const float windScale = std::max(m_oceanParams.windSpeed, 1.0f) / 10.0f;
        const float amplitude = std::max(m_oceanParams.spectrumScale * windScale * 0.35f, 0.05f);
        const float primaryWavelength = std::max(m_domainSize * 0.18f, 8.0f);
        const float secondaryWavelength = std::max(primaryWavelength * 0.47f, 4.0f);

        const float primaryK = 2.0f * 3.14159f / primaryWavelength;
        const float secondaryK = 2.0f * 3.14159f / secondaryWavelength;
        const float primaryPhase = primaryK * (dir.x * x + dir.y * z) - m_time * (0.8f + windScale);
        const Vec2 crossDir(-dir.y, dir.x);
        const float secondaryPhase =
            secondaryK * (crossDir.x * x + crossDir.y * z) - m_time * (0.5f + windScale * 0.5f);

        displacement.y = std::sin(primaryPhase) * amplitude;
        displacement.y += std::sin(secondaryPhase) * amplitude * 0.35f;
        displacement.x = std::cos(primaryPhase) * amplitude * m_oceanParams.choppiness * dir.x * 0.15f;
        displacement.z = std::cos(primaryPhase) * amplitude * m_oceanParams.choppiness * dir.y * 0.15f;
        break;
    }
    }

    return displacement;
}

Vec3 WaterSimulation::SampleNormal(float x, float z) const
{
    // Central difference for normal calculation
    const float epsilon = 0.1f;

    Vec3 dX = SampleDisplacement(x + epsilon, z) - SampleDisplacement(x - epsilon, z);
    Vec3 dZ = SampleDisplacement(x, z + epsilon) - SampleDisplacement(x, z - epsilon);

    Vec3 tangentX = Vec3(2.0f * epsilon, dX.y, 0.0f);
    Vec3 tangentZ = Vec3(0.0f, dZ.y, 2.0f * epsilon);

    return normalize(cross(tangentZ, tangentX));
}

} // namespace RVX
