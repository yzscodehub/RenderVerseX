/**
 * @file MaterialSystem.cpp
 * @brief MaterialSystem implementation
 */

#include "Render/Material/MaterialSystem.h"
#include "Core/Log.h"
#include "Render/GPUResourceManager.h"
#include "Render/GPUUploadService.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/TextureResource.h"
#include "Scene/Material.h"

#include <cstring>
#include <utility>

namespace RVX
{

namespace
{
    constexpr uint64 RVX_CONSTANT_BUFFER_ALIGNMENT = 256;
    constexpr uint64 RVX_MAX_MATERIAL_CONSTANTS_PER_FRAME = 8192;

    void HashCombine(size_t& seed, size_t value)
    {
        seed ^= value + 0x9e3779b9u + (seed << 6) + (seed >> 2);
    }
} // namespace

MaterialSystem::MaterialSystem() = default;

MaterialSystem::~MaterialSystem()
{
    Shutdown();
}

bool MaterialSystem::Initialize(IRHIDevice* device, GPUResourceManager* gpuResources,
                                RHIDescriptorSetLayout* materialSetLayout)
{
    if (m_initialized)
    {
        RVX_CORE_WARN("MaterialSystem already initialized");
        return true;
    }

    if (!device || !gpuResources || !materialSetLayout)
    {
        RVX_CORE_ERROR("MaterialSystem: Invalid initialization parameters");
        return false;
    }

    m_device = device;
    m_gpuResources = gpuResources;
    m_materialSetLayout = materialSetLayout;

    if (!CreateConstantBuffer())
    {
        RVX_CORE_ERROR("MaterialSystem: Failed to create material constant buffer");
        return false;
    }

    if (!CreateDefaultResources())
    {
        RVX_CORE_ERROR("MaterialSystem: Failed to create default material resources");
        return false;
    }

    ResolvedMaterialTextures defaultTextures;
    defaultTextures.baseColor = m_defaultWhiteTextureView.Get();
    defaultTextures.normal = m_defaultNormalTextureView.Get();
    defaultTextures.metallicRoughness = m_defaultWhiteTextureView.Get();
    defaultTextures.occlusion = m_defaultWhiteTextureView.Get();
    defaultTextures.emissive = m_defaultBlackTextureView.Get();

    m_defaultMaterialSet = CreateMaterialDescriptorSet(defaultTextures);
    if (!m_defaultMaterialSet)
    {
        RVX_CORE_ERROR("MaterialSystem: Failed to create default material descriptor set");
        return false;
    }

    m_initialized = true;
    RVX_CORE_DEBUG("MaterialSystem initialized");
    return true;
}

void MaterialSystem::Shutdown()
{
    if (!m_initialized)
        return;

    m_materialDescriptorCache.clear();
    m_defaultMaterialSet.Reset();
    m_defaultSampler.Reset();
    m_defaultWhiteTextureView.Reset();
    m_defaultNormalTextureView.Reset();
    m_defaultBlackTextureView.Reset();
    m_defaultWhiteTexture.Reset();
    m_defaultNormalTexture.Reset();
    m_defaultBlackTexture.Reset();
    m_materialConstantBuffer.Reset();
    m_materialSetLayout = nullptr;
    m_gpuResources = nullptr;
    m_device = nullptr;
    m_initialized = false;

    RVX_CORE_DEBUG("MaterialSystem shutdown");
}

void MaterialSystem::BeginFrame()
{
    m_materialConstantCursor = 0;
    m_currentMaterialConstantOffset = 0;
    m_lastBindingResult = {};
}

MaterialBindingResult MaterialSystem::PrepareMaterialBinding(const Resource::MaterialResource* materialResource,
                                                             ResourceViewCache* viewCache)
{
    if (!m_initialized)
    {
        MaterialBindingResult result;
        result.status = MaterialBindingStatus::NotInitialized;
        result.message = "MaterialSystem is not initialized";
        return SetLastBindingResult(std::move(result));
    }

    if (!m_materialConstantBuffer)
    {
        MaterialBindingResult result;
        result.status = MaterialBindingStatus::Unavailable;
        result.message = "Material constant buffer is unavailable";
        return SetLastBindingResult(std::move(result));
    }

    const ResolvedMaterialTextures textures = ResolveMaterialTextures(materialResource, viewCache);
    const MaterialGPUConstants constants = BuildConstants(materialResource, textures);

    void* mapped = m_materialConstantBuffer->Map();
    if (!mapped)
    {
        MaterialBindingResult result;
        result.status = MaterialBindingStatus::Error;
        result.usedFallback = textures.usedFallback;
        result.message = "Failed to map material constant buffer";
        return SetLastBindingResult(std::move(result));
    }

    const uint64 offset = AllocateMaterialConstantSlot();
    std::memcpy(static_cast<uint8*>(mapped) + offset, &constants, sizeof(MaterialGPUConstants));
    m_materialConstantBuffer->Unmap();

    MaterialSetResolveResult setResult = GetOrCreateMaterialSetForResolved(textures);
    if (!setResult.descriptorSet)
    {
        MaterialBindingResult result;
        result.status = setResult.status == MaterialBindingStatus::None ? MaterialBindingStatus::Error
                                                                        : setResult.status;
        result.constantsUpdated = true;
        result.usedFallback = textures.usedFallback || setResult.usedFallback;
        result.message = setResult.message.empty() ? "Material descriptor set is unavailable"
                                                   : std::move(setResult.message);
        return SetLastBindingResult(std::move(result));
    }

    MaterialBindingResult result;
    result.status = (textures.usedFallback || setResult.usedFallback) ? MaterialBindingStatus::Fallback
                                                                      : MaterialBindingStatus::Ready;
    result.descriptorSet = setResult.descriptorSet;
    result.dynamicOffsets = GetCurrentMaterialDynamicOffset();
    result.constantsUpdated = true;
    result.usedFallback = textures.usedFallback || setResult.usedFallback;
    if (result.status == MaterialBindingStatus::Fallback)
    {
        result.message = setResult.message.empty() ? "Material binding used explicit fallback resources"
                                                   : std::move(setResult.message);
    }
    else
    {
        result.message = "Material binding ready";
    }

    return SetLastBindingResult(std::move(result));
}

bool MaterialSystem::UpdateMaterialConstants(const Resource::MaterialResource* materialResource,
                                             ResourceViewCache* viewCache)
{
    const MaterialBindingResult result = PrepareMaterialBinding(materialResource, viewCache);
    return result.constantsUpdated && !result.IsError();
}

RHIDescriptorSet* MaterialSystem::GetOrCreateMaterialSet(const Resource::MaterialResource* materialResource,
                                                         ResourceViewCache* viewCache)
{
    if (!m_initialized)
    {
        MaterialBindingResult result;
        result.status = MaterialBindingStatus::NotInitialized;
        result.message = "MaterialSystem is not initialized";
        SetLastBindingResult(std::move(result));
        return nullptr;
    }

    const ResolvedMaterialTextures textures = ResolveMaterialTextures(materialResource, viewCache);
    MaterialSetResolveResult setResult = GetOrCreateMaterialSetForResolved(textures);

    MaterialBindingResult result;
    result.status = setResult.descriptorSet
        ? ((textures.usedFallback || setResult.usedFallback) ? MaterialBindingStatus::Fallback
                                                             : MaterialBindingStatus::Ready)
        : (setResult.status == MaterialBindingStatus::None ? MaterialBindingStatus::Error : setResult.status);
    result.descriptorSet = setResult.descriptorSet;
    result.dynamicOffsets = GetCurrentMaterialDynamicOffset();
    result.usedFallback = textures.usedFallback || setResult.usedFallback;
    result.message = setResult.message.empty()
        ? (result.usedFallback ? "Material descriptor used explicit fallback resources"
                               : "Material descriptor ready")
        : std::move(setResult.message);
    SetLastBindingResult(std::move(result));

    return setResult.descriptorSet;
}

RHIDescriptorSet* MaterialSystem::GetDefaultMaterialSet()
{
    return m_defaultMaterialSet.Get();
}

std::array<uint32, 1> MaterialSystem::GetCurrentMaterialDynamicOffset() const
{
    return {ToRHIConstantDynamicOffset(m_currentMaterialConstantOffset)};
}

uint64 MaterialSystem::AlignConstantBufferSize(uint64 size)
{
    return (size + RVX_CONSTANT_BUFFER_ALIGNMENT - 1) & ~(RVX_CONSTANT_BUFFER_ALIGNMENT - 1);
}

size_t MaterialSystem::MaterialDescriptorKeyHash::operator()(const MaterialDescriptorKey& key) const
{
    size_t seed = 0;
    HashCombine(seed, std::hash<RHITextureView*>{}(key.baseColor));
    HashCombine(seed, std::hash<RHITextureView*>{}(key.normal));
    HashCombine(seed, std::hash<RHITextureView*>{}(key.metallicRoughness));
    HashCombine(seed, std::hash<RHITextureView*>{}(key.occlusion));
    HashCombine(seed, std::hash<RHITextureView*>{}(key.emissive));
    HashCombine(seed, std::hash<uint64>{}(key.viewGeneration));
    return seed;
}

bool MaterialSystem::CreateConstantBuffer()
{
    m_materialConstantStride = AlignConstantBufferSize(sizeof(MaterialGPUConstants));

    RHIBufferDesc cbDesc;
    cbDesc.size = m_materialConstantStride * RVX_MAX_MATERIAL_CONSTANTS_PER_FRAME;
    cbDesc.usage = RHIBufferUsage::Constant;
    cbDesc.memoryType = RHIMemoryType::Upload;
    cbDesc.debugName = "MaterialConstantBuffer";

    m_materialConstantBuffer = m_device->CreateBuffer(cbDesc);
    return m_materialConstantBuffer != nullptr;
}

bool MaterialSystem::CreateDefaultResources()
{
    struct DefaultTexture
    {
        const char* name = nullptr;
        uint32 pixel = 0;
        RHITextureRef* texture = nullptr;
        RHITextureViewRef* view = nullptr;
    };

    DefaultTexture textures[] = {
        {"DefaultWhiteMaterialTexture", 0xFFFFFFFFu, &m_defaultWhiteTexture, &m_defaultWhiteTextureView},
        {"DefaultNormalMaterialTexture", 0xFFFF8080u, &m_defaultNormalTexture, &m_defaultNormalTextureView},
        {"DefaultBlackMaterialTexture", 0xFF000000u, &m_defaultBlackTexture, &m_defaultBlackTextureView},
    };

    GPUUploadService uploadService;
    uploadService.Initialize(m_device);

    for (DefaultTexture& defaultTexture : textures)
    {
        RHITextureDesc textureDesc = RHITextureDesc::Texture2D(1, 1, RHIFormat::RGBA8_UNORM);
        textureDesc.debugName = defaultTexture.name;

        GPUUploadTextureDesc uploadDesc;
        uploadDesc.textureDesc = textureDesc;
        uploadDesc.dataSize = sizeof(defaultTexture.pixel);

        GPUUploadTextureResult uploadResult =
            uploadService.UploadTextureDataWithResult(uploadDesc, &defaultTexture.pixel);
        if (!uploadResult)
        {
            RVX_CORE_ERROR("MaterialSystem: Failed to upload {}", defaultTexture.name);
            return false;
        }

        *defaultTexture.texture = uploadResult.resource;
    }

    uploadService.FlushAndWaitForUploads();

    auto commandContext = m_device->CreateCommandContext(RHICommandQueueType::Graphics);
    if (!commandContext)
    {
        RVX_CORE_ERROR("MaterialSystem: Failed to create default resource transition context");
        return false;
    }

    commandContext->Begin();
    for (DefaultTexture& defaultTexture : textures)
    {
        commandContext->TextureBarrier(defaultTexture.texture->Get(), RHIResourceState::Common,
                                       RHIResourceState::ShaderResource);
    }
    commandContext->End();
    m_device->SubmitCommandContext(commandContext.Get());
    m_device->WaitIdle();

    for (DefaultTexture& defaultTexture : textures)
    {
        RHITextureViewDesc viewDesc;
        viewDesc.debugName = defaultTexture.name;
        *defaultTexture.view = m_device->CreateTextureView(defaultTexture.texture->Get(), viewDesc);
        if (!*defaultTexture.view)
        {
            RVX_CORE_ERROR("MaterialSystem: Failed to create view for {}", defaultTexture.name);
            return false;
        }
    }

    RHISamplerDesc samplerDesc = RHISamplerDesc::LinearWrap();
    samplerDesc.debugName = "DefaultMaterialSampler";
    m_defaultSampler = m_device->CreateSampler(samplerDesc);
    if (!m_defaultSampler)
    {
        RVX_CORE_ERROR("MaterialSystem: Failed to create default material sampler");
        return false;
    }

    return true;
}

RHITextureView* MaterialSystem::ResolveTextureView(const Resource::TextureResource* textureResource,
                                                   RHITextureView* fallbackView,
                                                   ResourceViewCache* viewCache,
                                                   uint32 textureFlag,
                                                   uint32& textureFlags,
                                                   bool& usedFallback) const
{
    if (!textureResource || !m_gpuResources || !viewCache || !m_gpuResources->IsGPUReady(textureResource->GetId()))
    {
        usedFallback = true;
        return fallbackView;
    }

    RHITexture* texture = m_gpuResources->GetTexture(textureResource->GetId());
    if (!texture)
    {
        usedFallback = true;
        return fallbackView;
    }

    RHITextureView* view = viewCache->GetDefaultSRV(texture);
    if (!view)
    {
        usedFallback = true;
        return fallbackView;
    }

    textureFlags |= textureFlag;
    return view;
}

MaterialSystem::ResolvedMaterialTextures MaterialSystem::ResolveMaterialTextures(
    const Resource::MaterialResource* materialResource,
    ResourceViewCache* viewCache) const
{
    ResolvedMaterialTextures textures;
    textures.baseColor = m_defaultWhiteTextureView.Get();
    textures.normal = m_defaultNormalTextureView.Get();
    textures.metallicRoughness = m_defaultWhiteTextureView.Get();
    textures.occlusion = m_defaultWhiteTextureView.Get();
    textures.emissive = m_defaultBlackTextureView.Get();
    textures.viewGeneration = viewCache ? viewCache->GetGeneration() : 0;

    if (!materialResource)
    {
        textures.usedFallback = true;
        return textures;
    }

    textures.baseColor = ResolveTextureView(materialResource->GetAlbedoTexture().Get(), textures.baseColor,
                                            viewCache,
                                            static_cast<uint32>(MaterialTextureFlags::HasBaseColor),
                                            textures.textureFlags,
                                            textures.usedFallback);
    textures.normal = ResolveTextureView(materialResource->GetNormalTexture().Get(), textures.normal,
                                         viewCache,
                                         static_cast<uint32>(MaterialTextureFlags::HasNormal),
                                         textures.textureFlags,
                                         textures.usedFallback);
    textures.metallicRoughness =
        ResolveTextureView(materialResource->GetMetallicRoughnessTexture().Get(), textures.metallicRoughness,
                           viewCache,
                           static_cast<uint32>(MaterialTextureFlags::HasMetallicRoughness),
                           textures.textureFlags,
                           textures.usedFallback);
    textures.occlusion = ResolveTextureView(materialResource->GetAOTexture().Get(), textures.occlusion,
                                            viewCache,
                                            static_cast<uint32>(MaterialTextureFlags::HasOcclusion),
                                            textures.textureFlags,
                                            textures.usedFallback);
    textures.emissive = ResolveTextureView(materialResource->GetEmissiveTexture().Get(), textures.emissive,
                                           viewCache,
                                           static_cast<uint32>(MaterialTextureFlags::HasEmissive),
                                           textures.textureFlags,
                                           textures.usedFallback);

    return textures;
}

MaterialGPUConstants MaterialSystem::BuildConstants(const Resource::MaterialResource* materialResource,
                                                    const ResolvedMaterialTextures& textures) const
{
    MaterialGPUConstants constants;
    constants.textureFlags = textures.textureFlags;

    if (!materialResource || !materialResource->GetMaterial())
        return constants;

    const Material& material = *materialResource->GetMaterial();
    constants.baseColorFactor = material.GetBaseColor();
    constants.metallicFactor = material.GetMetallicFactor();
    constants.roughnessFactor = material.GetRoughnessFactor();
    constants.normalScale = material.GetNormalScale();
    constants.occlusionStrength = material.GetOcclusionStrength();
    constants.emissiveColor = material.GetEmissiveColor();
    constants.emissiveStrength = material.GetEmissiveStrength();
    constants.alphaCutoff = material.GetAlphaCutoff();
    constants.doubleSided = material.IsDoubleSided() ? 1u : 0u;

    switch (material.GetAlphaMode())
    {
        case Material::AlphaMode::Mask:
            constants.alphaMode = static_cast<uint32>(MaterialGPUAlphaMode::Mask);
            break;
        case Material::AlphaMode::Blend:
            constants.alphaMode = static_cast<uint32>(MaterialGPUAlphaMode::Blend);
            break;
        case Material::AlphaMode::Opaque:
        default:
            constants.alphaMode = static_cast<uint32>(MaterialGPUAlphaMode::Opaque);
            break;
    }

    switch (material.GetWorkflow())
    {
        case MaterialWorkflow::SpecularGlossiness:
            constants.workflow = static_cast<uint32>(MaterialGPUWorkflow::SpecularGlossiness);
            break;
        case MaterialWorkflow::Unlit:
            constants.workflow = static_cast<uint32>(MaterialGPUWorkflow::Unlit);
            break;
        case MaterialWorkflow::MetallicRoughness:
        default:
            constants.workflow = static_cast<uint32>(MaterialGPUWorkflow::MetallicRoughness);
            break;
    }

    return constants;
}

MaterialSystem::MaterialSetResolveResult MaterialSystem::GetOrCreateMaterialSetForResolved(
    const ResolvedMaterialTextures& textures)
{
    MaterialSetResolveResult result;

    if (!m_initialized)
    {
        result.status = MaterialBindingStatus::NotInitialized;
        result.message = "MaterialSystem is not initialized";
        return result;
    }

    MaterialDescriptorKey key;
    key.baseColor = textures.baseColor;
    key.normal = textures.normal;
    key.metallicRoughness = textures.metallicRoughness;
    key.occlusion = textures.occlusion;
    key.emissive = textures.emissive;
    key.viewGeneration = textures.viewGeneration;

    if (m_materialDescriptorCacheGeneration != textures.viewGeneration)
    {
        m_materialDescriptorCacheGeneration = textures.viewGeneration;
        m_materialDescriptorCache.clear();
    }

    auto it = m_materialDescriptorCache.find(key);
    if (it != m_materialDescriptorCache.end())
    {
        result.descriptorSet = it->second.Get();
        result.usedFallback = textures.usedFallback;
        result.status = textures.usedFallback ? MaterialBindingStatus::Fallback
                                              : MaterialBindingStatus::Ready;
        result.message = textures.usedFallback ? "Material descriptor used explicit fallback resources"
                                               : "Material descriptor ready";
        return result;
    }

    RHIDescriptorSetRef descriptorSet = CreateMaterialDescriptorSet(textures);
    if (!descriptorSet)
    {
        result.usedFallback = true;
        result.descriptorSet = GetDefaultMaterialSet();
        if (result.descriptorSet)
        {
            result.status = MaterialBindingStatus::Fallback;
            result.message = "Material descriptor creation failed; default material set used as explicit fallback";
        }
        else
        {
            result.status = MaterialBindingStatus::Error;
            result.message = "Material descriptor creation failed and default material set is unavailable";
        }
        return result;
    }

    result.descriptorSet = descriptorSet.Get();
    result.usedFallback = textures.usedFallback;
    result.status = textures.usedFallback ? MaterialBindingStatus::Fallback
                                          : MaterialBindingStatus::Ready;
    result.message = textures.usedFallback ? "Material descriptor used explicit fallback resources"
                                           : "Material descriptor ready";
    m_materialDescriptorCache.emplace(key, std::move(descriptorSet));
    return result;
}

RHIDescriptorSetRef MaterialSystem::CreateMaterialDescriptorSet(const ResolvedMaterialTextures& textures)
{
    if (!m_materialSetLayout || !m_materialConstantBuffer || !m_defaultSampler ||
        !textures.baseColor || !textures.normal || !textures.metallicRoughness ||
        !textures.occlusion || !textures.emissive)
    {
        return {};
    }

    RHIDescriptorSetDesc descSetDesc;
    descSetDesc.layout = m_materialSetLayout;
    descSetDesc.debugName = "MaterialDescriptorSet";
    descSetDesc.BindBuffer(0, m_materialConstantBuffer.Get(), 0, m_materialConstantStride);
    descSetDesc.BindTexture(1, textures.baseColor);
    descSetDesc.BindTexture(2, textures.normal);
    descSetDesc.BindTexture(3, textures.metallicRoughness);
    descSetDesc.BindTexture(4, textures.occlusion);
    descSetDesc.BindTexture(5, textures.emissive);
    descSetDesc.BindSampler(6, m_defaultSampler.Get());

    return m_device->CreateDescriptorSet(descSetDesc);
}

const MaterialBindingResult& MaterialSystem::SetLastBindingResult(MaterialBindingResult result)
{
    m_lastBindingResult = std::move(result);
    return m_lastBindingResult;
}

uint64 MaterialSystem::AllocateMaterialConstantSlot()
{
    if (m_materialConstantStride == 0)
        m_materialConstantStride = AlignConstantBufferSize(sizeof(MaterialGPUConstants));

    if (m_materialConstantCursor >= RVX_MAX_MATERIAL_CONSTANTS_PER_FRAME)
    {
        RVX_VERIFY(false,
                   "MaterialSystem: Material constant buffer exhausted for this frame (max {} material updates). "
                   "Reusing the final slot to avoid wrapping over earlier material constants.",
                   RVX_MAX_MATERIAL_CONSTANTS_PER_FRAME);
        const uint64 offset = (RVX_MAX_MATERIAL_CONSTANTS_PER_FRAME - 1) * m_materialConstantStride;
        m_currentMaterialConstantOffset = offset;
        return offset;
    }

    const uint64 offset = m_materialConstantCursor * m_materialConstantStride;
    ++m_materialConstantCursor;
    m_currentMaterialConstantOffset = offset;
    return offset;
}

} // namespace RVX
