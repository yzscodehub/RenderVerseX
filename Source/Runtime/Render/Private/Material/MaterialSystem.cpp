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
    constexpr uint64 RASTER_MATERIAL_SEMANTIC_DOMAIN =
        0x5256534D41544B31ull; // "RVSMATK1"
    constexpr uint64 RASTER_MATERIAL_MISSING_ASSET_TOKEN =
        0x4D4953534D41544Cull; // "MISSMATL"
    constexpr uint64 RASTER_DEFAULT_SAMPLER_TOKEN =
        0x44454653414D504Cull; // "DEFSAMPL"
    constexpr uint64 RASTER_CLASSIFICATION_SENTINEL_TOKEN =
        0x434C415353534E54ull; // "CLAS SNT"

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
        uint32 bits = std::bit_cast<uint32>(value);
        // GPU material semantics do not distinguish the two IEEE encodings
        // of zero. Keep the transcript key stable across either spelling.
        if ((bits & 0x7FFFFFFFu) == 0)
        {
            bits = 0;
        }
        HashBindingValue(hash, bits);
    }

    [[nodiscard]] uint64 GetRasterFallbackTextureToken(
        MaterialUploadTextureSlot slot) noexcept
    {
        switch (slot)
        {
            case MaterialUploadTextureSlot::BaseColor:
                return 0x46424B5F42415345ull;
            case MaterialUploadTextureSlot::Normal:
                return 0x46424B5F4E4F524Dull;
            case MaterialUploadTextureSlot::MetallicRoughness:
                return 0x46424B5F4D45524Full;
            case MaterialUploadTextureSlot::Occlusion:
                return 0x46424B5F4F43434Cull;
            case MaterialUploadTextureSlot::Emissive:
                return 0x46424B5F454D4953ull;
            case MaterialUploadTextureSlot::ToonRamp:
                return 0x46424B5F5452414Dull;
            case MaterialUploadTextureSlot::Transmission:
                return 0x46424B5F5452414Eull;
            case MaterialUploadTextureSlot::Thickness:
                return 0x46424B5F54484943ull;
        }
        return 0;
    }

    void HashRasterMaterialConstants(uint64& hash,
                                    MaterialGPUConstants constants) noexcept
    {
        const uint32 hasNormal = static_cast<uint32>(MaterialTextureFlags::HasNormal);
        // A disabled/omitted/fallback normal map makes normalScale
        // unobservable to the shader. Do not create transcript drift from
        // source values that were not selected for this draw.
        if ((constants.textureFlags & hasNormal) == 0)
        {
            constants.normalScale = 0.0f;
        }
        HashBindingFloat(hash, constants.baseColorFactor.x);
        HashBindingFloat(hash, constants.baseColorFactor.y);
        HashBindingFloat(hash, constants.baseColorFactor.z);
        HashBindingFloat(hash, constants.baseColorFactor.w);
        HashBindingFloat(hash, constants.metallicFactor);
        HashBindingFloat(hash, constants.roughnessFactor);
        HashBindingFloat(hash, constants.normalScale);
        HashBindingFloat(hash, constants.occlusionStrength);
        HashBindingFloat(hash, constants.emissiveColor.x);
        HashBindingFloat(hash, constants.emissiveColor.y);
        HashBindingFloat(hash, constants.emissiveColor.z);
        HashBindingFloat(hash, constants.emissiveStrength);
        HashBindingValue(hash, constants.textureFlags);
        HashBindingValue(hash, constants.alphaMode);
        HashBindingFloat(hash, constants.alphaCutoff);
        HashBindingValue(hash, constants.workflow);
        HashBindingValue(hash, constants.doubleSided);
        HashBindingValue(hash, constants.shadingModel);
        HashBindingValue(hash, constants.blendMode);
        // A stable table slot is an addressing detail, not draw semantics.
        // Classification-table content hashes still bind this field byte for
        // byte; descriptor semantic keys must remain stable if identical
        // assets are allocated at different slots.
        HashBindingFloat(hash, constants.toonBands);
        HashBindingFloat(hash, constants.toonSpecularThreshold);
        HashBindingFloat(hash, constants.toonRimStrength);
        HashBindingFloat(hash, constants.toonOutlineWidth);
        HashBindingFloat(hash, constants.toonShadowTint.x);
        HashBindingFloat(hash, constants.toonShadowTint.y);
        HashBindingFloat(hash, constants.toonShadowTint.z);
        HashBindingFloat(hash, constants.thinTransmissionParams.x);
        HashBindingFloat(hash, constants.thinTransmissionParams.y);
        HashBindingFloat(hash, constants.thinTransmissionParams.z);
        HashBindingFloat(hash, constants.thinTransmissionParams.w);
        HashBindingFloat(hash, constants.attenuationColor.x);
        HashBindingFloat(hash, constants.attenuationColor.y);
        HashBindingFloat(hash, constants.attenuationColor.z);
        const MaterialGPUUVTransform identityTransform{};
        for (uint32 slot = 0;
             slot < static_cast<uint32>(constants.uvTransforms.size());
             ++slot)
        {
            HashBindingValue(hash, slot);
            const uint32 textureFlag = 1u << slot;
            const MaterialGPUUVTransform& transform =
                (constants.textureFlags & textureFlag) != 0u
                    ? constants.uvTransforms[slot]
                    : identityTransform;
            HashBindingFloat(hash, transform.scaleOffset.x);
            HashBindingFloat(hash, transform.scaleOffset.y);
            HashBindingFloat(hash, transform.scaleOffset.z);
            HashBindingFloat(hash, transform.scaleOffset.w);
            HashBindingFloat(hash, transform.rotationUVSet.x);
            HashBindingFloat(hash, transform.rotationUVSet.y);
            HashBindingFloat(hash, transform.rotationUVSet.z);
            HashBindingFloat(hash, transform.rotationUVSet.w);
        }
    }

    void HashMaterialTableRowEvidence(
        uint64& hash,
        const MaterialParameterTableRowEvidence& evidence) noexcept
    {
        HashBindingValue(hash, evidence.stableSlot);
        HashBindingValue(hash, static_cast<uint8>(evidence.isAsset ? 1u : 0u));
        HashBindingValue(hash, evidence.shadingModel);
        HashBindingValue(hash, evidence.programFamilyHash);
        HashBindingValue(hash, evidence.parameterLayoutHash);
        HashBindingFloat(hash, evidence.toonBands);
        HashBindingFloat(hash, evidence.toonSpecularThreshold);
        HashBindingFloat(hash, evidence.toonRimStrength);
        HashBindingFloat(hash, evidence.toonOutlineWidth);
        HashBindingFloat(hash, evidence.toonShadowTint.x);
        HashBindingFloat(hash, evidence.toonShadowTint.y);
        HashBindingFloat(hash, evidence.toonShadowTint.z);
        HashBindingValue(
            hash,
            static_cast<uint8>(evidence.toonParametersValid ? 1u : 0u));
    }

    [[nodiscard]] MaterialParameterTableRowEvidence
    MakeDefaultMaterialTableRowEvidence(uint32 stableSlot) noexcept
    {
        MaterialParameterTableRowEvidence evidence;
        evidence.stableSlot = stableSlot;
        evidence.isAsset = false;
        evidence.shadingModel = MaterialShadingModel::StandardLit;
        evidence.programFamilyHash = GetMaterialProgramFamilyValueIdentity(
            GetMaterialProgramFamily(MaterialShadingModel::StandardLit));
        evidence.parameterLayoutHash = GetMaterialParameterLayoutHashV1();
        return evidence;
    }

    [[nodiscard]] MaterialParameterTableRowEvidence
    MakeMaterialTableRowEvidence(uint32 stableSlot,
                                 const MaterialStaticDescriptor& descriptor,
                                 const MaterialSourceData& source) noexcept
    {
        MaterialParameterTableRowEvidence evidence;
        evidence.stableSlot = stableSlot;
        evidence.isAsset = true;
        evidence.shadingModel = descriptor.shadingModel;
        evidence.programFamilyHash = GetMaterialProgramFamilyValueIdentity(
            descriptor.programFamily);
        evidence.parameterLayoutHash = descriptor.parameterLayoutHash;
        if (descriptor.shadingModel == MaterialShadingModel::ToonLit)
        {
            evidence.toonBands = source.toonBands;
            evidence.toonSpecularThreshold = source.toonSpecularThreshold;
            evidence.toonRimStrength = source.toonRimStrength;
            evidence.toonOutlineWidth = source.toonOutlineWidth;
            evidence.toonShadowTint = source.toonShadowTint;
            std::string validationError;
            evidence.toonParametersValid =
                ValidateMaterialSourceData(source, validationError);
        }
        return evidence;
    }

    uint64 CalculateMaterialTableContentHash(
        const std::vector<MaterialGPUConstants>& table,
        const std::vector<uint64>& semanticKeys,
        const std::vector<MaterialParameterTableRowEvidence>& rowEvidence) noexcept
    {
        if (table.empty() || table.size() != semanticKeys.size() ||
            table.size() != rowEvidence.size())
        {
            return 0;
        }

        uint64 hash = FNV_OFFSET;
        HashBindingValue(hash, RVX_MATERIAL_PARAMETER_TABLE_SCHEMA_VERSION);
        HashBindingValue(hash, static_cast<uint64>(sizeof(MaterialGPUConstants)));
        HashBindingValue(hash, static_cast<uint64>(table.size()));
        for (size_t index = 0; index < table.size(); ++index)
        {
            const auto* bytes = reinterpret_cast<const uint8*>(&table[index]);
            for (size_t byteIndex = 0; byteIndex < sizeof(MaterialGPUConstants);
                 ++byteIndex)
            {
                HashBindingByte(hash, bytes[byteIndex]);
            }
            HashBindingValue(hash, static_cast<uint64>(index));
            HashBindingValue(hash, semanticKeys[index]);
            HashMaterialTableRowEvidence(hash, rowEvidence[index]);
        }
        return hash == 0 ? 1 : hash;
    }

    void SetSuccessfulMaterialBindingIdentity(
        MaterialBindingResult& result,
        RenderResourceHandle material,
        uint64 contentRevision) noexcept
    {
        if (result.IsDrawable() && material.IsValid() && contentRevision != 0)
        {
            result.material = material;
            result.contentRevision = contentRevision;
        }
    }
} // namespace

