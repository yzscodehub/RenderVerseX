#include "ShaderCompiler/ShaderReflection.h"
#include "Core/Core.h"

#if defined(_WIN32)
#include <Windows.h>
#include <unknwn.h>
#include <oaidl.h>
#include <wrl/client.h>
#include <dxcapi.h>
#include <d3d12shader.h>
#include <d3dcompiler.h>
// Use spirv-reflect subdirectory to avoid include path issues
#include <spirv-reflect/spirv_reflect.h>
#define HAS_SPIRV_REFLECT 1
#elif defined(__linux__)
#include <spirv-reflect/spirv_reflect.h>
#define HAS_SPIRV_REFLECT 1
#else
// macOS: spirv-reflect not available via vcpkg, skip SPIR-V reflection
#define HAS_SPIRV_REFLECT 0
#endif

#include <algorithm>
#include <cctype>
#include <string_view>

namespace RVX
{
    namespace
    {
        void SetSemantic(ShaderReflection::InputAttribute& attribute,
                         const char* semantic,
                         uint32 explicitIndex = RVX_INVALID_INDEX)
        {
            attribute.semantic = semantic ? semantic : "";
            if (explicitIndex != RVX_INVALID_INDEX)
            {
                attribute.semanticIndex = explicitIndex;
                return;
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
                        return;
                    }
                }
                attribute.semanticIndex =
                    static_cast<uint32>(parsedIndex);
                attribute.semantic.resize(suffixBegin);
            }
        }

        std::string UpperAscii(std::string value)
        {
            std::transform(
                value.begin(),
                value.end(),
                value.begin(),
                [](unsigned char character)
                {
                    return static_cast<char>(std::toupper(character));
                });
            return value;
        }

        std::string NormalizeSpirvFallbackSemantic(const char* semantic)
        {
            std::string normalized = semantic ? semantic : "";
            const std::string uppercase = UpperAscii(normalized);
            constexpr char inputPrefix[] = "IN.VAR.";
            constexpr char outputPrefix[] = "OUT.VAR.";

            if (uppercase.compare(
                    0,
                    sizeof(inputPrefix) - 1,
                    inputPrefix) == 0)
            {
                return uppercase.substr(sizeof(inputPrefix) - 1);
            }
            if (uppercase.compare(
                    0,
                    sizeof(outputPrefix) - 1,
                    outputPrefix) == 0)
            {
                return uppercase.substr(sizeof(outputPrefix) - 1);
            }
            return normalized;
        }

