#pragma once

/**
 * @file Material.h
 * @brief PBR material system supporting multiple workflows
 *
 * Migrated from found::model::Material
 */

#include "Core/MathTypes.h"
#include <string>
#include <unordered_map>
#include <memory>
#include <optional>
#include <limits>
#include <cstdint>

namespace RVX
{
    /**
     * @brief Material workflow type
     */
    enum class MaterialWorkflow
    {
        MetallicRoughness,      // Metallic/Roughness workflow (modern PBR standard)
        SpecularGlossiness,     // Specular/Glossiness workflow (traditional)
        Unlit                   // Unlit material

    };

    /**
     * @brief Texture information structure
     */
    struct TextureInfo
    {
        std::string texturePath;
        int32_t uvSet = 0;
        int32_t imageId = -1;  // Resolved image ID (-1 = unresolved)

        // Texture transform
        Vec2 offset{0.0f, 0.0f};
        Vec2 scale{1.0f, 1.0f};
        float rotation = 0.0f;

        // Sampling parameters
        enum class WrapMode
        {
            Repeat,
            MirrorRepeat,
            ClampToEdge,
            ClampToBorder
        };
        WrapMode wrapS = WrapMode::Repeat;
        WrapMode wrapT = WrapMode::Repeat;

        enum class FilterMode
        {
            Nearest,
            Linear,
            NearestMipmapNearest,
            LinearMipmapNearest,
            NearestMipmapLinear,
            LinearMipmapLinear
        };
        FilterMode minFilter = FilterMode::LinearMipmapLinear;
        FilterMode magFilter = FilterMode::Linear;

        TextureInfo() = default;
        explicit TextureInfo(const std::string& path) : texturePath(path) {}
        TextureInfo(const std::string& path, int uv) : texturePath(path), uvSet(uv) {}
    };

    /**
     * @brief PBR Material class
     *
     * Supports:
     * - Metallic/Roughness workflow (glTF 2.0 standard)
     * - Specular/Glossiness workflow (legacy/traditional)
     * - Material extensions (Clearcoat, Sheen, Subsurface)
     */
    class Material
    {
    public:
        using Ptr = std::shared_ptr<Material>;
        using ConstPtr = std::shared_ptr<const Material>;
        using WeakPtr = std::weak_ptr<Material>;

        // =====================================================================
        // Construction
        // =====================================================================

        explicit Material(const std::string& name = "Material");
        ~Material() = default;

        Material(Material&&) = default;
        Material& operator=(Material&&) = default;
        Material(const Material&) = delete;
        Material& operator=(const Material&) = delete;

        // =====================================================================
        // Basic Properties
        // =====================================================================

        const std::string& GetName() const { return m_name; }
        void SetName(const std::string& name) { m_name = name; }

        uint32_t GetMaterialId() const { return m_materialId; }
        void SetMaterialId(uint32_t id) { m_materialId = id; }

        MaterialWorkflow GetWorkflow() const { return m_workflow; }
        void SetWorkflow(MaterialWorkflow workflow) { m_workflow = workflow; }

        bool IsDoubleSided() const { return m_doubleSided; }
        void SetDoubleSided(bool doubleSided) { m_doubleSided = doubleSided; }

        // =====================================================================
        // Alpha Mode
        // =====================================================================

        enum class AlphaMode
        {
            Opaque,     // Fully opaque
            Mask,       // Alpha testing
            Blend       // Alpha blending
        };

        AlphaMode GetAlphaMode() const { return m_alphaMode; }
        void SetAlphaMode(AlphaMode mode) { m_alphaMode = mode; }

        float GetAlphaCutoff() const { return m_alphaCutoff; }
        void SetAlphaCutoff(float cutoff) { m_alphaCutoff = cutoff; }

        // =====================================================================
        // Metallic/Roughness Workflow
        // =====================================================================

        const Vec4& GetBaseColor() const { return m_baseColor; }
        void SetBaseColor(const Vec4& color) { m_baseColor = color; }
        void SetBaseColor(float r, float g, float b, float a = 1.0f) { m_baseColor = Vec4(r, g, b, a); }

