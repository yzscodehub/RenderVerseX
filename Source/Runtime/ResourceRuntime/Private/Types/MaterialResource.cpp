#include "Resource/Types/MaterialResource.h"
#include "Core/Log.h"
#include "Core/Diagnostics/JsonWriter.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <utility>

namespace RVX::Resource
{
AssetMaterialMode MaterialResource::GetAssetMaterialMode() const
{
    switch (GetAlphaMode())
    {
        case MaterialAlphaMode::Mask:
            return AssetMaterialMode::Masked;
        case MaterialAlphaMode::Blend:
            return AssetMaterialMode::Transparent;
        case MaterialAlphaMode::Opaque:
        default:
            return AssetMaterialMode::Opaque;
    }
}

namespace
{
    using Diagnostics::JsonBool;
    using Diagnostics::JsonString;

    MaterialSamplerWrap ToMaterialSamplerWrap(TextureInfo::WrapMode mode)
    {
        switch (mode)
        {
            case TextureInfo::WrapMode::MirrorRepeat:
                return MaterialSamplerWrap::MirrorRepeat;
            case TextureInfo::WrapMode::ClampToEdge:
                return MaterialSamplerWrap::ClampToEdge;
            case TextureInfo::WrapMode::ClampToBorder:
                return MaterialSamplerWrap::ClampToBorder;
            case TextureInfo::WrapMode::Repeat:
            default:
                return MaterialSamplerWrap::Repeat;
        }
    }

    MaterialSamplerFilter ToMaterialSamplerFilter(TextureInfo::FilterMode mode)
    {
        switch (mode)
        {
            case TextureInfo::FilterMode::Nearest:
                return MaterialSamplerFilter::Nearest;
            case TextureInfo::FilterMode::NearestMipmapNearest:
                return MaterialSamplerFilter::NearestMipmapNearest;
            case TextureInfo::FilterMode::LinearMipmapNearest:
                return MaterialSamplerFilter::LinearMipmapNearest;
            case TextureInfo::FilterMode::NearestMipmapLinear:
                return MaterialSamplerFilter::NearestMipmapLinear;
            case TextureInfo::FilterMode::LinearMipmapLinear:
                return MaterialSamplerFilter::LinearMipmapLinear;
            case TextureInfo::FilterMode::Linear:
            default:
                return MaterialSamplerFilter::Linear;
        }
    }

    TextureInfo::WrapMode ToGeometryWrapMode(MaterialSamplerWrap mode)
    {
        switch (mode)
        {
            case MaterialSamplerWrap::MirrorRepeat:
                return TextureInfo::WrapMode::MirrorRepeat;
            case MaterialSamplerWrap::ClampToEdge:
                return TextureInfo::WrapMode::ClampToEdge;
            case MaterialSamplerWrap::ClampToBorder:
                return TextureInfo::WrapMode::ClampToBorder;
            case MaterialSamplerWrap::Repeat:
            default:
                return TextureInfo::WrapMode::Repeat;
        }
    }

    TextureInfo::FilterMode ToGeometryFilterMode(MaterialSamplerFilter mode)
    {
        switch (mode)
        {
            case MaterialSamplerFilter::Nearest:
                return TextureInfo::FilterMode::Nearest;
            case MaterialSamplerFilter::NearestMipmapNearest:
                return TextureInfo::FilterMode::NearestMipmapNearest;
            case MaterialSamplerFilter::LinearMipmapNearest:
                return TextureInfo::FilterMode::LinearMipmapNearest;
            case MaterialSamplerFilter::NearestMipmapLinear:
                return TextureInfo::FilterMode::NearestMipmapLinear;
            case MaterialSamplerFilter::LinearMipmapLinear:
                return TextureInfo::FilterMode::LinearMipmapLinear;
            case MaterialSamplerFilter::Linear:
            default:
                return TextureInfo::FilterMode::Linear;
        }
    }

