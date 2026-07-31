#include "SPIRVCrossTranslator.h"
#include "Core/Log.h"
#include "ShaderCompiler/GLSLBindingABI.h"

#ifdef __APPLE__
#include <spirv_cross/spirv_msl.hpp>
#endif
#include <spirv_cross/spirv_glsl.hpp>
#include <spirv_cross/spirv_cross.hpp>
#include <algorithm>
#include <cctype>

namespace RVX
{
    namespace
    {
        struct GLSLTextureBindingInfo
        {
            std::string name;
            uint32_t originalSet = 0;
            uint32_t originalBinding = 0;
            uint32_t glBinding = 0;
        };

        template <typename ResourceContainerT>
        void SortResourcesBySetBinding(spirv_cross::Compiler& compiler, ResourceContainerT& resources)
        {
            std::sort(resources.begin(), resources.end(),
                [&compiler](const auto& lhs, const auto& rhs)
                {
                    const uint32_t lhsSet = compiler.get_decoration(lhs.id, spv::DecorationDescriptorSet);
                    const uint32_t rhsSet = compiler.get_decoration(rhs.id, spv::DecorationDescriptorSet);
                    if (lhsSet != rhsSet)
                    {
                        return lhsSet < rhsSet;
                    }

                    const uint32_t lhsBinding = compiler.get_decoration(lhs.id, spv::DecorationBinding);
                    const uint32_t rhsBinding = compiler.get_decoration(rhs.id, spv::DecorationBinding);
                    if (lhsBinding != rhsBinding)
                    {
                        return lhsBinding < rhsBinding;
                    }

                    return compiler.get_name(lhs.id) < compiler.get_name(rhs.id);
                });
        }

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

        void PopulateInterfaceAttribute(
            spirv_cross::Compiler& compiler,
            const spirv_cross::Resource& resource,
            ShaderReflection::InputAttribute& attribute)
        {
            if (compiler.has_decoration(
                    resource.id,
                    spv::DecorationHlslSemanticGOOGLE))
            {
                attribute.semantic = compiler.get_decoration_string(
                    resource.id,
                    spv::DecorationHlslSemanticGOOGLE);
            }
            if (attribute.semantic.empty())
            {
                attribute.semantic = compiler.get_name(resource.id);
            }
            if (attribute.semantic.empty())
            {
                attribute.semantic =
                    compiler.get_fallback_name(resource.id);
            }

            size_t suffixBegin = attribute.semantic.size();
            while (suffixBegin > 0 &&
                   std::isdigit(static_cast<unsigned char>(
                       attribute.semantic[suffixBegin - 1])))
            {
                --suffixBegin;
            }
            if (suffixBegin < attribute.semantic.size())
            {
                uint64 parsedIndex = 0;
                for (size_t i = suffixBegin;
                     i < attribute.semantic.size();
                     ++i)
                {
                    parsedIndex =
                        parsedIndex * 10 +
                        static_cast<uint64>(
                            attribute.semantic[i] - '0');
                    if (parsedIndex > UINT32_MAX)
                    {
                        parsedIndex = 0;
                        suffixBegin = attribute.semantic.size();
                        break;
                    }
                }
                attribute.semanticIndex =
                    static_cast<uint32>(parsedIndex);
                attribute.semantic.resize(suffixBegin);
            }

            attribute.location = compiler.get_decoration(
                resource.id,
                spv::DecorationLocation);
            const auto& type = compiler.get_type(resource.type_id);
            attribute.format =
                ToRHIFormat(type.basetype, type.vecsize);
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
                PopulateInterfaceAttribute(
                    compiler,
                    input,
                    attr);
                reflection.inputs.push_back(attr);
            }

