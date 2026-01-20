#include "SPIRVCrossTranslator.h"
#include "Core/Log.h"

#ifdef __APPLE__
#include <spirv_cross/spirv_msl.hpp>
#endif
#include <spirv_cross/spirv_glsl.hpp>
#include <spirv_cross/spirv_cross.hpp>

namespace RVX
{
    namespace
    {
        RHIBindingType ToBindingType(spv::Op op, bool isImage)
        {
            switch (op)
            {
                case spv::OpTypeStruct:
                    return RHIBindingType::UniformBuffer;
                case spv::OpTypeImage:
                    return RHIBindingType::SampledTexture;
                case spv::OpTypeSampler:
                    return RHIBindingType::Sampler;
                case spv::OpTypeSampledImage:
                    return RHIBindingType::CombinedTextureSampler;
                default:
                    if (isImage)
                        return RHIBindingType::StorageTexture;
                    return RHIBindingType::StorageBuffer;
            }
        }

        RHIFormat ToRHIFormat(spirv_cross::SPIRType::BaseType baseType, uint32_t vecSize)
        {
            switch (baseType)
            {
                case spirv_cross::SPIRType::Float:
                    switch (vecSize)
                    {
                        case 1: return RHIFormat::R32_FLOAT;
                        case 2: return RHIFormat::RG32_FLOAT;
                        case 3: return RHIFormat::RGB32_FLOAT;
                        case 4: return RHIFormat::RGBA32_FLOAT;
                    }
                    break;
                case spirv_cross::SPIRType::Int:
                    switch (vecSize)
                    {
                        case 1: return RHIFormat::R32_SINT;
                        case 2: return RHIFormat::RG32_SINT;
                        case 3: return RHIFormat::RGB32_SINT;
                        case 4: return RHIFormat::RGBA32_SINT;
                    }
                    break;
                case spirv_cross::SPIRType::UInt:
                    switch (vecSize)
                    {
                        case 1: return RHIFormat::R32_UINT;
                        case 2: return RHIFormat::RG32_UINT;
                        case 3: return RHIFormat::RGB32_UINT;
                        case 4: return RHIFormat::RGBA32_UINT;
                    }
                    break;
                default:
                    break;
            }
            return RHIFormat::Unknown;
        }

