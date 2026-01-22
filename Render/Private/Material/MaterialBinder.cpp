/**
 * @file MaterialBinder.cpp
 * @brief MaterialBinder implementation
 */

#include "Render/Material/MaterialBinder.h"
#include "Render/GPUResourceManager.h"
#include "Core/Log.h"

namespace RVX
{

MaterialBinder::~MaterialBinder()
{
    Shutdown();
}

void MaterialBinder::Initialize(IRHIDevice* device, GPUResourceManager* gpuResources)
{
    if (m_device)
    {
        RVX_CORE_WARN("MaterialBinder: Already initialized");
        return;
    }

    m_device = device;
    m_gpuResources = gpuResources;
    m_defaultConstants = GetDefaultConstants();

    EnsureConstantBuffer();

    RVX_CORE_DEBUG("MaterialBinder: Initialized");
}

void MaterialBinder::Shutdown()
{
    if (!m_device)
        return;

    m_constantBuffer.Reset();
    m_device = nullptr;
    m_gpuResources = nullptr;
    m_currentMaterialId = 0;

    RVX_CORE_DEBUG("MaterialBinder: Shutdown");
}

void MaterialBinder::EnsureConstantBuffer()
{
    if (m_constantBuffer)
        return;

    RHIBufferDesc desc;
    desc.size = sizeof(MaterialGPUConstants);
    desc.usage = RHIBufferUsage::Constant;
    desc.memoryType = RHIMemoryType::Upload;
    desc.debugName = "MaterialConstantBuffer";

    m_constantBuffer = m_device->CreateBuffer(desc);
    if (!m_constantBuffer)
    {
        RVX_CORE_ERROR("MaterialBinder: Failed to create constant buffer");
    }
}

void MaterialBinder::UpdateConstantBuffer(const MaterialGPUConstants& constants)
{
    if (!m_constantBuffer)
        return;

    void* mappedData = m_constantBuffer->Map();
    if (mappedData)
    {
        memcpy(mappedData, &constants, sizeof(MaterialGPUConstants));
        m_constantBuffer->Unmap();
    }
}

void MaterialBinder::Bind(RHICommandContext& ctx, const Material& material, uint32 setIndex)
{
    (void)ctx;
    (void)setIndex;

    MaterialGPUConstants constants = ConvertToGPU(material);
    UpdateConstantBuffer(constants);

    // Bind constant buffer
    // Note: Actual binding depends on pipeline layout
    // ctx.SetConstantBuffer(setIndex, 0, m_constantBuffer.Get());

    // Bind textures based on material
    // This would use GPUResourceManager to get texture views
    // and bind them to appropriate slots
}

void MaterialBinder::Bind(RHICommandContext& ctx, uint64 materialId, uint32 setIndex)
{
    (void)ctx;
    (void)materialId;
    (void)setIndex;

    // TODO: Look up material by ID and bind
    // For now, bind default
    BindDefault(ctx, setIndex);
}

void MaterialBinder::BindDefault(RHICommandContext& ctx, uint32 setIndex)
{
    (void)ctx;
    (void)setIndex;

    UpdateConstantBuffer(m_defaultConstants);
    // ctx.SetConstantBuffer(setIndex, 0, m_constantBuffer.Get());
}

MaterialGPUConstants MaterialBinder::ConvertToGPU(const Material& material)
{
    (void)material;
    
    // TODO: Convert Material class properties to GPU constants
    // This requires access to the Material class definition
    
    MaterialGPUConstants constants;
    constants.baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f};
    constants.metallicFactor = 0.0f;
    constants.roughnessFactor = 0.5f;
    constants.normalScale = 1.0f;
    constants.occlusionStrength = 1.0f;
    constants.emissiveColor = {0.0f, 0.0f, 0.0f};
    constants.emissiveStrength = 0.0f;
    constants.textureFlags = 0;

    return constants;
}

MaterialGPUConstants MaterialBinder::GetDefaultConstants()
{
    MaterialGPUConstants constants;
    constants.baseColorFactor = {0.8f, 0.8f, 0.8f, 1.0f};
    constants.metallicFactor = 0.0f;
    constants.roughnessFactor = 0.5f;
    constants.normalScale = 1.0f;
    constants.occlusionStrength = 1.0f;
    constants.emissiveColor = {0.0f, 0.0f, 0.0f};
    constants.emissiveStrength = 0.0f;
    constants.textureFlags = 0;

    return constants;
}

} // namespace RVX