uint64 GetMaterialProgramFamilyValueIdentity(
    std::string_view programFamily) noexcept
{
    if (programFamily.empty())
    {
        return 0;
    }

    uint64 hash = FNV_OFFSET;
    for (const char character : programFamily)
    {
        HashBindingByte(hash, static_cast<uint8>(character));
    }
    return hash == 0 ? 1 : hash;
}

MaterialSystem::MaterialSystem() = default;

MaterialSystem::~MaterialSystem()
{
    Shutdown();
}

bool MaterialSystem::Initialize(IRHIDevice* device,
                                RHIDescriptorSetLayout* materialSetLayout,
                                RenderResourceRegistry* resourceRegistry,
                                ResourceViewCache* resourceViewCache)
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
    m_resourceViewCache = resourceViewCache;
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
    defaultTextures.toonRamp = m_defaultWhiteTextureView.Get();
    defaultTextures.transmission = m_defaultWhiteTextureView.Get();
    defaultTextures.thickness = m_defaultWhiteTextureView.Get();
    defaultTextures.baseColorSampler = m_defaultSampler.Get();
    defaultTextures.normalSampler = m_defaultSampler.Get();
    defaultTextures.metallicRoughnessSampler = m_defaultSampler.Get();
    defaultTextures.occlusionSampler = m_defaultSampler.Get();
    defaultTextures.emissiveSampler = m_defaultSampler.Get();
    defaultTextures.toonRampSampler = m_defaultSampler.Get();
    defaultTextures.transmissionSampler = m_defaultSampler.Get();
    defaultTextures.thicknessSampler = m_defaultSampler.Get();
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
    m_semanticDescriptorStates.clear();
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
    m_resourceViewCache = nullptr;
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
        const AssetId textureAssetId =
            m_resourceRegistry->GetExactAssetId(binding.texture);
        if (!textureAssetId.IsValid())
        {
            return {};
        }
        HashBindingValue(hash, binding.slot);
        HashBindingValue(hash, textureAssetId.value);
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

