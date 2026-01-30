/**
 * @file WaterSimulation.cpp
 * @brief Implementation of water wave simulation
 */

#include "Water/WaterSimulation.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHICommandContext.h"
#include "Core/Log.h"

#include <cmath>
#include <algorithm>

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

    RVX_CORE_INFO("WaterSimulation: Initialized {} simulation at {}x{} resolution",
                  m_type == WaterSimulationType::FFT ? "FFT" :
                  m_type == WaterSimulationType::Gerstner ? "Gerstner" : "Simple",
                  m_resolution, m_resolution);
    return true;
}

bool WaterSimulation::InitializeGPU(IRHIDevice* device)
{
    if (!device)
    {
        RVX_CORE_ERROR("WaterSimulation: Invalid device");
        return false;
    }

    // Create displacement map
    RHITextureDesc dispDesc;
    dispDesc.width = m_resolution;
    dispDesc.height = m_resolution;
    dispDesc.format = RHIFormat::RGBA16_FLOAT;
    dispDesc.usage = RHITextureUsage::ShaderResource | RHITextureUsage::UnorderedAccess;
    dispDesc.dimension = RHITextureDimension::Texture2D;
    dispDesc.debugName = "WaterDisplacement";

    m_displacementMap = device->CreateTexture(dispDesc);
    if (!m_displacementMap)
    {
        RVX_CORE_ERROR("WaterSimulation: Failed to create displacement map");
        return false;
    }

    // Create normal map
    RHITextureDesc normalDesc;
    normalDesc.width = m_resolution;
    normalDesc.height = m_resolution;
    normalDesc.format = RHIFormat::RGBA8_UNORM;
    normalDesc.usage = RHITextureUsage::ShaderResource | RHITextureUsage::UnorderedAccess;
    normalDesc.dimension = RHITextureDimension::Texture2D;
    normalDesc.debugName = "WaterNormal";

    m_normalMap = device->CreateTexture(normalDesc);
    if (!m_normalMap)
    {
        RVX_CORE_ERROR("WaterSimulation: Failed to create normal map");
        return false;
    }

    // Create foam map
    RHITextureDesc foamDesc;
    foamDesc.width = m_resolution;
    foamDesc.height = m_resolution;
    foamDesc.format = RHIFormat::R16_FLOAT;
    foamDesc.usage = RHITextureUsage::ShaderResource | RHITextureUsage::UnorderedAccess;
    foamDesc.dimension = RHITextureDimension::Texture2D;
    foamDesc.debugName = "WaterFoam";

    m_foamMap = device->CreateTexture(foamDesc);
    if (!m_foamMap)
    {
        RVX_CORE_ERROR("WaterSimulation: Failed to create foam map");
        return false;
    }

    // Create parameter buffer
    RHIBufferDesc paramDesc;
    paramDesc.size = 256;  // Enough for simulation parameters
    paramDesc.usage = RHIBufferUsage::Constant;
    paramDesc.memoryType = RHIMemoryType::Upload;
    paramDesc.debugName = "WaterSimParams";

    m_paramBuffer = device->CreateBuffer(paramDesc);
    if (!m_paramBuffer)
    {
        RVX_CORE_ERROR("WaterSimulation: Failed to create parameter buffer");
        return false;
    }

    // FFT-specific resources
    if (m_type == WaterSimulationType::FFT)
    {
        RHITextureDesc specDesc;
        specDesc.width = m_resolution;
        specDesc.height = m_resolution;
        specDesc.format = RHIFormat::RGBA32_FLOAT;  // Complex numbers
        specDesc.usage = RHITextureUsage::ShaderResource | RHITextureUsage::UnorderedAccess;
        specDesc.dimension = RHITextureDimension::Texture2D;
        specDesc.debugName = "WaterSpectrum";

        m_spectrumTexture = device->CreateTexture(specDesc);
        m_fftTempTexture = device->CreateTexture(specDesc);
    }

    m_gpuInitialized = true;
    RVX_CORE_INFO("WaterSimulation: GPU resources initialized");
    return true;
}

void WaterSimulation::Update(float deltaTime)
{
    if (m_paused) return;

    m_time += deltaTime * m_timeScale;
}

void WaterSimulation::Dispatch(RHICommandContext& ctx)
{
    if (!m_gpuInitialized) return;

    switch (m_type)
    {
    case WaterSimulationType::Simple:
        // Simple waves can be done entirely in vertex shader
        break;

    case WaterSimulationType::Gerstner:
        // Gerstner waves - update displacement map
        // This would dispatch a compute shader
        break;

    case WaterSimulationType::FFT:
        // FFT simulation
        if (m_spectrumDirty)
        {
            GenerateSpectrum();
            m_spectrumDirty = false;
        }
        PerformFFT(ctx);
        break;
    }
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
            float Q = wave.steepness / (k * wave.amplitude);

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
        // FFT displacement would be read from GPU texture
        // This is a CPU fallback
        displacement = SampleDisplacement(x, z);  // Would sample texture
        break;
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

void WaterSimulation::GenerateSpectrum()
{
    // Generate Phillips spectrum for FFT ocean
    // This would be done on GPU via compute shader
    RVX_CORE_INFO("WaterSimulation: Generating ocean spectrum");
}

void WaterSimulation::PerformFFT(RHICommandContext& ctx)
{
    (void)ctx;
    // Perform inverse FFT to generate displacement map
    // This would dispatch FFT compute passes
}

} // namespace RVX