    MaterialTextureSlotDefinition MakeSlotDefinition(
        MaterialTextureSlot slot,
        const TextureInfo& source)
    {
        MaterialTextureSlotDefinition definition;
        definition.slot = slot;
        definition.uvSet = source.uvSet >= 0
            ? static_cast<uint8>(source.uvSet)
            : std::numeric_limits<uint8>::max();
        definition.offset = source.offset;
        definition.scale = source.scale;
        definition.rotation = source.rotation;
        definition.wrapS = ToMaterialSamplerWrap(source.wrapS);
        definition.wrapT = ToMaterialSamplerWrap(source.wrapT);
        definition.minFilter = ToMaterialSamplerFilter(source.minFilter);
        definition.magFilter = ToMaterialSamplerFilter(source.magFilter);
        return definition;
    }

    constexpr uint64 GetMaterialFeatureBit(MaterialTextureSlot slot) noexcept
    {
        return 1ull << static_cast<uint32>(slot);
    }

    constexpr uint64 GetTextureSlotFeatureMask() noexcept
    {
        return (1ull <<
                (static_cast<uint32>(MaterialTextureSlot::Thickness) + 1u)) -
            1ull;
    }

    constexpr uint32 GetLegacyTextureFlag(MaterialTextureSlot slot) noexcept
    {
        const uint32 index = static_cast<uint32>(slot);
        return index < 5u ? (1u << index) : 0u;
    }

    void SynchronizeTextureSlotMasks(MaterialDefinition& definition) noexcept
    {
        uint64 featureMask = 0;
        uint32 textureFlags = 0;
        for (const MaterialTextureSlotDefinition& slot : definition.textureSlots)
        {
            featureMask |= GetMaterialFeatureBit(slot.slot);
            textureFlags |= GetLegacyTextureFlag(slot.slot);
        }
        definition.descriptor.featureMask &= ~GetTextureSlotFeatureMask();
        definition.descriptor.featureMask |= featureMask;
        definition.parameters.textureFlags = textureFlags;
    }

    void AddSlotDefinition(
        MaterialDefinition& definition,
        MaterialTextureSlot slot,
        const std::optional<TextureInfo>& source)
    {
        if (source)
        {
            definition.textureSlots.push_back(MakeSlotDefinition(slot, *source));
            SynchronizeTextureSlotMasks(definition);
        }
    }

    MaterialDefinition BuildMaterialDefinition(const Material& material)
    {
        MaterialDefinition definition;
        MaterialStaticDescriptor& descriptor = definition.descriptor;
        MaterialSourceData& parameters = definition.parameters;

        descriptor.shadingModel = material.GetWorkflow() == MaterialWorkflow::Unlit
            ? MaterialShadingModel::Unlit
            : MaterialShadingModel::StandardLit;
        descriptor.programFamily = std::string(GetMaterialProgramFamily(
            descriptor.shadingModel));
        descriptor.cullMode = material.IsDoubleSided()
            ? MaterialCullMode::NoCull
            : MaterialCullMode::Back;

        switch (material.GetAlphaMode())
        {
            case Material::AlphaMode::Mask:
                descriptor.blendMode = MaterialBlendMode::Masked;
                descriptor.translucencyPhase = MaterialTranslucencyPhase::Opaque;
                parameters.alphaMode = MaterialSourceAlphaMode::Mask;
                break;
            case Material::AlphaMode::Blend:
                descriptor.blendMode = MaterialBlendMode::AlphaBlend;
                descriptor.translucencyPhase = MaterialTranslucencyPhase::Standard;
                parameters.alphaMode = MaterialSourceAlphaMode::Blend;
                break;
            case Material::AlphaMode::Opaque:
            default:
                descriptor.blendMode = MaterialBlendMode::Opaque;
                descriptor.translucencyPhase = MaterialTranslucencyPhase::Opaque;
                parameters.alphaMode = MaterialSourceAlphaMode::Opaque;
                break;
        }

        parameters.baseColorFactor = material.GetBaseColor();
        parameters.metallicFactor = material.GetMetallicFactor();
        parameters.roughnessFactor = material.GetRoughnessFactor();
        parameters.normalScale = material.GetNormalScale();
        parameters.occlusionStrength = material.GetOcclusionStrength();
        parameters.emissiveColor = material.GetEmissiveColor();
        parameters.emissiveStrength = material.GetEmissiveStrength();
        parameters.alphaCutoff = material.GetAlphaCutoff();
        parameters.workflow = material.GetWorkflow() == MaterialWorkflow::SpecularGlossiness
            ? MaterialSourceWorkflow::SpecularGlossiness
            : material.GetWorkflow() == MaterialWorkflow::Unlit
                ? MaterialSourceWorkflow::Unlit
                : MaterialSourceWorkflow::MetallicRoughness;
        parameters.doubleSided = material.IsDoubleSided();

        AddSlotDefinition(
            definition, MaterialTextureSlot::BaseColor,
            material.GetBaseColorTexture());
        AddSlotDefinition(
            definition, MaterialTextureSlot::Normal,
            material.GetNormalTexture());
        AddSlotDefinition(
            definition, MaterialTextureSlot::MetallicRoughness,
            material.GetMetallicRoughnessTexture());
        AddSlotDefinition(
            definition, MaterialTextureSlot::Occlusion,
            material.GetOcclusionTexture());
        AddSlotDefinition(
            definition, MaterialTextureSlot::Emissive,
            material.GetEmissiveTexture());
        return definition;
    }

