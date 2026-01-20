#include "SPIRVCrossTranslator.h"
#include "Core/Log.h"

#include <spirv_cross/spirv_msl.hpp>
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

        return result;
    }

} // namespace RVX