        void ExtractReflection(spirv_cross::Compiler& compiler, ShaderReflection& reflection)
        {
            // Extract uniform buffers
            auto uniformBuffers = compiler.get_shader_resources().uniform_buffers;
            for (const auto& ub : uniformBuffers)
            {
                ShaderReflection::ResourceBinding binding;
                binding.name = compiler.get_name(ub.id);
                if (binding.name.empty())
                    binding.name = compiler.get_fallback_name(ub.id);
                binding.set = compiler.get_decoration(ub.id, spv::DecorationDescriptorSet);
                binding.binding = compiler.get_decoration(ub.id, spv::DecorationBinding);
                binding.type = RHIBindingType::UniformBuffer;
                binding.count = 1;
                reflection.resources.push_back(binding);
            }

            // Extract storage buffers
            auto storageBuffers = compiler.get_shader_resources().storage_buffers;
            for (const auto& sb : storageBuffers)
            {
                ShaderReflection::ResourceBinding binding;
                binding.name = compiler.get_name(sb.id);
                if (binding.name.empty())
                    binding.name = compiler.get_fallback_name(sb.id);
                binding.set = compiler.get_decoration(sb.id, spv::DecorationDescriptorSet);
                binding.binding = compiler.get_decoration(sb.id, spv::DecorationBinding);
                binding.type = RHIBindingType::StorageBuffer;
                binding.count = 1;
                reflection.resources.push_back(binding);
            }

            // Extract sampled images (textures)
            auto sampledImages = compiler.get_shader_resources().sampled_images;
            for (const auto& si : sampledImages)
            {
                ShaderReflection::ResourceBinding binding;
                binding.name = compiler.get_name(si.id);
                if (binding.name.empty())
                    binding.name = compiler.get_fallback_name(si.id);
                binding.set = compiler.get_decoration(si.id, spv::DecorationDescriptorSet);
                binding.binding = compiler.get_decoration(si.id, spv::DecorationBinding);
                binding.type = RHIBindingType::CombinedTextureSampler;
                binding.count = 1;
                reflection.resources.push_back(binding);
            }

            // Extract separate images
            auto separateImages = compiler.get_shader_resources().separate_images;
            for (const auto& img : separateImages)
            {
                ShaderReflection::ResourceBinding binding;
                binding.name = compiler.get_name(img.id);
                if (binding.name.empty())
                    binding.name = compiler.get_fallback_name(img.id);
                binding.set = compiler.get_decoration(img.id, spv::DecorationDescriptorSet);
                binding.binding = compiler.get_decoration(img.id, spv::DecorationBinding);
                binding.type = RHIBindingType::SampledTexture;
                binding.count = 1;
                reflection.resources.push_back(binding);
            }

            // Extract separate samplers
            auto separateSamplers = compiler.get_shader_resources().separate_samplers;
            for (const auto& smp : separateSamplers)
            {
                ShaderReflection::ResourceBinding binding;
                binding.name = compiler.get_name(smp.id);
                if (binding.name.empty())
                    binding.name = compiler.get_fallback_name(smp.id);
                binding.set = compiler.get_decoration(smp.id, spv::DecorationDescriptorSet);
                binding.binding = compiler.get_decoration(smp.id, spv::DecorationBinding);
                binding.type = RHIBindingType::Sampler;
                binding.count = 1;
                reflection.resources.push_back(binding);
            }

            // Extract storage images (UAV textures)
            auto storageImages = compiler.get_shader_resources().storage_images;
            for (const auto& si : storageImages)
            {
                ShaderReflection::ResourceBinding binding;
                binding.name = compiler.get_name(si.id);
                if (binding.name.empty())
                    binding.name = compiler.get_fallback_name(si.id);
                binding.set = compiler.get_decoration(si.id, spv::DecorationDescriptorSet);
                binding.binding = compiler.get_decoration(si.id, spv::DecorationBinding);
                binding.type = RHIBindingType::StorageTexture;
                binding.count = 1;
                reflection.resources.push_back(binding);
            }

            // Extract push constants
            auto pushConstants = compiler.get_shader_resources().push_constant_buffers;
            for (const auto& pc : pushConstants)
            {
                const auto& type = compiler.get_type(pc.base_type_id);
                ShaderReflection::PushConstantRange range;
                range.offset = 0;
                range.size = static_cast<uint32>(compiler.get_declared_struct_size(type));
                reflection.pushConstants.push_back(range);
            }

            // Extract stage inputs
            auto stageInputs = compiler.get_shader_resources().stage_inputs;
            for (const auto& input : stageInputs)
            {
                ShaderReflection::InputAttribute attr;
                attr.semantic = compiler.get_name(input.id);
                if (attr.semantic.empty())
                    attr.semantic = compiler.get_fallback_name(input.id);
                attr.location = compiler.get_decoration(input.id, spv::DecorationLocation);

                const auto& type = compiler.get_type(input.type_id);
                attr.format = ToRHIFormat(type.basetype, type.vecsize);

                reflection.inputs.push_back(attr);
            }
        }
    }

    ShaderReflection SPIRVCrossTranslator::ReflectSPIRV(
        const std::vector<uint8_t>& spirvBytecode,
        RHIShaderStage stage)
    {
        ShaderReflection reflection;

        if (spirvBytecode.empty() || spirvBytecode.size() % sizeof(uint32_t) != 0)
        {
            return reflection;
        }

        try
        {
            const uint32_t* spirvData = reinterpret_cast<const uint32_t*>(spirvBytecode.data());
            size_t spirvWordCount = spirvBytecode.size() / sizeof(uint32_t);
            std::vector<uint32_t> spirvWords(spirvData, spirvData + spirvWordCount);

            spirv_cross::Compiler compiler(std::move(spirvWords));
            ExtractReflection(compiler, reflection);
        }
        catch (const std::exception& e)
        {
            RVX_CORE_ERROR("SPIRV-Cross reflection failed: {}", e.what());
        }

        return reflection;
    }