    bool IsDefinitionRoutingConsistent(
        const MaterialDefinition& definition,
        std::string& outError)
    {
        const MaterialSourceAlphaMode expectedAlpha =
            definition.descriptor.blendMode == MaterialBlendMode::Masked
                ? MaterialSourceAlphaMode::Mask
                : definition.descriptor.blendMode == MaterialBlendMode::Opaque
                    ? MaterialSourceAlphaMode::Opaque
                    : MaterialSourceAlphaMode::Blend;
        if (definition.parameters.alphaMode != expectedAlpha ||
            definition.parameters.doubleSided !=
                (definition.descriptor.cullMode == MaterialCullMode::NoCull))
        {
            outError =
                "Material static descriptor and compatibility parameters disagree";
            return false;
        }
        return true;
    }

    TextureInfo MakeCompatibilityTextureInfo(
        const MaterialTextureSlotDefinition& definition)
    {
        TextureInfo result;
        result.uvSet = definition.uvSet;
        result.offset = definition.offset;
        result.scale = definition.scale;
        result.rotation = definition.rotation;
        result.wrapS = ToGeometryWrapMode(definition.wrapS);
        result.wrapT = ToGeometryWrapMode(definition.wrapT);
        result.minFilter = ToGeometryFilterMode(definition.minFilter);
        result.magFilter = ToGeometryFilterMode(definition.magFilter);
        return result;
    }

    std::shared_ptr<Material> BuildCompatibilityMaterial(
        const MaterialDefinition& definition,
        const std::string& name)
    {
        auto material = std::make_shared<Material>(
            name.empty() ? "Material" : name);
        const MaterialSourceData& parameters = definition.parameters;
        material->SetWorkflow(
            parameters.workflow == MaterialSourceWorkflow::SpecularGlossiness
                ? MaterialWorkflow::SpecularGlossiness
                : parameters.workflow == MaterialSourceWorkflow::Unlit
                    ? MaterialWorkflow::Unlit
                    : MaterialWorkflow::MetallicRoughness);
        material->SetDoubleSided(parameters.doubleSided);
        material->SetAlphaMode(
            parameters.alphaMode == MaterialSourceAlphaMode::Mask
                ? Material::AlphaMode::Mask
                : parameters.alphaMode == MaterialSourceAlphaMode::Blend
                    ? Material::AlphaMode::Blend
                    : Material::AlphaMode::Opaque);
        material->SetAlphaCutoff(parameters.alphaCutoff);
        material->SetBaseColor(parameters.baseColorFactor);
        material->SetMetallicFactor(parameters.metallicFactor);
        material->SetRoughnessFactor(parameters.roughnessFactor);
        material->SetNormalScale(parameters.normalScale);
        material->SetOcclusionStrength(parameters.occlusionStrength);
        material->SetEmissiveColor(parameters.emissiveColor);
        material->SetEmissiveStrength(parameters.emissiveStrength);
        for (const MaterialTextureSlotDefinition& slot : definition.textureSlots)
        {
            const TextureInfo info = MakeCompatibilityTextureInfo(slot);
            switch (slot.slot)
            {
                case MaterialTextureSlot::BaseColor:
                    material->SetBaseColorTexture(info);
                    break;
                case MaterialTextureSlot::Normal:
                    material->SetNormalTexture(info);
                    break;
                case MaterialTextureSlot::MetallicRoughness:
                    material->SetMetallicRoughnessTexture(info);
                    break;
                case MaterialTextureSlot::Occlusion:
                    material->SetOcclusionTexture(info);
                    break;
                case MaterialTextureSlot::Emissive:
                    material->SetEmissiveTexture(info);
                    break;
                default:
                    break;
            }
        }
        return material;
    }