#if HAS_SPIRV_REFLECT
        SpvReflectShaderStageFlagBits ToSpirvReflectStage(
            RHIShaderStage stage)
        {
            switch (stage)
            {
                case RHIShaderStage::Vertex:
                    return SPV_REFLECT_SHADER_STAGE_VERTEX_BIT;
                case RHIShaderStage::Pixel:
                    return SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT;
                case RHIShaderStage::Compute:
                    return SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT;
                case RHIShaderStage::Geometry:
                    return SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT;
                case RHIShaderStage::Hull:
                    return SPV_REFLECT_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
                case RHIShaderStage::Domain:
                    return SPV_REFLECT_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
                case RHIShaderStage::Mesh:
                    return SPV_REFLECT_SHADER_STAGE_MESH_BIT_EXT;
                case RHIShaderStage::Amplification:
                    return SPV_REFLECT_SHADER_STAGE_TASK_BIT_EXT;
                case RHIShaderStage::RayGeneration:
                    return SPV_REFLECT_SHADER_STAGE_RAYGEN_BIT_KHR;
                case RHIShaderStage::AnyHit:
                    return SPV_REFLECT_SHADER_STAGE_ANY_HIT_BIT_KHR;
                case RHIShaderStage::ClosestHit:
                    return SPV_REFLECT_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
                case RHIShaderStage::Miss:
                    return SPV_REFLECT_SHADER_STAGE_MISS_BIT_KHR;
                case RHIShaderStage::Intersection:
                    return SPV_REFLECT_SHADER_STAGE_INTERSECTION_BIT_KHR;
                case RHIShaderStage::Callable:
                    return SPV_REFLECT_SHADER_STAGE_CALLABLE_BIT_KHR;
                default:
                    return static_cast<SpvReflectShaderStageFlagBits>(0);
            }
        }

        RHIBindingType ToBindingType(
            SpvReflectDescriptorType type,
            SpvReflectResourceType resourceType)
        {
            switch (type)
            {
                case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                    return RHIBindingType::UniformBuffer;
                case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                {
                    const bool isSrv =
                        (resourceType & SPV_REFLECT_RESOURCE_FLAG_SRV) != 0;
                    const bool isUav =
                        (resourceType & SPV_REFLECT_RESOURCE_FLAG_UAV) != 0;
                    // Ambiguous or unknown storage access must not select an SRV layout.
                    return isSrv && !isUav
                        ? RHIBindingType::ShaderResourceBuffer
                        : RHIBindingType::StorageBuffer;
                }
                case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                    return RHIBindingType::SampledTexture;
                case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                    return RHIBindingType::StorageTexture;
                case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
                    return RHIBindingType::Sampler;
                case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                    return RHIBindingType::CombinedTextureSampler;
                case SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                    return RHIBindingType::AccelerationStructure;
                default:
                    return RHIBindingType::UniformBuffer;
            }
        }

        RHIFormat ToRhiFormat(SpvReflectFormat format)
        {
            switch (format)
            {
                case SPV_REFLECT_FORMAT_R32_SFLOAT: return RHIFormat::R32_FLOAT;
                case SPV_REFLECT_FORMAT_R32G32_SFLOAT: return RHIFormat::RG32_FLOAT;
                case SPV_REFLECT_FORMAT_R32G32B32_SFLOAT: return RHIFormat::RGB32_FLOAT;
                case SPV_REFLECT_FORMAT_R32G32B32A32_SFLOAT: return RHIFormat::RGBA32_FLOAT;
                case SPV_REFLECT_FORMAT_R32_UINT: return RHIFormat::R32_UINT;
                case SPV_REFLECT_FORMAT_R32G32_UINT: return RHIFormat::RG32_UINT;
                case SPV_REFLECT_FORMAT_R32G32B32_UINT: return RHIFormat::RGB32_UINT;
                case SPV_REFLECT_FORMAT_R32G32B32A32_UINT: return RHIFormat::RGBA32_UINT;
                case SPV_REFLECT_FORMAT_R32_SINT: return RHIFormat::R32_SINT;
                case SPV_REFLECT_FORMAT_R32G32_SINT: return RHIFormat::RG32_SINT;
                case SPV_REFLECT_FORMAT_R32G32B32_SINT: return RHIFormat::RGB32_SINT;
                case SPV_REFLECT_FORMAT_R32G32B32A32_SINT: return RHIFormat::RGBA32_SINT;
                default: return RHIFormat::Unknown;
            }
        }
#endif // HAS_SPIRV_REFLECT

