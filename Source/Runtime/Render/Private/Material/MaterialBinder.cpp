/**
 * @file MaterialBinder.cpp
 * @brief MaterialBinder implementation
 */

#include "Render/Material/MaterialBinder.h"
#include "Render/Material/MaterialGPUEncoding.h"
#include "Core/Log.h"
#include "Resources/RenderResourceRegistry.h"

#include <cstring>
#include <utility>

namespace RVX
{

MaterialBinder::~MaterialBinder()
{
    Shutdown();
}

void MaterialBinder::Initialize(
    IRHIDevice* device,
    const RenderResourceRegistry* resourceRegistry)
{
    if (m_device)
    {
        RVX_CORE_WARN("MaterialBinder: Already initialized");
        return;
    }

    if (!device)
    {
        RVX_CORE_ERROR("MaterialBinder: Cannot initialize without an RHI device");
        SetBindResult(MaterialBindStatus::Error, "Cannot initialize MaterialBinder without an RHI device");
        return;
    }

    m_device = device;
    m_resourceRegistry = resourceRegistry;
    m_defaultConstants = GetDefaultConstants();

    if (!EnsureConstantBuffer())
    {
        m_device = nullptr;
        m_resourceRegistry = nullptr;
        return;
    }

    SetBindResult(MaterialBindStatus::None, {});
    RVX_CORE_DEBUG("MaterialBinder: Initialized");
}

void MaterialBinder::Shutdown()
{
    if (!m_device)
        return;

    m_constantBuffer.Reset();
    m_device = nullptr;
    m_resourceRegistry = nullptr;
    m_currentMaterialId = 0;
    m_lastBindStatus = MaterialBindStatus::None;
    m_lastBindMessage.clear();

    RVX_CORE_DEBUG("MaterialBinder: Shutdown");
}

bool MaterialBinder::EnsureConstantBuffer()
{
    if (m_constantBuffer)
        return true;

    if (!m_device)
    {
        SetBindResult(MaterialBindStatus::Error, "Cannot create material constant buffer without an RHI device");
        return false;
    }

    RHIBufferDesc desc;
    desc.size = sizeof(MaterialGPUConstants);
    desc.usage = RHIBufferUsage::Constant;
    desc.memoryType = RHIMemoryType::Upload;
    desc.debugName = "MaterialConstantBuffer";

    m_constantBuffer = m_device->CreateBuffer(desc);
    if (!m_constantBuffer)
    {
        SetBindResult(MaterialBindStatus::Error, "Failed to create material constant buffer");
        RVX_CORE_ERROR("MaterialBinder: {}", m_lastBindMessage);
        return false;
    }

    return true;
}

bool MaterialBinder::UpdateConstantBuffer(const MaterialGPUConstants& constants)
{
    if (!EnsureConstantBuffer())
    {
        return false;
    }

    void* mappedData = m_constantBuffer->Map();
    if (!mappedData)
    {
        SetBindResult(MaterialBindStatus::Error, "Failed to map material constant buffer");
        RVX_CORE_ERROR("MaterialBinder: {}", m_lastBindMessage);
        return false;
    }

    std::memcpy(mappedData, &constants, sizeof(MaterialGPUConstants));
    m_constantBuffer->Unmap();
    return true;
}

void MaterialBinder::Bind(RHICommandContext& ctx, const MaterialSourceData& material, uint32 setIndex)
{
    (void)ctx;
    (void)setIndex;

    if (!m_device || !m_constantBuffer)
    {
        RVX_CORE_ERROR("MaterialBinder: Cannot bind material before successful initialization");
        SetBindResult(MaterialBindStatus::Error, "Cannot bind material before successful initialization");
        return;
    }

    MaterialGPUConstants constants = ConvertToGPU(material);
    if (!UpdateConstantBuffer(constants))
    {
        return;
    }

    SetBindResult(MaterialBindStatus::Unsupported,
                  "Material constants were updated, but descriptor/pipeline binding is not wired until R5b");
    RVX_CORE_WARN("MaterialBinder: {}", m_lastBindMessage);

    // Bind constant buffer
    // Note: Actual binding depends on pipeline layout
    // ctx.SetConstantBuffer(setIndex, 0, m_constantBuffer.Get());

    // Texture descriptors are owned by MaterialSystem and the exact registry.
}

void MaterialBinder::Bind(RHICommandContext& ctx, uint64 materialId, uint32 setIndex)
{
    (void)ctx;
    (void)materialId;
    (void)setIndex;

    RVX_CORE_WARN("MaterialBinder: Binding material by ID is not implemented; using explicit default fallback");
    BindDefault(ctx, setIndex);
}

void MaterialBinder::Bind(RHICommandContext& ctx,
                          RenderResourceHandle material,
                          uint32 setIndex)
{
    (void)ctx;
    (void)setIndex;
    const RenderMaterialResourceData* materialData =
        m_resourceRegistry ? m_resourceRegistry->ResolveMaterial(material)
                           : nullptr;
    if (materialData == nullptr || !materialData->metadataValid)
    {
        BindDefault(ctx, setIndex);
        return;
    }
    if (!m_device || !m_constantBuffer)
    {
        SetBindResult(
            MaterialBindStatus::Error,
            "Cannot bind material before successful initialization");
        return;
    }

    const MaterialGPUConstants constants = ConvertToGPU(
        materialData->staticDescriptor,
        materialData->sourceData,
        materialData->textureBindings,
        material.slot);
    if (!UpdateConstantBuffer(constants))
    {
        return;
    }
    SetBindResult(
        MaterialBindStatus::Unsupported,
        "Material constants were updated, but descriptor/pipeline binding is not wired until R5b");
}

void MaterialBinder::BindDefault(RHICommandContext& ctx, uint32 setIndex)
{
    (void)ctx;
    (void)setIndex;

    if (!m_device || !m_constantBuffer)
    {
        RVX_CORE_ERROR("MaterialBinder: Cannot bind default material before successful initialization");
        SetBindResult(MaterialBindStatus::Error, "Cannot bind default material before successful initialization");
        return;
    }

    if (!UpdateConstantBuffer(m_defaultConstants))
    {
        return;
    }

    SetBindResult(MaterialBindStatus::BoundDefaultMaterial, "Explicit default material fallback bound");
    // ctx.SetConstantBuffer(setIndex, 0, m_constantBuffer.Get());
}

MaterialGPUConstants MaterialBinder::ConvertToGPU(const MaterialSourceData& material)
{
    return EncodeMaterialGPUConstants(material);
}

MaterialGPUConstants MaterialBinder::ConvertToGPU(
    const MaterialSourceData& material,
    std::span<const MaterialUploadTextureBinding> textureBindings)
{
    return EncodeMaterialGPUConstants(material, textureBindings);
}

MaterialGPUConstants MaterialBinder::ConvertToGPU(
    const MaterialStaticDescriptor& staticDescriptor,
    const MaterialSourceData& material,
    std::span<const MaterialUploadTextureBinding> textureBindings,
    uint32 materialTableIndex)
{
    return EncodeMaterialGPUConstants(
        staticDescriptor, material, textureBindings, materialTableIndex);
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
    constants.shadingModel = static_cast<uint32>(
        MaterialGPUShadingModel::StandardLit);
    constants.materialTableIndex = 0;

    return constants;
}

void MaterialBinder::SetBindResult(MaterialBindStatus status, std::string message)
{
    m_lastBindStatus = status;
    m_lastBindMessage = std::move(message);
}

} // namespace RVX