        const std::optional<TextureInfo>& GetBaseColorTexture() const { return m_baseColorTexture; }
        void SetBaseColorTexture(const TextureInfo& texture) { m_baseColorTexture = texture; }
        void ClearBaseColorTexture() { m_baseColorTexture.reset(); }

        float GetMetallicFactor() const { return m_metallicFactor; }
        void SetMetallicFactor(float metallic) { m_metallicFactor = glm::clamp(metallic, 0.0f, 1.0f); }

        float GetRoughnessFactor() const { return m_roughnessFactor; }
        void SetRoughnessFactor(float roughness) { m_roughnessFactor = glm::clamp(roughness, 0.0f, 1.0f); }

        const std::optional<TextureInfo>& GetMetallicRoughnessTexture() const { return m_metallicRoughnessTexture; }
        void SetMetallicRoughnessTexture(const TextureInfo& texture) { m_metallicRoughnessTexture = texture; }
        void ClearMetallicRoughnessTexture() { m_metallicRoughnessTexture.reset(); }

        // =====================================================================
        // Common Textures
        // =====================================================================

        // Normal map
        const std::optional<TextureInfo>& GetNormalTexture() const { return m_normalTexture; }
        void SetNormalTexture(const TextureInfo& texture) { m_normalTexture = texture; }
        void ClearNormalTexture() { m_normalTexture.reset(); }
        float GetNormalScale() const { return m_normalScale; }
        void SetNormalScale(float scale) { m_normalScale = scale; }

        // Ambient occlusion
        const std::optional<TextureInfo>& GetOcclusionTexture() const { return m_occlusionTexture; }
        void SetOcclusionTexture(const TextureInfo& texture) { m_occlusionTexture = texture; }
        void ClearOcclusionTexture() { m_occlusionTexture.reset(); }
        float GetOcclusionStrength() const { return m_occlusionStrength; }
        void SetOcclusionStrength(float strength) { m_occlusionStrength = glm::clamp(strength, 0.0f, 1.0f); }

        // Emissive
        const Vec3& GetEmissiveColor() const { return m_emissiveColor; }
        void SetEmissiveColor(const Vec3& color) { m_emissiveColor = color; }
        const std::optional<TextureInfo>& GetEmissiveTexture() const { return m_emissiveTexture; }
        void SetEmissiveTexture(const TextureInfo& texture) { m_emissiveTexture = texture; }
        void ClearEmissiveTexture() { m_emissiveTexture.reset(); }
        float GetEmissiveStrength() const { return m_emissiveStrength; }
        void SetEmissiveStrength(float strength) { m_emissiveStrength = strength; }

        // KHR_materials_specular
        float GetSpecularFactor() const { return m_specularFactor; }
        void SetSpecularFactor(float factor) { m_specularFactor = factor; }
        const Vec3& GetSpecularColor() const { return m_specularColor; }
        void SetSpecularColor(const Vec3& color) { m_specularColor = color; }
        const std::optional<TextureInfo>& GetSpecularTexture() const { return m_specularTexture; }
        void SetSpecularTexture(const TextureInfo& texture) { m_specularTexture = texture; }
        const std::optional<TextureInfo>& GetSpecularColorTexture() const { return m_specularColorTexture; }
        void SetSpecularColorTexture(const TextureInfo& texture) { m_specularColorTexture = texture; }

        // KHR_materials_clearcoat
        float GetClearcoatFactor() const { return m_clearcoatFactor; }
        void SetClearcoatFactor(float factor) { m_clearcoatFactor = factor; }
        const std::optional<TextureInfo>& GetClearcoatTexture() const { return m_clearcoatTexture; }
        void SetClearcoatTexture(const TextureInfo& texture) { m_clearcoatTexture = texture; }
        float GetClearcoatRoughness() const { return m_clearcoatRoughness; }
        void SetClearcoatRoughness(float roughness) { m_clearcoatRoughness = roughness; }
        const std::optional<TextureInfo>& GetClearcoatRoughnessTexture() const { return m_clearcoatRoughnessTexture; }
        void SetClearcoatRoughnessTexture(const TextureInfo& texture) { m_clearcoatRoughnessTexture = texture; }
        float GetClearcoatNormalScale() const { return m_clearcoatNormalScale; }
        void SetClearcoatNormalScale(float scale) { m_clearcoatNormalScale = scale; }
        const std::optional<TextureInfo>& GetClearcoatNormalTexture() const { return m_clearcoatNormalTexture; }
        void SetClearcoatNormalTexture(const TextureInfo& texture) { m_clearcoatNormalTexture = texture; }