bool MaterialSystem::ResolveSurfacePipelinePolicy(
    RenderResourceHandle material,
    MaterialCullMode& outCullMode,
    MaterialShadowPolicy& outShadowPolicy,
    bool& outRequiresUV1) const noexcept
{
    SurfacePathEligibility ignoredEligibility =
        SurfacePathEligibility::Unsupported;
    return ResolveSurfacePipelinePolicy(material,
                                        outCullMode,
                                        outShadowPolicy,
                                        outRequiresUV1,
                                        ignoredEligibility);
}

bool MaterialSystem::ResolveSurfacePipelinePolicy(
    RenderResourceHandle material,
    MaterialCullMode& outCullMode,
    MaterialShadowPolicy& outShadowPolicy,
    bool& outRequiresUV1,
    SurfacePathEligibility& outSurfacePathEligibility) const noexcept
{
    MaterialBlendMode ignoredBlendMode = MaterialBlendMode::Opaque;
    MaterialShadingModel ignoredShadingModel = MaterialShadingModel::StandardLit;
    int32 ignoredSortPriority = 0;
    return ResolveSurfacePipelinePolicy(material,
                                        outCullMode,
                                        outShadowPolicy,
                                        outRequiresUV1,
                                        outSurfacePathEligibility,
                                        ignoredBlendMode,
                                        ignoredShadingModel,
                                        ignoredSortPriority);
}

