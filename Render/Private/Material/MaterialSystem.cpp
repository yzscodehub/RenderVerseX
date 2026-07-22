/**
 * @file MaterialSystem.cpp
 * @brief MaterialSystem implementation
 */

#include "Render/Material/MaterialSystem.h"
#include "Core/Log.h"
#include "Render/GPUResourceManager.h"
#include "Render/GPUUploadService.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Material/MaterialBinder.h"
#include "Resources/RenderResourceRegistry.h"
#include "Resources/RenderOwnerSnapshotRetirement.h"
#include "RHI/RHICommandContext.h"

#include <algorithm>
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
                                RHIDescriptorSetLayout* materialSetLayout,
                                const RenderResourceRegistry* resourceRegistry)
{
    if (m_initialized)
    {
        RVX_CORE_WARN("MaterialSystem already initialized");
        return true;
    }

    if (!device || (!gpuResources && !resourceRegistry) ||
        !materialSetLayout)
    {
        RVX_CORE_ERROR("MaterialSystem: Invalid initialization parameters");
        return false;
    }

    m_device = device;
    m_gpuResources = gpuResources;
    m_resourceRegistry = resourceRegistry;
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
    defaultTextures.irradiance = m_defaultBlackCubemapView.Get();
    defaultTextures.prefilteredEnvironment = m_defaultBlackCubemapView.Get();
    defaultTextures.brdfLUT = m_defaultBlackTextureView.Get();

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
    m_pendingOwnerRetirements.clear();
    m_defaultMaterialSet.Reset();
    m_defaultSampler.Reset();
    m_defaultWhiteTextureView.Reset();
    m_defaultNormalTextureView.Reset();
    m_defaultBlackTextureView.Reset();
    m_defaultBlackCubemapView.Reset();
    m_defaultWhiteTexture.Reset();
    m_defaultNormalTexture.Reset();
    m_defaultBlackTexture.Reset();
    m_defaultBlackCubemap.Reset();
    m_materialConstantBuffer.Reset();
    m_environmentIBL = {};
    m_materialSetLayout = nullptr;
    m_gpuResources = nullptr;
    m_resourceRegistry = nullptr;
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

MaterialBindingResult MaterialSystem::PrepareMaterialBinding(const IRenderMaterialSource* materialResource,
                                                             ResourceViewCache* viewCache,
                                                             MaterialBindingOptions options)
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

    const ResolvedMaterialTextures textures =
        ResolveMaterialTextures(materialResource, viewCache, options);
    MaterialSourceData source;
    if (materialResource)
    {
        source = materialResource->GetRenderMaterialSourceData();
    }
    const std::string materialName =
        materialResource ? std::string(materialResource->GetRenderResourceName()) : std::string();
    return PrepareResolvedMaterialBinding(std::move(source),
                                          textures,
                                          materialName);
}

void MaterialSystem::RetireOwnerSnapshots(
    const GPUCompletionToken& completion,
    RenderRetirementQueue& retirement)
{
    FlushRenderOwnerRetirements(
        m_pendingOwnerRetirements, completion, retirement);
}

void MaterialSystem::QueueMaterialDescriptorCacheRetirement()
{
    for (auto& [key, descriptorSet] : m_materialDescriptorCache)
    {
        static_cast<void>(key);
        QueueRenderOwnerRetirement(
            descriptorSet, m_pendingOwnerRetirements);
    }
    m_materialDescriptorCache.clear();
}

MaterialBindingResult MaterialSystem::PrepareMaterialBinding(
    RenderResourceHandle material,
    ResourceViewCache* viewCache,
    MaterialBindingOptions options)
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

    const RenderMaterialResourceData* materialData =
        m_resourceRegistry ? m_resourceRegistry->ResolveMaterial(material)
                           : nullptr;
    MaterialSourceData source;
    if (materialData && materialData->metadataValid)
    {
        source = materialData->sourceData;
    }
    const ResolvedMaterialTextures textures =
        ResolveMaterialTextures(material, viewCache, options);
    const std::string materialName = material.IsValid()
        ? "render-material[" + std::to_string(material.slot) + ":" +
              std::to_string(material.generation) + "]"
        : std::string();
    return PrepareResolvedMaterialBinding(std::move(source),
                                          textures,
                                          materialName);
}

