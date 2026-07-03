#pragma once

/**
 * @file ShaderResource.h
 * @brief Cooked shader resource type
 */

#include "Resource/IResource.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RVX::Resource
{
    inline constexpr std::uint32_t RVX_SHADER_RESOURCE_METADATA_SCHEMA_VERSION = 1;
    inline constexpr const char* RVX_SHADER_RUNTIME_CONTRACT_SCHEMA_ID =
        "RVX.Resource.ShaderRuntimeContract";
    inline constexpr std::uint32_t RVX_SHADER_RUNTIME_CONTRACT_SCHEMA_VERSION = 1;

    enum class ShaderBackendType : std::uint8_t
    {
        None = 0,
        Auto,
        DX11,
        DX12,
        Vulkan,
        Metal,
        OpenGL
    };

    enum class ShaderStage : std::uint8_t
    {
        None = 0,
        Vertex,
        Hull,
        Domain,
        Geometry,
        Pixel,
        Compute,
        Mesh,
        Amplification,
        RayGeneration,
        AnyHit,
        ClosestHit,
        Miss,
        Intersection,
        Callable
    };

    /**
     * @brief Metadata stored with a cooked shader artifact.
     */
    struct ShaderMetadata
    {
        std::uint32_t schemaVersion = RVX_SHADER_RESOURCE_METADATA_SCHEMA_VERSION;
        std::string sourcePath;
        ShaderBackendType backend = ShaderBackendType::None;
        ShaderStage stage = ShaderStage::None;
        std::string entryPoint = "main";
        std::string targetProfile = "auto";
        std::uint64_t sourceHash = 0;
        std::uint32_t reflectionResourceCount = 0;
    };

    enum class ShaderRuntimeContractStatus : std::uint8_t
    {
        Valid = 0,
        MissingRuntimePayload,
        MissingSourcePath,
        MissingEntryPoint,
        MissingTargetProfile,
        MissingBackend,
        MissingStage
    };

    struct ShaderRuntimeContract
    {
        std::uint32_t schemaVersion = RVX_SHADER_RUNTIME_CONTRACT_SCHEMA_VERSION;
        bool valid = false;
        ShaderRuntimeContractStatus status = ShaderRuntimeContractStatus::MissingRuntimePayload;
        std::string diagnosticMessage;
        std::string cacheKey;
        std::uint64_t contractHash = 0;
        std::uint64_t payloadHash = 0;
        std::uint64_t sourceHash = 0;
        std::uint32_t reflectionResourceCount = 0;
        std::uint64_t bytecodeSize = 0;
        std::uint64_t glslSourceSize = 0;
        std::uint64_t mslSourceSize = 0;
    };

    /**
     * @brief Runtime representation of a cooked shader artifact.
     */
    class ShaderResource : public IResource
    {
    public:
        ShaderResource();
        ~ShaderResource() override;

        // =====================================================================
        // Resource Interface
        // =====================================================================

        ResourceType GetType() const override { return ResourceType::Shader; }
        const char* GetTypeName() const override { return "Shader"; }
        size_t GetMemoryUsage() const override;
        size_t GetGPUMemoryUsage() const override;

        // =====================================================================
        // Metadata
        // =====================================================================

        const ShaderMetadata& GetMetadata() const { return m_metadata; }
        const ShaderRuntimeContract& GetRuntimeContract() const { return m_runtimeContract; }
        ShaderBackendType GetBackend() const { return m_metadata.backend; }
        ShaderStage GetStage() const { return m_metadata.stage; }
        const std::string& GetEntryPoint() const { return m_metadata.entryPoint; }
        const std::string& GetTargetProfile() const { return m_metadata.targetProfile; }
        std::uint64_t GetSourceHash() const { return m_metadata.sourceHash; }
        std::uint32_t GetReflectionResourceCount() const { return m_metadata.reflectionResourceCount; }
        bool HasValidRuntimeContract() const { return m_runtimeContract.valid; }
        std::uint64_t GetRuntimeContractHash() const { return m_runtimeContract.contractHash; }

        // =====================================================================
        // Data Access
        // =====================================================================

        const std::vector<std::uint8_t>& GetBytecode() const { return m_bytecode; }
        const std::string& GetGLSLSource() const { return m_glslSource; }
        const std::string& GetMSLSource() const { return m_mslSource; }
        bool HasBytecode() const { return !m_bytecode.empty(); }
        bool HasTranslatedSource() const { return !m_glslSource.empty() || !m_mslSource.empty(); }

        void SetData(std::vector<std::uint8_t> bytecode,
                     std::string glslSource,
                     std::string mslSource,
                     const ShaderMetadata& metadata);

        /// Export the runtime shader contract as a stable machine-readable JSON artifact.
        std::string ExportRuntimeContractJson() const;

        /// Save the runtime shader contract JSON artifact for tools/CI.
        bool SaveRuntimeContractJson(const char* filename) const;

    private:
        void RebuildRuntimeContract();

        ShaderMetadata m_metadata;
        ShaderRuntimeContract m_runtimeContract;
        std::vector<std::uint8_t> m_bytecode;
        std::string m_glslSource;
        std::string m_mslSource;
    };
} // namespace RVX::Resource