bool MaterialSystem::ResolveSurfacePipelinePolicy(
    RenderResourceHandle material,
    MaterialCullMode& outCullMode,
    MaterialShadowPolicy& outShadowPolicy,
    bool& outRequiresUV1,
    SurfacePathEligibility& outSurfacePathEligibility,
    MaterialBlendMode& outBlendMode,
    MaterialShadingModel& outShadingModel,
    int32& outSortPriority) const noexcept
{
    outCullMode = MaterialCullMode::Back;
    outShadowPolicy = MaterialShadowPolicy::CastAndReceive;
    outRequiresUV1 = false;
    outSurfacePathEligibility = SurfacePathEligibility::Unsupported;
    outBlendMode = MaterialBlendMode::Opaque;
    outShadingModel = MaterialShadingModel::StandardLit;
    outSortPriority = 0;
    if (!material.IsValid() || m_resourceRegistry == nullptr)
    {
        return false;
    }

    const RenderMaterialResourceData* materialData =
        m_resourceRegistry->ResolveMaterial(material);
    if (materialData == nullptr || !materialData->metadataValid)
    {
        return false;
    }

    const MaterialStaticDescriptor& descriptor =
        materialData->staticDescriptor;
    const SurfacePathEligibility surfacePathEligibility =
        ClassifySurfacePathEligibility(descriptor);
    if (surfacePathEligibility == SurfacePathEligibility::Unsupported)
    {
        return false;
    }
    const bool opaqueOrMasked =
        descriptor.blendMode == MaterialBlendMode::Opaque ||
        descriptor.blendMode == MaterialBlendMode::Masked;
    const bool translucent = !opaqueOrMasked;
    bool implementedMaterial = false;
    switch (descriptor.shadingModel)
    {
        case MaterialShadingModel::StandardLit:
            implementedMaterial = opaqueOrMasked ||
                descriptor.blendMode == MaterialBlendMode::AlphaBlend ||
                descriptor.blendMode == MaterialBlendMode::Premultiplied;
            break;
        case MaterialShadingModel::Unlit:
            implementedMaterial = opaqueOrMasked ||
                descriptor.blendMode == MaterialBlendMode::AlphaBlend ||
                descriptor.blendMode == MaterialBlendMode::Premultiplied ||
                descriptor.blendMode == MaterialBlendMode::Additive;
            break;
        case MaterialShadingModel::ThinTransmission:
            implementedMaterial = !opaqueOrMasked &&
                (descriptor.blendMode == MaterialBlendMode::AlphaBlend ||
                 descriptor.blendMode == MaterialBlendMode::Premultiplied);
            break;
        case MaterialShadingModel::ToonLit:
            // GFX15 deliberately does not widen ToonLit to translucency.
            implementedMaterial = opaqueOrMasked;
            break;
    }
    const bool implementedPhase = opaqueOrMasked
        ? descriptor.translucencyPhase == MaterialTranslucencyPhase::Opaque
        : descriptor.translucencyPhase == MaterialTranslucencyPhase::Standard;
    if (descriptor.domain != MaterialDomain::Surface ||
        !implementedMaterial ||
        !implementedPhase ||
        ((descriptor.shadingModel == MaterialShadingModel::StandardLit ||
          descriptor.shadingModel == MaterialShadingModel::ToonLit) &&
         materialData->sourceData.workflow !=
             MaterialSourceWorkflow::MetallicRoughness) ||
        (translucent && MaterialShadowPolicyCasts(descriptor.shadowPolicy)) ||
        (descriptor.shadingModel == MaterialShadingModel::ToonLit &&
         (descriptor.featureMask &
          (1ull << static_cast<uint8>(MaterialTextureSlot::ToonRamp))) != 0) ||
        descriptor.programFamily !=
            GetMaterialProgramFamily(descriptor.shadingModel) ||
        descriptor.parameterLayoutHash != GetMaterialParameterLayoutHashV1())
    {
        // GFX15 supports the explicit raster translucency matrix only:
        // StandardLit Alpha/Premultiplied, Unlit Alpha/Premultiplied/Additive,
        // and ThinTransmission Alpha/Premultiplied. After-tone-map materials,
        // Toon translucency, StandardLit additive, Thin additive, and every
        // transparent shadow caster remain fail-closed.
        return false;
    }

    switch (descriptor.cullMode)
    {
        case MaterialCullMode::Back:
        case MaterialCullMode::NoCull:
            outCullMode = descriptor.cullMode;
            break;
        default:
            return false;
    }
    switch (descriptor.shadowPolicy)
    {
        case MaterialShadowPolicy::None:
        case MaterialShadowPolicy::Cast:
        case MaterialShadowPolicy::Receive:
        case MaterialShadowPolicy::CastAndReceive:
            outShadowPolicy = descriptor.shadowPolicy;
            break;
        default:
            return false;
    }

    for (const MaterialUploadTextureBinding& binding :
         materialData->textureBindings)
    {
        const uint64 featureBit =
            1ull << static_cast<uint32>(binding.slot);
        // Pipeline vertex requirements must represent the texture bindings
        // that the shader can actually sample. Default semantic resources and
        // descriptor-disabled slots resolve to an identity texture and do not
        // require an authored secondary UV stream.
        if (binding.isDefaultFallback || !binding.texture.IsValid() ||
            (descriptor.featureMask & featureBit) == 0)
        {
            continue;
        }
        if (binding.uvSet < 0 || binding.uvSet > 1)
        {
            return false;
        }
        outRequiresUV1 = outRequiresUV1 || binding.uvSet == 1;
    }
    outSurfacePathEligibility = surfacePathEligibility;
    outBlendMode = descriptor.blendMode;
    outShadingModel = descriptor.shadingModel;
    outSortPriority = descriptor.sortPriority;
    return true;
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

    ResourceViewCache* const resolvedViewCache = viewCache
        ? viewCache : m_resourceViewCache;

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

    std::vector<MaterialGPUConstants> table(
        static_cast<size_t>(maxSlot) + 1U);
    for (uint32 slot = 0; slot <= maxSlot; ++slot)
    {
        table[slot] = MaterialBinder::ConvertToGPU(
            MaterialStaticDescriptor{}, MaterialSourceData{}, {}, slot);
    }
    std::vector<uint64> rasterMaterialSemanticKeysBySlot(table.size(), 0);
    std::vector<MaterialParameterTableRowEvidence> rowEvidence;
    rowEvidence.reserve(table.size());
    for (uint32 slot = 0; slot < table.size(); ++slot)
    {
        rowEvidence.emplace_back(MakeDefaultMaterialTableRowEvidence(slot));
    }
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
            request.material, resolvedViewCache, options);
        if (textures.usedFallback)
        {
            return false;
        }
        MaterialSourceData source = materialData->sourceData;
        source.textureFlags = textures.textureFlags;
        const MaterialGPUConstants constants = MaterialBinder::ConvertToGPU(
            materialData->staticDescriptor,
            source,
            textures.textureBindings,
            request.material.slot);
        const uint64 semanticKey = BuildSemanticDescriptorContentKey(
            request.material, constants, textures.textureEntries);
        table[slot] = constants;
        rasterMaterialSemanticKeysBySlot[slot] = semanticKey;
        rowEvidence[slot] = MakeMaterialTableRowEvidence(
            slot, materialData->staticDescriptor, materialData->sourceData);
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
    if (!buffer->CommitMappedWrite())
    {
        return false;
    }

    outSnapshot.buffer = std::move(buffer);
    outSnapshot.schemaVersion = RVX_MATERIAL_PARAMETER_TABLE_SCHEMA_VERSION;
    outSnapshot.stride = sizeof(MaterialGPUConstants);
    outSnapshot.slotCount = static_cast<uint32>(table.size());
    outSnapshot.materialCount = static_cast<uint32>(unique.size());
    outSnapshot.rasterMaterialSemanticKeysBySlot =
        std::move(rasterMaterialSemanticKeysBySlot);
    outSnapshot.valueOnlyRowEvidence = std::move(rowEvidence);
    outSnapshot.contentHash = CalculateMaterialTableContentHash(
        table,
        outSnapshot.rasterMaterialSemanticKeysBySlot,
        outSnapshot.valueOnlyRowEvidence);
    return outSnapshot.IsValid();
}

