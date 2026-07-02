#pragma once

/**
 * @file MaterialResource.h
 * @brief Material resource type
 */

#include "Resource/IResource.h"
#include "Resource/ResourceHandle.h"
#include "Resource/Types/TextureResource.h"
#include "RenderContracts/RenderResource.h"
#include "Geometry/Asset/Material.h"
#include <memory>
#include <string>
#include <unordered_map>

namespace RVX::Resource
{
    enum class MaterialAlphaMode : uint8
    {
        Opaque = 0,
        Mask,
        Blend
    };

    enum class MaterialWorkflowMode : uint8
    {
        MetallicRoughness = 0,
        SpecularGlossiness,
        Unlit
    };

    /**
     * @brief Material resource - encapsulates Scene::Material with texture references
     */
    class MaterialResource : public IResource, public IRenderMaterialSource
    {
    public:
        MaterialResource();
        ~MaterialResource() override;

        // =====================================================================
        // Resource Interface
        // =====================================================================

        ResourceType GetType() const override { return ResourceType::Material; }
        const char* GetTypeName() const override { return "Material"; }
        size_t GetMemoryUsage() const override;

        std::vector<ResourceId> GetRequiredDependencies() const override;

        uint64 GetRenderResourceId() const override { return GetId(); }
        std::string_view GetRenderResourceName() const override { return GetName(); }
        uint32 GetRenderResourceRefCount() const override { return GetRefCount(); }
        RefCounted* GetRenderResourceRefCounted() override { return this; }
        MaterialSourceData GetRenderMaterialSourceData() const override;
        IRenderTextureUploadSource* GetRenderMaterialTexture(RenderMaterialTextureSlot slot) const override;
        RenderMaterialTextureBinding GetRenderMaterialTextureBinding(RenderMaterialTextureSlot slot) const override;

        // =====================================================================
        // Material Data
        // =====================================================================

        std::shared_ptr<Material> GetMaterial() const { return m_material; }
        void SetMaterialData(std::shared_ptr<Material> material);

        const std::string& GetMaterialName() const;
        MaterialWorkflow GetWorkflow() const;
        MaterialWorkflowMode GetWorkflowMode() const;
        MaterialAlphaMode GetAlphaMode() const;
        Vec4 GetBaseColor() const;
        float GetMetallicFactor() const;
        float GetRoughnessFactor() const;
        float GetNormalScale() const;
        float GetOcclusionStrength() const;
        Vec3 GetEmissiveColor() const;
        float GetEmissiveStrength() const;
        float GetAlphaCutoff() const;
        bool IsDoubleSided() const;

        // =====================================================================
        // Textures
        // =====================================================================

        void SetTexture(const std::string& slot, ResourceHandle<TextureResource> texture);
        ResourceHandle<TextureResource> GetTexture(const std::string& slot) const;
        const std::unordered_map<std::string, ResourceHandle<TextureResource>>& GetTextures() const;

        // Standard texture slots
        ResourceHandle<TextureResource> GetAlbedoTexture() const { return GetTexture("albedo"); }
        ResourceHandle<TextureResource> GetNormalTexture() const { return GetTexture("normal"); }
        ResourceHandle<TextureResource> GetMetallicRoughnessTexture() const { return GetTexture("metallic_roughness"); }
        ResourceHandle<TextureResource> GetAOTexture() const { return GetTexture("ao"); }
        ResourceHandle<TextureResource> GetEmissiveTexture() const { return GetTexture("emissive"); }

        // =====================================================================
        // Shader (future)
        // =====================================================================

        // AssetHandle<ShaderAsset> GetShader() const;
        // void SetShader(AssetHandle<ShaderAsset> shader);

    private:
        std::shared_ptr<Material> m_material;
        std::unordered_map<std::string, ResourceHandle<TextureResource>> m_textures;

        // Future: shader reference
        // AssetHandle<ShaderAsset> m_shader;
    };

} // namespace RVX::Resource
