/**
 * @file MaterialInstance.h
 * @brief Material instance - runtime parameters for a material template
 * 
 * MaterialInstance stores the actual parameter values for rendering.
 * Multiple instances can share the same template with different values.
 */

#pragma once

#include "Render/Material/MaterialTemplate.h"
#include "Core/Types.h"
#include "Core/MathTypes.h"
#include "RHI/RHI.h"
#include <memory>
#include <string>
#include <vector>

namespace RVX
{
    /**
     * @brief Instance of a material template with specific parameter values
     * 
     * MaterialInstance provides:
     * - Override values for template parameters
     * - Constant buffer management for GPU upload
     * - Parent instance inheritance
     * 
     * Usage:
     * @code
     * auto instance = MaterialInstance::Create(pbrTemplate);
     * instance->SetVector("BaseColor", Vec4(1.0f, 0.0f, 0.0f, 1.0f));
     * instance->SetFloat("Roughness", 0.3f);
     * instance->SetTexture("AlbedoMap", albedoTextureId);
     * @endcode
     */
    class MaterialInstance
    {
    public:
        using Ptr = std::shared_ptr<MaterialInstance>;
        using ConstPtr = std::shared_ptr<const MaterialInstance>;

        // =========================================================================
        // Construction
        // =========================================================================

        MaterialInstance() = default;
        explicit MaterialInstance(MaterialTemplate::ConstPtr materialTemplate);

        static Ptr Create(MaterialTemplate::ConstPtr materialTemplate)
        {
            return std::make_shared<MaterialInstance>(materialTemplate);
        }

        // =========================================================================
        // Template
        // =========================================================================

        /// Get the material template
        MaterialTemplate::ConstPtr GetTemplate() const { return m_template; }

        /// Change the template (resets overrides)
        void SetTemplate(MaterialTemplate::ConstPtr materialTemplate);

        // =========================================================================
        // Parent Instance (for inheritance)
        // =========================================================================

        /// Set a parent instance to inherit values from
        void SetParent(MaterialInstance::ConstPtr parent) { m_parent = parent; }
        MaterialInstance::ConstPtr GetParent() const { return m_parent; }

        // =========================================================================
        // Scalar Parameters
        // =========================================================================

        /// Set a float parameter
        void SetFloat(const std::string& name, float value);
        float GetFloat(const std::string& name) const;

        /// Set a Vec2 parameter
        void SetVector2(const std::string& name, const Vec2& value);
        Vec2 GetVector2(const std::string& name) const;

        /// Set a Vec3 parameter
        void SetVector3(const std::string& name, const Vec3& value);
        Vec3 GetVector3(const std::string& name) const;

        /// Set a Vec4 parameter
        void SetVector4(const std::string& name, const Vec4& value);
        Vec4 GetVector4(const std::string& name) const;

        /// Set an int parameter
        void SetInt(const std::string& name, int32 value);
        int32 GetInt(const std::string& name) const;

        /// Set a bool parameter
        void SetBool(const std::string& name, bool value);
        bool GetBool(const std::string& name) const;

        // =========================================================================
        // Texture Parameters
        // =========================================================================

        /// Set a texture parameter by resource ID
        void SetTexture(const std::string& name, uint64 textureId);
        uint64 GetTexture(const std::string& name) const;

        // =========================================================================
        // Override Management
        // =========================================================================

        /// Check if a parameter has been overridden
        bool HasOverride(const std::string& name) const;

        /// Clear an override (use template/parent default)
        void ClearOverride(const std::string& name);

        /// Clear all overrides
        void ClearAllOverrides();

        /// Get number of overrides
        size_t GetOverrideCount() const { return m_overrides.size(); }

        // =========================================================================
        // GPU Data
        // =========================================================================

        /**
         * @brief Get constant buffer data for GPU upload
         * @param outData Pointer to write data to (must be at least GetConstantBufferSize() bytes)
         */
        void GetConstantBufferData(void* outData) const;

        /**
         * @brief Get the size of the constant buffer
         */
        uint32 GetConstantBufferSize() const;

        /**
         * @brief Mark instance as dirty (needs GPU update)
         */
        void MarkDirty() { m_dirty = true; }

        /**
         * @brief Check if instance needs GPU update
         */
        bool IsDirty() const { return m_dirty; }

        /**
         * @brief Clear dirty flag after GPU update
         */
        void ClearDirty() { m_dirty = false; }

        // =========================================================================
        // Texture Bindings
        // =========================================================================

        struct TextureBinding
        {
            uint32 binding;
            uint64 textureId;
        };

        /**
         * @brief Get all texture bindings for this instance
         */
        std::vector<TextureBinding> GetTextureBindings() const;

        // =========================================================================
        // Identity
        // =========================================================================

        const std::string& GetName() const { return m_name; }
        void SetName(const std::string& name) { m_name = name; }

        uint64 GetId() const { return m_id; }
        void SetId(uint64 id) { m_id = id; }

    private:
        MaterialParamValue GetParameterValue(const std::string& name) const;
        void SetParameterValue(const std::string& name, MaterialParamValue value);

        std::string m_name;
        uint64 m_id = 0;

        MaterialTemplate::ConstPtr m_template;
        MaterialInstance::ConstPtr m_parent;

        // Override values (parameter name -> value)
        std::unordered_map<std::string, MaterialParamValue> m_overrides;

        bool m_dirty = true;
    };

    /**
     * @brief Dynamic material instance that can be modified at runtime
     * 
     * Provides convenience methods and automatic dirty tracking.
     */
    class DynamicMaterialInstance : public MaterialInstance
    {
    public:
        using Ptr = std::shared_ptr<DynamicMaterialInstance>;

        DynamicMaterialInstance() = default;
        explicit DynamicMaterialInstance(MaterialTemplate::ConstPtr materialTemplate)
            : MaterialInstance(materialTemplate) {}

        static Ptr Create(MaterialTemplate::ConstPtr materialTemplate)
        {
            return std::make_shared<DynamicMaterialInstance>(materialTemplate);
        }

        // =========================================================================
        // Animation-friendly setters
        // =========================================================================

        /**
         * @brief Lerp a float parameter over time
         */
        void LerpFloat(const std::string& name, float target, float t);

        /**
         * @brief Lerp a vector parameter over time
         */
        void LerpVector4(const std::string& name, const Vec4& target, float t);

        /**
         * @brief Pulse a float parameter
         */
        void PulseFloat(const std::string& name, float amplitude, float frequency, float time);
    };

} // namespace RVX
