#pragma once

#include "ShaderCompiler/ShaderCompiler.h"
#include "ShaderCompiler/ShaderTypes.h"
#include "RHI/RHIShader.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <functional>

namespace RVX
{
    class ShaderCompileService;
    class ShaderCacheManager;
    class IRHIDevice;

    // =========================================================================
    // Permutation Dimension Definition
    // =========================================================================
    struct ShaderPermutationDimension
    {
        std::string name;                     // Macro name e.g. "LIGHTING_MODEL"
        std::vector<std::string> values;      // Possible values e.g. {"PHONG", "PBR", "UNLIT"}
        bool optional = false;                // If true, macro can be undefined
        std::string defaultValue;             // Default value when not specified
    };

    // =========================================================================
    // Permutation Space (all possible variant combinations)
    // =========================================================================
    struct ShaderPermutationSpace
    {
        std::vector<ShaderPermutationDimension> dimensions;

        /** @brief Get total number of variants */
        uint64 GetTotalVariantCount() const;

        /** @brief Enumerate all variant combinations */
        std::vector<std::vector<ShaderMacro>> EnumerateAll() const;

        /** @brief Compute hash for a specific variant */
        uint64 ComputePermutationHash(const std::vector<ShaderMacro>& defines) const;

        /** @brief Normalize defines (sort by name, fill defaults) */
        std::vector<ShaderMacro> Normalize(const std::vector<ShaderMacro>& defines) const;

        /** @brief Validate if defines are within the space */
        bool IsValid(const std::vector<ShaderMacro>& defines) const;

        /** @brief Check if empty */
        bool IsEmpty() const { return dimensions.empty(); }
    };

    // =========================================================================
    // Variant Priority (for prewarm ordering)
    // =========================================================================
    enum class VariantPriority : uint8
    {
        Critical = 0,   // Required at startup
        High = 1,       // Commonly used
        Medium = 2,     // Occasionally used
        Low = 3         // Rarely used
    };

    // =========================================================================
    // Shader Load Description (for permutation system)
    // =========================================================================
    struct ShaderPermutationLoadDesc
    {
        std::string path;
        std::string entryPoint;
        RHIShaderStage stage = RHIShaderStage::None;
        RHIBackendType backend = RHIBackendType::DX12;
        std::string targetProfile;
        bool enableDebugInfo = false;
        bool enableOptimization = true;
    };

    // =========================================================================
    // Shader Permutation System
    // =========================================================================
    class ShaderPermutationSystem
    {
    public:
        // =====================================================================
        // Construction
        // =====================================================================
        ShaderPermutationSystem(
            ShaderCompileService* compileService,
            ShaderCacheManager* cacheManager);

        ~ShaderPermutationSystem() = default;

        // Non-copyable
        ShaderPermutationSystem(const ShaderPermutationSystem&) = delete;
        ShaderPermutationSystem& operator=(const ShaderPermutationSystem&) = delete;

        // =====================================================================
        // Shader Registration
        // =====================================================================

        /** @brief Register shader with its permutation space */
        void RegisterShader(
            const std::string& shaderPath,
            const ShaderPermutationSpace& space,
            const ShaderPermutationLoadDesc& baseDesc);

        /** @brief Unregister shader */
        void UnregisterShader(const std::string& shaderPath);

        /** @brief Check if shader is registered */
        bool IsRegistered(const std::string& shaderPath) const;

        // =====================================================================
        // Variant Access
        // =====================================================================

        /** @brief Get or create variant (may trigger compilation) */
        RHIShaderRef GetVariant(
            IRHIDevice* device,
            const std::string& shaderPath,
            const std::vector<ShaderMacro>& defines);

        /** @brief Async get variant */
        CompileHandle GetVariantAsync(
            IRHIDevice* device,
            const std::string& shaderPath,
            const std::vector<ShaderMacro>& defines,
            std::function<void(RHIShaderRef)> callback);

        /** @brief Check if variant is already compiled */
        bool HasVariant(
            const std::string& shaderPath,
            const std::vector<ShaderMacro>& defines) const;

        // =====================================================================
        // Prewarming
        // =====================================================================

        /** @brief Prewarm specific variants (background compile) */
        void PrewarmVariants(
            IRHIDevice* device,
            const std::string& shaderPath,
            const std::vector<std::vector<ShaderMacro>>& variants,
            VariantPriority priority = VariantPriority::Medium);

        /** @brief Prewarm all variants */
        void PrewarmAllVariants(
            IRHIDevice* device,
            const std::string& shaderPath,
            VariantPriority priority = VariantPriority::Low);

        /** @brief Get prewarm progress (0.0 to 1.0) */
        float GetPrewarmProgress(const std::string& shaderPath) const;

        // =====================================================================
        // Statistics
        // =====================================================================

        /** @brief Get compiled variant count for a shader */
        uint32 GetCompiledVariantCount(const std::string& shaderPath) const;

        /** @brief Get total variant count for a shader */
        uint32 GetTotalVariantCount(const std::string& shaderPath) const;

        /** @brief Get pending compile count */
        uint32 GetPendingCompileCount() const;

        /** @brief Clear variants for a shader */
        void ClearVariants(const std::string& shaderPath);

        /** @brief Clear all variants for all shaders */
        void ClearAllVariants();

    private:
        // =====================================================================
        // Internal Structures
        // =====================================================================
        struct ShaderEntry
        {
            std::string source;
            ShaderPermutationSpace space;
            ShaderPermutationLoadDesc baseDesc;
            std::unordered_map<uint64, RHIShaderRef> variants;
            std::unordered_map<uint64, CompileHandle> pendingCompiles;
            mutable std::mutex mutex;
        };

        uint64 ComputeVariantKey(
            const std::string& shaderPath,
            const std::vector<ShaderMacro>& defines) const;

        bool LoadShaderSource(const std::string& path, std::string& outSource) const;

        ShaderCompileService* m_compileService;
        ShaderCacheManager* m_cacheManager;

        mutable std::shared_mutex m_shadersMutex;
        std::unordered_map<std::string, std::unique_ptr<ShaderEntry>> m_shaders;
    };

} // namespace RVX
