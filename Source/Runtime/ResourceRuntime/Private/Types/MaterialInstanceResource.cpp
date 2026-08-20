#include "Resource/Types/MaterialInstanceResource.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace RVX::Resource
{
namespace
{
    constexpr float RVX_MAX_EMISSIVE_VALUE = 65504.0f;
    constexpr float RVX_MAX_NORMAL_SCALE = 8.0f;

    bool IsFiniteInRange(float value, float minimum, float maximum)
    {
        return std::isfinite(value) && value >= minimum && value <= maximum;
    }

    bool IsUnitColor(const Vec4& value)
    {
        return IsFiniteInRange(value.x, 0.0f, 1.0f) &&
               IsFiniteInRange(value.y, 0.0f, 1.0f) &&
               IsFiniteInRange(value.z, 0.0f, 1.0f) &&
               IsFiniteInRange(value.w, 0.0f, 1.0f);
    }

    bool IsEmissiveColor(const Vec3& value)
    {
        return IsFiniteInRange(value.x, 0.0f, RVX_MAX_EMISSIVE_VALUE) &&
               IsFiniteInRange(value.y, 0.0f, RVX_MAX_EMISSIVE_VALUE) &&
               IsFiniteInRange(value.z, 0.0f, RVX_MAX_EMISSIVE_VALUE);
    }

    bool Equal(const Vec4& left, const Vec4& right)
    {
        return left.x == right.x && left.y == right.y &&
               left.z == right.z && left.w == right.w;
    }

    bool Equal(const Vec3& left, const Vec3& right)
    {
        return left.x == right.x && left.y == right.y && left.z == right.z;
    }

    bool EqualTextureMaps(
        const std::unordered_map<std::string, ResourceHandle<TextureResource>>& left,
        const std::unordered_map<std::string, ResourceHandle<TextureResource>>& right)
    {
        if (left.size() != right.size())
        {
            return false;
        }
        for (const auto& [slot, texture] : left)
        {
            const auto found = right.find(slot);
            if (found == right.end() || found->second.Get() != texture.Get())
            {
                return false;
            }
        }
        return true;
    }

    bool IsSupportedTextureSlot(const std::string& slot)
    {
        return slot == "albedo" || slot == "normal" ||
               slot == "metallic_roughness" || slot == "ao" ||
               slot == "emissive";
    }

    std::vector<ResourceId> BuildDependencies(
        const ResourceHandle<MaterialResource>& parent,
        const std::unordered_map<std::string, ResourceHandle<TextureResource>>& textures,
        const ResourceHandle<ShaderResource>& shader)
    {
        std::vector<ResourceId> dependencies;
        dependencies.reserve(textures.size() + 2);
        if (parent)
        {
            dependencies.push_back(parent.GetId());
        }
        for (const auto& [slot, texture] : textures)
        {
            (void)slot;
            if (texture)
            {
                dependencies.push_back(texture.GetId());
            }
        }
        if (shader)
        {
            dependencies.push_back(shader.GetId());
        }
        std::sort(dependencies.begin(), dependencies.end());
        dependencies.erase(std::remove(dependencies.begin(), dependencies.end(),
                                       InvalidResourceId),
                           dependencies.end());
        dependencies.erase(std::unique(dependencies.begin(), dependencies.end()),
                           dependencies.end());
        return dependencies;
    }
} // namespace

MaterialInstanceResource::MaterialInstanceResource(
    ResourceHandle<MaterialResource> parent,
    std::string runtimeKey)
    : m_parent(std::move(parent))
    , m_runtimeKey(std::move(runtimeKey))
{
}

MaterialInstanceResource::~MaterialInstanceResource() = default;

size_t MaterialInstanceResource::GetMemoryUsage() const
{
    return MaterialResource::GetMemoryUsage() + sizeof(m_parent) +
           m_runtimeKey.capacity();
}

std::vector<ResourceId> MaterialInstanceResource::GetRequiredDependencies() const
{
    return BuildDependencies(m_parent, GetTextures(), GetShader());
}

bool MaterialInstanceResource::InitializeFromParent()
{
    if (!m_parent || !m_parent.IsLoaded())
    {
        return false;
    }

    const std::shared_ptr<const Material> parentMaterial = m_parent->GetMaterial();
    if (!parentMaterial)
    {
        return false;
    }

    CommitManagedMaterialState(parentMaterial->Clone(),
                               m_parent->GetTextures(),
                               m_parent->GetShader());
    m_revision = 1;
    return true;
}

MaterialInstanceMutationCode MaterialInstanceResource::BuildPatchedState(
    const MaterialInstancePatch& patch,
    PreparedState& outState,
    std::string& outMessage) const
{
    if (patch.workflowMode || patch.alphaMode || patch.doubleSided || patch.shader)
    {
        outMessage = "Material workflow, alpha mode, double-sided state and shader are immutable for a runtime material instance.";
        return MaterialInstanceMutationCode::StaticPropertyImmutable;
    }

    const std::shared_ptr<const Material> current = GetMaterial();
    if (!m_parent || !current)
    {
        outMessage = "The material instance has no valid immutable parent state.";
        return MaterialInstanceMutationCode::InvalidInstance;
    }

    if ((patch.baseColor && !IsUnitColor(*patch.baseColor)) ||
        (patch.metallicFactor && !IsFiniteInRange(*patch.metallicFactor, 0.0f, 1.0f)) ||
        (patch.roughnessFactor && !IsFiniteInRange(*patch.roughnessFactor, 0.0f, 1.0f)) ||
        (patch.normalScale && !IsFiniteInRange(*patch.normalScale, 0.0f, RVX_MAX_NORMAL_SCALE)) ||
        (patch.occlusionStrength && !IsFiniteInRange(*patch.occlusionStrength, 0.0f, 1.0f)) ||
        (patch.emissiveColor && !IsEmissiveColor(*patch.emissiveColor)) ||
        (patch.emissiveStrength && !IsFiniteInRange(*patch.emissiveStrength, 0.0f, RVX_MAX_EMISSIVE_VALUE)) ||
        (patch.alphaCutoff && !IsFiniteInRange(*patch.alphaCutoff, 0.0f, 1.0f)))
    {
        outMessage = "A runtime material parameter is NaN, infinite or outside its declared PBR range.";
        return MaterialInstanceMutationCode::InvalidPatch;
    }

    outState.material = current->Clone();
    outState.textures = GetTextures();
    outState.shader = GetShader();
    if (!outState.material)
    {
        outMessage = "Unable to clone the current material instance state.";
        return MaterialInstanceMutationCode::TransactionFailed;
    }

    if (patch.baseColor)
    {
        outState.material->SetBaseColor(*patch.baseColor);
    }
    if (patch.metallicFactor)
    {
        outState.material->SetMetallicFactor(*patch.metallicFactor);
    }
    if (patch.roughnessFactor)
    {
        outState.material->SetRoughnessFactor(*patch.roughnessFactor);
    }
    if (patch.normalScale)
    {
        outState.material->SetNormalScale(*patch.normalScale);
    }
    if (patch.occlusionStrength)
    {
        outState.material->SetOcclusionStrength(*patch.occlusionStrength);
    }
    if (patch.emissiveColor)
    {
        outState.material->SetEmissiveColor(*patch.emissiveColor);
    }
    if (patch.emissiveStrength)
    {
        outState.material->SetEmissiveStrength(*patch.emissiveStrength);
    }
    if (patch.alphaCutoff)
    {
        outState.material->SetAlphaCutoff(*patch.alphaCutoff);
    }

    for (const auto& [slot, overrideTexture] : patch.textureOverrides)
    {
        if (!IsSupportedTextureSlot(slot))
        {
            outMessage = "The patch references a texture slot outside the v1 PBR contract: " + slot;
            return MaterialInstanceMutationCode::InvalidPatch;
        }

        if (overrideTexture)
        {
            if (!overrideTexture->IsValid())
            {
                outMessage = "A texture override has no resource handle: " + slot;
                return MaterialInstanceMutationCode::InvalidPatch;
            }
            outState.textures[slot] = *overrideTexture;
        }
        else
        {
            const ResourceHandle<TextureResource> parentTexture = m_parent->GetTexture(slot);
            if (parentTexture)
            {
                outState.textures[slot] = parentTexture;
            }
            else
            {
                outState.textures.erase(slot);
            }
        }
    }

    outState.dependencies = BuildDependencies(m_parent, outState.textures, outState.shader);
    return MaterialInstanceMutationCode::Applied;
}

bool MaterialInstanceResource::HasSameState(const PreparedState& state) const
{
    const std::shared_ptr<const Material> current = GetMaterial();
    if (!current || !state.material)
    {
        return false;
    }

    return Equal(current->GetBaseColor(), state.material->GetBaseColor()) &&
           current->GetMetallicFactor() == state.material->GetMetallicFactor() &&
           current->GetRoughnessFactor() == state.material->GetRoughnessFactor() &&
           current->GetNormalScale() == state.material->GetNormalScale() &&
           current->GetOcclusionStrength() == state.material->GetOcclusionStrength() &&
           Equal(current->GetEmissiveColor(), state.material->GetEmissiveColor()) &&
           current->GetEmissiveStrength() == state.material->GetEmissiveStrength() &&
           current->GetAlphaCutoff() == state.material->GetAlphaCutoff() &&
           EqualTextureMaps(GetTextures(), state.textures) &&
           GetShader().Get() == state.shader.Get();
}

void MaterialInstanceResource::CommitPatchedState(PreparedState&& state) noexcept
{
    CommitManagedMaterialState(std::move(state.material),
                               std::move(state.textures),
                               std::move(state.shader));
    ++m_revision;
}
} // namespace RVX::Resource
