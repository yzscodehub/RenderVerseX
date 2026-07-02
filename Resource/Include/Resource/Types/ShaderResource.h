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
        std::string sourcePath;
        ShaderBackendType backend = ShaderBackendType::None;
        ShaderStage stage = ShaderStage::None;
        std::string entryPoint = "main";
        std::string targetProfile = "auto";
        std::uint64_t sourceHash = 0;
        std::uint32_t reflectionResourceCount = 0;
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
        ShaderBackendType GetBackend() const { return m_metadata.backend; }
        ShaderStage GetStage() const { return m_metadata.stage; }
        const std::string& GetEntryPoint() const { return m_metadata.entryPoint; }
        const std::string& GetTargetProfile() const { return m_metadata.targetProfile; }
        std::uint64_t GetSourceHash() const { return m_metadata.sourceHash; }
        std::uint32_t GetReflectionResourceCount() const { return m_metadata.reflectionResourceCount; }

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

    private:
        ShaderMetadata m_metadata;
        std::vector<std::uint8_t> m_bytecode;
        std::string m_glslSource;
        std::string m_mslSource;
    };
} // namespace RVX::Resource
