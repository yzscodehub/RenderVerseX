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
#endif
    }

#if HAS_SPIRV_REFLECT
    static ShaderReflection ReflectSpirv(const std::vector<uint8>& bytecode)
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
        reflection.valid = true;

        uint32 bindingCount = 0;
        spvReflectEnumerateDescriptorBindings(&module, &bindingCount, nullptr);
        std::vector<SpvReflectDescriptorBinding*> bindings(bindingCount);
        spvReflectEnumerateDescriptorBindings(&module, &bindingCount, bindings.data());

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
        spvReflectEnumeratePushConstantBlocks(&module, &pushConstantCount, nullptr);
        std::vector<SpvReflectBlockVariable*> blocks(pushConstantCount);
        spvReflectEnumeratePushConstantBlocks(&module, &pushConstantCount, blocks.data());
        for (auto* block : blocks)
        {
            ShaderReflection::PushConstantRange pc{};
            pc.offset = block->offset;
            pc.size = block->size;
            reflection.pushConstants.push_back(pc);
        }

        uint32 inputCount = 0;
        spvReflectEnumerateInputVariables(&module, &inputCount, nullptr);
        std::vector<SpvReflectInterfaceVariable*> inputs(inputCount);
        spvReflectEnumerateInputVariables(&module, &inputCount, inputs.data());

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
        spvReflectEnumerateOutputVariables(
            &module,
            &outputCount,
            nullptr);
        std::vector<SpvReflectInterfaceVariable*> outputs(outputCount);
        spvReflectEnumerateOutputVariables(
            &module,
            &outputCount,
            outputs.data());
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
    static ShaderReflection ReflectSpirv(const std::vector<uint8>& /* bytecode */)
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

    static ShaderReflection ReflectDxil(const std::vector<uint8>& bytecode)
    {
        ShaderReflection reflection;

        using Microsoft::WRL::ComPtr;
        ComPtr<IDxcContainerReflection> container;
        ComPtr<IDxcUtils> utils;
        ComPtr<IDxcBlobEncoding> blob;

        if (FAILED(DxcCreateInstance(CLSID_DxcContainerReflection, IID_PPV_ARGS(&container))) ||
            FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils))))
        {
            return reflection;
        }

        if (FAILED(utils->CreateBlobFromPinned(bytecode.data(),
                static_cast<uint32_t>(bytecode.size()), DXC_CP_ACP, &blob)))
        {
            return reflection;
        }

        if (FAILED(container->Load(blob.Get())))
        {
            return reflection;
        }

        UINT32 dxilIndex = 0;
        if (FAILED(container->FindFirstPartKind(DXC_PART_DXIL, &dxilIndex)))
        {
            return ReflectDxbc(bytecode);
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
                                   const std::vector<uint8>& bytecode)
    {
        (void)stage;
        if (bytecode.empty())
        {
            return {};
        }

        switch (backend)
        {
            case RHIBackendType::Vulkan:
                return ReflectSpirv(bytecode);
            case RHIBackendType::DX12:
#if defined(_WIN32)
                return ReflectDxil(bytecode);
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

    RHIShaderInterface BuildRHIShaderInterface(
        RHIShaderStage stage,
        const ShaderReflection& reflection)
    {
        RHIShaderInterface shaderInterface;
        shaderInterface.available = reflection.valid;
        shaderInterface.stage = stage;

        auto appendVariables =
            [](const std::vector<ShaderReflection::InputAttribute>& source,
               std::vector<RHIShaderInterfaceVariable>& destination)
        {
            destination.reserve(source.size());
            for (const ShaderReflection::InputAttribute& attribute : source)
            {
                RHIShaderInterfaceVariable variable;
                variable.location = attribute.location;
                variable.format = attribute.format;
                variable.systemValue = attribute.systemValue;
                variable.semanticName =
                    UpperAscii(attribute.semantic);
                variable.semanticIndex = attribute.semanticIndex;
                destination.push_back(std::move(variable));
            }
        };
        appendVariables(reflection.inputs, shaderInterface.inputs);
        appendVariables(reflection.outputs, shaderInterface.outputs);

        shaderInterface.bindings.reserve(
            reflection.resources.size());
        for (const ShaderReflection::ResourceBinding& resource :
             reflection.resources)
        {
            shaderInterface.bindings.push_back(
                {resource.set,
                 resource.binding,
                 resource.type,
                 resource.count});
        }
        shaderInterface.pushConstants.reserve(
            reflection.pushConstants.size());
        for (const ShaderReflection::PushConstantRange& range :
             reflection.pushConstants)
        {
            shaderInterface.pushConstants.push_back(
                {range.offset, range.size});
        }

        return FinalizeRHIShaderInterface(
            std::move(shaderInterface));
    }

} // namespace RVX