#if defined(_WIN32)
        bool IsD3DBufferDimension(D3D_SRV_DIMENSION dimension)
        {
            return dimension == D3D_SRV_DIMENSION_BUFFER ||
                   dimension == D3D_SRV_DIMENSION_BUFFEREX;
        }

        bool TryGetD3DBindingType(
            const D3D12_SHADER_INPUT_BIND_DESC& desc,
            RHIBindingType& bindingType)
        {
            switch (desc.Type)
            {
                case D3D_SIT_CBUFFER:
                    bindingType = RHIBindingType::UniformBuffer;
                    return true;
                case D3D_SIT_SAMPLER:
                    bindingType = RHIBindingType::Sampler;
                    return true;
                case D3D_SIT_TBUFFER:
                case D3D_SIT_STRUCTURED:
                case D3D_SIT_BYTEADDRESS:
                    bindingType =
                        RHIBindingType::ShaderResourceBuffer;
                    return true;
                case D3D_SIT_TEXTURE:
                    bindingType = IsD3DBufferDimension(desc.Dimension)
                        ? RHIBindingType::ShaderResourceBuffer
                        : RHIBindingType::SampledTexture;
                    return true;
                case D3D_SIT_UAV_RWTYPED:
                    bindingType = IsD3DBufferDimension(desc.Dimension)
                        ? RHIBindingType::StorageBuffer
                        : RHIBindingType::StorageTexture;
                    return true;
                case D3D_SIT_UAV_RWSTRUCTURED:
                case D3D_SIT_UAV_RWBYTEADDRESS:
                case D3D_SIT_UAV_APPEND_STRUCTURED:
                case D3D_SIT_UAV_CONSUME_STRUCTURED:
                case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
                    bindingType = RHIBindingType::StorageBuffer;
                    return true;
                case D3D_SIT_RTACCELERATIONSTRUCTURE:
                    bindingType =
                        RHIBindingType::AccelerationStructure;
                    return true;
                case D3D_SIT_UAV_FEEDBACKTEXTURE:
                    bindingType = RHIBindingType::StorageTexture;
                    return true;
                default:
                    return false;
            }
        }

        RHIFormat ToRhiFormat(const D3D12_SIGNATURE_PARAMETER_DESC& desc)
        {
            uint32 comps = 0;
            if (desc.Mask & 0x1) comps++;
            if (desc.Mask & 0x2) comps++;
            if (desc.Mask & 0x4) comps++;
            if (desc.Mask & 0x8) comps++;

            if (desc.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32)
            {
                switch (comps)
                {
                    case 1: return RHIFormat::R32_FLOAT;
                    case 2: return RHIFormat::RG32_FLOAT;
                    case 3: return RHIFormat::RGB32_FLOAT;
                    case 4: return RHIFormat::RGBA32_FLOAT;
                }
            }
            else if (desc.ComponentType == D3D_REGISTER_COMPONENT_UINT32)
            {
                switch (comps)
                {
                    case 1: return RHIFormat::R32_UINT;
                    case 2: return RHIFormat::RG32_UINT;
                    case 3: return RHIFormat::RGB32_UINT;
                    case 4: return RHIFormat::RGBA32_UINT;
                }
            }
            else if (desc.ComponentType == D3D_REGISTER_COMPONENT_SINT32)
            {
                switch (comps)
                {
                    case 1: return RHIFormat::R32_SINT;
                    case 2: return RHIFormat::RG32_SINT;
                    case 3: return RHIFormat::RGB32_SINT;
                    case 4: return RHIFormat::RGBA32_SINT;
                }
            }

            return RHIFormat::Unknown;
        }

        void AppendD3DSignature(
            ID3D12ShaderReflection* shaderReflection,
            const D3D12_SHADER_DESC& shaderDesc,
            ShaderReflection& reflection)
        {
            for (UINT i = 0; i < shaderDesc.InputParameters; ++i)
            {
                D3D12_SIGNATURE_PARAMETER_DESC inputDesc{};
                if (FAILED(shaderReflection->GetInputParameterDesc(
                        i,
                        &inputDesc)))
                {
                    reflection.valid = false;
                    continue;
                }

                ShaderReflection::InputAttribute attribute{};
                SetSemantic(
                    attribute,
                    inputDesc.SemanticName,
                    inputDesc.SemanticIndex);
                attribute.location = inputDesc.Register;
                attribute.format = ToRhiFormat(inputDesc);
                attribute.systemValue =
                    inputDesc.SystemValueType != D3D_NAME_UNDEFINED;
                reflection.inputs.push_back(std::move(attribute));
            }

            for (UINT i = 0; i < shaderDesc.OutputParameters; ++i)
            {
                D3D12_SIGNATURE_PARAMETER_DESC outputDesc{};
                if (FAILED(shaderReflection->GetOutputParameterDesc(
                        i,
                        &outputDesc)))
                {
                    reflection.valid = false;
                    continue;
                }

                ShaderReflection::InputAttribute attribute{};
                SetSemantic(
                    attribute,
                    outputDesc.SemanticName,
                    outputDesc.SemanticIndex);
                attribute.location = outputDesc.Register;
                attribute.format = ToRhiFormat(outputDesc);
                attribute.systemValue =
                    outputDesc.SystemValueType != D3D_NAME_UNDEFINED;
                reflection.outputs.push_back(std::move(attribute));
            }
        }

        bool IsRayTracingStage(RHIShaderStage stage)
        {
            switch (stage)
            {
                case RHIShaderStage::RayGeneration:
                case RHIShaderStage::AnyHit:
                case RHIShaderStage::ClosestHit:
                case RHIShaderStage::Miss:
                case RHIShaderStage::Intersection:
                case RHIShaderStage::Callable:
                    return true;
                default:
                    return false;
            }
        }

        bool AppendD3DResource(
            const D3D12_SHADER_INPUT_BIND_DESC& bindDesc,
            ShaderReflection& reflection)
        {
            ShaderReflection::ResourceBinding resource{};
            resource.name = bindDesc.Name ? bindDesc.Name : "";
            resource.binding = bindDesc.BindPoint;
            resource.set = bindDesc.Space;
            resource.count = bindDesc.BindCount;
            if (!TryGetD3DBindingType(bindDesc, resource.type))
            {
                reflection.valid = false;
                RVX_CORE_WARN(
                    "D3D reflection found unsupported resource type {} for '{}'",
                    static_cast<uint32>(bindDesc.Type),
                    resource.name);
                return false;
            }
            reflection.resources.push_back(std::move(resource));
            return true;
        }

        ShaderReflection ReflectDxilLibrary(
            IDxcContainerReflection* container,
            uint32 dxilIndex,
            const char* entryPoint)
        {
            ShaderReflection reflection;
            using Microsoft::WRL::ComPtr;
            ComPtr<ID3D12LibraryReflection> libraryReflection;
            if (!container || FAILED(container->GetPartReflection(
                    dxilIndex, IID_PPV_ARGS(&libraryReflection))) ||
                !libraryReflection)
            {
                RVX_CORE_WARN("DXIL library reflection interface is unavailable");
                return reflection;
            }
            D3D12_LIBRARY_DESC libraryDesc{};
            if (FAILED(libraryReflection->GetDesc(&libraryDesc)))
            {
                RVX_CORE_WARN("DXIL library reflection descriptor is unavailable");
                return reflection;
            }

            const std::string requestedEntry = entryPoint ? entryPoint : "";
            const std::string decoratedEntry =
                "?" + requestedEntry + "@@";
            bool foundEntry = false;
            for (uint32 index = 0; index < libraryDesc.FunctionCount; ++index)
            {
                ID3D12FunctionReflection* function =
                    libraryReflection->GetFunctionByIndex(
                        static_cast<int>(index));
                if (!function)
                {
                    continue;
                }
                D3D12_FUNCTION_DESC functionDesc{};
                if (FAILED(function->GetDesc(&functionDesc)))
                {
                    continue;
                }
                const std::string functionName =
                    functionDesc.Name ? functionDesc.Name : "";
                if (!requestedEntry.empty() &&
                    functionName != requestedEntry &&
                    functionName.rfind(requestedEntry + "(", 0) != 0 &&
                    functionName.find(decoratedEntry) == std::string::npos)
                {
                    continue;
                }

                foundEntry = true;
                reflection.valid = true;
                for (uint32 resourceIndex = 0;
                     resourceIndex < functionDesc.BoundResources;
                     ++resourceIndex)
                {
                    D3D12_SHADER_INPUT_BIND_DESC bindDesc{};
                    if (FAILED(function->GetResourceBindingDesc(
                            resourceIndex, &bindDesc)))
                    {
                        reflection.valid = false;
                        RVX_CORE_WARN(
                            "DXIL library reflection failed to read resource {} for '{}'",
                            resourceIndex,
                            functionName);
                        continue;
                    }
                    AppendD3DResource(bindDesc, reflection);
                }
                if (!requestedEntry.empty())
                {
                    break;
                }
            }
            if (!foundEntry)
            {
                RVX_CORE_WARN(
                    "DXIL library reflection did not find requested entry '{}'",
                    requestedEntry);
                reflection.valid = false;
            }
            return reflection;
        }
