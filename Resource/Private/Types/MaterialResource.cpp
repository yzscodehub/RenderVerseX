#include "Resource/Types/MaterialResource.h"

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
    
    return deps;
}

} // namespace RVX::Resource
