#pragma once

/**
 * @file DeterministicShaderCompiler.h
 * @brief Test-only shader compiler with stable artifacts and reflection.
 */

#include "ShaderCompiler/ShaderCompiler.h"

#include <cstring>
#include <filesystem>
#include <memory>

namespace RVX::Tests
{
    class DeterministicShaderCompiler final : public IShaderCompiler
    {
    public:
        ShaderCompileSupport QuerySupport(
            const ShaderCompileOptions&) const override
        {
            return ShaderCompileSupport::Supported();
        }

        ShaderCompileResult Compile(
            const ShaderCompileOptions& options) override
        {
            ShaderCompileResult result;
            if (!options.sourceCode || !options.entryPoint)
            {
                result.errorMessage = "Missing shader source or entry point";
                return result;
            }

            const uint64 artifactHash = BuildArtifactHash(options);
            result.success = true;
            result.permutationHash = artifactHash;
            result.sourceInfo.mainFile =
                options.sourcePath ? options.sourcePath : "deterministic-shader";
            result.sourceInfo.combinedHash = artifactHash;
            result.bytecode.resize(sizeof(artifactHash));
            std::memcpy(
                result.bytecode.data(), &artifactHash, sizeof(artifactHash));

            if (options.targetBackend == RHIBackendType::OpenGL)
            {
                result.glslSource = "void main() {}";
            }
            else if (options.targetBackend == RHIBackendType::Metal)
            {
                result.mslSource = "kernel void main0() {}";
                result.mslEntryPoint = "main0";
            }

            if (options.sourcePath &&
                std::filesystem::path(options.sourcePath).filename() ==
                    "DefaultLit.hlsl")
            {
                PopulateDefaultLitReflection(options.stage, result.reflection);
            }
            return result;
        }

    private:
        static void HashBytes(
            uint64& hash, const void* data, size_t size)
        {
            constexpr uint64 prime = 0x100000001b3ull;
            const auto* bytes = static_cast<const uint8*>(data);
            for (size_t index = 0; index < size; ++index)
            {
                hash ^= bytes[index];
                hash *= prime;
            }
        }

        static void HashString(uint64& hash, const char* value)
        {
            if (value)
            {
                HashBytes(hash, value, std::strlen(value));
            }
        }

        static uint64 BuildArtifactHash(
            const ShaderCompileOptions& options)
        {
            uint64 hash = 0xcbf29ce484222325ull;
            HashString(hash, options.sourceCode);
            HashString(hash, options.entryPoint);
            HashString(hash, options.sourcePath);
            HashString(hash, options.targetProfile);
            HashBytes(hash, &options.stage, sizeof(options.stage));
            HashBytes(
                hash, &options.targetBackend, sizeof(options.targetBackend));
            for (const ShaderMacro& define : options.defines)
            {
                HashString(hash, define.name.c_str());
                HashString(hash, define.value.c_str());
            }
            return hash;
        }

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

        static void PopulateDefaultLitReflection(
            RHIShaderStage stage,
            ShaderReflection& reflection)
        {
            reflection.valid = true;
            if (stage == RHIShaderStage::Vertex)
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

            if (stage != RHIShaderStage::Pixel)
            {
                return;
            }

            AddResource(
                reflection, "ViewConstants", 0, 0,
                RHIBindingType::UniformBuffer);
            AddResource(
                reflection, "EnvironmentTexture", 0, 1,
                RHIBindingType::SampledTexture);
            AddResource(
                reflection, "EnvironmentSampler", 0, 2,
                RHIBindingType::Sampler);
            AddResource(
                reflection, "LightConstants", 0, 3,
                RHIBindingType::UniformBuffer);
            AddResource(
                reflection, "PointLights", 0, 4,
                RHIBindingType::ShaderResourceBuffer);
            AddResource(
                reflection, "SpotLights", 0, 5,
                RHIBindingType::ShaderResourceBuffer);
            AddResource(
                reflection, "ShadowMap", 0, 6,
                RHIBindingType::SampledTexture);
            AddResource(
                reflection, "ClusterConstants", 0, 7,
                RHIBindingType::UniformBuffer);
            AddResource(
                reflection, "Clusters", 0, 8,
                RHIBindingType::ShaderResourceBuffer);
            AddResource(
                reflection, "ClusterLightIndices", 0, 9,
                RHIBindingType::ShaderResourceBuffer);
            AddResource(
                reflection, "IrradianceTexture", 0, 10,
                RHIBindingType::SampledTexture);
            AddResource(
                reflection, "PrefilteredEnvironmentTexture", 0, 11,
                RHIBindingType::SampledTexture);
            AddResource(
                reflection, "BRDFLUTTexture", 0, 12,
                RHIBindingType::SampledTexture);
            AddResource(
                reflection, "IBLLinearClampSampler", 0, 13,
                RHIBindingType::Sampler);

            AddResource(
                reflection, "MaterialConstants", 2, 0,
                RHIBindingType::UniformBuffer);
            for (uint32 binding = 1; binding <= 5; ++binding)
            {
                AddResource(
                    reflection, "MaterialTexture", 2, binding,
                    RHIBindingType::SampledTexture);
            }
            constexpr const char* samplerNames[] = {
                "BaseColorSampler",
                "NormalSampler",
                "MetallicRoughnessSampler",
                "OcclusionSampler",
                "EmissiveSampler"};
            for (uint32 samplerIndex = 0; samplerIndex < 5; ++samplerIndex)
            {
                AddResource(
                    reflection, samplerNames[samplerIndex], 2,
                    6 + samplerIndex, RHIBindingType::Sampler);
            }
        }
    };

    inline std::unique_ptr<IShaderCompiler>
        CreateDeterministicShaderCompiler()
    {
        return std::make_unique<DeterministicShaderCompiler>();
    }
} // namespace RVX::Tests