#endif
    }

#if HAS_SPIRV_REFLECT
    static ShaderReflection ReflectSpirv(
        const std::vector<uint8>& bytecode,
        RHIShaderStage stage,
        const char* entryPoint)
    {
        ShaderReflection reflection;

        SpvReflectShaderModule module{};
        SpvReflectResult result = spvReflectCreateShaderModule(bytecode.size(),
            bytecode.data(), &module);
        if (result != SPV_REFLECT_RESULT_SUCCESS)
        {
            RVX_CORE_WARN("SPIRV reflection failed: {}", static_cast<int>(result));
            return reflection;
        }
        const SpvReflectShaderStageFlagBits expectedStage =
            ToSpirvReflectStage(stage);
        const SpvReflectEntryPoint* selectedEntry = nullptr;
        const std::string_view requestedEntry =
            entryPoint ? std::string_view(entryPoint) : std::string_view{};
        for (uint32 index = 0; index < module.entry_point_count; ++index)
        {
            const SpvReflectEntryPoint& candidate = module.entry_points[index];
            if (requestedEntry.empty() ||
                (candidate.name && requestedEntry == candidate.name))
            {
                selectedEntry = &candidate;
                break;
            }
        }
        if (selectedEntry == nullptr || expectedStage == 0 ||
            selectedEntry->shader_stage != expectedStage)
        {
            RVX_CORE_WARN(
                "SPIR-V reflection entry point '{}' is missing or has the wrong execution model",
                requestedEntry.empty() ? "<unspecified>" : requestedEntry);
            spvReflectDestroyShaderModule(&module);
            return reflection;
        }
        const char* selectedEntryName = selectedEntry->name;
        reflection.valid = true;

        uint32 bindingCount = 0;
        if (spvReflectEnumerateEntryPointDescriptorBindings(
                &module, selectedEntryName, &bindingCount, nullptr) !=
            SPV_REFLECT_RESULT_SUCCESS)
        {
            reflection.valid = false;
        }
        std::vector<SpvReflectDescriptorBinding*> bindings(bindingCount);
        if (bindingCount != 0 &&
            spvReflectEnumerateEntryPointDescriptorBindings(
                &module, selectedEntryName, &bindingCount, bindings.data()) !=
                SPV_REFLECT_RESULT_SUCCESS)
        {
            reflection.valid = false;
        }

        for (auto* binding : bindings)
        {
            ShaderReflection::ResourceBinding res{};
            res.name = binding->name ? binding->name : "";
            res.set = binding->set;
            res.binding = binding->binding;
            res.count = binding->count;
            res.type = ToBindingType(
                binding->descriptor_type,
                binding->resource_type);
            reflection.resources.push_back(std::move(res));
        }

        uint32 pushConstantCount = 0;
        if (spvReflectEnumerateEntryPointPushConstantBlocks(
                &module, selectedEntryName, &pushConstantCount, nullptr) !=
            SPV_REFLECT_RESULT_SUCCESS)
        {
            reflection.valid = false;
        }
        std::vector<SpvReflectBlockVariable*> blocks(pushConstantCount);
        if (pushConstantCount != 0 &&
            spvReflectEnumerateEntryPointPushConstantBlocks(
                &module, selectedEntryName, &pushConstantCount, blocks.data()) !=
                SPV_REFLECT_RESULT_SUCCESS)
        {
            reflection.valid = false;
        }
        for (auto* block : blocks)
        {
            ShaderReflection::PushConstantRange pc{};
            pc.offset = block->offset;
            pc.size = block->size;
            reflection.pushConstants.push_back(pc);
        }

        uint32 inputCount = 0;
        if (spvReflectEnumerateEntryPointInputVariables(
                &module, selectedEntryName, &inputCount, nullptr) !=
            SPV_REFLECT_RESULT_SUCCESS)
        {
            reflection.valid = false;
        }
        std::vector<SpvReflectInterfaceVariable*> inputs(inputCount);
        if (inputCount != 0 &&
            spvReflectEnumerateEntryPointInputVariables(
                &module, selectedEntryName, &inputCount, inputs.data()) !=
                SPV_REFLECT_RESULT_SUCCESS)
        {
            reflection.valid = false;
        }

        for (auto* input : inputs)
        {
            ShaderReflection::InputAttribute attr{};
            attr.systemValue =
                (input->decoration_flags &
                 SPV_REFLECT_DECORATION_BUILT_IN) != 0;
            attr.location =
                attr.systemValue ? RVX_INVALID_INDEX : input->location;
            attr.format = ToRhiFormat(input->format);
            const std::string semantic = NormalizeSpirvFallbackSemantic(
                input->semantic && input->semantic[0] != '\0'
                    ? input->semantic
                    : input->name);
            SetSemantic(
                attr,
                semantic.c_str());
            reflection.inputs.push_back(std::move(attr));
        }

        uint32 outputCount = 0;
        if (spvReflectEnumerateEntryPointOutputVariables(
                &module, selectedEntryName, &outputCount, nullptr) !=
            SPV_REFLECT_RESULT_SUCCESS)
        {
            reflection.valid = false;
        }
        std::vector<SpvReflectInterfaceVariable*> outputs(outputCount);
        if (outputCount != 0 &&
            spvReflectEnumerateEntryPointOutputVariables(
                &module, selectedEntryName, &outputCount, outputs.data()) !=
                SPV_REFLECT_RESULT_SUCCESS)
        {
            reflection.valid = false;
        }
        for (auto* output : outputs)
        {
            ShaderReflection::InputAttribute attr{};
            attr.systemValue =
                (output->decoration_flags &
                 SPV_REFLECT_DECORATION_BUILT_IN) != 0;
            attr.location =
                attr.systemValue ? RVX_INVALID_INDEX : output->location;
            attr.format = ToRhiFormat(output->format);
            const std::string semantic = NormalizeSpirvFallbackSemantic(
                output->semantic && output->semantic[0] != '\0'
                    ? output->semantic
                    : output->name);
            SetSemantic(
                attr,
                semantic.c_str());
            reflection.outputs.push_back(std::move(attr));
        }

        spvReflectDestroyShaderModule(&module);
        return reflection;
    }