bool MaterialSystem::CreateMaterialClassificationTableSnapshot(
    std::span<const RenderResourceHandle> materials,
    MaterialParameterTableSnapshot& outSnapshot) const
{
    outSnapshot = {};
    if (!m_initialized || !m_device || !m_resourceRegistry)
    {
        return false;
    }

    std::unordered_map<uint32, RenderResourceHandle> unique;
    uint32 maxSlot = 0;
    for (const RenderResourceHandle material : materials)
    {
        if (!material.IsValid() ||
            material.slot >= RVX_MAX_MATERIAL_CONSTANTS_PER_FRAME)
        {
            return false;
        }
        const auto [it, inserted] = unique.emplace(material.slot, material);
        if (!inserted && it->second.generation != material.generation)
        {
            return false;
        }
        maxSlot = std::max(maxSlot, material.slot);
    }

    const bool usesDefaultClassificationSentinel = materials.empty();
    std::vector<MaterialGPUConstants> table(
        usesDefaultClassificationSentinel ? 1U
                                          : static_cast<size_t>(maxSlot) + 1U);
    std::vector<uint64> semanticKeys(table.size(), 0);
    std::vector<MaterialParameterTableRowEvidence> rowEvidence;
    rowEvidence.reserve(table.size());
    for (uint32 slot = 0; slot < table.size(); ++slot)
    {
        table[slot] = MaterialBinder::ConvertToGPU(
            MaterialStaticDescriptor{}, MaterialSourceData{}, {}, slot);
        rowEvidence.emplace_back(MakeDefaultMaterialTableRowEvidence(slot));
    }

    if (usesDefaultClassificationSentinel)
    {
        // Empty Deferred scenes still bind t7. Publish an explicit default
        // StandardLit row rather than pretending a registered material exists.
        semanticKeys[0] = RASTER_CLASSIFICATION_SENTINEL_TOKEN;
    }

    for (const auto& [slot, material] : unique)
    {
        const RenderMaterialResourceData* materialData =
            m_resourceRegistry->ResolveMaterial(material);
        if (materialData == nullptr || !materialData->metadataValid)
        {
            return false;
        }

        const MaterialGPUConstants constants = MaterialBinder::ConvertToGPU(
            materialData->staticDescriptor,
            materialData->sourceData,
            {},
            slot);
        const uint64 semanticKey = BuildMaterialClassificationSemanticKey(
            material, constants);
        if (semanticKey == 0)
        {
            return false;
        }
        table[slot] = constants;
        semanticKeys[slot] = semanticKey;
        rowEvidence[slot] = MakeMaterialTableRowEvidence(
            slot, materialData->staticDescriptor, materialData->sourceData);
    }

    const uint64 contentHash = CalculateMaterialTableContentHash(
        table, semanticKeys, rowEvidence);
    if (contentHash == 0)
    {
        return false;
    }

    RHIBufferDesc desc;
    desc.size = static_cast<uint64>(table.size()) *
                sizeof(MaterialGPUConstants);
    desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    desc.memoryType = RHIMemoryType::Upload;
    desc.stride = sizeof(MaterialGPUConstants);
    desc.debugName = "MaterialClassificationTable";
    RHIBufferRef buffer = m_device->CreateBuffer(desc);
    void* mapped = buffer ? buffer->Map() : nullptr;
    if (mapped == nullptr)
    {
        return false;
    }
    std::memcpy(mapped, table.data(), static_cast<size_t>(desc.size));
    if (!buffer->CommitMappedWrite())
    {
        return false;
    }

    outSnapshot.buffer = std::move(buffer);
    outSnapshot.schemaVersion = RVX_MATERIAL_PARAMETER_TABLE_SCHEMA_VERSION;
    outSnapshot.stride = sizeof(MaterialGPUConstants);
    outSnapshot.slotCount = static_cast<uint32>(table.size());
    outSnapshot.materialCount = usesDefaultClassificationSentinel
        ? 1U
        : static_cast<uint32>(unique.size());
    outSnapshot.usesDefaultClassificationSentinel =
        usesDefaultClassificationSentinel;
    outSnapshot.contentHash = contentHash;
    outSnapshot.valueOnlyRowEvidence = std::move(rowEvidence);
    outSnapshot.rasterMaterialSemanticKeysBySlot = std::move(semanticKeys);
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
    const uint64 contentRevision = materialData != nullptr &&
            m_resourceRegistry != nullptr
        ? m_resourceRegistry->GetContentRevision(material)
        : 0;
    MaterialStaticDescriptor staticDescriptor;
    MaterialSourceData source;
    if (materialData && materialData->metadataValid)
    {
        staticDescriptor = materialData->staticDescriptor;
        source = materialData->sourceData;
    }
    ResourceViewCache* const resolvedViewCache = viewCache
        ? viewCache : m_resourceViewCache;
    const ResolvedMaterialTextures textures =
        ResolveMaterialTextures(material, resolvedViewCache, options);
    const std::string materialName = material.IsValid()
        ? "render-material[" + std::to_string(material.slot) + ":" +
              std::to_string(material.generation) + "]"
        : std::string();
    return PrepareResolvedMaterialBinding(material,
                                          contentRevision,
                                          staticDescriptor,
                                          std::move(source),
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
    MaterialStaticDescriptor staticDescriptor;
    MaterialSourceData source;
    if (materialData && materialData->metadataValid)
    {
        staticDescriptor = materialData->staticDescriptor;
        source = materialData->sourceData;
    }
    ResourceViewCache* const resolvedViewCache = viewCache
        ? viewCache : m_resourceViewCache;
    const ResolvedMaterialTextures textures =
        ResolveMaterialTextures(material, resolvedViewCache, options);
    if (!textures.baseColor || !textures.normal || !textures.metallicRoughness ||
        !textures.occlusion || !textures.emissive ||
        !textures.toonRamp || !textures.transmission || !textures.thickness ||
        !textures.baseColorSampler || !textures.normalSampler ||
        !textures.metallicRoughnessSampler || !textures.occlusionSampler ||
        !textures.emissiveSampler || !textures.toonRampSampler ||
        !textures.transmissionSampler || !textures.thicknessSampler ||
        !textures.materialParameterTable)
    {
        return false;
    }

    source.textureFlags = textures.textureFlags;
    const MaterialGPUConstants constants = MaterialBinder::ConvertToGPU(
        staticDescriptor, source, textures.textureBindings, material.slot);
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
    if (!constantBuffer->CommitMappedWrite())
    {
        return false;
    }

    RHIDescriptorSetDesc descriptorDesc;
    descriptorDesc.layout = m_materialSetLayout;
    descriptorDesc.debugName = "ObjectVelocityRecordMaterialDescriptorSet";
    descriptorDesc.BindBuffer(RVX_MATERIAL_DESCRIPTOR_CONSTANT_BUFFER_BINDING,
                              constantBuffer.Get(), 0, stride);
    descriptorDesc.BindTexture(GetMaterialTextureDescriptorBinding(
                                   MaterialUploadTextureSlot::BaseColor),
                               textures.baseColor);
    descriptorDesc.BindTexture(GetMaterialTextureDescriptorBinding(
                                   MaterialUploadTextureSlot::Normal),
                               textures.normal);
    descriptorDesc.BindTexture(GetMaterialTextureDescriptorBinding(
                                   MaterialUploadTextureSlot::MetallicRoughness),
                               textures.metallicRoughness);
    descriptorDesc.BindTexture(GetMaterialTextureDescriptorBinding(
                                   MaterialUploadTextureSlot::Occlusion),
                               textures.occlusion);
    descriptorDesc.BindTexture(GetMaterialTextureDescriptorBinding(
                                   MaterialUploadTextureSlot::Emissive),
                               textures.emissive);
    descriptorDesc.BindTexture(GetMaterialTextureDescriptorBinding(
                                   MaterialUploadTextureSlot::ToonRamp),
                               textures.toonRamp);
    descriptorDesc.BindTexture(GetMaterialTextureDescriptorBinding(
                                   MaterialUploadTextureSlot::Transmission),
                               textures.transmission);
    descriptorDesc.BindTexture(GetMaterialTextureDescriptorBinding(
                                   MaterialUploadTextureSlot::Thickness),
                               textures.thickness);
    descriptorDesc.BindSampler(GetMaterialSamplerDescriptorBinding(
                                   MaterialUploadTextureSlot::BaseColor),
                               textures.baseColorSampler);
    descriptorDesc.BindSampler(GetMaterialSamplerDescriptorBinding(
                                   MaterialUploadTextureSlot::Normal),
                               textures.normalSampler);
    descriptorDesc.BindSampler(GetMaterialSamplerDescriptorBinding(
                                   MaterialUploadTextureSlot::MetallicRoughness),
                               textures.metallicRoughnessSampler);
    descriptorDesc.BindSampler(GetMaterialSamplerDescriptorBinding(
                                   MaterialUploadTextureSlot::Occlusion),
                               textures.occlusionSampler);
    descriptorDesc.BindSampler(GetMaterialSamplerDescriptorBinding(
                                   MaterialUploadTextureSlot::Emissive),
                               textures.emissiveSampler);
    descriptorDesc.BindSampler(GetMaterialSamplerDescriptorBinding(
                                   MaterialUploadTextureSlot::ToonRamp),
                               textures.toonRampSampler);
    descriptorDesc.BindSampler(GetMaterialSamplerDescriptorBinding(
                                   MaterialUploadTextureSlot::Transmission),
                               textures.transmissionSampler);
    descriptorDesc.BindSampler(GetMaterialSamplerDescriptorBinding(
                                   MaterialUploadTextureSlot::Thickness),
                               textures.thicknessSampler);
    descriptorDesc.BindBuffer(RVX_MATERIAL_DESCRIPTOR_PARAMETER_TABLE_BINDING,
                              textures.materialParameterTable);
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
    SetSuccessfulMaterialBindingIdentity(
        binding,
        material,
        materialData != nullptr && m_resourceRegistry != nullptr
            ? m_resourceRegistry->GetContentRevision(material)
            : 0);
    FinalizeSemanticDescriptorEvidence(
        binding, material, constants, textures);

    outSnapshot.constantBuffer = std::move(constantBuffer);
    outSnapshot.descriptorSet = std::move(descriptorSet);
    outSnapshot.layout = RHIDescriptorSetLayoutRef(m_materialSetLayout);
    outSnapshot.textureViews.reserve(RVX_MATERIAL_GPU_TEXTURE_SLOT_COUNT);
    outSnapshot.textureViews.emplace_back(textures.baseColor);
    outSnapshot.textureViews.emplace_back(textures.normal);
    outSnapshot.textureViews.emplace_back(textures.metallicRoughness);
    outSnapshot.textureViews.emplace_back(textures.occlusion);
    outSnapshot.textureViews.emplace_back(textures.emissive);
    outSnapshot.textureViews.emplace_back(textures.toonRamp);
    outSnapshot.textureViews.emplace_back(textures.transmission);
    outSnapshot.textureViews.emplace_back(textures.thickness);
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
        RHISamplerRef(textures.emissiveSampler),
        RHISamplerRef(textures.toonRampSampler),
        RHISamplerRef(textures.transmissionSampler),
        RHISamplerRef(textures.thicknessSampler)};
    outSnapshot.binding = std::move(binding);
    outSnapshot.binding.descriptorSet = outSnapshot.descriptorSet.Get();
    return outSnapshot.IsDrawable();
}