        // KHR_materials_transmission / volume / ior
        float GetTransmissionFactor() const { return m_transmissionFactor; }
        void SetTransmissionFactor(float factor) { m_transmissionFactor = factor; }
        const std::optional<TextureInfo>& GetTransmissionTexture() const { return m_transmissionTexture; }
        void SetTransmissionTexture(const TextureInfo& texture) { m_transmissionTexture = texture; }
        float GetIndexOfRefraction() const { return m_indexOfRefraction; }
        void SetIndexOfRefraction(float ior) { m_indexOfRefraction = ior; }
        float GetThicknessFactor() const { return m_thicknessFactor; }
        void SetThicknessFactor(float thickness) { m_thicknessFactor = thickness; }
        const std::optional<TextureInfo>& GetThicknessTexture() const { return m_thicknessTexture; }
        void SetThicknessTexture(const TextureInfo& texture) { m_thicknessTexture = texture; }
        const Vec3& GetAttenuationColor() const { return m_attenuationColor; }
        void SetAttenuationColor(const Vec3& color) { m_attenuationColor = color; }
        float GetAttenuationDistance() const { return m_attenuationDistance; }
        void SetAttenuationDistance(float distance) { m_attenuationDistance = distance; }

        // =====================================================================
        // Utility
        // =====================================================================

        bool IsTransparent() const;
        Vec4 GetEffectiveBaseColor() const;
        Ptr Clone(bool generateNewId = true) const;

    private:
        std::string m_name;
        uint32_t m_materialId = 0;
        MaterialWorkflow m_workflow = MaterialWorkflow::MetallicRoughness;
        bool m_doubleSided = false;

        // Alpha
        AlphaMode m_alphaMode = AlphaMode::Opaque;
        float m_alphaCutoff = 0.5f;

        // Metallic/Roughness workflow
        Vec4 m_baseColor{1.0f, 1.0f, 1.0f, 1.0f};
        std::optional<TextureInfo> m_baseColorTexture;
        float m_metallicFactor = 1.0f;
        float m_roughnessFactor = 1.0f;
        std::optional<TextureInfo> m_metallicRoughnessTexture;

        // Common textures
        std::optional<TextureInfo> m_normalTexture;
        float m_normalScale = 1.0f;
        std::optional<TextureInfo> m_occlusionTexture;
        float m_occlusionStrength = 1.0f;
        Vec3 m_emissiveColor{0.0f, 0.0f, 0.0f};
        std::optional<TextureInfo> m_emissiveTexture;
        float m_emissiveStrength = 1.0f;

        float m_specularFactor = 1.0f;
        Vec3 m_specularColor{1.0f, 1.0f, 1.0f};
        std::optional<TextureInfo> m_specularTexture;
        std::optional<TextureInfo> m_specularColorTexture;

        float m_clearcoatFactor = 0.0f;
        std::optional<TextureInfo> m_clearcoatTexture;
        float m_clearcoatRoughness = 0.0f;
        std::optional<TextureInfo> m_clearcoatRoughnessTexture;
        float m_clearcoatNormalScale = 1.0f;
        std::optional<TextureInfo> m_clearcoatNormalTexture;

        float m_transmissionFactor = 0.0f;
        std::optional<TextureInfo> m_transmissionTexture;
        float m_indexOfRefraction = 1.5f;
        float m_thicknessFactor = 0.0f;
        std::optional<TextureInfo> m_thicknessTexture;
        Vec3 m_attenuationColor{1.0f, 1.0f, 1.0f};
        // Zero is the canonical finite representation of infinite distance.
        float m_attenuationDistance = 0.0f;
    };

} // namespace RVX
