/**
 * @file MaterialSystem.cpp
 * @brief MaterialSystem implementation
 */

#include "Render/Material/MaterialSystem.h"
#include "Core/Log.h"
#include "Render/GPUUploadService.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Material/MaterialBinder.h"
#include "Resources/FrameConstantUploadArena.h"
#include "Resources/RenderResourceRegistry.h"
#include "Resources/RenderOwnerSnapshotRetirement.h"
#include "Resources/RenderSubmissionTracker.h"
#include "RHI/RHICommandContext.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <type_traits>
#include <unordered_map>
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

    constexpr uint64 FNV_OFFSET = 14695981039346656037ULL;
    constexpr uint64 FNV_PRIME = 1099511628211ULL;

    void HashBindingByte(uint64& hash, uint8 value) noexcept
    {
        hash ^= value;
        hash *= FNV_PRIME;
    }

    template <typename TValue>
    void HashBindingValue(uint64& hash, TValue value) noexcept
    {
        static_assert(std::is_integral_v<TValue> || std::is_enum_v<TValue>);
        uint64 bits = 0;
        uint32 bitCount = 0;
        if constexpr (std::is_enum_v<TValue>)
        {
            using ValueType = std::underlying_type_t<TValue>;
            using UnsignedType = std::make_unsigned_t<ValueType>;
            bits = static_cast<uint64>(static_cast<UnsignedType>(value));
            bitCount = sizeof(UnsignedType) * 8U;
        }
        else
        {
            using UnsignedType = std::make_unsigned_t<TValue>;
            bits = static_cast<uint64>(static_cast<UnsignedType>(value));
            bitCount = sizeof(UnsignedType) * 8U;
        }
        for (uint32 shift = 0; shift < bitCount; shift += 8U)
        {
            HashBindingByte(hash, static_cast<uint8>(bits >> shift));
        }
    }

    void HashBindingFloat(uint64& hash, float32 value) noexcept
    {
        HashBindingValue(hash, std::bit_cast<uint32>(value));
    }
} // namespace

MaterialSystem::MaterialSystem() = default;

MaterialSystem::~MaterialSystem()
{
    Shutdown();
}

bool MaterialSystem::Initialize(IRHIDevice* device,
                                RHIDescriptorSetLayout* materialSetLayout,
                                RenderResourceRegistry* resourceRegistry)
{
    if (m_initialized)
    {
        RVX_CORE_WARN("MaterialSystem already initialized");
        return true;
    }

    if (!device || !resourceRegistry || !materialSetLayout)
    {
        RVX_CORE_ERROR("MaterialSystem: Invalid initialization parameters");
        return false;
    }

    m_device = device;
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
    defaultTextures.baseColorSampler = m_defaultSampler.Get();
    defaultTextures.normalSampler = m_defaultSampler.Get();
    defaultTextures.metallicRoughnessSampler = m_defaultSampler.Get();
    defaultTextures.occlusionSampler = m_defaultSampler.Get();
    defaultTextures.emissiveSampler = m_defaultSampler.Get();
    defaultTextures.materialParameterTable =
        m_defaultMaterialParameterTable.Get();

    m_defaultMaterialSet = CreateMaterialDescriptorSet(
        defaultTextures, m_materialConstantBuffer.Get());
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
    m_defaultWhiteTexture.Reset();
    m_defaultNormalTexture.Reset();
    m_defaultBlackTexture.Reset();
    m_materialConstantBuffer.Reset();
    m_defaultMaterialParameterTable.Reset();
    if (m_materialConstantUploadArena)
    {
        m_materialConstantUploadArena->Shutdown();
        m_materialConstantUploadArena.reset();
    }
    m_materialSetLayout = nullptr;
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

    // Recording-owned material tables are rebuilt from the current immutable
    // frame plan. They must not remain in the persistent descriptor cache:
    // after submission retirement the allocator may reuse the same C++ buffer
    // address while the cached native descriptor still targets the old GPU
    // resource. Retire those descriptor snapshots once per frame and retain
    // their table owners until the next completion token.
    for (auto it = m_materialDescriptorCache.begin();
         it != m_materialDescriptorCache.end();)
    {
        MaterialDescriptorCacheEntry& entry = it->second;
        if (!entry.materialParameterTable ||
            entry.materialParameterTable.Get() ==
                m_defaultMaterialParameterTable.Get())
        {
            ++it;
            continue;
        }
        QueueRenderOwnerRetirement(
            entry.descriptorSet, m_pendingOwnerRetirements);
        for (RHISamplerRef& sampler : entry.samplers)
        {
            QueueRenderOwnerRetirement(
                sampler, m_pendingOwnerRetirements);
        }
        QueueRenderOwnerRetirement(
            entry.materialParameterTable, m_pendingOwnerRetirements);
        it = m_materialDescriptorCache.erase(it);
    }
    if (m_materialConstantUploadArena)
    {
        m_materialConstantUploadArena->PollCompletions();
    }
}

