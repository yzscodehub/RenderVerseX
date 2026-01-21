#pragma once

/**
 * @file MaterialResource.h
 * @brief Material resource type
 */

#include "Resource/IResource.h"
#include "Resource/ResourceHandle.h"
#include "Resource/Types/TextureResource.h"
#include "Scene/Material.h"
#include <memory>
#include <string>
#include <unordered_map>

namespace RVX::Resource
{
    /**
     * @brief Material resource - encapsulates Scene::Material with texture references
     */
    class MaterialResource : public IResource
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

        // =====================================================================
        // Material Data
        // =====================================================================

        std::shared_ptr<Material> GetMaterial() const { return m_material; }
        void SetMaterialData(std::shared_ptr<Material> material);

        const std::string& GetMaterialName() const;
        MaterialWorkflow GetWorkflow() const;

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
