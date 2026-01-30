/**
 * @file MaterialTemplate.h
 * @brief Material template definition for creating material instances
 * 
 * A MaterialTemplate defines the shader, parameters, and default values
 * that can be instantiated into MaterialInstances.
 */

#pragma once

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include "RHI/RHI.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <variant>

namespace RVX
{
    // Forward declarations
    class IRHIDevice;
    class RHIPipeline;

    /**
     * @brief Parameter types supported by materials
     */
    enum class MaterialParamType : uint8
    {
        Float,
        Float2,
        Float3,
        Float4,
        Int,
        Bool,
        Texture2D,
        TextureCube,
        Sampler
    };

    /**
     * @brief Blend mode for materials
     */
    enum class MaterialBlendMode : uint8
    {
        Opaque,
        Masked,
        Translucent,
        Additive,
        Modulate
    };

    /**
     * @brief Shading model for materials
     */
    enum class MaterialShadingModel : uint8
    {
        Unlit,
        DefaultLit,          // Standard PBR
        Subsurface,
        ClearCoat,
        Cloth,
        Hair,
        Eye,
        TwoSidedFoliage
    };

    /**
     * @brief Material domain (where the material applies)
     */
    enum class MaterialDomain : uint8
    {
        Surface,             // Normal surface material
        PostProcess,         // Post-processing material
        UI,                  // UI material
        Decal,               // Decal material
        Volume               // Volumetric material
    };

    /**
     * @brief Default value for material parameter
     */
    using MaterialParamValue = std::variant<
        float, Vec2, Vec3, Vec4, int32, bool, uint64  // uint64 for texture IDs
    >;

    /**
     * @brief Material parameter definition
     */
    struct MaterialParameterDef
    {
        std::string name;
        MaterialParamType type = MaterialParamType::Float;
        MaterialParamValue defaultValue;
        
        // For numeric types
        float minValue = 0.0f;
        float maxValue = 1.0f;
        
        // UI hints
        std::string displayName;
        std::string group;
        bool hidden = false;
        
        // Shader binding
        uint32 offset = 0;      // Offset in constant buffer
        uint32 binding = 0;     // Binding slot for textures
    };

    /**
     * @brief Material template definition
     * 
     * Defines the shader and parameters for a class of materials.
     * Instances can override parameter values.
     */
    class MaterialTemplate
    {
    public:
        using Ptr = std::shared_ptr<MaterialTemplate>;
        using ConstPtr = std::shared_ptr<const MaterialTemplate>;

        // =========================================================================
        // Construction
        // =========================================================================

        MaterialTemplate() = default;
        explicit MaterialTemplate(const std::string& name);

        static Ptr Create(const std::string& name)
        {
            return std::make_shared<MaterialTemplate>(name);
        }

        // =========================================================================
        // Identity
        // =========================================================================

        const std::string& GetName() const { return m_name; }
        void SetName(const std::string& name) { m_name = name; }

        uint64 GetId() const { return m_id; }
        void SetId(uint64 id) { m_id = id; }

        // =========================================================================
        // Shader Configuration
        // =========================================================================

        /// Set the vertex shader
        void SetVertexShader(const std::string& path) { m_vertexShaderPath = path; }
        const std::string& GetVertexShaderPath() const { return m_vertexShaderPath; }

        /// Set the pixel/fragment shader
        void SetPixelShader(const std::string& path) { m_pixelShaderPath = path; }
        const std::string& GetPixelShaderPath() const { return m_pixelShaderPath; }

        /// Set shader defines
        void SetDefines(const std::vector<std::string>& defines) { m_shaderDefines = defines; }
        const std::vector<std::string>& GetDefines() const { return m_shaderDefines; }

        // =========================================================================
        // Material Properties
        // =========================================================================

        /// Blend mode
        MaterialBlendMode GetBlendMode() const { return m_blendMode; }
        void SetBlendMode(MaterialBlendMode mode) { m_blendMode = mode; }

        /// Shading model
        MaterialShadingModel GetShadingModel() const { return m_shadingModel; }
        void SetShadingModel(MaterialShadingModel model) { m_shadingModel = model; }

        /// Material domain
        MaterialDomain GetDomain() const { return m_domain; }
        void SetDomain(MaterialDomain domain) { m_domain = domain; }

        /// Two-sided rendering
        bool IsTwoSided() const { return m_twoSided; }
        void SetTwoSided(bool twoSided) { m_twoSided = twoSided; }

        /// Wireframe mode
        bool IsWireframe() const { return m_wireframe; }
        void SetWireframe(bool wireframe) { m_wireframe = wireframe; }

        // =========================================================================
        // Parameters
        // =========================================================================

        /**
         * @brief Add a float parameter
         */
        void AddFloatParameter(const std::string& name, float defaultValue,
                               float minVal = 0.0f, float maxVal = 1.0f);

        /**
         * @brief Add a vector parameter
         */
        void AddVectorParameter(const std::string& name, const Vec4& defaultValue);
        void AddVector3Parameter(const std::string& name, const Vec3& defaultValue);
        void AddVector2Parameter(const std::string& name, const Vec2& defaultValue);

        /**
         * @brief Add a texture parameter
         */
        void AddTextureParameter(const std::string& name, uint32 binding = 0,
                                 uint64 defaultTextureId = 0);

        /**
         * @brief Get all parameters
         */
        const std::vector<MaterialParameterDef>& GetParameters() const { return m_parameters; }

        /**
         * @brief Find parameter by name
         */
        const MaterialParameterDef* FindParameter(const std::string& name) const;

        /**
         * @brief Get parameter index
         */
        int32 GetParameterIndex(const std::string& name) const;

        // =========================================================================
        // Compilation
        // =========================================================================

        /**
         * @brief Compile the material template for a specific device
         */
        bool Compile(IRHIDevice* device);

        /**
         * @brief Check if compiled
         */
        bool IsCompiled() const { return m_compiled; }

        /**
         * @brief Get the compiled pipeline
         */
        RHIPipeline* GetPipeline() const { return m_pipeline.Get(); }

        /**
         * @brief Get constant buffer size needed for parameters
         */
        uint32 GetConstantBufferSize() const { return m_constantBufferSize; }

    private:
        void CalculateParameterOffsets();

        std::string m_name;
        uint64 m_id = 0;

        // Shaders
        std::string m_vertexShaderPath;
        std::string m_pixelShaderPath;
        std::vector<std::string> m_shaderDefines;

        // Properties
        MaterialBlendMode m_blendMode = MaterialBlendMode::Opaque;
        MaterialShadingModel m_shadingModel = MaterialShadingModel::DefaultLit;
        MaterialDomain m_domain = MaterialDomain::Surface;
        bool m_twoSided = false;
        bool m_wireframe = false;

        // Parameters
        std::vector<MaterialParameterDef> m_parameters;
        std::unordered_map<std::string, size_t> m_parameterLookup;
        uint32 m_constantBufferSize = 0;

        // Compiled state
        bool m_compiled = false;
        RHIPipelineRef m_pipeline;
    };

} // namespace RVX