    SPIRVToMSLResult SPIRVCrossTranslator::TranslateToMSL(
        const std::vector<uint8_t>& spirvBytecode,
        RHIShaderStage stage,
        const char* entryPoint,
        const SPIRVToMSLOptions& options)
    {
        SPIRVToMSLResult result;

#ifdef __APPLE__
        if (spirvBytecode.empty())
        {
            result.errorMessage = "Empty SPIR-V bytecode";
            return result;
        }

        // SPIR-V bytecode is uint32_t aligned
        if (spirvBytecode.size() % sizeof(uint32_t) != 0)
        {
            result.errorMessage = "Invalid SPIR-V bytecode size (not uint32 aligned)";
            return result;
        }

        try
        {
            // Create SPIR-V words from bytecode
            const uint32_t* spirvData = reinterpret_cast<const uint32_t*>(spirvBytecode.data());
            size_t spirvWordCount = spirvBytecode.size() / sizeof(uint32_t);
            std::vector<uint32_t> spirvWords(spirvData, spirvData + spirvWordCount);

            // Create MSL compiler
            spirv_cross::CompilerMSL mslCompiler(spirvWords);  // Keep a copy for reflection

            // Extract reflection BEFORE modifying the compiler
            ExtractReflection(mslCompiler, result.reflection);

            // Set MSL options
            spirv_cross::CompilerMSL::Options mslOptions;
            mslOptions.set_msl_version(options.mslVersionMajor, options.mslVersionMinor);
            mslOptions.platform = options.iOS ? 
                spirv_cross::CompilerMSL::Options::Platform::iOS :
                spirv_cross::CompilerMSL::Options::Platform::macOS;
            mslOptions.enable_point_size_builtin = options.enablePointSizeBuiltin;
            mslOptions.argument_buffers = options.useArgumentBuffers;
            
            // Use DX layout compatibility for better HLSL interop
            mslOptions.pad_fragment_output_components = true;
            
            mslCompiler.set_msl_options(mslOptions);

            // Set entry point
            spv::ExecutionModel executionModel;
            switch (stage)
            {
                case RHIShaderStage::Vertex:
                    executionModel = spv::ExecutionModelVertex;
                    break;
                case RHIShaderStage::Pixel:
                    executionModel = spv::ExecutionModelFragment;
                    break;
                case RHIShaderStage::Compute:
                    executionModel = spv::ExecutionModelGLCompute;
                    break;
                case RHIShaderStage::Hull:
                    executionModel = spv::ExecutionModelTessellationControl;
                    break;
                case RHIShaderStage::Domain:
                    executionModel = spv::ExecutionModelTessellationEvaluation;
                    break;
                default:
                    result.errorMessage = "Unsupported shader stage for Metal";
                    return result;
            }

            mslCompiler.set_entry_point(entryPoint, executionModel);

            // Compile to MSL
            result.mslSource = mslCompiler.compile();
            result.entryPointName = mslCompiler.get_cleansed_entry_point_name(entryPoint, executionModel);
            result.success = true;

            RVX_CORE_DEBUG("SPIRV-Cross: Translated {} bytes SPIR-V to MSL (entry: {} -> {})",
                spirvBytecode.size(), entryPoint, result.entryPointName);
        }
        catch (const spirv_cross::CompilerError& e)
        {
            result.errorMessage = std::string("SPIRV-Cross error: ") + e.what();
            RVX_CORE_ERROR("SPIRV-Cross translation failed: {}", e.what());
        }
        catch (const std::exception& e)
        {
            result.errorMessage = std::string("Exception during MSL translation: ") + e.what();
            RVX_CORE_ERROR("SPIRV-Cross exception: {}", e.what());
        }
#else
        // Suppress unused parameter warnings
        (void)spirvBytecode;
        (void)stage;
        (void)entryPoint;
        (void)options;
        result.errorMessage = "MSL translation is only supported on Apple platforms";
#endif

        return result;
    }