            auto stageOutputs =
                compiler.get_shader_resources().stage_outputs;
            for (const auto& output : stageOutputs)
            {
                ShaderReflection::InputAttribute attr;
                PopulateInterfaceAttribute(
                    compiler,
                    output,
                    attr);
                reflection.outputs.push_back(attr);
            }
            reflection.valid = true;
        }
    }

    ShaderReflection SPIRVCrossTranslator::ReflectSPIRV(
        const std::vector<uint8_t>& spirvBytecode,
        RHIShaderStage stage)
    {
        (void)stage;

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
            std::unordered_map<spirv_cross::VariableID, GLSLTextureBindingInfo> separateImageBindings;
            std::unordered_map<spirv_cross::VariableID, std::string> separateSamplerNames;

            SortResourcesBySetBinding(glslCompiler, resources.uniform_buffers);
            SortResourcesBySetBinding(glslCompiler, resources.storage_buffers);
            SortResourcesBySetBinding(glslCompiler, resources.sampled_images);
            SortResourcesBySetBinding(glslCompiler, resources.separate_images);
            SortResourcesBySetBinding(glslCompiler, resources.separate_samplers);
            SortResourcesBySetBinding(glslCompiler, resources.storage_images);

            // Process uniform buffers
            for (const auto& ubo : resources.uniform_buffers)
            {
                uint32_t set = glslCompiler.get_decoration(ubo.id, spv::DecorationDescriptorSet);
                uint32_t binding = glslCompiler.get_decoration(ubo.id, spv::DecorationBinding);
                std::string name = glslCompiler.get_name(ubo.id);
                if (name.empty()) name = glslCompiler.get_fallback_name(ubo.id);

                const uint32_t glBinding =
                    GLSLBindingABI::FlattenBinding(RHIBindingType::UniformBuffer, set, binding);
                glslCompiler.set_decoration(ubo.id, spv::DecorationBinding, glBinding);
                glslCompiler.unset_decoration(ubo.id, spv::DecorationDescriptorSet);

                result.bindingRemaps.push_back({
                    name, set, binding, glBinding, RHIBindingType::UniformBuffer
                });
            }

            // Process storage buffers (SSBO)
            for (const auto& ssbo : resources.storage_buffers)
            {
                uint32_t set = glslCompiler.get_decoration(ssbo.id, spv::DecorationDescriptorSet);
                uint32_t binding = glslCompiler.get_decoration(ssbo.id, spv::DecorationBinding);
                std::string name = glslCompiler.get_name(ssbo.id);
                if (name.empty()) name = glslCompiler.get_fallback_name(ssbo.id);

                const uint32_t glBinding =
                    GLSLBindingABI::FlattenBinding(RHIBindingType::StorageBuffer, set, binding);
                glslCompiler.set_decoration(ssbo.id, spv::DecorationBinding, glBinding);
                glslCompiler.unset_decoration(ssbo.id, spv::DecorationDescriptorSet);

                result.bindingRemaps.push_back({
                    name, set, binding, glBinding, RHIBindingType::StorageBuffer
                });
            }

            // Process sampled images (combined texture samplers)
            for (const auto& tex : resources.sampled_images)
            {
                uint32_t set = glslCompiler.get_decoration(tex.id, spv::DecorationDescriptorSet);
                uint32_t binding = glslCompiler.get_decoration(tex.id, spv::DecorationBinding);
                std::string name = glslCompiler.get_name(tex.id);
                if (name.empty()) name = glslCompiler.get_fallback_name(tex.id);

                const uint32_t glBinding =
                    GLSLBindingABI::FlattenBinding(RHIBindingType::CombinedTextureSampler, set, binding);
                glslCompiler.set_decoration(tex.id, spv::DecorationBinding, glBinding);
                glslCompiler.unset_decoration(tex.id, spv::DecorationDescriptorSet);

                result.bindingRemaps.push_back({
                    name, set, binding, glBinding, RHIBindingType::CombinedTextureSampler
                });
            }

            // Process separate images
            for (const auto& img : resources.separate_images)
            {
                uint32_t set = glslCompiler.get_decoration(img.id, spv::DecorationDescriptorSet);
                uint32_t binding = glslCompiler.get_decoration(img.id, spv::DecorationBinding);
                std::string name = glslCompiler.get_name(img.id);
                if (name.empty()) name = glslCompiler.get_fallback_name(img.id);

                const uint32_t glBinding =
                    GLSLBindingABI::FlattenBinding(RHIBindingType::SampledTexture, set, binding);
                glslCompiler.set_decoration(img.id, spv::DecorationBinding, glBinding);
                glslCompiler.unset_decoration(img.id, spv::DecorationDescriptorSet);

                result.bindingRemaps.push_back({
                    name, set, binding, glBinding, RHIBindingType::SampledTexture
                });
                separateImageBindings[img.id] = GLSLTextureBindingInfo{name, set, binding, glBinding};
            }

            // Process separate samplers
            for (const auto& smp : resources.separate_samplers)
            {
                uint32_t set = glslCompiler.get_decoration(smp.id, spv::DecorationDescriptorSet);
                uint32_t binding = glslCompiler.get_decoration(smp.id, spv::DecorationBinding);
                std::string name = glslCompiler.get_name(smp.id);
                if (name.empty()) name = glslCompiler.get_fallback_name(smp.id);

                const uint32_t glBinding =
                    GLSLBindingABI::FlattenBinding(RHIBindingType::Sampler, set, binding);
                glslCompiler.set_decoration(smp.id, spv::DecorationBinding, glBinding);
                glslCompiler.unset_decoration(smp.id, spv::DecorationDescriptorSet);

                result.bindingRemaps.push_back({
                    name, set, binding, glBinding, RHIBindingType::Sampler
                });
                separateSamplerNames[smp.id] = name;
            }

            if (!separateImageBindings.empty())
            {
                const spirv_cross::VariableID dummySamplerId =
                    glslCompiler.build_dummy_sampler_for_combined_images();
                if (dummySamplerId != 0)
                {
                    constexpr const char* DummySamplerName = "SPIRVCrossDummySampler";
                    glslCompiler.set_name(dummySamplerId, DummySamplerName);
                    glslCompiler.set_decoration(dummySamplerId,
                                                spv::DecorationBinding,
                                                GLSLBindingABI::RVX_GLSL_DUMMY_SAMPLER_BINDING);
                    glslCompiler.unset_decoration(dummySamplerId, spv::DecorationDescriptorSet);
                    separateSamplerNames[dummySamplerId] = DummySamplerName;
                }
            }

            if (!separateImageBindings.empty() && !separateSamplerNames.empty())
            {
                glslCompiler.build_combined_image_samplers();

                for (const auto& combined : glslCompiler.get_combined_image_samplers())
                {
                    const auto imageIt = separateImageBindings.find(combined.image_id);
                    if (imageIt == separateImageBindings.end())
                    {
                        continue;
                    }

                    const auto samplerIt = separateSamplerNames.find(combined.sampler_id);
                    const std::string samplerName = samplerIt != separateSamplerNames.end()
                                                        ? samplerIt->second
                                                        : glslCompiler.get_fallback_name(combined.sampler_id);
                    const std::string combinedName = imageIt->second.name + "_" + samplerName;

                    glslCompiler.set_name(combined.combined_id, combinedName);
                    glslCompiler.set_decoration(combined.combined_id, spv::DecorationBinding, imageIt->second.glBinding);
                    glslCompiler.unset_decoration(combined.combined_id, spv::DecorationDescriptorSet);

                    result.bindingRemaps.push_back({
                        combinedName,
                        imageIt->second.originalSet,
                        imageIt->second.originalBinding,
                        imageIt->second.glBinding,
                        RHIBindingType::CombinedTextureSampler
                    });
                }
            }

            // Process storage images
            for (const auto& img : resources.storage_images)
            {
                uint32_t set = glslCompiler.get_decoration(img.id, spv::DecorationDescriptorSet);
                uint32_t binding = glslCompiler.get_decoration(img.id, spv::DecorationBinding);
                std::string name = glslCompiler.get_name(img.id);
                if (name.empty()) name = glslCompiler.get_fallback_name(img.id);

                const uint32_t glBinding =
                    GLSLBindingABI::FlattenBinding(RHIBindingType::StorageTexture, set, binding);
                glslCompiler.set_decoration(img.id, spv::DecorationBinding, glBinding);
                glslCompiler.unset_decoration(img.id, spv::DecorationDescriptorSet);

                result.bindingRemaps.push_back({
                    name, set, binding, glBinding, RHIBindingType::StorageTexture
                });
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
                    GLSLBindingABI::RVX_GLSL_PUSH_CONSTANT_UBO_BINDING,
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
