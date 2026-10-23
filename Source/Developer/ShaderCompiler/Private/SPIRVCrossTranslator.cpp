#include "SPIRVCrossTranslator.h"
#include "Core/Log.h"
#include "ShaderContracts/GLSLBindingABI.h"
#include "ShaderContracts/MetalBindingABI.h"

#ifdef __APPLE__
#include <spirv_cross/spirv_msl.hpp>
#endif
#include <spirv_cross/spirv_glsl.hpp>
#include <spirv_cross/spirv_cross.hpp>
#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <unordered_set>

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

        uint32_t GetStaticDescriptorArrayCount(
            spirv_cross::Compiler& compiler,
            const spirv_cross::Resource& resource)
        {
            // Resource::type_id retains descriptor-array information whereas
            // base_type_id deliberately strips it. Reflection must preserve a
            // finite descriptor count because descriptor layout construction
            // cannot safely bind an unknown or overflowing range.
            const spirv_cross::SPIRType& type = compiler.get_type(resource.type_id);
            if (type.array.empty())
            {
                return 1u;
            }
            if (type.array.size() != type.array_size_literal.size())
            {
                throw std::runtime_error(
                    "SPIR-V resource array has incomplete size metadata");
            }

            uint64_t count = 1u;
            for (size_t dimension = 0;
                 dimension < type.array.size();
                 ++dimension)
            {
                if (!type.array_size_literal[dimension])
                {
                    throw std::runtime_error(
                        "SPIR-V resource array has a non-literal descriptor count");
                }

                const uint32_t extent = type.array[dimension];
                if (extent == 0u ||
                    count > std::numeric_limits<uint32_t>::max() / extent)
                {
                    throw std::runtime_error(
                        "SPIR-V resource array descriptor count is not representable");
                }
                count *= extent;
            }
            return static_cast<uint32_t>(count);
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
            // SPIR-V modules can retain declarations shared by several HLSL
            // entry points. Reflection describes the selected entry point, so
            // exclude globals and interfaces that it does not statically use.
            const std::unordered_set<spirv_cross::VariableID>
                activeVariables = compiler.get_active_interface_variables();
            const auto isActive = [&activeVariables](
                const spirv_cross::Resource& resource)
            {
                return activeVariables.contains(resource.id);
            };

            // Extract uniform buffers
            auto uniformBuffers = compiler.get_shader_resources().uniform_buffers;
            for (const auto& ub : uniformBuffers)
            {
                if (!isActive(ub))
                    continue;
                ShaderReflection::ResourceBinding binding;
                binding.name = compiler.get_name(ub.id);
                if (binding.name.empty())
                    binding.name = compiler.get_fallback_name(ub.id);
                binding.set = compiler.get_decoration(ub.id, spv::DecorationDescriptorSet);
                binding.binding = compiler.get_decoration(ub.id, spv::DecorationBinding);
                binding.type = RHIBindingType::UniformBuffer;
                binding.count = GetStaticDescriptorArrayCount(compiler, ub);
                reflection.resources.push_back(binding);
            }

            // Extract storage buffers
            auto storageBuffers = compiler.get_shader_resources().storage_buffers;
            for (const auto& sb : storageBuffers)
            {
                if (!isActive(sb))
                    continue;
                ShaderReflection::ResourceBinding binding;
                binding.name = compiler.get_name(sb.id);
                if (binding.name.empty())
                    binding.name = compiler.get_fallback_name(sb.id);
                binding.set = compiler.get_decoration(sb.id, spv::DecorationDescriptorSet);
                binding.binding = compiler.get_decoration(sb.id, spv::DecorationBinding);
                binding.type =
                    compiler.get_buffer_block_flags(sb.id).get(
                        spv::DecorationNonWritable)
                        ? RHIBindingType::ShaderResourceBuffer
                        : RHIBindingType::StorageBuffer;
                binding.count = GetStaticDescriptorArrayCount(compiler, sb);
                reflection.resources.push_back(binding);
            }

            // Extract sampled images (textures)
            auto sampledImages = compiler.get_shader_resources().sampled_images;
            for (const auto& si : sampledImages)
            {
                if (!isActive(si))
                    continue;
                ShaderReflection::ResourceBinding binding;
                binding.name = compiler.get_name(si.id);
                if (binding.name.empty())
                    binding.name = compiler.get_fallback_name(si.id);
                binding.set = compiler.get_decoration(si.id, spv::DecorationDescriptorSet);
                binding.binding = compiler.get_decoration(si.id, spv::DecorationBinding);
                binding.type = RHIBindingType::CombinedTextureSampler;
                binding.count = GetStaticDescriptorArrayCount(compiler, si);
                reflection.resources.push_back(binding);
            }

            // Extract separate images
            auto separateImages = compiler.get_shader_resources().separate_images;
            for (const auto& img : separateImages)
            {
                if (!isActive(img))
                    continue;
                ShaderReflection::ResourceBinding binding;
                binding.name = compiler.get_name(img.id);
                if (binding.name.empty())
                    binding.name = compiler.get_fallback_name(img.id);
                binding.set = compiler.get_decoration(img.id, spv::DecorationDescriptorSet);
                binding.binding = compiler.get_decoration(img.id, spv::DecorationBinding);
                binding.type = RHIBindingType::SampledTexture;
                binding.count = GetStaticDescriptorArrayCount(compiler, img);
                reflection.resources.push_back(binding);
            }

            // Extract separate samplers
            auto separateSamplers = compiler.get_shader_resources().separate_samplers;
            for (const auto& smp : separateSamplers)
            {
                if (!isActive(smp))
                    continue;
                ShaderReflection::ResourceBinding binding;
                binding.name = compiler.get_name(smp.id);
                if (binding.name.empty())
                    binding.name = compiler.get_fallback_name(smp.id);
                binding.set = compiler.get_decoration(smp.id, spv::DecorationDescriptorSet);
                binding.binding = compiler.get_decoration(smp.id, spv::DecorationBinding);
                binding.type = RHIBindingType::Sampler;
                binding.count = GetStaticDescriptorArrayCount(compiler, smp);
                reflection.resources.push_back(binding);
            }

            // Extract storage images (UAV textures)
            auto storageImages = compiler.get_shader_resources().storage_images;
            for (const auto& si : storageImages)
            {
                if (!isActive(si))
                    continue;
                ShaderReflection::ResourceBinding binding;
                binding.name = compiler.get_name(si.id);
                if (binding.name.empty())
                    binding.name = compiler.get_fallback_name(si.id);
                binding.set = compiler.get_decoration(si.id, spv::DecorationDescriptorSet);
                binding.binding = compiler.get_decoration(si.id, spv::DecorationBinding);
                binding.type = RHIBindingType::StorageTexture;
                binding.count = GetStaticDescriptorArrayCount(compiler, si);
                reflection.resources.push_back(binding);
            }

            // Extract push constants
            auto pushConstants = compiler.get_shader_resources().push_constant_buffers;
            for (const auto& pc : pushConstants)
            {
                if (!isActive(pc))
                    continue;
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
                if (!isActive(input))
                    continue;
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
                if (!isActive(output))
                    continue;
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

            const std::unordered_set<spirv_cross::VariableID>
                activeVariables = mslCompiler.get_active_interface_variables();
            std::unordered_set<uint32> usedBufferIndices;
            std::unordered_set<uint32> usedTextureIndices;
            std::unordered_set<uint32> usedSamplerIndices;

            auto addMSLResourceBinding =
                [&](const spirv_cross::Resource& resource,
                    RHIBindingType type) -> bool
            {
                if (!activeVariables.contains(resource.id))
                {
                    return true;
                }

                const uint32 set = mslCompiler.get_decoration(
                    resource.id, spv::DecorationDescriptorSet);
                const uint32 binding = mslCompiler.get_decoration(
                    resource.id, spv::DecorationBinding);
                uint32 metalIndex = RVX_INVALID_INDEX;
                bool mapped = false;
                std::unordered_set<uint32>* usedIndices = nullptr;

                switch (type)
                {
                    case RHIBindingType::UniformBuffer:
                    case RHIBindingType::DynamicUniformBuffer:
                    case RHIBindingType::ShaderResourceBuffer:
                    case RHIBindingType::StorageBuffer:
                    case RHIBindingType::DynamicStorageBuffer:
                        mapped = MetalBindingABI::TryGetBufferIndex(
                            set, binding, 0, metalIndex);
                        usedIndices = &usedBufferIndices;
                        break;
                    case RHIBindingType::SampledTexture:
                        mapped = MetalBindingABI::TryGetTextureIndex(
                            set, binding, 0, metalIndex);
                        usedIndices = &usedTextureIndices;
                        break;
                    case RHIBindingType::Sampler:
                        mapped = MetalBindingABI::TryGetSamplerIndex(
                            set, binding, 0, metalIndex);
                        usedIndices = &usedSamplerIndices;
                        break;
                    case RHIBindingType::StorageTexture:
                        mapped = MetalBindingABI::TryGetStorageTextureIndex(
                            set, binding, 0, metalIndex);
                        usedIndices = &usedTextureIndices;
                        break;
                    default:
                        break;
                }

                if (!mapped || !usedIndices || !usedIndices->insert(metalIndex).second)
                {
                    std::string name = mslCompiler.get_name(resource.id);
                    if (name.empty())
                    {
                        name = mslCompiler.get_fallback_name(resource.id);
                    }
                    result.errorMessage =
                        "Metal binding ABI rejects resource '" + name +
                        "' at set " + std::to_string(set) +
                        ", binding " + std::to_string(binding);
                    return false;
                }

                spirv_cross::MSLResourceBinding mslBinding;
                mslBinding.stage = executionModel;
                mslBinding.desc_set = set;
                mslBinding.binding = binding;
                switch (type)
                {
                    case RHIBindingType::UniformBuffer:
                    case RHIBindingType::DynamicUniformBuffer:
                    case RHIBindingType::ShaderResourceBuffer:
                    case RHIBindingType::StorageBuffer:
                    case RHIBindingType::DynamicStorageBuffer:
                        mslBinding.msl_buffer = metalIndex;
                        break;
                    case RHIBindingType::SampledTexture:
                    case RHIBindingType::StorageTexture:
                        mslBinding.msl_texture = metalIndex;
                        break;
                    case RHIBindingType::Sampler:
                        mslBinding.msl_sampler = metalIndex;
                        break;
                    default:
                        return false;
                }
                mslCompiler.add_msl_resource_binding(mslBinding);
                return true;
            };

            const auto addResources =
                [&](const auto& resources, RHIBindingType type) -> bool
            {
                for (const spirv_cross::Resource& resource : resources)
                {
                    if (!addMSLResourceBinding(resource, type))
                    {
                        return false;
                    }
                }
                return true;
            };

            const auto resources = mslCompiler.get_shader_resources();
            if (!addResources(resources.uniform_buffers, RHIBindingType::UniformBuffer) ||
                !addResources(resources.storage_buffers, RHIBindingType::StorageBuffer) ||
                !addResources(resources.separate_images, RHIBindingType::SampledTexture) ||
                !addResources(resources.separate_samplers, RHIBindingType::Sampler) ||
                !addResources(resources.storage_images, RHIBindingType::StorageTexture))
            {
                return result;
            }

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

            const auto flattenGLSLBinding =
                [&](RHIBindingType type,
                    uint32 set,
                    uint32 binding,
                    const std::string& name,
                    uint32& outBinding) -> bool
            {
                if (GLSLBindingABI::TryFlattenBinding(
                        type, set, binding, outBinding))
                {
                    return true;
                }

                result.errorMessage =
                    "OpenGL binding ABI rejects resource '" + name +
                    "' at set " + std::to_string(set) +
                    ", binding " + std::to_string(binding);
                RVX_CORE_ERROR("{}", result.errorMessage);
                return false;
            };

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

                uint32 glBinding = RVX_INVALID_INDEX;
                if (!flattenGLSLBinding(
                        RHIBindingType::UniformBuffer,
                        set,
                        binding,
                        name,
                        glBinding))
                {
                    return result;
                }
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

                const RHIBindingType bindingType =
                    glslCompiler.get_buffer_block_flags(ssbo.id).get(
                        spv::DecorationNonWritable)
                        ? RHIBindingType::ShaderResourceBuffer
                        : RHIBindingType::StorageBuffer;
                uint32 glBinding = RVX_INVALID_INDEX;
                if (!flattenGLSLBinding(
                        bindingType, set, binding, name, glBinding))
                {
                    return result;
                }
                glslCompiler.set_decoration(ssbo.id, spv::DecorationBinding, glBinding);
                glslCompiler.unset_decoration(ssbo.id, spv::DecorationDescriptorSet);

                result.bindingRemaps.push_back({
                    name, set, binding, glBinding, bindingType
                });
            }

            // Process sampled images (combined texture samplers)
            for (const auto& tex : resources.sampled_images)
            {
                uint32_t set = glslCompiler.get_decoration(tex.id, spv::DecorationDescriptorSet);
                uint32_t binding = glslCompiler.get_decoration(tex.id, spv::DecorationBinding);
                std::string name = glslCompiler.get_name(tex.id);
                if (name.empty()) name = glslCompiler.get_fallback_name(tex.id);

                uint32 glBinding = RVX_INVALID_INDEX;
                if (!flattenGLSLBinding(
                        RHIBindingType::CombinedTextureSampler,
                        set,
                        binding,
                        name,
                        glBinding))
                {
                    return result;
                }
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

                uint32 glBinding = RVX_INVALID_INDEX;
                if (!flattenGLSLBinding(
                        RHIBindingType::SampledTexture,
                        set,
                        binding,
                        name,
                        glBinding))
                {
                    return result;
                }
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

                uint32 glBinding = RVX_INVALID_INDEX;
                if (!flattenGLSLBinding(
                        RHIBindingType::Sampler,
                        set,
                        binding,
                        name,
                        glBinding))
                {
                    return result;
                }
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

                uint32 glBinding = RVX_INVALID_INDEX;
                if (!flattenGLSLBinding(
                        RHIBindingType::StorageTexture,
                        set,
                        binding,
                        name,
                        glBinding))
                {
                    return result;
                }
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