    SPIRVToGLSLResult SPIRVCrossTranslator::TranslateToGLSL(
        const std::vector<uint8_t>& spirvBytecode,
        RHIShaderStage stage,
        const char* entryPoint,
        const SPIRVToGLSLOptions& options)
    {
        SPIRVToGLSLResult result;

        if (spirvBytecode.empty())
        {
            result.errorMessage = "Empty SPIR-V bytecode";
            return result;
        }

        if (spirvBytecode.size() % sizeof(uint32_t) != 0)
        {
            result.errorMessage = "Invalid SPIR-V bytecode size (not uint32 aligned)";
            return result;
        }

        try
        {
            // Create SPIR-V words from bytecode
            const uint32_t* spirvData = reinterpret_cast<const uint32_t*>(spirvBytecode.data());
            size_t spirvWordCount = spirvBytecode.size() / sizeof(uint32_t);
            std::vector<uint32_t> spirvWords(spirvData, spirvData + spirvWordCount);

            // Create GLSL compiler
            spirv_cross::CompilerGLSL glslCompiler(spirvWords);

            // Extract reflection BEFORE modifying the compiler
            ExtractReflection(glslCompiler, result.reflection);

            // Set GLSL options
            spirv_cross::CompilerGLSL::Options glslOptions;
            glslOptions.version = options.glslVersion;
            glslOptions.es = options.es;
            glslOptions.vulkan_semantics = options.vulkanSemantics;
            glslOptions.enable_420pack_extension = options.enable420Pack;
            glslOptions.emit_push_constant_as_uniform_buffer = options.emitPushConstantAsUBO;
            glslOptions.emit_uniform_buffer_as_plain_uniforms = false;
            glslOptions.force_zero_initialized_variables = options.forceZeroInit;

            glslCompiler.set_common_options(glslOptions);

            // Set entry point
            spv::ExecutionModel executionModel;
            switch (stage)
            {
                case RHIShaderStage::Vertex:
                    executionModel = spv::ExecutionModelVertex;
                    break;
                case RHIShaderStage::Pixel:
                    executionModel = spv::ExecutionModelFragment;
                    break;
                case RHIShaderStage::Compute:
                    executionModel = spv::ExecutionModelGLCompute;
                    break;
                case RHIShaderStage::Hull:
                    executionModel = spv::ExecutionModelTessellationControl;
                    break;
                case RHIShaderStage::Domain:
                    executionModel = spv::ExecutionModelTessellationEvaluation;
                    break;
                case RHIShaderStage::Geometry:
                    executionModel = spv::ExecutionModelGeometry;
                    break;
                default:
                    result.errorMessage = "Unsupported shader stage for GLSL";
                    return result;
            }

            glslCompiler.set_entry_point(entryPoint, executionModel);

            // Remap bindings: flatten set/binding to OpenGL binding points
            // Note: UBO binding 0 is reserved for push constants (see OpenGLPipeline.h PUSH_CONSTANT_BINDING)
            auto resources = glslCompiler.get_shader_resources();
            uint32_t uboIndex = 1;  // Start from 1, 0 is reserved for push constants
            uint32_t ssboIndex = 0;
            uint32_t textureIndex = 0;
            uint32_t samplerIndex = 0;
            uint32_t imageIndex = 0;

            // Process uniform buffers
            for (const auto& ubo : resources.uniform_buffers)
            {
                uint32_t set = glslCompiler.get_decoration(ubo.id, spv::DecorationDescriptorSet);
                uint32_t binding = glslCompiler.get_decoration(ubo.id, spv::DecorationBinding);
                std::string name = glslCompiler.get_name(ubo.id);
                if (name.empty()) name = glslCompiler.get_fallback_name(ubo.id);

                // Remap to OpenGL binding
                glslCompiler.set_decoration(ubo.id, spv::DecorationBinding, uboIndex);
                glslCompiler.unset_decoration(ubo.id, spv::DecorationDescriptorSet);

                result.bindingRemaps.push_back({
                    name, set, binding, uboIndex, RHIBindingType::UniformBuffer
                });
                uboIndex++;
            }

            // Process storage buffers (SSBO)
            for (const auto& ssbo : resources.storage_buffers)
            {
                uint32_t set = glslCompiler.get_decoration(ssbo.id, spv::DecorationDescriptorSet);
                uint32_t binding = glslCompiler.get_decoration(ssbo.id, spv::DecorationBinding);
                std::string name = glslCompiler.get_name(ssbo.id);
                if (name.empty()) name = glslCompiler.get_fallback_name(ssbo.id);

                glslCompiler.set_decoration(ssbo.id, spv::DecorationBinding, ssboIndex);
                glslCompiler.unset_decoration(ssbo.id, spv::DecorationDescriptorSet);

                result.bindingRemaps.push_back({
                    name, set, binding, ssboIndex, RHIBindingType::StorageBuffer
                });
                ssboIndex++;
            }

            // Process sampled images (combined texture samplers)
            for (const auto& tex : resources.sampled_images)
            {
                uint32_t set = glslCompiler.get_decoration(tex.id, spv::DecorationDescriptorSet);
                uint32_t binding = glslCompiler.get_decoration(tex.id, spv::DecorationBinding);
                std::string name = glslCompiler.get_name(tex.id);
                if (name.empty()) name = glslCompiler.get_fallback_name(tex.id);

                glslCompiler.set_decoration(tex.id, spv::DecorationBinding, textureIndex);
                glslCompiler.unset_decoration(tex.id, spv::DecorationDescriptorSet);

                result.bindingRemaps.push_back({
                    name, set, binding, textureIndex, RHIBindingType::CombinedTextureSampler
                });
                textureIndex++;
            }

            // Process separate images
            for (const auto& img : resources.separate_images)
            {
                uint32_t set = glslCompiler.get_decoration(img.id, spv::DecorationDescriptorSet);
                uint32_t binding = glslCompiler.get_decoration(img.id, spv::DecorationBinding);
                std::string name = glslCompiler.get_name(img.id);
                if (name.empty()) name = glslCompiler.get_fallback_name(img.id);

                glslCompiler.set_decoration(img.id, spv::DecorationBinding, textureIndex);
                glslCompiler.unset_decoration(img.id, spv::DecorationDescriptorSet);

                result.bindingRemaps.push_back({
                    name, set, binding, textureIndex, RHIBindingType::SampledTexture
                });
                textureIndex++;
            }

            // Process separate samplers
            for (const auto& smp : resources.separate_samplers)
            {
                uint32_t set = glslCompiler.get_decoration(smp.id, spv::DecorationDescriptorSet);
                uint32_t binding = glslCompiler.get_decoration(smp.id, spv::DecorationBinding);
                std::string name = glslCompiler.get_name(smp.id);
                if (name.empty()) name = glslCompiler.get_fallback_name(smp.id);

                glslCompiler.set_decoration(smp.id, spv::DecorationBinding, samplerIndex);
                glslCompiler.unset_decoration(smp.id, spv::DecorationDescriptorSet);

                result.bindingRemaps.push_back({
                    name, set, binding, samplerIndex, RHIBindingType::Sampler
                });
                samplerIndex++;
            }

            // Process storage images
            for (const auto& img : resources.storage_images)
            {
                uint32_t set = glslCompiler.get_decoration(img.id, spv::DecorationDescriptorSet);
                uint32_t binding = glslCompiler.get_decoration(img.id, spv::DecorationBinding);
                std::string name = glslCompiler.get_name(img.id);
                if (name.empty()) name = glslCompiler.get_fallback_name(img.id);

                glslCompiler.set_decoration(img.id, spv::DecorationBinding, imageIndex);
                glslCompiler.unset_decoration(img.id, spv::DecorationDescriptorSet);

                result.bindingRemaps.push_back({
                    name, set, binding, imageIndex, RHIBindingType::StorageTexture
                });
                imageIndex++;
            }

            // Process push constants
            if (!resources.push_constant_buffers.empty())
            {
                const auto& pc = resources.push_constant_buffers[0];
                const auto& type = glslCompiler.get_type(pc.base_type_id);
                std::string pcName = glslCompiler.get_name(pc.id);
                if (pcName.empty()) pcName = "PushConstants";

                result.pushConstantInfo = SPIRVToGLSLResult::PushConstantInfo{
                    pcName,
                    0,  // Push constant UBO at binding 0
                    static_cast<uint32_t>(glslCompiler.get_declared_struct_size(type))
                };
            }

            // Compile to GLSL
            result.glslSource = glslCompiler.compile();
            result.success = true;

            RVX_CORE_DEBUG("SPIRV-Cross: Translated {} bytes SPIR-V to GLSL {} (entry: {})",
                spirvBytecode.size(), options.glslVersion, entryPoint);
        }
        catch (const spirv_cross::CompilerError& e)
        {
            result.errorMessage = std::string("SPIRV-Cross error: ") + e.what();
            RVX_CORE_ERROR("SPIRV-Cross GLSL translation failed: {}", e.what());
        }
        catch (const std::exception& e)
        {
            result.errorMessage = std::string("Exception during GLSL translation: ") + e.what();
            RVX_CORE_ERROR("SPIRV-Cross exception: {}", e.what());
        }

        return result;
    }

} // namespace RVX