void MaterialSystem::RetireOwnerSnapshots(
    const GPUCompletionToken& completion,
    RenderRetirementQueue& retirement)
{
    FlushRenderOwnerRetirements(
        m_pendingOwnerRetirements, completion, retirement);
}

void MaterialSystem::SetMaterialConstantSubmissionTracker(
    RenderSubmissionTracker* tracker) noexcept
{
    if (m_materialConstantUploadArena)
    {
        m_materialConstantUploadArena->SetSubmissionTracker(tracker);
    }
}

bool MaterialSystem::NotifyMaterialConstantSubmission(
    const GPUCompletionToken& completion) noexcept
{
    return !m_materialConstantUploadArena ||
           m_materialConstantUploadArena->NotifySubmission(completion);
}

void MaterialSystem::ReleaseUnsubmittedMaterialConstants() noexcept
{
    if (m_materialConstantUploadArena)
    {
        m_materialConstantUploadArena->ReleaseUnsubmittedFrame();
    }
}

void MaterialSystem::QueueMaterialDescriptorCacheRetirement()
{
    for (auto& [key, entry] : m_materialDescriptorCache)
    {
        static_cast<void>(key);
        QueueRenderOwnerRetirement(
            entry.descriptorSet, m_pendingOwnerRetirements);
        for (RHISamplerRef& sampler : entry.samplers)
        {
            QueueRenderOwnerRetirement(
                sampler, m_pendingOwnerRetirements);
        }
        QueueRenderOwnerRetirement(
            entry.materialParameterTable, m_pendingOwnerRetirements);
    }
    m_materialDescriptorCache.clear();
}

MaterialInstanceBindingKey MaterialSystem::ResolveInstanceBindingKey(
    RenderResourceHandle material) const noexcept
{
    MaterialInstanceBindingKey result;
    const RenderMaterialResourceData* materialData =
        m_resourceRegistry ? m_resourceRegistry->ResolveMaterial(material)
                           : nullptr;
    if (!material.IsValid() || material.slot >= RVX_MAX_MATERIAL_CONSTANTS_PER_FRAME ||
        materialData == nullptr || !materialData->metadataValid ||
        materialData->sourceData.alphaMode != MaterialSourceAlphaMode::Opaque ||
        materialData->samplers.size() != materialData->textureBindings.size())
    {
        return result;
    }

    std::vector<MaterialUploadTextureBinding> bindings =
        materialData->textureBindings;
    std::sort(bindings.begin(), bindings.end(),
        [](const MaterialUploadTextureBinding& lhs,
           const MaterialUploadTextureBinding& rhs)
        {
            return static_cast<uint8>(lhs.slot) < static_cast<uint8>(rhs.slot);
        });

    uint64 hash = FNV_OFFSET;
    HashBindingValue(hash, static_cast<uint32>(bindings.size()));
    MaterialUploadTextureSlot previousSlot = MaterialUploadTextureSlot::BaseColor;
    bool hasPrevious = false;
    for (const MaterialUploadTextureBinding& binding : bindings)
    {
        if (binding.isDefaultFallback || !binding.texture.IsValid() ||
            (hasPrevious && binding.slot == previousSlot))
        {
            return {};
        }
        previousSlot = binding.slot;
        hasPrevious = true;
        HashBindingValue(hash, binding.slot);
        HashBindingValue(hash, binding.texture.slot);
        HashBindingValue(hash, binding.texture.generation);
        HashBindingValue(hash, binding.uvSet);
        HashBindingFloat(hash, binding.offset.x);
        HashBindingFloat(hash, binding.offset.y);
        HashBindingFloat(hash, binding.scale.x);
        HashBindingFloat(hash, binding.scale.y);
        HashBindingFloat(hash, binding.rotation);
        HashBindingValue(hash, binding.wrapS);
        HashBindingValue(hash, binding.wrapT);
        HashBindingValue(hash, binding.minFilter);
        HashBindingValue(hash, binding.magFilter);
    }

    result.textureBindingHash = hash;
    result.parameterTableCompatible = true;
    return result;
}