MaterialBindingResult MaterialSystem::PrepareResolvedMaterialBinding(
    RenderResourceHandle material,
    uint64 contentRevision,
    MaterialStaticDescriptor staticDescriptor,
    MaterialSourceData source,
    const ResolvedMaterialTextures& textures,
    std::string materialName)
{
    source.textureFlags = textures.textureFlags;
    const MaterialGPUConstants constants = MaterialBinder::ConvertToGPU(
        staticDescriptor, source, textures.textureBindings, material.slot);

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
    SetSuccessfulMaterialBindingIdentity(result, material, contentRevision);
    FinalizeSemanticDescriptorEvidence(
        result, material, constants, textures);

    return SetLastBindingResult(std::move(result));
}

void MaterialSystem::FinalizeSemanticDescriptorEvidence(
    MaterialBindingResult& result,
    RenderResourceHandle requestedMaterial,
    const MaterialGPUConstants& constants,
    const ResolvedMaterialTextures& textures)
{
    result.textureEntries = textures.textureEntries;
    if (!result.IsDrawable())
    {
        return;
    }

    result.descriptorContentKey = BuildSemanticDescriptorContentKey(
        requestedMaterial, constants, result.textureEntries);
    if (result.descriptorContentKey == 0 || !result.material.IsValid() ||
        result.contentRevision == 0)
    {
        return;
    }
    SemanticDescriptorState& state =
        m_semanticDescriptorStates[result.material];
    if (state.revision == 0)
    {
        state.contentKey = result.descriptorContentKey;
        state.revision = 1;
    }
    else if (state.contentKey != result.descriptorContentKey)
    {
        state.contentKey = result.descriptorContentKey;
        if (state.revision != std::numeric_limits<uint64>::max())
        {
            ++state.revision;
        }
    }
    result.descriptorRevision = state.revision;
}

