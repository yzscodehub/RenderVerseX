/**
 * @file LightManager.cpp
 * @brief LightManager implementation
 */

#include "Render/Lighting/LightManager.h"
#include "Render/Renderer/RenderScene.h"
#include "Core/Log.h"

namespace RVX
{

LightManager::~LightManager()
{
    Shutdown();
}

void LightManager::Initialize(IRHIDevice* device)
{
    if (m_device)
    {
        RVX_CORE_WARN("LightManager: Already initialized");
        return;
    }

    m_device = device;
    m_pointLights.reserve(MaxPointLights);
    m_spotLights.reserve(MaxSpotLights);

    EnsureBuffers();

    RVX_CORE_DEBUG("LightManager: Initialized");
}

void LightManager::Shutdown()
{
    if (!m_device)
        return;

    m_lightConstantsBuffer.Reset();
    m_pointLightsBuffer.Reset();
    m_spotLightsBuffer.Reset();
    m_pointLights.clear();
    m_spotLights.clear();
    m_device = nullptr;

    RVX_CORE_DEBUG("LightManager: Shutdown");
}

void LightManager::EnsureBuffers()
{
    if (!m_device)
        return;

    // Light constants buffer
    {
        RHIBufferDesc desc;
        desc.size = sizeof(LightConstants);
        desc.usage = RHIBufferUsage::Constant;
        desc.memoryType = RHIMemoryType::Upload;
        desc.debugName = "LightConstantsBuffer";
        m_lightConstantsBuffer = m_device->CreateBuffer(desc);
    }

    // Point lights structured buffer
    {
        RHIBufferDesc desc;
        desc.size = sizeof(GPUPointLight) * MaxPointLights;
        desc.usage = RHIBufferUsage::ShaderResource;
        desc.memoryType = RHIMemoryType::Upload;
        desc.stride = sizeof(GPUPointLight);
        desc.debugName = "PointLightsBuffer";
        m_pointLightsBuffer = m_device->CreateBuffer(desc);
    }

    // Spot lights structured buffer
    {
        RHIBufferDesc desc;
        desc.size = sizeof(GPUSpotLight) * MaxSpotLights;
        desc.usage = RHIBufferUsage::ShaderResource;
        desc.memoryType = RHIMemoryType::Upload;
        desc.stride = sizeof(GPUSpotLight);
        desc.debugName = "SpotLightsBuffer";
        m_spotLightsBuffer = m_device->CreateBuffer(desc);
    }
}

void LightManager::CollectLights(const RenderScene& scene)
{
    Clear();

    for (size_t i = 0; i < scene.GetLightCount(); ++i)
    {
        const RenderLight& light = scene.GetLight(i);

        switch (light.type)
        {
        case RenderLight::Type::Directional:
            SetMainLight(light.direction, light.color, light.intensity);
            break;
        case RenderLight::Type::Point:
            AddPointLight(light.position, light.color, light.intensity, light.range);
            break;
        case RenderLight::Type::Spot:
            AddSpotLight(light.position, light.direction, light.color,
                        light.intensity, light.range, light.innerConeAngle, light.outerConeAngle);
            break;
        }
    }
}

void LightManager::SetMainLight(const Vec3& direction, const Vec3& color, float intensity)
{
    m_mainLight.direction = direction;
    m_mainLight.color = color;
    m_mainLight.intensity = intensity;
}

void LightManager::AddPointLight(const Vec3& position, const Vec3& color, float intensity, float range)
{
    if (m_pointLights.size() >= MaxPointLights)
    {
        RVX_CORE_WARN("LightManager: Max point lights reached");
        return;
    }

    GPUPointLight light;
    light.position = position;
    light.color = color;
    light.intensity = intensity;
    light.range = range;
    m_pointLights.push_back(light);
}

void LightManager::AddSpotLight(const Vec3& position, const Vec3& direction, const Vec3& color,
                                float intensity, float range, float innerAngle, float outerAngle)
{
    if (m_spotLights.size() >= MaxSpotLights)
    {
        RVX_CORE_WARN("LightManager: Max spot lights reached");
        return;
    }

    GPUSpotLight light;
    light.position = position;
    light.direction = direction;
    light.color = color;
    light.intensity = intensity;
    light.range = range;
    light.innerConeAngle = std::cos(innerAngle);
    light.outerConeAngle = std::cos(outerAngle);
    m_spotLights.push_back(light);
}

void LightManager::Clear()
{
    m_pointLights.clear();
    m_spotLights.clear();
    m_mainLight = GPUDirectionalLight{};
}

void LightManager::UpdateGPUBuffers()
{
    // Update light constants
    if (m_lightConstantsBuffer)
    {
        LightConstants constants;
        constants.mainLight = m_mainLight;
        constants.numPointLights = static_cast<uint32>(m_pointLights.size());
        constants.numSpotLights = static_cast<uint32>(m_spotLights.size());

        void* data = m_lightConstantsBuffer->Map();
        if (data)
        {
            memcpy(data, &constants, sizeof(LightConstants));
            m_lightConstantsBuffer->Unmap();
        }
    }

    // Update point lights buffer
    if (m_pointLightsBuffer && !m_pointLights.empty())
    {
        void* data = m_pointLightsBuffer->Map();
        if (data)
        {
            memcpy(data, m_pointLights.data(), m_pointLights.size() * sizeof(GPUPointLight));
            m_pointLightsBuffer->Unmap();
        }
    }

    // Update spot lights buffer
    if (m_spotLightsBuffer && !m_spotLights.empty())
    {
        void* data = m_spotLightsBuffer->Map();
        if (data)
        {
            memcpy(data, m_spotLights.data(), m_spotLights.size() * sizeof(GPUSpotLight));
            m_spotLightsBuffer->Unmap();
        }
    }
}

} // namespace RVX