    std::optional<MaterialTextureSlot> FindLegacyTextureSlot(
        const std::string& slot)
    {
        if (slot == "albedo") return MaterialTextureSlot::BaseColor;
        if (slot == "normal") return MaterialTextureSlot::Normal;
        if (slot == "metallic_roughness")
            return MaterialTextureSlot::MetallicRoughness;
        if (slot == "ao") return MaterialTextureSlot::Occlusion;
        if (slot == "emissive") return MaterialTextureSlot::Emissive;
        return std::nullopt;
    }

    bool EnsureDefinitionTextureSlot(
        MaterialDefinition& definition,
        MaterialTextureSlot slot)
    {
        const auto found = std::find_if(
            definition.textureSlots.begin(), definition.textureSlots.end(),
            [slot](const MaterialTextureSlotDefinition& candidate)
            {
                return candidate.slot == slot;
            });
        if (found != definition.textureSlots.end())
        {
            SynchronizeTextureSlotMasks(definition);
            return true;
        }
        MaterialTextureSlotDefinition definitionSlot;
        definitionSlot.slot = slot;
        definition.textureSlots.push_back(definitionSlot);
        std::sort(
            definition.textureSlots.begin(), definition.textureSlots.end(),
            [](const MaterialTextureSlotDefinition& left,
               const MaterialTextureSlotDefinition& right)
            {
                return left.slot < right.slot;
            });
        SynchronizeTextureSlotMasks(definition);
        return true;
    }

