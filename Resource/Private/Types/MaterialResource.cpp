#include "Resource/Types/MaterialResource.h"

#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

namespace RVX::Resource
{
namespace
{
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

    RenderTextureWrapMode ToRenderTextureWrapMode(TextureInfo::WrapMode mode)
    {
        switch (mode)
        {
            case TextureInfo::WrapMode::MirrorRepeat:
                return RenderTextureWrapMode::MirrorRepeat;
            case TextureInfo::WrapMode::ClampToEdge:
                return RenderTextureWrapMode::ClampToEdge;
            case TextureInfo::WrapMode::ClampToBorder:
                return RenderTextureWrapMode::ClampToBorder;
            case TextureInfo::WrapMode::Repeat:
            default:
                return RenderTextureWrapMode::Repeat;
        }
    }

    RenderTextureFilterMode ToRenderTextureFilterMode(TextureInfo::FilterMode mode)
    {
        switch (mode)
        {
            case TextureInfo::FilterMode::Nearest:
                return RenderTextureFilterMode::Nearest;
            case TextureInfo::FilterMode::NearestMipmapNearest:
                return RenderTextureFilterMode::NearestMipmapNearest;
            case TextureInfo::FilterMode::LinearMipmapNearest:
                return RenderTextureFilterMode::LinearMipmapNearest;
            case TextureInfo::FilterMode::NearestMipmapLinear:
                return RenderTextureFilterMode::NearestMipmapLinear;
            case TextureInfo::FilterMode::LinearMipmapLinear:
                return RenderTextureFilterMode::LinearMipmapLinear;
            case TextureInfo::FilterMode::Linear:
            default:
                return RenderTextureFilterMode::Linear;
        }
    }

    void ApplyTextureInfo(RenderMaterialTextureBinding& binding,
                          const std::optional<TextureInfo>& textureInfo)
    {
        if (!textureInfo)
        {
            return;
        }

        binding.hasTextureInfo = true;
        binding.uvSet = textureInfo->uvSet;
        binding.offset = textureInfo->offset;
        binding.scale = textureInfo->scale;
        binding.rotation = textureInfo->rotation;
        binding.wrapS = ToRenderTextureWrapMode(textureInfo->wrapS);
        binding.wrapT = ToRenderTextureWrapMode(textureInfo->wrapT);
        binding.minFilter = ToRenderTextureFilterMode(textureInfo->minFilter);
        binding.magFilter = ToRenderTextureFilterMode(textureInfo->magFilter);
    }

    const char* JsonBool(bool value)
    {
        return value ? "true" : "false";
    }

    std::string JsonString(std::string_view value)
    {
        std::string escaped;
        escaped.reserve(value.size() + 2);
        escaped.push_back('"');
        for (char ch : value)
        {
            switch (ch)
            {
                case '\\':
                    escaped += "\\\\";
                    break;
                case '"':
                    escaped += "\\\"";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\r':
                    escaped += "\\r";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                default:
                    escaped.push_back(ch);
                    break;
            }
        }
        escaped.push_back('"');
        return escaped;
    }
} // namespace

MaterialResource::MaterialResource() = default;
MaterialResource::~MaterialResource() = default;

void MaterialResource::SetMaterialData(std::shared_ptr<Material> material)
{
    m_material = std::move(material);
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

MaterialSourceData MaterialResource::GetRenderMaterialSourceData() const
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

IRenderTextureUploadSource* MaterialResource::GetRenderMaterialTexture(RenderMaterialTextureSlot slot) const
{
    switch (slot)
    {
        case RenderMaterialTextureSlot::BaseColor:
            return GetAlbedoTexture().Get();
        case RenderMaterialTextureSlot::Normal:
            return GetNormalTexture().Get();
        case RenderMaterialTextureSlot::MetallicRoughness:
            return GetMetallicRoughnessTexture().Get();
        case RenderMaterialTextureSlot::Occlusion:
            return GetAOTexture().Get();
        case RenderMaterialTextureSlot::Emissive:
            return GetEmissiveTexture().Get();
        default:
            return nullptr;
    }
}

RenderMaterialTextureBinding MaterialResource::GetRenderMaterialTextureBinding(RenderMaterialTextureSlot slot) const
{
    RenderMaterialTextureBinding binding;
    binding.texture = GetRenderMaterialTexture(slot);
    binding.textureId = binding.texture ? binding.texture->GetRenderResourceId() : 0;

    if (!m_material)
    {
        return binding;
    }

    switch (slot)
    {
        case RenderMaterialTextureSlot::BaseColor:
            ApplyTextureInfo(binding, m_material->GetBaseColorTexture());
            break;
        case RenderMaterialTextureSlot::Normal:
            ApplyTextureInfo(binding, m_material->GetNormalTexture());
            break;
        case RenderMaterialTextureSlot::MetallicRoughness:
            ApplyTextureInfo(binding, m_material->GetMetallicRoughnessTexture());
            break;
        case RenderMaterialTextureSlot::Occlusion:
            ApplyTextureInfo(binding, m_material->GetOcclusionTexture());
            break;
        case RenderMaterialTextureSlot::Emissive:
            ApplyTextureInfo(binding, m_material->GetEmissiveTexture());
            break;
        default:
            break;
    }

    return binding;
}

void MaterialResource::SetTexture(const std::string& slot, ResourceHandle<TextureResource> texture)
{
    m_textures[slot] = std::move(texture);
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

void MaterialResource::SetShader(ResourceHandle<ShaderResource> shader)
{
    m_shader = std::move(shader);
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

} // namespace RVX::Resource
