#pragma once

/**
 * @file DeterministicShaderProgramProvider.h
 * @brief Test-only shader provider with stable programs and reflection.
 */

#include "RHI/RHI.h"
#include "ShaderContracts/ShaderProgramProvider.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

namespace RVX::Tests
{
    class DeterministicShaderProgramProvider
        : public IShaderProgramProvider
    {
    public:
        explicit DeterministicShaderProgramProvider(
            std::filesystem::path sourceRoot = {})
            : m_sourceRoot(std::move(sourceRoot))
        {
        }

        ShaderProgramResult Load(
            IRHIDevice& device,
            const ShaderProgramRequest& request) override
        {
            ShaderProgramResult result;
            if (request.logicalAssetId.empty() || request.entryPoint.empty() ||
                request.stage == RHIShaderStage::None ||
                request.backend == RHIBackendType::None ||
                request.backend == RHIBackendType::Auto)
            {
                result.diagnostic = "Incomplete deterministic shader request";
                return result;
            }

            result.contentHash = BuildArtifactHash(request);
            if (!m_sourceRoot.empty())
            {
                const std::filesystem::path sourcePath =
                    m_sourceRoot / request.logicalAssetId;
                std::ifstream source(sourcePath, std::ios::binary);
                if (!source)
                {
                    result.status = ShaderProgramStatus::NotFound;
                    result.diagnostic =
                        "Shader program not found: " +
                        sourcePath.generic_string();
                    return result;
                }
                const std::string bytes{
                    std::istreambuf_iterator<char>(source),
                    std::istreambuf_iterator<char>()};
                HashBytes(result.contentHash, bytes.data(), bytes.size());
            }
            PopulateReflection(request, result.reflection);
            RHIShaderInterface shaderInterface = BuildShaderInterface(
                request.stage, result.reflection);

            const uint64 bytecode = result.contentHash;
            RHIShaderDesc desc;
            desc.stage = request.stage;
            desc.bytecode = &bytecode;
            desc.bytecodeSize = sizeof(bytecode);
            desc.entryPoint = request.entryPoint.c_str();
            desc.debugName = request.logicalAssetId.c_str();
            desc.shaderInterface = &shaderInterface;
            result.shader = device.CreateShader(desc);
            if (!result.shader)
            {
                result.status = ShaderProgramStatus::DeviceCreationFailed;
                result.diagnostic = "Test RHI rejected deterministic shader";
                return result;
            }

            result.status = ShaderProgramStatus::Success;
            return result;
        }

    protected:
        static void AddResource(
            ShaderReflection& reflection,
            const char* name,
            uint32 set,
            uint32 binding,
            RHIBindingType type)
        {
            reflection.resources.push_back(
                {name, set, binding, type, 1});
        }

    private:
        static void HashBytes(
            uint64& hash, const void* data, size_t size) noexcept
        {
            constexpr uint64 prime = 0x100000001b3ull;
            const auto* bytes = static_cast<const uint8*>(data);
            for (size_t index = 0; index < size; ++index)
            {
                hash ^= bytes[index];
                hash *= prime;
            }
        }

        static void HashString(uint64& hash, const std::string& value) noexcept
        {
            HashBytes(hash, value.data(), value.size());
        }

        static uint64 BuildArtifactHash(
            const ShaderProgramRequest& request) noexcept
        {
            uint64 hash = 0xcbf29ce484222325ull;
            HashString(hash, request.logicalAssetId);
            HashString(hash, request.entryPoint);
            HashBytes(hash, &request.stage, sizeof(request.stage));
            HashBytes(hash, &request.backend, sizeof(request.backend));
            for (const ShaderMacro& define : request.defines)
            {
                HashString(hash, define.name);
                HashString(hash, define.value);
            }
            return hash;
        }

        static void PopulateReflection(
            const ShaderProgramRequest& request,
            ShaderReflection& reflection)
        {
            if (request.logicalAssetId.find("DefaultLit.hlsl") ==
                std::string::npos)
            {
                return;
            }

            reflection.valid = true;
            if (request.stage == RHIShaderStage::Vertex)
            {
                AddResource(
                    reflection, "ViewConstants", 0, 0,
                    RHIBindingType::UniformBuffer);
                AddResource(
                    reflection, "ObjectConstants", 1, 0,
                    RHIBindingType::UniformBuffer);
                AddResource(
                    reflection, "ObjectInstances", 1, 1,
                    RHIBindingType::ShaderResourceBuffer);
                return;
            }
            if (request.stage != RHIShaderStage::Pixel)
            {
                return;
            }

            AddResource(reflection, "ViewConstants", 0, 0,
                        RHIBindingType::UniformBuffer);
            AddResource(reflection, "EnvironmentTexture", 0, 1,
                        RHIBindingType::SampledTexture);
            AddResource(reflection, "EnvironmentSampler", 0, 2,
                        RHIBindingType::Sampler);
            AddResource(reflection, "LightConstants", 0, 3,
                        RHIBindingType::UniformBuffer);
            AddResource(reflection, "PointLights", 0, 4,
                        RHIBindingType::ShaderResourceBuffer);
            AddResource(reflection, "SpotLights", 0, 5,
                        RHIBindingType::ShaderResourceBuffer);
            AddResource(reflection, "ShadowMap", 0, 6,
                        RHIBindingType::SampledTexture);
            AddResource(reflection, "ClusterConstants", 0, 7,
                        RHIBindingType::UniformBuffer);
            AddResource(reflection, "Clusters", 0, 8,
                        RHIBindingType::ShaderResourceBuffer);
            AddResource(reflection, "ClusterLightIndices", 0, 9,
                        RHIBindingType::ShaderResourceBuffer);
            AddResource(reflection, "IrradianceTexture", 0, 10,
                        RHIBindingType::SampledTexture);
            AddResource(reflection, "PrefilteredEnvironmentTexture", 0, 11,
                        RHIBindingType::SampledTexture);
            AddResource(reflection, "BRDFLUTTexture", 0, 12,
                        RHIBindingType::SampledTexture);
            AddResource(reflection, "IBLLinearClampSampler", 0, 13,
                        RHIBindingType::Sampler);
            AddResource(reflection, "MaterialConstants", 2, 0,
                        RHIBindingType::UniformBuffer);
            for (uint32 binding = 1; binding <= 5; ++binding)
            {
                AddResource(reflection, "MaterialTexture", 2, binding,
                            RHIBindingType::SampledTexture);
            }
            for (uint32 binding = 6; binding <= 10; ++binding)
            {
                AddResource(reflection, "MaterialSampler", 2, binding,
                            RHIBindingType::Sampler);
            }
        }

        static RHIShaderInterface BuildShaderInterface(
            RHIShaderStage stage,
            const ShaderReflection& reflection)
        {
            RHIShaderInterface shaderInterface;
            shaderInterface.available = reflection.valid;
            shaderInterface.stage = stage;
            for (const ShaderReflection::ResourceBinding& resource :
                 reflection.resources)
            {
                shaderInterface.bindings.push_back({
                    resource.set,
                    resource.binding,
                    resource.type,
                    resource.count,
                });
            }
            return FinalizeRHIShaderInterface(std::move(shaderInterface));
        }

        std::filesystem::path m_sourceRoot;
    };

    inline IShaderProgramProvider& GetDeterministicShaderProgramProvider()
    {
        static DeterministicShaderProgramProvider provider;
        return provider;
    }

    inline std::unique_ptr<IShaderProgramProvider>
        CreateDeterministicShaderProgramProvider()
    {
        return std::make_unique<DeterministicShaderProgramProvider>();
    }
} // namespace RVX::Tests