    void PreserveBoundTextureSlots(
        MaterialDefinition& definition,
        const MaterialDefinition& previousDefinition,
        const std::unordered_map<
            std::string, ResourceHandle<TextureResource>>& textures)
    {
        for (const auto& [legacyName, texture] : textures)
        {
            if (!texture)
                continue;

            const std::optional<MaterialTextureSlot> canonicalSlot =
                FindLegacyTextureSlot(legacyName);
            if (!canonicalSlot)
                continue;

            const auto alreadyDeclared = std::find_if(
                definition.textureSlots.begin(), definition.textureSlots.end(),
                [canonicalSlot](const MaterialTextureSlotDefinition& candidate)
                {
                    return candidate.slot == *canonicalSlot;
                });
            if (alreadyDeclared != definition.textureSlots.end())
                continue;

            const auto previous = std::find_if(
                previousDefinition.textureSlots.begin(),
                previousDefinition.textureSlots.end(),
                [canonicalSlot](const MaterialTextureSlotDefinition& candidate)
                {
                    return candidate.slot == *canonicalSlot;
                });
            if (previous != previousDefinition.textureSlots.end())
                definition.textureSlots.push_back(*previous);
            else
            {
                MaterialTextureSlotDefinition slot;
                slot.slot = *canonicalSlot;
                definition.textureSlots.push_back(slot);
            }
        }

        std::sort(
            definition.textureSlots.begin(), definition.textureSlots.end(),
            [](const MaterialTextureSlotDefinition& left,
               const MaterialTextureSlotDefinition& right)
            {
                return left.slot < right.slot;
            });
        SynchronizeTextureSlotMasks(definition);
    }

} // namespace

MaterialResource::MaterialResource() = default;
MaterialResource::~MaterialResource() = default;

bool MaterialResource::SetMaterialData(std::shared_ptr<Material> material)
{
    if (!CanModifyPreparedState())
    {
        RVX_CORE_ERROR(
            "Rejected direct material-data mutation for published material resource {}. "
            "Use a ResourceManager-managed MaterialInstanceResource instead.",
            GetId());
        return false;
    }
    if (!material)
    {
        RVX_CORE_ERROR("Rejected null prepared material data for resource {}.",
                       GetId());
        return false;
    }

    MaterialDefinition definition = BuildMaterialDefinition(*material);
    PreserveBoundTextureSlots(definition, m_definition, m_textures);
    std::string error;
    if (!ValidateMaterialDefinition(definition, error) ||
        !IsDefinitionRoutingConsistent(definition, error))
    {
        RVX_CORE_ERROR("Rejected invalid Geometry material compatibility input for resource {}: {}",
                       GetId(), error);
        return false;
    }
    const std::string compatibilityName = material->GetName();
    m_definition = std::move(definition);
    m_material = BuildCompatibilityMaterial(m_definition, compatibilityName);
    return true;
}

bool MaterialResource::SetMaterialDefinition(const MaterialDefinition& definition)
{
    if (!CanModifyPreparedState())
    {
        RVX_CORE_ERROR(
            "Rejected direct material-definition mutation for published material resource {}. "
            "Use a ResourceManager-managed MaterialInstanceResource instead.",
            GetId());
        return false;
    }

    std::string error;
    if (!ValidateMaterialDefinition(definition, error) ||
        !IsDefinitionRoutingConsistent(definition, error))
    {
        RVX_CORE_ERROR("Rejected invalid prepared material definition for resource {}: {}",
                       GetId(), error);
        return false;
    }

    m_definition = definition;
    m_material = BuildCompatibilityMaterial(m_definition, GetName());
    return true;
}

const std::string& MaterialResource::GetMaterialName() const
{
    return m_material ? m_material->GetName() : GetName();
}

MaterialWorkflow MaterialResource::GetWorkflow() const
{
    switch (m_definition.parameters.workflow)
    {
        case MaterialSourceWorkflow::SpecularGlossiness:
            return MaterialWorkflow::SpecularGlossiness;
        case MaterialSourceWorkflow::Unlit:
            return MaterialWorkflow::Unlit;
        case MaterialSourceWorkflow::MetallicRoughness:
        default:
            return MaterialWorkflow::MetallicRoughness;
    }
}

MaterialWorkflowMode MaterialResource::GetWorkflowMode() const
{
    switch (m_definition.parameters.workflow)
    {
        case MaterialSourceWorkflow::SpecularGlossiness:
            return MaterialWorkflowMode::SpecularGlossiness;
        case MaterialSourceWorkflow::Unlit:
            return MaterialWorkflowMode::Unlit;
        case MaterialSourceWorkflow::MetallicRoughness:
        default:
            return MaterialWorkflowMode::MetallicRoughness;
    }
}

MaterialAlphaMode MaterialResource::GetAlphaMode() const
{
    switch (m_definition.parameters.alphaMode)
    {
        case MaterialSourceAlphaMode::Mask:
            return MaterialAlphaMode::Mask;
        case MaterialSourceAlphaMode::Blend:
            return MaterialAlphaMode::Blend;
        case MaterialSourceAlphaMode::Opaque:
        default:
            return MaterialAlphaMode::Opaque;
    }
}

Vec4 MaterialResource::GetBaseColor() const
{
    return m_definition.parameters.baseColorFactor;
}

float MaterialResource::GetMetallicFactor() const
{
    return m_definition.parameters.metallicFactor;
}

float MaterialResource::GetRoughnessFactor() const
{
    return m_definition.parameters.roughnessFactor;
}

float MaterialResource::GetNormalScale() const
{
    return m_definition.parameters.normalScale;
}

float MaterialResource::GetOcclusionStrength() const
{
    return m_definition.parameters.occlusionStrength;
}

Vec3 MaterialResource::GetEmissiveColor() const
{
    return m_definition.parameters.emissiveColor;
}

float MaterialResource::GetEmissiveStrength() const
{
    return m_definition.parameters.emissiveStrength;
}

float MaterialResource::GetAlphaCutoff() const
{
    return m_definition.parameters.alphaCutoff;
}

bool MaterialResource::IsDoubleSided() const
{
    return m_definition.parameters.doubleSided;
}

MaterialSourceData MaterialResource::GetMaterialSourceData() const
{
    return m_definition.parameters;
}

bool MaterialResource::SetTexture(const std::string& slot,
                                  ResourceHandle<TextureResource> texture)
{
    if (!CanModifyPreparedState())
    {
        RVX_CORE_ERROR(
            "Rejected direct texture mutation for published material resource {}. "
            "Use ResourceManager::UpdateMaterialInstance() instead.",
            GetId());
        return false;
    }
    const std::optional<MaterialTextureSlot> canonicalSlot =
        FindLegacyTextureSlot(slot);
    if (!canonicalSlot || !EnsureDefinitionTextureSlot(
                             m_definition, *canonicalSlot))
    {
        RVX_CORE_ERROR("Rejected unknown material texture slot '{}' for resource {}.",
                       slot, GetId());
        return false;
    }
    m_textures[slot] = std::move(texture);
    return true;
}

ResourceHandle<TextureResource> MaterialResource::GetTexture(const std::string& slot) const
{
    auto it = m_textures.find(slot);
    return it != m_textures.end() ? it->second : ResourceHandle<TextureResource>();
}

const std::unordered_map<std::string, ResourceHandle<TextureResource>>& MaterialResource::GetTextures() const
{
    return m_textures;
}

bool MaterialResource::SetShader(ResourceHandle<ShaderResource> shader)
{
    if (!CanModifyPreparedState())
    {
        RVX_CORE_ERROR(
            "Rejected direct shader mutation for published material resource {}.",
            GetId());
        return false;
    }
    m_shader = std::move(shader);
    return true;
}

ResourceHandle<ShaderResource> MaterialResource::GetShader() const
{
    return m_shader;
}

bool MaterialResource::HasShader() const
{
    return m_shader.IsValid();
}

bool MaterialResource::HasValidShaderRuntimeContract() const
{
    return m_shader.IsValid() && m_shader->HasValidRuntimeContract();
}

uint64 MaterialResource::GetShaderRuntimeContractHash() const
{
    return m_shader.IsValid() ? m_shader->GetRuntimeContractHash() : 0;
}

MaterialShaderContractSnapshot MaterialResource::GetShaderContractSnapshot() const
{
    MaterialShaderContractSnapshot snapshot;
    snapshot.shaderAssigned = m_shader.IsValid();

    if (!m_shader.IsValid())
    {
        snapshot.diagnosticMessage = "Material has no shader assigned.";
        return snapshot;
    }

    snapshot.shaderLoaded = m_shader.IsLoaded();
    snapshot.shaderResourceId = m_shader.GetId();

    const ShaderRuntimeContract& contract = m_shader->GetRuntimeContract();
    snapshot.shaderContractValid = contract.valid;
    snapshot.shaderContractHash = contract.contractHash;
    snapshot.shaderPayloadHash = contract.payloadHash;
    snapshot.shaderContractKey = contract.cacheKey;
    snapshot.diagnosticMessage = contract.diagnosticMessage;
    return snapshot;
}

std::string MaterialResource::ExportShaderContractSnapshotJson() const
{
    const MaterialShaderContractSnapshot snapshot = GetShaderContractSnapshot();

    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"schemaVersion\": " << RVX_MATERIAL_SHADER_CONTRACT_SNAPSHOT_SCHEMA_VERSION << ",\n";
    ss << "  \"schemaId\": " << JsonString(RVX_MATERIAL_SHADER_CONTRACT_SNAPSHOT_SCHEMA_ID) << ",\n";
    ss << "  \"id\": \"materialShaderContractSnapshotJson\",\n";
    ss << "  \"kind\": \"MaterialShaderContractSnapshotJson\",\n";
    ss << "  \"contentType\": \"application/json\",\n";
    ss << "  \"contentHash\": \"\",\n";
    ss << "  \"relativePath\": \"\",\n";
    ss << "  \"material\": {\n";
    ss << "    \"resourceId\": " << GetId() << ",\n";
    ss << "    \"name\": " << JsonString(GetName()) << ",\n";
    ss << "    \"path\": " << JsonString(GetPath()) << ",\n";
    ss << "    \"workflow\": " << static_cast<uint32>(GetWorkflowMode()) << ",\n";
    ss << "    \"alphaMode\": " << static_cast<uint32>(GetAlphaMode()) << "\n";
    ss << "  },\n";
    ss << "  \"shader\": {\n";
    ss << "    \"assigned\": " << JsonBool(snapshot.shaderAssigned) << ",\n";
    ss << "    \"loaded\": " << JsonBool(snapshot.shaderLoaded) << ",\n";
    ss << "    \"contractValid\": " << JsonBool(snapshot.shaderContractValid) << ",\n";
    ss << "    \"resourceId\": " << snapshot.shaderResourceId << ",\n";
    ss << "    \"contractHash\": " << snapshot.shaderContractHash << ",\n";
    ss << "    \"payloadHash\": " << snapshot.shaderPayloadHash << ",\n";
    ss << "    \"contractKey\": " << JsonString(snapshot.shaderContractKey) << ",\n";
    ss << "    \"diagnosticMessage\": " << JsonString(snapshot.diagnosticMessage) << "\n";
    ss << "  }\n";
    ss << "}\n";
    return ss.str();
}

bool MaterialResource::SaveShaderContractSnapshotJson(const char* filename) const
{
    if (!filename || filename[0] == '\0')
    {
        return false;
    }

    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    file << ExportShaderContractSnapshotJson();
    return file.good();
}

size_t MaterialResource::GetMemoryUsage() const
{
    size_t size = sizeof(*this) + m_definition.descriptor.programFamily.capacity();
    size += m_definition.textureSlots.size() *
        sizeof(MaterialTextureSlotDefinition);

    if (m_material)
    {
        size += sizeof(Material);
    }

    return size;
}

std::vector<ResourceId> MaterialResource::GetRequiredDependencies() const
{
    std::vector<ResourceId> deps;

    for (const auto& [slot, texture] : m_textures)
    {
        (void)slot;
        if (texture.IsValid())
        {
            deps.push_back(texture.GetId());
        }
    }

    if (m_shader.IsValid())
    {
        deps.push_back(m_shader.GetId());
    }

    std::sort(deps.begin(), deps.end());
    deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
    return deps;
}

void MaterialResource::CommitManagedMaterialState(
    std::shared_ptr<Material> material,
    std::unordered_map<std::string, ResourceHandle<TextureResource>> textures,
    ResourceHandle<ShaderResource> shader)
{
    if (material)
    {
        MaterialDefinition definition = BuildMaterialDefinition(*material);
        PreserveBoundTextureSlots(definition, m_definition, textures);
        const std::string compatibilityName = material->GetName();
        m_definition = std::move(definition);
        m_material = BuildCompatibilityMaterial(m_definition, compatibilityName);
    }
    else
    {
        m_definition = {};
        m_material.reset();
    }
    m_textures = std::move(textures);
    m_shader = std::move(shader);
}

bool MaterialResource::CanModifyPreparedState() const noexcept
{
    return !IsLoaded() && GetState() != ResourceState::Unloading;
}

} // namespace RVX::Resource