uint64 MaterialSystem::BuildSemanticDescriptorContentKey(
    RenderResourceHandle requestedMaterial,
    const MaterialGPUConstants& constants,
    const std::vector<MaterialBindingTextureEntry>& textureEntries) const noexcept
{
    uint64 hash = FNV_OFFSET;
    HashBindingValue(hash, RASTER_MATERIAL_SEMANTIC_DOMAIN);
    if (requestedMaterial.IsValid())
    {
        const AssetId materialAsset = m_resourceRegistry != nullptr
            ? m_resourceRegistry->GetExactAssetId(requestedMaterial)
            : AssetId{};
        if (!materialAsset.IsValid())
        {
            return 0;
        }
        HashBindingValue(hash, materialAsset.value);
    }
    else
    {
        HashBindingValue(hash, RASTER_MATERIAL_MISSING_ASSET_TOKEN);
    }
    HashRasterMaterialConstants(hash, constants);

    for (uint8 slotValue =
             static_cast<uint8>(MaterialUploadTextureSlot::BaseColor);
         slotValue <= static_cast<uint8>(MaterialUploadTextureSlot::Thickness);
         ++slotValue)
    {
        const MaterialUploadTextureSlot slot =
            static_cast<MaterialUploadTextureSlot>(slotValue);
        const MaterialBindingTextureEntry* selected = nullptr;
        for (const MaterialBindingTextureEntry& entry : textureEntries)
        {
            if (entry.slot != slot)
            {
                continue;
            }
            if (selected != nullptr)
            {
                return 0;
            }
            selected = &entry;
        }

        HashBindingValue(hash, slot);
        if (selected == nullptr || selected->fallbackUsed)
        {
            HashBindingValue(hash, GetRasterFallbackTextureToken(slot));
            HashBindingValue(hash, RASTER_DEFAULT_SAMPLER_TOKEN);
            continue;
        }

        const AssetId textureAsset = m_resourceRegistry != nullptr
            ? m_resourceRegistry->GetExactAssetId(selected->texture)
            : AssetId{};
        if (!textureAsset.IsValid())
        {
            return 0;
        }
        HashBindingValue(hash, textureAsset.value);
        HashBindingValue(hash, selected->sampler.wrapS);
        HashBindingValue(hash, selected->sampler.wrapT);
        HashBindingValue(hash, selected->sampler.minFilter);
        HashBindingValue(hash, selected->sampler.magFilter);
    }
    // Zero denotes unavailable evidence at the public boundary.
    return hash == 0 ? 1 : hash;
}

