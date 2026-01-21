#pragma once

/**
 * @file MaterialAsset.h
 * @brief Material asset type
 */

#include "Asset/Asset.h"
#include "Asset/AssetHandle.h"
#include "Asset/Types/TextureAsset.h"
#include "Scene/Material.h"
#include <unordered_map>

namespace RVX::Asset
{
    /**
     * @brief Material asset - encapsulates Scene::Material with resource lifecycle
     * 
     * Note: In the future, Scene::Material will be renamed to MaterialData
     * and this class will encapsulate it.
     */
    class MaterialAsset : public Asset
    {
    public:
        MaterialAsset();
        ~MaterialAsset() override;

        // =====================================================================
        // Asset Interface
        // =====================================================================

        AssetType GetType() const override { return AssetType::Material; }
        const char* GetTypeName() const override { return "Material"; }
        size_t GetMemoryUsage() const override;
        std::vector<AssetId> GetRequiredDependencies() const override;

        // =====================================================================
        // Material Data (from Scene module)
        // =====================================================================

        Material* GetMaterialData() { return m_material.get(); }
        const Material* GetMaterialData() const { return m_material.get(); }
        void SetMaterialData(std::shared_ptr<Material> material);

        // Convenience accessors
        const std::string& GetMaterialName() const;
        MaterialWorkflow GetWorkflow() const;

        // =====================================================================
        // Texture References
        // =====================================================================

        void SetTexture(const std::string& slot, AssetHandle<TextureAsset> texture);
        AssetHandle<TextureAsset> GetTexture(const std::string& slot) const;
        const std::unordered_map<std::string, AssetHandle<TextureAsset>>& GetTextures() const;

        // =====================================================================
        // Runtime Instance
        // =====================================================================

        // Creates a runtime instance that can be modified without affecting the asset
        // std::unique_ptr<MaterialInstance> CreateInstance() const;

    private:
        std::shared_ptr<Material> m_material;
        std::unordered_map<std::string, AssetHandle<TextureAsset>> m_textures;
    };

} // namespace RVX::Asset