bool MaterialSystem::CreateMaterialParameterTableSnapshot(
    std::span<const MaterialParameterTableEntryRequest> requests,
    ResourceViewCache* viewCache,
    MaterialParameterTableSnapshot& outSnapshot) const
{
    outSnapshot = {};
    if (!m_initialized || !m_device || requests.empty())
    {
        return false;
    }

    struct UniqueRequest
    {
        RenderResourceHandle material;
        bool allowNormalMap = true;
    };
    std::unordered_map<uint32, UniqueRequest> unique;
    uint32 maxSlot = 0;
    for (const MaterialParameterTableEntryRequest& request : requests)
    {
        if (!request.material.IsValid() ||
            request.material.slot >= RVX_MAX_MATERIAL_CONSTANTS_PER_FRAME)
        {
            return false;
        }
        const auto [it, inserted] = unique.emplace(
            request.material.slot,
            UniqueRequest{request.material, request.allowNormalMap});
        if (!inserted &&
            (it->second.material.generation != request.material.generation ||
             it->second.allowNormalMap != request.allowNormalMap))
        {
            return false;
        }
        maxSlot = std::max(maxSlot, request.material.slot);
    }

    const MaterialGPUConstants defaultConstants =
        MaterialBinder::ConvertToGPU(MaterialSourceData{});
    std::vector<MaterialGPUConstants> table(
        static_cast<size_t>(maxSlot) + 1U, defaultConstants);
    for (const auto& [slot, request] : unique)
    {
        const RenderMaterialResourceData* materialData =
            m_resourceRegistry->ResolveMaterial(request.material);
        if (materialData == nullptr || !materialData->metadataValid ||
            !ResolveInstanceBindingKey(request.material).parameterTableCompatible)
        {
            return false;
        }
        MaterialBindingOptions options;
        options.allowNormalMap = request.allowNormalMap;
        const ResolvedMaterialTextures textures = ResolveMaterialTextures(
            request.material, viewCache, options);
        if (textures.usedFallback)
        {
            return false;
        }
        MaterialSourceData source = materialData->sourceData;
        source.textureFlags = textures.textureFlags;
        table[slot] = MaterialBinder::ConvertToGPU(source);
    }

    RHIBufferDesc desc;
    desc.size = static_cast<uint64>(table.size()) *
                sizeof(MaterialGPUConstants);
    desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    desc.memoryType = RHIMemoryType::Upload;
    desc.stride = sizeof(MaterialGPUConstants);
    desc.debugName = "MaterialParameterTable";
    RHIBufferRef buffer = m_device->CreateBuffer(desc);
    void* mapped = buffer ? buffer->Map() : nullptr;
    if (mapped == nullptr)
    {
        return false;
    }
    std::memcpy(mapped, table.data(), static_cast<size_t>(desc.size));
    buffer->Unmap();

    outSnapshot.buffer = std::move(buffer);
    outSnapshot.slotCount = static_cast<uint32>(table.size());
    outSnapshot.materialCount = static_cast<uint32>(unique.size());
    return outSnapshot.IsValid();
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

bool MaterialSystem::CreateMaterialBindingSnapshot(
    RenderResourceHandle material,
    ResourceViewCache* viewCache,
    MaterialBindingOptions options,
    MaterialBindingSnapshot& outSnapshot)
{
    outSnapshot = {};
    if (!m_initialized || !m_device || !m_materialSetLayout || !m_defaultSampler)
    {
        return false;
    }

    const RenderMaterialResourceData* materialData =
        m_resourceRegistry ? m_resourceRegistry->ResolveMaterial(material) : nullptr;
    MaterialSourceData source;
    if (materialData && materialData->metadataValid)
    {
        source = materialData->sourceData;
    }
    const ResolvedMaterialTextures textures =
        ResolveMaterialTextures(material, viewCache, options);
    if (!textures.baseColor || !textures.normal || !textures.metallicRoughness ||
        !textures.occlusion || !textures.emissive ||
        !textures.baseColorSampler || !textures.normalSampler ||
        !textures.metallicRoughnessSampler || !textures.occlusionSampler ||
        !textures.emissiveSampler || !textures.materialParameterTable)
    {
        return false;
    }

    source.textureFlags = textures.textureFlags;
    const MaterialGPUConstants constants = MaterialBinder::ConvertToGPU(source);
    const uint64 stride = m_materialConstantStride != 0
        ? m_materialConstantStride : AlignConstantBufferSize(sizeof(MaterialGPUConstants));
    RHIBufferDesc bufferDesc;
    bufferDesc.size = stride;
    bufferDesc.usage = RHIBufferUsage::Constant;
    bufferDesc.memoryType = RHIMemoryType::Upload;
    bufferDesc.debugName = "ObjectVelocityRecordMaterialConstants";
    RHIBufferRef constantBuffer = m_device->CreateBuffer(bufferDesc);
    if (!constantBuffer)
    {
        return false;
    }
    void* mapped = constantBuffer->Map();
    if (!mapped)
    {
        return false;
    }
    std::memcpy(mapped, &constants, sizeof(MaterialGPUConstants));
    constantBuffer->Unmap();

    RHIDescriptorSetDesc descriptorDesc;
    descriptorDesc.layout = m_materialSetLayout;
    descriptorDesc.debugName = "ObjectVelocityRecordMaterialDescriptorSet";
    descriptorDesc.BindBuffer(0, constantBuffer.Get(), 0, stride);
    descriptorDesc.BindTexture(1, textures.baseColor);
    descriptorDesc.BindTexture(2, textures.normal);
    descriptorDesc.BindTexture(3, textures.metallicRoughness);
    descriptorDesc.BindTexture(4, textures.occlusion);
    descriptorDesc.BindTexture(5, textures.emissive);
    descriptorDesc.BindSampler(6, textures.baseColorSampler);
    descriptorDesc.BindSampler(7, textures.normalSampler);
    descriptorDesc.BindSampler(8, textures.metallicRoughnessSampler);
    descriptorDesc.BindSampler(9, textures.occlusionSampler);
    descriptorDesc.BindSampler(10, textures.emissiveSampler);
    descriptorDesc.BindBuffer(11, textures.materialParameterTable);
    RHIDescriptorSetRef descriptorSet = m_device->CreateDescriptorSet(descriptorDesc);
    if (!descriptorSet)
    {
        return false;
    }

    MaterialBindingResult binding;
    binding.status = textures.usedFallback ? MaterialBindingStatus::Fallback
                                            : MaterialBindingStatus::Ready;
    binding.descriptorSet = descriptorSet.Get();
    binding.dynamicOffsets = {0};
    binding.textureFlags = constants.textureFlags;
    binding.fallbackTextureFlags = textures.fallbackTextureFlags;
    binding.constantsUpdated = true;
    binding.usedFallback = textures.usedFallback;
    binding.materialName = material.IsValid()
        ? "render-material[" + std::to_string(material.slot) + ":" +
              std::to_string(material.generation) + "]"
        : std::string();
    binding.message = textures.usedFallback
        ? "Material binding used explicit fallback resources"
        : "Material binding ready";

    outSnapshot.constantBuffer = std::move(constantBuffer);
    outSnapshot.descriptorSet = std::move(descriptorSet);
    outSnapshot.layout = RHIDescriptorSetLayoutRef(m_materialSetLayout);
    outSnapshot.textureViews.reserve(5);
    outSnapshot.textureViews.emplace_back(textures.baseColor);
    outSnapshot.textureViews.emplace_back(textures.normal);
    outSnapshot.textureViews.emplace_back(textures.metallicRoughness);
    outSnapshot.textureViews.emplace_back(textures.occlusion);
    outSnapshot.textureViews.emplace_back(textures.emissive);
    outSnapshot.textures.reserve(outSnapshot.textureViews.size());
    for (const RHITextureViewRef& view : outSnapshot.textureViews)
    {
        RHITexture* texture = view ? view->GetTexture() : nullptr;
        if (!texture)
        {
            outSnapshot = {};
            return false;
        }
        outSnapshot.textures.emplace_back(texture);
    }
    outSnapshot.samplers = {
        RHISamplerRef(textures.baseColorSampler),
        RHISamplerRef(textures.normalSampler),
        RHISamplerRef(textures.metallicRoughnessSampler),
        RHISamplerRef(textures.occlusionSampler),
        RHISamplerRef(textures.emissiveSampler)};
    outSnapshot.binding = std::move(binding);
    outSnapshot.binding.descriptorSet = outSnapshot.descriptorSet.Get();
    return outSnapshot.IsDrawable();
}

MaterialBindingResult MaterialSystem::PrepareResolvedMaterialBinding(
    MaterialSourceData source,
    const ResolvedMaterialTextures& textures,
    std::string materialName)
{
    source.textureFlags = textures.textureFlags;
    const MaterialGPUConstants constants = MaterialBinder::ConvertToGPU(source);

    FrameConstantUploadAllocation allocation;
    if (!m_materialConstantUploadArena ||
        !m_materialConstantUploadArena->Allocate(
            &constants, sizeof(constants), allocation))
    {
        MaterialBindingResult result;
        result.status = MaterialBindingStatus::Error;
        result.textureFlags = constants.textureFlags;
        result.fallbackTextureFlags = textures.fallbackTextureFlags;
        result.usedFallback = textures.usedFallback;
        result.materialName = materialName;
        result.message = "Failed to allocate a completion-tracked material constant page";
        return SetLastBindingResult(std::move(result));
    }

    ResolvedMaterialTextures pageTextures = textures;
    pageTextures.pageIdentity = allocation.pageIdentity;
    MaterialSetResolveResult setResult = GetOrCreateMaterialSetForResolved(
        pageTextures, allocation.buffer);
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
    result.constantBuffer = std::move(allocation.buffer);
    result.descriptorSetRef = std::move(setResult.descriptorSetRef);
    result.dynamicOffsets = {allocation.dynamicOffset};
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

RHIDescriptorSet* MaterialSystem::GetDefaultMaterialSet()
{
    return m_defaultMaterialSet.Get();
}

void MaterialSystem::TransitionMaterialTextures(RenderResourceHandle material,
                                                RHICommandContext& ctx,
                                                MaterialBindingOptions options) const
{
    if (!m_resourceRegistry)
        return;
    const RenderMaterialResourceData* materialData =
        m_resourceRegistry->ResolveMaterial(material);
    if (materialData == nullptr || !materialData->metadataValid)
        return;
    for (const MaterialUploadTextureBinding& binding :
         materialData->textureBindings)
    {
        if (!options.allowNormalMap &&
            binding.slot == MaterialUploadTextureSlot::Normal)
        {
            continue;
        }
        static_cast<void>(m_resourceRegistry->TransitionTexture(
            binding.texture, ctx, RHIResourceState::ShaderResource));
    }
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
    HashCombine(seed, std::hash<RHISampler*>{}(key.baseColorSampler));
    HashCombine(seed, std::hash<RHISampler*>{}(key.normalSampler));
    HashCombine(seed, std::hash<RHISampler*>{}(key.metallicRoughnessSampler));
    HashCombine(seed, std::hash<RHISampler*>{}(key.occlusionSampler));
    HashCombine(seed, std::hash<RHISampler*>{}(key.emissiveSampler));
    HashCombine(seed, std::hash<RHIBuffer*>{}(key.materialParameterTable));
    HashCombine(seed, std::hash<uint64>{}(key.viewGeneration));
    HashCombine(seed, std::hash<uint64>{}(key.pageIdentity));
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
    if (!m_materialConstantBuffer)
    {
        return false;
    }

    RHIBufferDesc tableDesc;
    tableDesc.size = sizeof(MaterialGPUConstants);
    tableDesc.usage = RHIBufferUsage::Structured |
                      RHIBufferUsage::ShaderResource;
    tableDesc.memoryType = RHIMemoryType::Upload;
    tableDesc.stride = sizeof(MaterialGPUConstants);
    tableDesc.debugName = "DefaultMaterialParameterTable";
    m_defaultMaterialParameterTable = m_device->CreateBuffer(tableDesc);
    if (!m_defaultMaterialParameterTable)
    {
        m_materialConstantBuffer.Reset();
        return false;
    }

    m_materialConstantUploadArena = std::make_unique<FrameConstantUploadArena>();
    if (!m_materialConstantUploadArena->Initialize(
            m_device,
            m_materialConstantStride,
            static_cast<uint32>(RVX_MAX_MATERIAL_CONSTANTS_PER_FRAME),
            "MaterialConstantUpload"))
    {
        m_materialConstantUploadArena.reset();
        return false;
    }
    return true;
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
    textures.baseColorSampler = m_defaultSampler.Get();
    textures.normalSampler = m_defaultSampler.Get();
    textures.metallicRoughnessSampler = m_defaultSampler.Get();
    textures.occlusionSampler = m_defaultSampler.Get();
    textures.emissiveSampler = m_defaultSampler.Get();
    textures.materialParameterTable = options.materialParameterTable
        ? options.materialParameterTable
        : m_defaultMaterialParameterTable.Get();
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
        for (size_t bindingIndex = 0;
             bindingIndex < materialData->textureBindings.size();
             ++bindingIndex)
        {
            const MaterialUploadTextureBinding& binding =
                materialData->textureBindings[bindingIndex];
            uint32 textureFlag = 0;
            RHITextureView** destination = nullptr;
            RHISampler** samplerDestination = nullptr;
            switch (binding.slot)
            {
                case MaterialUploadTextureSlot::BaseColor:
                    textureFlag = static_cast<uint32>(
                        MaterialTextureFlags::HasBaseColor);
                    destination = &textures.baseColor;
                    samplerDestination = &textures.baseColorSampler;
                    break;
                case MaterialUploadTextureSlot::Normal:
                    textureFlag = static_cast<uint32>(
                        MaterialTextureFlags::HasNormal);
                    destination = &textures.normal;
                    samplerDestination = &textures.normalSampler;
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
                    samplerDestination = &textures.metallicRoughnessSampler;
                    break;
                case MaterialUploadTextureSlot::Occlusion:
                    textureFlag = static_cast<uint32>(
                        MaterialTextureFlags::HasOcclusion);
                    destination = &textures.occlusion;
                    samplerDestination = &textures.occlusionSampler;
                    break;
                case MaterialUploadTextureSlot::Emissive:
                    textureFlag = static_cast<uint32>(
                        MaterialTextureFlags::HasEmissive);
                    destination = &textures.emissive;
                    samplerDestination = &textures.emissiveSampler;
                    break;
                default:
                    continue;
            }

            RHITexture* texture =
                m_resourceRegistry->ResolveTextureObject(binding.texture);
            RHITextureView* textureView =
                texture && viewCache ? viewCache->GetDefaultSRV(texture)
                                     : nullptr;
            RHISampler* sampler =
                bindingIndex < materialData->samplers.size()
                    ? materialData->samplers[bindingIndex].Get()
                    : nullptr;
            if (binding.isDefaultFallback || textureView == nullptr ||
                sampler == nullptr)
            {
                textures.usedFallback = true;
                textures.fallbackTextureFlags |= textureFlag;
                continue;
            }
            *destination = textureView;
            *samplerDestination = sampler;
            textures.textureFlags |= textureFlag;
        }
    }

    return textures;
}

MaterialSystem::MaterialSetResolveResult MaterialSystem::GetOrCreateMaterialSetForResolved(
    const ResolvedMaterialTextures& textures,
    const RHIBufferRef& constantBuffer)
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
    key.baseColorSampler = textures.baseColorSampler;
    key.normalSampler = textures.normalSampler;
    key.metallicRoughnessSampler = textures.metallicRoughnessSampler;
    key.occlusionSampler = textures.occlusionSampler;
    key.emissiveSampler = textures.emissiveSampler;
    key.materialParameterTable = textures.materialParameterTable;
    key.viewGeneration = textures.viewGeneration;
    key.pageIdentity = textures.pageIdentity;

    if (m_materialDescriptorCacheGeneration != textures.viewGeneration)
    {
        m_materialDescriptorCacheGeneration = textures.viewGeneration;
        QueueMaterialDescriptorCacheRetirement();
    }

    auto it = m_materialDescriptorCache.find(key);
    if (it != m_materialDescriptorCache.end())
    {
        result.descriptorSet = it->second.descriptorSet.Get();
        result.descriptorSetRef = it->second.descriptorSet;
        result.usedFallback = textures.usedFallback;
        result.status = textures.usedFallback ? MaterialBindingStatus::Fallback
                                              : MaterialBindingStatus::Ready;
        result.message = textures.usedFallback ? "Material descriptor used explicit fallback resources"
                                               : "Material descriptor ready";
        return result;
    }

    RHIDescriptorSetRef descriptorSet = CreateMaterialDescriptorSet(
        textures, constantBuffer.Get());
    if (!descriptorSet)
    {
        // A legacy default set is bound to its legacy upload buffer. Pairing
        // it with this page's dynamic offset would silently read unrelated
        // constants, so recording fails closed instead of changing buffers.
        result.status = MaterialBindingStatus::Error;
        result.message = "Material page descriptor creation failed";
        return result;
    }

    result.descriptorSet = descriptorSet.Get();
    result.descriptorSetRef = descriptorSet;
    result.usedFallback = textures.usedFallback;
    result.status = textures.usedFallback ? MaterialBindingStatus::Fallback
                                          : MaterialBindingStatus::Ready;
    result.message = textures.usedFallback ? "Material descriptor used explicit fallback resources"
                                           : "Material descriptor ready";
    MaterialDescriptorCacheEntry cacheEntry;
    cacheEntry.descriptorSet = std::move(descriptorSet);
    cacheEntry.samplers = {
        RHISamplerRef(textures.baseColorSampler),
        RHISamplerRef(textures.normalSampler),
        RHISamplerRef(textures.metallicRoughnessSampler),
        RHISamplerRef(textures.occlusionSampler),
        RHISamplerRef(textures.emissiveSampler)};
    cacheEntry.materialParameterTable =
        RHIBufferRef(textures.materialParameterTable);
    m_materialDescriptorCache.emplace(key, std::move(cacheEntry));
    return result;
}