MaterialBindingResult MaterialSystem::PrepareResolvedMaterialBinding(
    MaterialSourceData source,
    const ResolvedMaterialTextures& textures,
    std::string materialName)
{
    source.textureFlags = textures.textureFlags;
    const MaterialGPUConstants constants = MaterialBinder::ConvertToGPU(source);

    void* mapped = m_materialConstantBuffer->Map();
    if (!mapped)
    {
        MaterialBindingResult result;
        result.status = MaterialBindingStatus::Error;
        result.textureFlags = constants.textureFlags;
        result.fallbackTextureFlags = textures.fallbackTextureFlags;
        result.usedFallback = textures.usedFallback;
        result.materialName = materialName;
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
        result.textureFlags = constants.textureFlags;
        result.fallbackTextureFlags = textures.fallbackTextureFlags;
        result.usedFallback = textures.usedFallback || setResult.usedFallback;
        result.materialName = materialName;
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
    result.textureFlags = constants.textureFlags;
    result.fallbackTextureFlags = textures.fallbackTextureFlags;
    result.usedFallback = textures.usedFallback || setResult.usedFallback;
    result.materialName = materialName;
    if (result.status == MaterialBindingStatus::Fallback)
    {
        if (textures.normalMapDisabled)
        {
            result.message = "Material normal map disabled because mesh tangent basis is unavailable";
        }
        else
        {
            result.message = setResult.message.empty() ? "Material binding used explicit fallback resources"
                                                       : std::move(setResult.message);
        }
    }
    else
    {
        result.message = "Material binding ready";
    }

    return SetLastBindingResult(std::move(result));
}

bool MaterialSystem::UpdateMaterialConstants(const IRenderMaterialSource* materialResource,
                                             ResourceViewCache* viewCache,
                                             MaterialBindingOptions options)
{
    const MaterialBindingResult result = PrepareMaterialBinding(materialResource, viewCache, options);
    return result.constantsUpdated && !result.IsError();
}

RHIDescriptorSet* MaterialSystem::GetOrCreateMaterialSet(const IRenderMaterialSource* materialResource,
                                                         ResourceViewCache* viewCache,
                                                         MaterialBindingOptions options)
{
    if (!m_initialized)
    {
        MaterialBindingResult result;
        result.status = MaterialBindingStatus::NotInitialized;
        result.message = "MaterialSystem is not initialized";
        SetLastBindingResult(std::move(result));
        return nullptr;
    }

    const ResolvedMaterialTextures textures = ResolveMaterialTextures(materialResource, viewCache, options);
    MaterialSetResolveResult setResult = GetOrCreateMaterialSetForResolved(textures);
    const std::string materialName =
        materialResource ? std::string(materialResource->GetRenderResourceName()) : std::string();

    MaterialBindingResult result;
    result.status = setResult.descriptorSet
        ? ((textures.usedFallback || setResult.usedFallback) ? MaterialBindingStatus::Fallback
                                                             : MaterialBindingStatus::Ready)
        : (setResult.status == MaterialBindingStatus::None ? MaterialBindingStatus::Error : setResult.status);
    result.descriptorSet = setResult.descriptorSet;
    result.dynamicOffsets = GetCurrentMaterialDynamicOffset();
    result.textureFlags = textures.textureFlags;
    result.fallbackTextureFlags = textures.fallbackTextureFlags;
    result.usedFallback = textures.usedFallback || setResult.usedFallback;
    result.materialName = materialName;
    if (textures.normalMapDisabled)
    {
        result.message = "Material normal map disabled because mesh tangent basis is unavailable";
    }
    else
    {
        result.message = setResult.message.empty()
            ? (result.usedFallback ? "Material descriptor used explicit fallback resources"
                                   : "Material descriptor ready")
            : std::move(setResult.message);
    }
    SetLastBindingResult(std::move(result));

    return setResult.descriptorSet;
}

RHIDescriptorSet* MaterialSystem::GetDefaultMaterialSet()
{
    return m_defaultMaterialSet.Get();
}

void MaterialSystem::RequestMaterialTextures(const IRenderMaterialSource* materialResource) const
{
    if (!materialResource || !m_gpuResources)
        return;

    const auto requestTexture = [this](IRenderTextureUploadSource* texture)
    {
        if (!texture)
            return;

        if (!m_gpuResources->IsResident(texture))
        {
            m_gpuResources->RequestUpload(texture, UploadPriority::High);
        }
        m_gpuResources->MarkUsed(texture);
    };

    requestTexture(materialResource->GetRenderMaterialTexture(RenderMaterialTextureSlot::BaseColor));
    requestTexture(materialResource->GetRenderMaterialTexture(RenderMaterialTextureSlot::Normal));
    requestTexture(materialResource->GetRenderMaterialTexture(RenderMaterialTextureSlot::MetallicRoughness));
    requestTexture(materialResource->GetRenderMaterialTexture(RenderMaterialTextureSlot::Occlusion));
    requestTexture(materialResource->GetRenderMaterialTexture(RenderMaterialTextureSlot::Emissive));
}

void MaterialSystem::TransitionMaterialTextures(const IRenderMaterialSource* materialResource,
                                                RHICommandContext& ctx,
                                                MaterialBindingOptions options) const
{
    if (!materialResource || !m_gpuResources)
        return;

    const auto transitionTexture = [this, &ctx](IRenderTextureUploadSource* textureResource)
    {
        if (!textureResource || !m_gpuResources->IsGPUReady(textureResource->GetRenderResourceId()))
            return;

        m_gpuResources->TransitionTexture(textureResource->GetRenderResourceId(), ctx, RHIResourceState::ShaderResource);
    };

    transitionTexture(materialResource->GetRenderMaterialTexture(RenderMaterialTextureSlot::BaseColor));
    if (options.allowNormalMap)
    {
        transitionTexture(materialResource->GetRenderMaterialTexture(RenderMaterialTextureSlot::Normal));
    }
    transitionTexture(materialResource->GetRenderMaterialTexture(RenderMaterialTextureSlot::MetallicRoughness));
    transitionTexture(materialResource->GetRenderMaterialTexture(RenderMaterialTextureSlot::Occlusion));
    transitionTexture(materialResource->GetRenderMaterialTexture(RenderMaterialTextureSlot::Emissive));
}

void MaterialSystem::SetEnvironmentIBLResources(const EnvironmentIBLResources& resources)
{
    const bool changed =
        m_environmentIBL.irradianceMap != resources.irradianceMap ||
        m_environmentIBL.prefilteredMap != resources.prefilteredMap ||
        m_environmentIBL.brdfLUT != resources.brdfLUT ||
        m_environmentIBL.irradianceHandle != resources.irradianceHandle ||
        m_environmentIBL.prefilteredHandle != resources.prefilteredHandle ||
        m_environmentIBL.brdfLUTHandle != resources.brdfLUTHandle ||
        m_environmentIBL.textureIBLEnabled != resources.textureIBLEnabled;

    m_environmentIBL = resources;
    m_environmentIBL.prefilteredMipLevels = std::max(1u, m_environmentIBL.prefilteredMipLevels);

    if (changed)
    {
        QueueMaterialDescriptorCacheRetirement();
    }
}

void MaterialSystem::SetEnvironmentIBLResources(
    RenderResourceHandle irradiance,
    RenderResourceHandle prefiltered,
    RenderResourceHandle brdfLUT,
    float intensity)
{
    EnvironmentIBLResources resources;
    resources.irradianceHandle = irradiance;
    resources.prefilteredHandle = prefiltered;
    resources.brdfLUTHandle = brdfLUT;
    resources.intensity = intensity;
    resources.textureIBLEnabled = irradiance.IsValid() &&
                                  prefiltered.IsValid() &&
                                  brdfLUT.IsValid();
    SetEnvironmentIBLResources(resources);
}

void MaterialSystem::ClearEnvironmentIBLResources()
{
    SetEnvironmentIBLResources({});
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
    HashCombine(seed, std::hash<RHITextureView*>{}(key.irradiance));
    HashCombine(seed, std::hash<RHITextureView*>{}(key.prefilteredEnvironment));
    HashCombine(seed, std::hash<RHITextureView*>{}(key.brdfLUT));
    HashCombine(seed, std::hash<uint64>{}(key.viewGeneration));
    HashCombine(seed, std::hash<bool>{}(key.textureIBLEnabled));
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

    const uint32 blackCubePixels[6] = {};
    RHITextureDesc blackCubemapDesc = RHITextureDesc::Texture2D(1, 1, RHIFormat::RGBA8_UNORM);
    blackCubemapDesc.dimension = RHITextureDimension::TextureCube;
    blackCubemapDesc.arraySize = 1;
    blackCubemapDesc.debugName = "DefaultBlackIBLCubemap";

    GPUUploadTextureDesc blackCubemapUploadDesc;
    blackCubemapUploadDesc.textureDesc = blackCubemapDesc;
    blackCubemapUploadDesc.dataSize = sizeof(blackCubePixels);

    GPUUploadTextureResult blackCubemapUploadResult =
        uploadService.UploadTextureDataWithResult(blackCubemapUploadDesc, blackCubePixels);
    if (!blackCubemapUploadResult)
    {
        RVX_CORE_ERROR("MaterialSystem: Failed to upload DefaultBlackIBLCubemap");
        return false;
    }

    m_defaultBlackCubemap = blackCubemapUploadResult.resource;

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
    commandContext->TextureBarrier(m_defaultBlackCubemap.Get(), RHIResourceState::Common,
                                   RHIResourceState::ShaderResource);
    commandContext->End();
    m_device->SubmitCommandContext(commandContext.Get());
    m_device->WaitIdle();

    for (DefaultTexture& defaultTexture : textures)
    {
        RHITextureViewDesc viewDesc;
        viewDesc.dimension = RHITextureDimension::Texture2D;
        viewDesc.type = RHITextureViewType::ShaderResource;
        viewDesc.debugName = defaultTexture.name;
        *defaultTexture.view = m_device->CreateTextureView(defaultTexture.texture->Get(), viewDesc);
        if (!*defaultTexture.view)
        {
            RVX_CORE_ERROR("MaterialSystem: Failed to create view for {}", defaultTexture.name);
            return false;
        }
    }

    RHITextureViewDesc blackCubemapViewDesc;
    blackCubemapViewDesc.dimension = RHITextureDimension::TextureCube;
    blackCubemapViewDesc.type = RHITextureViewType::ShaderResource;
    blackCubemapViewDesc.debugName = "DefaultBlackIBLCubemap";
    m_defaultBlackCubemapView = m_device->CreateTextureView(m_defaultBlackCubemap.Get(), blackCubemapViewDesc);
    if (!m_defaultBlackCubemapView)
    {
        RVX_CORE_ERROR("MaterialSystem: Failed to create view for DefaultBlackIBLCubemap");
        return false;
    }

    RHISamplerDesc samplerDesc = RHISamplerDesc::Anisotropic(8.0f);
    samplerDesc.minFilter = RHIFilterMode::Linear;
    samplerDesc.magFilter = RHIFilterMode::Linear;
    samplerDesc.mipFilter = RHIFilterMode::Linear;
    samplerDesc.addressU = RHIAddressMode::Repeat;
    samplerDesc.addressV = RHIAddressMode::Repeat;
    samplerDesc.addressW = RHIAddressMode::Repeat;
    samplerDesc.debugName = "DefaultMaterialSampler";
    m_defaultSampler = m_device->CreateSampler(samplerDesc);
    if (!m_defaultSampler && samplerDesc.anisotropyEnable)
    {
        RVX_CORE_WARN("MaterialSystem: Anisotropic material sampler creation failed; falling back to linear mip sampler");
        samplerDesc.anisotropyEnable = false;
        samplerDesc.maxAnisotropy = 1.0f;
        samplerDesc.debugName = "DefaultMaterialLinearMipSampler";
        m_defaultSampler = m_device->CreateSampler(samplerDesc);
    }
    if (!m_defaultSampler)
    {
        RVX_CORE_ERROR("MaterialSystem: Failed to create default material sampler");
        return false;
    }

    return true;
}

RHITextureView* MaterialSystem::ResolveTextureView(IRenderTextureUploadSource* textureResource,
                                                   RHITextureView* fallbackView,
                                                   ResourceViewCache* viewCache,
                                                   uint32 textureFlag,
                                                   uint32& textureFlags,
                                                   uint32& fallbackTextureFlags,
                                                   bool& usedFallback) const
{
    if (!textureResource)
    {
        return fallbackView;
    }

    const uint64 textureId = textureResource->GetRenderResourceId();
    if (textureResource->IsRenderDefaultFallbackTexture() || !m_gpuResources || !viewCache ||
        !m_gpuResources->IsGPUReady(textureId))
    {
        usedFallback = true;
        fallbackTextureFlags |= textureFlag;
        return fallbackView;
    }

    RHITexture* texture = m_gpuResources->GetTexture(textureId);
    if (!texture)
    {
        usedFallback = true;
        fallbackTextureFlags |= textureFlag;
        return fallbackView;
    }

    RHITextureView* view = viewCache->GetDefaultSRV(texture);
    if (!view)
    {
        usedFallback = true;
        fallbackTextureFlags |= textureFlag;
        return fallbackView;
    }

    textureFlags |= textureFlag;
    return view;
}

MaterialSystem::ResolvedMaterialTextures MaterialSystem::ResolveMaterialTextures(
    const IRenderMaterialSource* materialResource,
    ResourceViewCache* viewCache,
    MaterialBindingOptions options) const
{
    ResolvedMaterialTextures textures;
    textures.baseColor = m_defaultWhiteTextureView.Get();
    textures.normal = m_defaultNormalTextureView.Get();
    textures.metallicRoughness = m_defaultWhiteTextureView.Get();
    textures.occlusion = m_defaultWhiteTextureView.Get();
    textures.emissive = m_defaultBlackTextureView.Get();
    textures.irradiance = m_defaultBlackCubemapView.Get();
    textures.prefilteredEnvironment = m_defaultBlackCubemapView.Get();
    textures.brdfLUT = m_defaultBlackTextureView.Get();
    textures.viewGeneration = viewCache ? viewCache->GetGeneration() : 0;

    if (!materialResource)
    {
        textures.usedFallback = true;
    }
    else
    {
        textures.baseColor =
            ResolveTextureView(materialResource->GetRenderMaterialTexture(RenderMaterialTextureSlot::BaseColor),
                               textures.baseColor,
                               viewCache,
                               static_cast<uint32>(MaterialTextureFlags::HasBaseColor),
                               textures.textureFlags,
                               textures.fallbackTextureFlags,
                               textures.usedFallback);
        IRenderTextureUploadSource* normalTexture =
            materialResource->GetRenderMaterialTexture(RenderMaterialTextureSlot::Normal);
        if (!options.allowNormalMap && normalTexture)
        {
            textures.usedFallback = true;
            textures.normalMapDisabled = true;
            textures.fallbackTextureFlags |= static_cast<uint32>(MaterialTextureFlags::HasNormal);
        }
        else
        {
            textures.normal = ResolveTextureView(normalTexture, textures.normal,
                                                 viewCache,
                                                 static_cast<uint32>(MaterialTextureFlags::HasNormal),
                                                 textures.textureFlags,
                                                 textures.fallbackTextureFlags,
                                                 textures.usedFallback);
        }
        textures.metallicRoughness =
            ResolveTextureView(materialResource->GetRenderMaterialTexture(RenderMaterialTextureSlot::MetallicRoughness),
                               textures.metallicRoughness,
                               viewCache,
                               static_cast<uint32>(MaterialTextureFlags::HasMetallicRoughness),
                               textures.textureFlags,
                               textures.fallbackTextureFlags,
                               textures.usedFallback);
        textures.occlusion =
            ResolveTextureView(materialResource->GetRenderMaterialTexture(RenderMaterialTextureSlot::Occlusion),
                               textures.occlusion,
                               viewCache,
                               static_cast<uint32>(MaterialTextureFlags::HasOcclusion),
                               textures.textureFlags,
                               textures.fallbackTextureFlags,
                               textures.usedFallback);
        textures.emissive =
            ResolveTextureView(materialResource->GetRenderMaterialTexture(RenderMaterialTextureSlot::Emissive),
                               textures.emissive,
                               viewCache,
                               static_cast<uint32>(MaterialTextureFlags::HasEmissive),
                               textures.textureFlags,
                               textures.fallbackTextureFlags,
                               textures.usedFallback);
    }

    if (m_environmentIBL.textureIBLEnabled)
    {
        bool iblUsedFallback = false;
        textures.irradiance = ResolveTextureView(m_environmentIBL.irradianceMap,
                                                 textures.irradiance,
                                                 viewCache,
                                                 0,
                                                 textures.textureFlags,
                                                 textures.fallbackTextureFlags,
                                                 iblUsedFallback);
        textures.prefilteredEnvironment = ResolveTextureView(m_environmentIBL.prefilteredMap,
                                                             textures.prefilteredEnvironment,
                                                             viewCache,
                                                             0,
                                                             textures.textureFlags,
                                                             textures.fallbackTextureFlags,
                                                             iblUsedFallback);
        textures.brdfLUT = ResolveTextureView(m_environmentIBL.brdfLUT,
                                              textures.brdfLUT,
                                              viewCache,
                                              0,
                                              textures.textureFlags,
                                              textures.fallbackTextureFlags,
                                              iblUsedFallback);

        if (iblUsedFallback)
        {
            textures.usedFallback = true;
        }
        else
        {
            textures.textureIBLEnabled = true;
        }
    }

    return textures;
}

MaterialSystem::ResolvedMaterialTextures MaterialSystem::ResolveMaterialTextures(
    RenderResourceHandle material,
    ResourceViewCache* viewCache,
    MaterialBindingOptions options) const
{
    ResolvedMaterialTextures textures;
    textures.baseColor = m_defaultWhiteTextureView.Get();
    textures.normal = m_defaultNormalTextureView.Get();
    textures.metallicRoughness = m_defaultWhiteTextureView.Get();
    textures.occlusion = m_defaultWhiteTextureView.Get();
    textures.emissive = m_defaultBlackTextureView.Get();
    textures.irradiance = m_defaultBlackCubemapView.Get();
    textures.prefilteredEnvironment = m_defaultBlackCubemapView.Get();
    textures.brdfLUT = m_defaultBlackTextureView.Get();
    textures.viewGeneration = viewCache ? viewCache->GetGeneration() : 0;

    const RenderMaterialResourceData* materialData =
        m_resourceRegistry ? m_resourceRegistry->ResolveMaterial(material)
                           : nullptr;
    if (materialData == nullptr || !materialData->metadataValid)
    {
        textures.usedFallback = true;
    }
    else
    {
        for (const MaterialUploadTextureBinding& binding :
             materialData->textureBindings)
        {
            uint32 textureFlag = 0;
            RHITextureView** destination = nullptr;
            switch (binding.slot)
            {
                case MaterialUploadTextureSlot::BaseColor:
                    textureFlag = static_cast<uint32>(
                        MaterialTextureFlags::HasBaseColor);
                    destination = &textures.baseColor;
                    break;
                case MaterialUploadTextureSlot::Normal:
                    textureFlag = static_cast<uint32>(
                        MaterialTextureFlags::HasNormal);
                    destination = &textures.normal;
                    if (!options.allowNormalMap)
                    {
                        textures.usedFallback = true;
                        textures.normalMapDisabled = true;
                        textures.fallbackTextureFlags |= textureFlag;
                        continue;
                    }
                    break;
                case MaterialUploadTextureSlot::MetallicRoughness:
                    textureFlag = static_cast<uint32>(
                        MaterialTextureFlags::HasMetallicRoughness);
                    destination = &textures.metallicRoughness;
                    break;
                case MaterialUploadTextureSlot::Occlusion:
                    textureFlag = static_cast<uint32>(
                        MaterialTextureFlags::HasOcclusion);
                    destination = &textures.occlusion;
                    break;
                case MaterialUploadTextureSlot::Emissive:
                    textureFlag = static_cast<uint32>(
                        MaterialTextureFlags::HasEmissive);
                    destination = &textures.emissive;
                    break;
                default:
                    continue;
            }

            RHITexture* texture =
                m_resourceRegistry->ResolveTextureObject(binding.texture);
            RHITextureView* textureView =
                texture && viewCache ? viewCache->GetDefaultSRV(texture)
                                     : nullptr;
            if (textureView == nullptr)
            {
                textures.usedFallback = true;
                textures.fallbackTextureFlags |= textureFlag;
                continue;
            }
            *destination = textureView;
            textures.textureFlags |= textureFlag;
        }
    }

    if (m_environmentIBL.textureIBLEnabled && m_resourceRegistry)
    {
        RHITexture* irradiance = m_resourceRegistry->ResolveTextureObject(
            m_environmentIBL.irradianceHandle);
        RHITexture* prefiltered = m_resourceRegistry->ResolveTextureObject(
            m_environmentIBL.prefilteredHandle);
        RHITexture* brdfLUT = m_resourceRegistry->ResolveTextureObject(
            m_environmentIBL.brdfLUTHandle);
        RHITextureView* irradianceView =
            irradiance && viewCache ? viewCache->GetDefaultSRV(irradiance)
                                    : nullptr;
        RHITextureView* prefilteredView =
            prefiltered && viewCache ? viewCache->GetDefaultSRV(prefiltered)
                                     : nullptr;
        RHITextureView* brdfLUTView =
            brdfLUT && viewCache ? viewCache->GetDefaultSRV(brdfLUT)
                                 : nullptr;
        if (irradianceView && prefilteredView && brdfLUTView)
        {
            textures.irradiance = irradianceView;
            textures.prefilteredEnvironment = prefilteredView;
            textures.brdfLUT = brdfLUTView;
            textures.textureIBLEnabled = true;
        }
        else
        {
            textures.usedFallback = true;
        }
    }

    return textures;
}

MaterialGPUConstants MaterialSystem::BuildConstants(const IRenderMaterialSource* materialResource,
                                                    const ResolvedMaterialTextures& textures) const
{
    MaterialSourceData source;
    source.textureFlags = textures.textureFlags;

    if (!materialResource)
        return MaterialBinder::ConvertToGPU(source);

    source = materialResource->GetRenderMaterialSourceData();
    source.textureFlags = textures.textureFlags;

    return MaterialBinder::ConvertToGPU(source);
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
    key.irradiance = textures.irradiance;
    key.prefilteredEnvironment = textures.prefilteredEnvironment;
    key.brdfLUT = textures.brdfLUT;
    key.viewGeneration = textures.viewGeneration;
    key.textureIBLEnabled = textures.textureIBLEnabled;

    if (m_materialDescriptorCacheGeneration != textures.viewGeneration)
    {
        m_materialDescriptorCacheGeneration = textures.viewGeneration;
        QueueMaterialDescriptorCacheRetirement();
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
        !textures.occlusion || !textures.emissive || !textures.irradiance ||
        !textures.prefilteredEnvironment || !textures.brdfLUT)
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
    descSetDesc.BindTexture(7, textures.irradiance);
    descSetDesc.BindTexture(8, textures.prefilteredEnvironment);
    descSetDesc.BindTexture(9, textures.brdfLUT);

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