#else
    // macOS stub - SPIR-V reflection not available
    static ShaderReflection ReflectSpirv(
        const std::vector<uint8>& /* bytecode */,
        RHIShaderStage /* stage */,
        const char* /* entryPoint */)
    {
        // On macOS, shader reflection happens at MSL compilation time via SPIRV-Cross
        return ShaderReflection{};
    }
#endif // HAS_SPIRV_REFLECT

#if defined(_WIN32)
    static ShaderReflection ReflectDxbc(const std::vector<uint8>& bytecode)
    {
        ShaderReflection reflection;
        if (bytecode.empty())
            return reflection;

        using Microsoft::WRL::ComPtr;
        ComPtr<ID3D12ShaderReflection> shaderReflection;
        if (FAILED(D3DReflect(bytecode.data(), bytecode.size(), IID_PPV_ARGS(&shaderReflection))))
        {
            return reflection;
        }

        D3D12_SHADER_DESC shaderDesc{};
        if (FAILED(shaderReflection->GetDesc(&shaderDesc)))
        {
            return reflection;
        }
        reflection.valid = true;

        for (UINT i = 0; i < shaderDesc.BoundResources; ++i)
        {
            D3D12_SHADER_INPUT_BIND_DESC bindDesc{};
            shaderReflection->GetResourceBindingDesc(i, &bindDesc);

            ShaderReflection::ResourceBinding res{};
            res.name = bindDesc.Name ? bindDesc.Name : "";
            res.binding = bindDesc.BindPoint;
            res.set = bindDesc.Space;
            res.count = bindDesc.BindCount;

            if (!TryGetD3DBindingType(bindDesc, res.type))
            {
                reflection.valid = false;
                RVX_CORE_WARN(
                    "DXBC reflection found unsupported resource type {} for '{}'",
                    static_cast<uint32>(bindDesc.Type),
                    res.name);
                continue;
            }

            reflection.resources.push_back(std::move(res));
        }

        AppendD3DSignature(
            shaderReflection.Get(),
            shaderDesc,
            reflection);

        return reflection;
    }

    static ShaderReflection ReflectDxil(const std::vector<uint8>& bytecode,
                                        RHIShaderStage stage,
                                        const char* entryPoint)
    {
        ShaderReflection reflection;

        using Microsoft::WRL::ComPtr;
        ComPtr<IDxcContainerReflection> container;
        ComPtr<IDxcUtils> utils;
        ComPtr<IDxcBlobEncoding> blob;

        if (FAILED(DxcCreateInstance(CLSID_DxcContainerReflection, IID_PPV_ARGS(&container))) ||
            FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils))))
        {
            RVX_CORE_WARN("DXIL reflection could not create DXC reflection services");
            return reflection;
        }

        if (FAILED(utils->CreateBlobFromPinned(bytecode.data(),
                static_cast<uint32_t>(bytecode.size()), DXC_CP_ACP, &blob)))
        {
            RVX_CORE_WARN("DXIL reflection could not create a bytecode blob");
            return reflection;
        }

        if (FAILED(container->Load(blob.Get())))
        {
            RVX_CORE_WARN("DXIL reflection could not load the bytecode container");
            return reflection;
        }

        UINT32 dxilIndex = 0;
        if (FAILED(container->FindFirstPartKind(DXC_PART_DXIL, &dxilIndex)))
        {
            RVX_CORE_WARN("DXIL reflection did not find a DXIL container part");
            return ReflectDxbc(bytecode);
        }

        if (IsRayTracingStage(stage))
        {
            return ReflectDxilLibrary(container.Get(), dxilIndex, entryPoint);
        }

        ComPtr<ID3D12ShaderReflection> shaderReflection;
        if (FAILED(container->GetPartReflection(dxilIndex, IID_PPV_ARGS(&shaderReflection))))
        {
            return ReflectDxbc(bytecode);
        }

        D3D12_SHADER_DESC shaderDesc{};
        if (FAILED(shaderReflection->GetDesc(&shaderDesc)))
        {
            return reflection;
        }
        reflection.valid = true;

        for (UINT i = 0; i < shaderDesc.BoundResources; ++i)
        {
            D3D12_SHADER_INPUT_BIND_DESC bindDesc{};
            shaderReflection->GetResourceBindingDesc(i, &bindDesc);

            ShaderReflection::ResourceBinding res{};
            res.name = bindDesc.Name ? bindDesc.Name : "";
            res.binding = bindDesc.BindPoint;
            res.set = bindDesc.Space;
            res.count = bindDesc.BindCount;

            if (!TryGetD3DBindingType(bindDesc, res.type))
            {
                reflection.valid = false;
                RVX_CORE_WARN(
                    "DXIL reflection found unsupported resource type {} for '{}'",
                    static_cast<uint32>(bindDesc.Type),
                    res.name);
                continue;
            }

            reflection.resources.push_back(std::move(res));
        }

        AppendD3DSignature(
            shaderReflection.Get(),
            shaderDesc,
            reflection);

        return reflection;
    }
#endif

    ShaderReflection ReflectShader(RHIBackendType backend,
                                   RHIShaderStage stage,
                                   const std::vector<uint8>& bytecode,
                                   const char* entryPoint)
    {
        if (bytecode.empty())
        {
            return {};
        }

        switch (backend)
        {
            case RHIBackendType::Vulkan:
                return ReflectSpirv(bytecode, stage, entryPoint);
            case RHIBackendType::DX12:
#if defined(_WIN32)
                return ReflectDxil(bytecode, stage, entryPoint);
#else
                return {};
#endif
            case RHIBackendType::DX11:
#if defined(_WIN32)
                return ReflectDxbc(bytecode);
#else
                return {};
#endif
            default:
                return {};
        }
    }

} // namespace RVX
