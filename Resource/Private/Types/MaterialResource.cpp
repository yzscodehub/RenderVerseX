#include "Resource/Types/MaterialResource.h"
#include "Core/Log.h"
#include "Core/Diagnostics/JsonWriter.h"

#include <fstream>
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

    MaterialSourceAlphaMode ToMaterialSourceAlphaMode(MaterialAlphaMode alphaMode)
    {
        switch (alphaMode)
        {
            case MaterialAlphaMode::Mask:
                return MaterialSourceAlphaMode::Mask;
            case MaterialAlphaMode::Blend:
                return MaterialSourceAlphaMode::Blend;
            case MaterialAlphaMode::Opaque:
            default:
                return MaterialSourceAlphaMode::Opaque;
        }
    }

    MaterialSourceWorkflow ToMaterialSourceWorkflow(MaterialWorkflowMode workflow)
    {
        switch (workflow)
        {
            case MaterialWorkflowMode::SpecularGlossiness:
                return MaterialSourceWorkflow::SpecularGlossiness;
            case MaterialWorkflowMode::Unlit:
                return MaterialSourceWorkflow::Unlit;
            case MaterialWorkflowMode::MetallicRoughness:
            default:
                return MaterialSourceWorkflow::MetallicRoughness;
        }
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
    m_material = std::move(material);
    return true;
}

const std::string& MaterialResource::GetMaterialName() const
{
    static std::string empty;
    return m_material ? m_material->GetName() : empty;
}

MaterialWorkflow MaterialResource::GetWorkflow() const
{
    return m_material ? m_material->GetWorkflow() : MaterialWorkflow::MetallicRoughness;
}

MaterialWorkflowMode MaterialResource::GetWorkflowMode() const
{
    if (!m_material)
        return MaterialWorkflowMode::MetallicRoughness;

    switch (m_material->GetWorkflow())
    {
        case MaterialWorkflow::SpecularGlossiness:
            return MaterialWorkflowMode::SpecularGlossiness;
        case MaterialWorkflow::Unlit:
            return MaterialWorkflowMode::Unlit;
        case MaterialWorkflow::MetallicRoughness:
        default:
            return MaterialWorkflowMode::MetallicRoughness;
    }
}

MaterialAlphaMode MaterialResource::GetAlphaMode() const
{
    if (!m_material)
        return MaterialAlphaMode::Opaque;

    switch (m_material->GetAlphaMode())
    {
        case Material::AlphaMode::Mask:
            return MaterialAlphaMode::Mask;
        case Material::AlphaMode::Blend:
            return MaterialAlphaMode::Blend;
        case Material::AlphaMode::Opaque:
        default:
            return MaterialAlphaMode::Opaque;
    }
}

Vec4 MaterialResource::GetBaseColor() const
{
    return m_material ? m_material->GetBaseColor() : Vec4{1.0f, 1.0f, 1.0f, 1.0f};
}

float MaterialResource::GetMetallicFactor() const
{
    return m_material ? m_material->GetMetallicFactor() : 1.0f;
}

float MaterialResource::GetRoughnessFactor() const
{
    return m_material ? m_material->GetRoughnessFactor() : 1.0f;
}

float MaterialResource::GetNormalScale() const
{
    return m_material ? m_material->GetNormalScale() : 1.0f;
}

float MaterialResource::GetOcclusionStrength() const
{
    return m_material ? m_material->GetOcclusionStrength() : 1.0f;
}

Vec3 MaterialResource::GetEmissiveColor() const
{
    return m_material ? m_material->GetEmissiveColor() : Vec3{0.0f, 0.0f, 0.0f};
}

float MaterialResource::GetEmissiveStrength() const
{
    return m_material ? m_material->GetEmissiveStrength() : 1.0f;
}

float MaterialResource::GetAlphaCutoff() const
{
    return m_material ? m_material->GetAlphaCutoff() : 0.5f;
}

bool MaterialResource::IsDoubleSided() const
{
    return m_material ? m_material->IsDoubleSided() : false;
}

MaterialSourceData MaterialResource::GetMaterialSourceData() const
{
    MaterialSourceData source;
    source.baseColorFactor = GetBaseColor();
    source.metallicFactor = GetMetallicFactor();
    source.roughnessFactor = GetRoughnessFactor();
    source.normalScale = GetNormalScale();
    source.occlusionStrength = GetOcclusionStrength();
    source.emissiveColor = GetEmissiveColor();
    source.emissiveStrength = GetEmissiveStrength();
    source.alphaMode = ToMaterialSourceAlphaMode(GetAlphaMode());
    source.alphaCutoff = GetAlphaCutoff();
    source.workflow = ToMaterialSourceWorkflow(GetWorkflowMode());
    source.doubleSided = IsDoubleSided();
    return source;
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
    size_t size = sizeof(*this);

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
        if (texture.IsValid())
        {
            deps.push_back(texture.GetId());
        }
    }

    if (m_shader.IsValid())
    {
        deps.push_back(m_shader.GetId());
    }

    return deps;
}

void MaterialResource::CommitManagedMaterialState(
    std::shared_ptr<Material> material,
    std::unordered_map<std::string, ResourceHandle<TextureResource>> textures,
    ResourceHandle<ShaderResource> shader)
{
    m_material = std::move(material);
    m_textures = std::move(textures);
    m_shader = std::move(shader);
}

bool MaterialResource::CanModifyPreparedState() const noexcept
{
    return !IsLoaded() && GetState() != ResourceState::Unloading;
}

} // namespace RVX::Resource