uint64 MaterialSystem::BuildMaterialClassificationSemanticKey(
    RenderResourceHandle material,
    const MaterialGPUConstants& constants) const noexcept
{
    if (!material.IsValid() || m_resourceRegistry == nullptr)
    {
        return 0;
    }
    const AssetId materialAsset = m_resourceRegistry->GetExactAssetId(material);
    const uint64 contentRevision =
        m_resourceRegistry->GetContentRevision(material);
    if (!materialAsset.IsValid() || contentRevision == 0)
    {
        return 0;
    }

    uint64 hash = FNV_OFFSET;
    HashBindingValue(hash, RASTER_MATERIAL_SEMANTIC_DOMAIN);
    HashBindingValue(hash, materialAsset.value);
    HashBindingValue(hash, material.slot);
    HashBindingValue(hash, material.generation);
    HashBindingValue(hash, contentRevision);
    HashRasterMaterialConstants(hash, constants);
    return hash == 0 ? 1 : hash;
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
    HashCombine(seed, std::hash<RHITextureView*>{}(key.toonRamp));
    HashCombine(seed, std::hash<RHITextureView*>{}(key.transmission));
    HashCombine(seed, std::hash<RHITextureView*>{}(key.thickness));
    HashCombine(seed, std::hash<RHISampler*>{}(key.baseColorSampler));
    HashCombine(seed, std::hash<RHISampler*>{}(key.normalSampler));
    HashCombine(seed, std::hash<RHISampler*>{}(key.metallicRoughnessSampler));
    HashCombine(seed, std::hash<RHISampler*>{}(key.occlusionSampler));
    HashCombine(seed, std::hash<RHISampler*>{}(key.emissiveSampler));
    HashCombine(seed, std::hash<RHISampler*>{}(key.toonRampSampler));
    HashCombine(seed, std::hash<RHISampler*>{}(key.transmissionSampler));
    HashCombine(seed, std::hash<RHISampler*>{}(key.thicknessSampler));
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
    textures.toonRamp = m_defaultWhiteTextureView.Get();
    textures.transmission = m_defaultWhiteTextureView.Get();
    textures.thickness = m_defaultWhiteTextureView.Get();
    textures.baseColorSampler = m_defaultSampler.Get();
    textures.normalSampler = m_defaultSampler.Get();
    textures.metallicRoughnessSampler = m_defaultSampler.Get();
    textures.occlusionSampler = m_defaultSampler.Get();
    textures.emissiveSampler = m_defaultSampler.Get();
    textures.toonRampSampler = m_defaultSampler.Get();
    textures.transmissionSampler = m_defaultSampler.Get();
    textures.thicknessSampler = m_defaultSampler.Get();
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
        textures.textureBindings = materialData->textureBindings;
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
                case MaterialUploadTextureSlot::ToonRamp:
                    textureFlag = static_cast<uint32>(
                        MaterialTextureFlags::HasToonRamp);
                    destination = &textures.toonRamp;
                    samplerDestination = &textures.toonRampSampler;
                    break;
                case MaterialUploadTextureSlot::Transmission:
                    textureFlag = static_cast<uint32>(
                        MaterialTextureFlags::HasTransmission);
                    destination = &textures.transmission;
                    samplerDestination = &textures.transmissionSampler;
                    break;
                case MaterialUploadTextureSlot::Thickness:
                    textureFlag = static_cast<uint32>(
                        MaterialTextureFlags::HasThickness);
                    destination = &textures.thickness;
                    samplerDestination = &textures.thicknessSampler;
                    break;
                default:
                    continue;
            }

            MaterialBindingTextureEntry entry;
            entry.slot = binding.slot;
            entry.texture = binding.texture;
            entry.contentRevision = m_resourceRegistry != nullptr
                ? m_resourceRegistry->GetContentRevision(binding.texture)
                : 0;
            entry.fallbackUsed = binding.isDefaultFallback;
            entry.sampler = {
                binding.wrapS,
                binding.wrapT,
                binding.minFilter,
                binding.magFilter};
            if (binding.slot == MaterialUploadTextureSlot::Normal &&
                !options.allowNormalMap)
            {
                textures.usedFallback = true;
                textures.normalMapDisabled = true;
                textures.fallbackTextureFlags |= textureFlag;
                entry.fallbackUsed = true;
                textures.textureEntries.push_back(entry);
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
                entry.fallbackUsed = true;
                textures.textureEntries.push_back(entry);
                continue;
            }
            *destination = textureView;
            *samplerDestination = sampler;
            textures.textureFlags |= textureFlag;
            textures.textureEntries.push_back(entry);
        }

        std::sort(textures.textureEntries.begin(), textures.textureEntries.end(),
            [](const MaterialBindingTextureEntry& lhs,
               const MaterialBindingTextureEntry& rhs)
            {
                return lhs.slot < rhs.slot;
            });
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
    key.toonRamp = textures.toonRamp;
    key.transmission = textures.transmission;
    key.thickness = textures.thickness;
    key.baseColorSampler = textures.baseColorSampler;
    key.normalSampler = textures.normalSampler;
    key.metallicRoughnessSampler = textures.metallicRoughnessSampler;
    key.occlusionSampler = textures.occlusionSampler;
    key.emissiveSampler = textures.emissiveSampler;
    key.toonRampSampler = textures.toonRampSampler;
    key.transmissionSampler = textures.transmissionSampler;
    key.thicknessSampler = textures.thicknessSampler;
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
        RHISamplerRef(textures.emissiveSampler),
        RHISamplerRef(textures.toonRampSampler),
        RHISamplerRef(textures.transmissionSampler),
        RHISamplerRef(textures.thicknessSampler)};
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
        !textures.toonRamp || !textures.transmission || !textures.thickness ||
        !textures.baseColorSampler || !textures.normalSampler ||
        !textures.metallicRoughnessSampler || !textures.occlusionSampler ||
        !textures.emissiveSampler || !textures.toonRampSampler ||
        !textures.transmissionSampler || !textures.thicknessSampler ||
        !textures.materialParameterTable)
    {
        return {};
    }

    RHIDescriptorSetDesc descSetDesc;
    descSetDesc.layout = m_materialSetLayout;
    descSetDesc.debugName = "MaterialDescriptorSet";
    descSetDesc.BindBuffer(RVX_MATERIAL_DESCRIPTOR_CONSTANT_BUFFER_BINDING,
                           constantBuffer, 0, m_materialConstantStride);
    descSetDesc.BindTexture(GetMaterialTextureDescriptorBinding(
                                MaterialUploadTextureSlot::BaseColor),
                            textures.baseColor);
    descSetDesc.BindTexture(GetMaterialTextureDescriptorBinding(
                                MaterialUploadTextureSlot::Normal),
                            textures.normal);
    descSetDesc.BindTexture(GetMaterialTextureDescriptorBinding(
                                MaterialUploadTextureSlot::MetallicRoughness),
                            textures.metallicRoughness);
    descSetDesc.BindTexture(GetMaterialTextureDescriptorBinding(
                                MaterialUploadTextureSlot::Occlusion),
                            textures.occlusion);
    descSetDesc.BindTexture(GetMaterialTextureDescriptorBinding(
                                MaterialUploadTextureSlot::Emissive),
                            textures.emissive);
    descSetDesc.BindTexture(GetMaterialTextureDescriptorBinding(
                                MaterialUploadTextureSlot::ToonRamp),
                            textures.toonRamp);
    descSetDesc.BindTexture(GetMaterialTextureDescriptorBinding(
                                MaterialUploadTextureSlot::Transmission),
                            textures.transmission);
    descSetDesc.BindTexture(GetMaterialTextureDescriptorBinding(
                                MaterialUploadTextureSlot::Thickness),
                            textures.thickness);
    descSetDesc.BindSampler(GetMaterialSamplerDescriptorBinding(
                                MaterialUploadTextureSlot::BaseColor),
                            textures.baseColorSampler);
    descSetDesc.BindSampler(GetMaterialSamplerDescriptorBinding(
                                MaterialUploadTextureSlot::Normal),
                            textures.normalSampler);
    descSetDesc.BindSampler(GetMaterialSamplerDescriptorBinding(
                                MaterialUploadTextureSlot::MetallicRoughness),
                            textures.metallicRoughnessSampler);
    descSetDesc.BindSampler(GetMaterialSamplerDescriptorBinding(
                                MaterialUploadTextureSlot::Occlusion),
                            textures.occlusionSampler);
    descSetDesc.BindSampler(GetMaterialSamplerDescriptorBinding(
                                MaterialUploadTextureSlot::Emissive),
                            textures.emissiveSampler);
    descSetDesc.BindSampler(GetMaterialSamplerDescriptorBinding(
                                MaterialUploadTextureSlot::ToonRamp),
                            textures.toonRampSampler);
    descSetDesc.BindSampler(GetMaterialSamplerDescriptorBinding(
                                MaterialUploadTextureSlot::Transmission),
                            textures.transmissionSampler);
    descSetDesc.BindSampler(GetMaterialSamplerDescriptorBinding(
                                MaterialUploadTextureSlot::Thickness),
                            textures.thicknessSampler);
    descSetDesc.BindBuffer(RVX_MATERIAL_DESCRIPTOR_PARAMETER_TABLE_BINDING,
                           textures.materialParameterTable);

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
