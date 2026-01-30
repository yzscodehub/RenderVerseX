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

namespace RVX
{
    namespace
    {
#if HAS_SPIRV_REFLECT
        RHIBindingType ToBindingType(SpvReflectDescriptorType type)
        {
            switch (type)
            {
                case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                    return RHIBindingType::UniformBuffer;
                case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                    return RHIBindingType::StorageBuffer;
                case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                    return RHIBindingType::SampledTexture;
                case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                    return RHIBindingType::StorageTexture;
                case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
                    return RHIBindingType::Sampler;
                case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                    return RHIBindingType::CombinedTextureSampler;
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
            res.type = ToBindingType(binding->descriptor_type);
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
            if (input->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN)
                continue;

            ShaderReflection::InputAttribute attr{};
            attr.location = input->location;
            attr.format = ToRhiFormat(input->format);
            attr.semantic = input->name ? input->name : "";
            reflection.inputs.push_back(std::move(attr));
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
        shaderReflection->GetDesc(&shaderDesc);

        for (UINT i = 0; i < shaderDesc.BoundResources; ++i)
        {
            D3D12_SHADER_INPUT_BIND_DESC bindDesc{};
            shaderReflection->GetResourceBindingDesc(i, &bindDesc);

            ShaderReflection::ResourceBinding res{};
            res.name = bindDesc.Name ? bindDesc.Name : "";
            res.binding = bindDesc.BindPoint;
            res.set = bindDesc.Space;
            res.count = bindDesc.BindCount;

            switch (bindDesc.Type)
            {
                case D3D_SIT_CBUFFER:
                    res.type = RHIBindingType::UniformBuffer;
                    break;
                case D3D_SIT_SAMPLER:
                    res.type = RHIBindingType::Sampler;
                    break;
                case D3D_SIT_TBUFFER:
                case D3D_SIT_TEXTURE:
                    res.type = RHIBindingType::SampledTexture;
                    break;
                case D3D_SIT_UAV_RWTYPED:
                    res.type = RHIBindingType::StorageTexture;
                    break;
                case D3D_SIT_UAV_RWSTRUCTURED:
                case D3D_SIT_UAV_RWBYTEADDRESS:
                case D3D_SIT_STRUCTURED:
                case D3D_SIT_BYTEADDRESS:
                    res.type = RHIBindingType::StorageBuffer;
                    break;
                default:
                    res.type = RHIBindingType::UniformBuffer;
                    break;
            }

            reflection.resources.push_back(std::move(res));
        }

        for (UINT i = 0; i < shaderDesc.InputParameters; ++i)
        {
            D3D12_SIGNATURE_PARAMETER_DESC inputDesc{};
            shaderReflection->GetInputParameterDesc(i, &inputDesc);

            ShaderReflection::InputAttribute attr{};
            attr.semantic = inputDesc.SemanticName ? inputDesc.SemanticName : "";
            attr.location = inputDesc.Register;
            attr.format = ToRhiFormat(inputDesc);
            reflection.inputs.push_back(std::move(attr));
        }

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
        shaderReflection->GetDesc(&shaderDesc);

        for (UINT i = 0; i < shaderDesc.BoundResources; ++i)
        {
            D3D12_SHADER_INPUT_BIND_DESC bindDesc{};
            shaderReflection->GetResourceBindingDesc(i, &bindDesc);

            ShaderReflection::ResourceBinding res{};
            res.name = bindDesc.Name ? bindDesc.Name : "";
            res.binding = bindDesc.BindPoint;
            res.set = bindDesc.Space;
            res.count = bindDesc.BindCount;

            switch (bindDesc.Type)
            {
                case D3D_SIT_CBUFFER:
                    res.type = RHIBindingType::UniformBuffer;
                    break;
                case D3D_SIT_SAMPLER:
                    res.type = RHIBindingType::Sampler;
                    break;
                case D3D_SIT_TBUFFER:
                case D3D_SIT_TEXTURE:
                    res.type = RHIBindingType::SampledTexture;
                    break;
                case D3D_SIT_UAV_RWTYPED:
                    res.type = RHIBindingType::StorageTexture;
                    break;
                case D3D_SIT_UAV_RWSTRUCTURED:
                case D3D_SIT_UAV_RWBYTEADDRESS:
                case D3D_SIT_STRUCTURED:
                case D3D_SIT_BYTEADDRESS:
                    res.type = RHIBindingType::StorageBuffer;
                    break;
                default:
                    res.type = RHIBindingType::UniformBuffer;
                    break;
            }

            reflection.resources.push_back(std::move(res));
        }

        for (UINT i = 0; i < shaderDesc.InputParameters; ++i)
        {
            D3D12_SIGNATURE_PARAMETER_DESC inputDesc{};
            shaderReflection->GetInputParameterDesc(i, &inputDesc);

            ShaderReflection::InputAttribute attr{};
            attr.semantic = inputDesc.SemanticName ? inputDesc.SemanticName : "";
            attr.location = inputDesc.Register;
            attr.format = ToRhiFormat(inputDesc);
            reflection.inputs.push_back(std::move(attr));
        }

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

} // namespace RVX