RHIDescriptorSetRef MaterialSystem::CreateMaterialDescriptorSet(
    const ResolvedMaterialTextures& textures,
    RHIBuffer* constantBuffer)
{
    if (!m_materialSetLayout || constantBuffer == nullptr ||
        !textures.baseColor || !textures.normal || !textures.metallicRoughness ||
        !textures.occlusion || !textures.emissive ||
        !textures.baseColorSampler || !textures.normalSampler ||
        !textures.metallicRoughnessSampler || !textures.occlusionSampler ||
        !textures.emissiveSampler || !textures.materialParameterTable)
    {
        return {};
    }

    RHIDescriptorSetDesc descSetDesc;
    descSetDesc.layout = m_materialSetLayout;
    descSetDesc.debugName = "MaterialDescriptorSet";
    descSetDesc.BindBuffer(0, constantBuffer, 0, m_materialConstantStride);
    descSetDesc.BindTexture(1, textures.baseColor);
    descSetDesc.BindTexture(2, textures.normal);
    descSetDesc.BindTexture(3, textures.metallicRoughness);
    descSetDesc.BindTexture(4, textures.occlusion);
    descSetDesc.BindTexture(5, textures.emissive);
    descSetDesc.BindSampler(6, textures.baseColorSampler);
    descSetDesc.BindSampler(7, textures.normalSampler);
    descSetDesc.BindSampler(8, textures.metallicRoughnessSampler);
    descSetDesc.BindSampler(9, textures.occlusionSampler);
    descSetDesc.BindSampler(10, textures.emissiveSampler);
    descSetDesc.BindBuffer(11, textures.materialParameterTable);

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
                   "MaterialSystem: legacy material constant buffer exhausted for this frame (max {} material updates). "
                   "Recording must use completion-tracked material pages.",
                   RVX_MAX_MATERIAL_CONSTANTS_PER_FRAME);
        m_currentMaterialConstantOffset = std::numeric_limits<uint64>::max();
        return m_currentMaterialConstantOffset;
    }

    const uint64 offset = m_materialConstantCursor * m_materialConstantStride;
    ++m_materialConstantCursor;
    m_currentMaterialConstantOffset = offset;
    return offset;
}

} // namespace RVX
