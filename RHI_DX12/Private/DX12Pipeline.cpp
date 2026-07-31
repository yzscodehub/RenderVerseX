#include "DX12Pipeline.h"
#include "DX12Device.h"
#include "DX12Resources.h"
#include "RHI/RHIPipelineValidation.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include <d3d12sdklayers.h>

namespace RVX
{
    namespace
    {
        constexpr uint64 RVX_DX12_MAX_SHADER_RECORD_STRIDE = 4096;

        void DumpD3D12InfoQueue(ID3D12Device* device)
        {
            ComPtr<ID3D12InfoQueue> infoQueue;
            if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))))
            {
                return;
            }

            const UINT64 messageCount = infoQueue->GetNumStoredMessages();
            for (UINT64 i = 0; i < messageCount; ++i)
            {
                SIZE_T messageLength = 0;
                infoQueue->GetMessage(i, nullptr, &messageLength);
                if (messageLength == 0)
                {
                    continue;
                }

                std::vector<char> messageData(messageLength);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(messageData.data());
                if (SUCCEEDED(infoQueue->GetMessage(i, message, &messageLength)))
                {
                    if (message->pDescription)
                    {
                        RVX_RHI_ERROR("  D3D12: {}", message->pDescription);
                    }
                }
            }

            infoQueue->ClearStoredMessages();
        }

        bool TryAddUint64(uint64 lhs, uint64 rhs, uint64& result)
        {
            if (lhs > std::numeric_limits<uint64>::max() - rhs)
            {
                return false;
            }

            result = lhs + rhs;
            return true;
        }

        bool TryMultiplyUint64(uint64 lhs, uint64 rhs, uint64& result)
        {
            if (lhs != 0 && rhs > std::numeric_limits<uint64>::max() / lhs)
            {
                return false;
            }

            result = lhs * rhs;
            return true;
        }

        bool TryAlignUpUint64(uint64 value, uint64 alignment, uint64& result)
        {
            if (alignment == 0)
            {
                result = value;
                return true;
            }

            uint64 biasedValue = 0;
            if (!TryAddUint64(value, alignment - 1ull, biasedValue))
            {
                return false;
            }

            result = (biasedValue / alignment) * alignment;
            return true;
        }

        std::wstring ToWideString(const char* text)
        {
            if (!text || text[0] == '\0')
            {
                return L"";
            }

            const int requiredSize = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
            if (requiredSize <= 0)
            {
                return L"";
            }

            std::wstring result(static_cast<size_t>(requiredSize), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), requiredSize);
            result.pop_back();
            return result;
        }

    }
    // =============================================================================
    // Helper: Convert blend factor
    // =============================================================================
    static D3D12_BLEND ToD3D12BlendFactor(RHIBlendFactor factor)
    {
        switch (factor)
        {
            case RHIBlendFactor::Zero:             return D3D12_BLEND_ZERO;
            case RHIBlendFactor::One:              return D3D12_BLEND_ONE;
            case RHIBlendFactor::SrcColor:         return D3D12_BLEND_SRC_COLOR;
            case RHIBlendFactor::InvSrcColor:      return D3D12_BLEND_INV_SRC_COLOR;
            case RHIBlendFactor::SrcAlpha:         return D3D12_BLEND_SRC_ALPHA;
            case RHIBlendFactor::InvSrcAlpha:      return D3D12_BLEND_INV_SRC_ALPHA;
            case RHIBlendFactor::DstColor:         return D3D12_BLEND_DEST_COLOR;
            case RHIBlendFactor::InvDstColor:      return D3D12_BLEND_INV_DEST_COLOR;
            case RHIBlendFactor::DstAlpha:         return D3D12_BLEND_DEST_ALPHA;
            case RHIBlendFactor::InvDstAlpha:      return D3D12_BLEND_INV_DEST_ALPHA;
            case RHIBlendFactor::SrcAlphaSaturate: return D3D12_BLEND_SRC_ALPHA_SAT;
            case RHIBlendFactor::ConstantColor:    return D3D12_BLEND_BLEND_FACTOR;
            case RHIBlendFactor::InvConstantColor: return D3D12_BLEND_INV_BLEND_FACTOR;
            default: return D3D12_BLEND_ONE;
        }
    }

    static D3D12_BLEND_OP ToD3D12BlendOp(RHIBlendOp op)
    {
        switch (op)
        {
            case RHIBlendOp::Add:             return D3D12_BLEND_OP_ADD;
            case RHIBlendOp::Subtract:        return D3D12_BLEND_OP_SUBTRACT;
            case RHIBlendOp::ReverseSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
            case RHIBlendOp::Min:             return D3D12_BLEND_OP_MIN;
            case RHIBlendOp::Max:             return D3D12_BLEND_OP_MAX;
            default: return D3D12_BLEND_OP_ADD;
        }
    }

    static D3D12_COMPARISON_FUNC ToD3D12CompareFunc(RHICompareOp op)
    {
        switch (op)
        {
            case RHICompareOp::Never:        return D3D12_COMPARISON_FUNC_NEVER;
            case RHICompareOp::Less:         return D3D12_COMPARISON_FUNC_LESS;
            case RHICompareOp::Equal:        return D3D12_COMPARISON_FUNC_EQUAL;
            case RHICompareOp::LessEqual:    return D3D12_COMPARISON_FUNC_LESS_EQUAL;
            case RHICompareOp::Greater:      return D3D12_COMPARISON_FUNC_GREATER;
            case RHICompareOp::NotEqual:     return D3D12_COMPARISON_FUNC_NOT_EQUAL;
            case RHICompareOp::GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            case RHICompareOp::Always:       return D3D12_COMPARISON_FUNC_ALWAYS;
            default: return D3D12_COMPARISON_FUNC_LESS;
        }
    }

    static D3D12_STENCIL_OP ToD3D12StencilOp(RHIStencilOp op)
    {
        switch (op)
        {
            case RHIStencilOp::Keep:           return D3D12_STENCIL_OP_KEEP;
            case RHIStencilOp::Zero:           return D3D12_STENCIL_OP_ZERO;
            case RHIStencilOp::Replace:        return D3D12_STENCIL_OP_REPLACE;
            case RHIStencilOp::IncrementClamp: return D3D12_STENCIL_OP_INCR_SAT;
            case RHIStencilOp::DecrementClamp: return D3D12_STENCIL_OP_DECR_SAT;
            case RHIStencilOp::Invert:         return D3D12_STENCIL_OP_INVERT;
            case RHIStencilOp::IncrementWrap:  return D3D12_STENCIL_OP_INCR;
            case RHIStencilOp::DecrementWrap:  return D3D12_STENCIL_OP_DECR;
            default: return D3D12_STENCIL_OP_KEEP;
        }
    }

    static D3D12_CULL_MODE ToD3D12CullMode(RHICullMode mode)
    {
        switch (mode)
        {
            case RHICullMode::None:  return D3D12_CULL_MODE_NONE;
            case RHICullMode::Front: return D3D12_CULL_MODE_FRONT;
            case RHICullMode::Back:  return D3D12_CULL_MODE_BACK;
            default: return D3D12_CULL_MODE_BACK;
        }
    }

    static D3D12_FILL_MODE ToD3D12FillMode(RHIFillMode mode)
    {
        switch (mode)
        {
            case RHIFillMode::Solid:     return D3D12_FILL_MODE_SOLID;
            case RHIFillMode::Wireframe: return D3D12_FILL_MODE_WIREFRAME;
            default: return D3D12_FILL_MODE_SOLID;
        }
    }

    // =============================================================================
    // DX12 Descriptor Set Layout
    // =============================================================================
    DX12DescriptorSetLayout::DX12DescriptorSetLayout(DX12Device* device, const RHIDescriptorSetLayoutDesc& desc)
        : m_device(device)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        m_entries = desc.entries;

        uint32 cbvSrvUavIndex = 0;
        uint32 samplerIndex = 0;
        uint32 dynamicIndex = 0;

        for (size_t i = 0; i < m_entries.size(); ++i)
        {
            const auto& entry = m_entries[i];
            m_entryIndices[entry.binding] = static_cast<uint32>(i);

            if (entry.isDynamic)
            {
                m_dynamicBindingIndices[entry.binding] = dynamicIndex++;
            }

            switch (entry.type)
            {
                case RHIBindingType::SampledTexture:
                case RHIBindingType::ShaderResourceBuffer:
                case RHIBindingType::StorageTexture:
                case RHIBindingType::StorageBuffer:
                case RHIBindingType::DynamicStorageBuffer:
                case RHIBindingType::CombinedTextureSampler:
                case RHIBindingType::AccelerationStructure:
                    m_cbvSrvUavIndices[entry.binding] = cbvSrvUavIndex;
                    cbvSrvUavIndex += std::max(1u, entry.count);
                    break;
                case RHIBindingType::Sampler:
                    m_samplerIndices[entry.binding] = samplerIndex;
                    samplerIndex += std::max(1u, entry.count);
                    break;
                case RHIBindingType::UniformBuffer:
                case RHIBindingType::DynamicUniformBuffer:
                default:
                    break;
            }

            if (entry.type == RHIBindingType::CombinedTextureSampler)
            {
                m_samplerIndices[entry.binding] = samplerIndex;
                samplerIndex += std::max(1u, entry.count);
            }
        }

        m_cbvSrvUavCount = cbvSrvUavIndex;
        m_samplerCount = samplerIndex;
    }

    DX12DescriptorSetLayout::~DX12DescriptorSetLayout() = default;

    const RHIBindingLayoutEntry* DX12DescriptorSetLayout::FindEntry(uint32 binding) const
    {
        auto it = m_entryIndices.find(binding);
        if (it == m_entryIndices.end())
            return nullptr;
        return &m_entries[it->second];
    }

    uint32 DX12DescriptorSetLayout::GetCbvSrvUavIndex(uint32 binding) const
    {
        auto it = m_cbvSrvUavIndices.find(binding);
        return it != m_cbvSrvUavIndices.end() ? it->second : UINT32_MAX;
    }

    uint32 DX12DescriptorSetLayout::GetSamplerIndex(uint32 binding) const
    {
        auto it = m_samplerIndices.find(binding);
        return it != m_samplerIndices.end() ? it->second : UINT32_MAX;
    }

    uint32 DX12DescriptorSetLayout::GetDynamicBindingIndex(uint32 binding) const
    {
        auto it = m_dynamicBindingIndices.find(binding);
        return it != m_dynamicBindingIndices.end() ? it->second : UINT32_MAX;
    }

    // =============================================================================
    // DX12 Pipeline Layout
    // =============================================================================
    DX12PipelineLayout::DX12PipelineLayout(DX12Device* device, const RHIPipelineLayoutDesc& desc)
        : RHIPipelineLayout(desc)
        , m_device(device)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        CreateRootSignature(desc);
    }

    DX12PipelineLayout::~DX12PipelineLayout() = default;

    DX12DescriptorSetLayout* DX12PipelineLayout::GetSetLayout(uint32 setIndex) const
    {
        return setIndex < m_setLayouts.size() ? m_setLayouts[setIndex] : nullptr;
    }

    uint32 DX12PipelineLayout::GetSrvUavTableIndex(uint32 setIndex) const
    {
        auto it = m_srvUavTableIndices.find(setIndex);
        return it != m_srvUavTableIndices.end() ? it->second : UINT32_MAX;
    }

    uint32 DX12PipelineLayout::GetSamplerTableIndex(uint32 setIndex) const
    {
        auto it = m_samplerTableIndices.find(setIndex);
        return it != m_samplerTableIndices.end() ? it->second : UINT32_MAX;
    }

    void DX12PipelineLayout::CreateRootSignature(const RHIPipelineLayoutDesc& desc)
    {
        auto d3dDevice = m_device->GetD3DDevice();

        std::vector<D3D12_ROOT_PARAMETER1> rootParams;
        std::vector<std::vector<D3D12_DESCRIPTOR_RANGE1>> rangesStorage;

        m_setLayouts.clear();
        m_setLayouts.reserve(desc.setLayouts.size());

        // Process each descriptor set layout
        // Use root CBVs for uniform buffers for efficiency
        uint32 cbvRegister = 0;

        for (uint32 setIndex = 0; setIndex < desc.setLayouts.size(); ++setIndex)
        {
            auto* setLayout = static_cast<DX12DescriptorSetLayout*>(desc.setLayouts[setIndex]);
            m_setLayouts.push_back(setLayout);
            if (!setLayout) continue;

            const auto& entries = setLayout->GetEntries();
            std::vector<D3D12_DESCRIPTOR_RANGE1> srvUavRanges;
            std::vector<D3D12_DESCRIPTOR_RANGE1> samplerRanges;

            for (const auto& entry : entries)
            {
                if (entry.type == RHIBindingType::UniformBuffer ||
                    entry.type == RHIBindingType::DynamicUniformBuffer)
                {
                    // Use root CBV for uniform buffers (most efficient for per-draw updates)
                    D3D12_ROOT_PARAMETER1 param = {};
                    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                    param.Descriptor.ShaderRegister = entry.binding;
                    param.Descriptor.RegisterSpace = setIndex;
                    param.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
                    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

                    m_rootCBVIndices[{setIndex, entry.binding}] = static_cast<uint32>(rootParams.size());
                    rootParams.push_back(param);
                    cbvRegister++;
                }
                else if (entry.type == RHIBindingType::SampledTexture ||
                         entry.type == RHIBindingType::ShaderResourceBuffer ||
                         entry.type == RHIBindingType::CombinedTextureSampler ||
                         entry.type == RHIBindingType::AccelerationStructure)
                {
                    D3D12_DESCRIPTOR_RANGE1 range = {};
                    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                    range.NumDescriptors = std::max(1u, entry.count);
                    range.BaseShaderRegister = entry.binding;
                    range.RegisterSpace = setIndex;
                    range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
                    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                    srvUavRanges.push_back(range);
                }
                else if (entry.type == RHIBindingType::StorageTexture ||
                         entry.type == RHIBindingType::StorageBuffer ||
                         entry.type == RHIBindingType::DynamicStorageBuffer)
                {
                    D3D12_DESCRIPTOR_RANGE1 range = {};
                    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
                    range.NumDescriptors = std::max(1u, entry.count);
                    range.BaseShaderRegister = entry.binding;
                    range.RegisterSpace = setIndex;
                    range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
                    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                    srvUavRanges.push_back(range);
                }

                if (entry.type == RHIBindingType::Sampler ||
                    entry.type == RHIBindingType::CombinedTextureSampler)
                {
                    D3D12_DESCRIPTOR_RANGE1 range = {};
                    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                    range.NumDescriptors = std::max(1u, entry.count);
                    range.BaseShaderRegister = entry.binding;
                    range.RegisterSpace = setIndex;
                    // Samplers don't point to data, so DATA_* flags are not allowed
                    range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
                    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                    samplerRanges.push_back(range);
                }
            }

            if (!srvUavRanges.empty())
            {
                rangesStorage.push_back(std::move(srvUavRanges));
                auto& ranges = rangesStorage.back();

                D3D12_ROOT_PARAMETER1 tableParam = {};
                tableParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                tableParam.DescriptorTable.NumDescriptorRanges = static_cast<UINT>(ranges.size());
                tableParam.DescriptorTable.pDescriptorRanges = ranges.data();
                tableParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

                m_srvUavTableIndices[setIndex] = static_cast<uint32>(rootParams.size());
                rootParams.push_back(tableParam);
            }

            if (!samplerRanges.empty())
            {
                rangesStorage.push_back(std::move(samplerRanges));
                auto& ranges = rangesStorage.back();

                D3D12_ROOT_PARAMETER1 tableParam = {};
                tableParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                tableParam.DescriptorTable.NumDescriptorRanges = static_cast<UINT>(ranges.size());
                tableParam.DescriptorTable.pDescriptorRanges = ranges.data();
                tableParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

                m_samplerTableIndices[setIndex] = static_cast<uint32>(rootParams.size());
                rootParams.push_back(tableParam);
            }
        }

        // Add push constants as root constants
        if (desc.pushConstantSize > 0)
        {
            D3D12_ROOT_PARAMETER1 param = {};
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            param.Constants.ShaderRegister = cbvRegister;  // Use next available CBV register
            param.Constants.RegisterSpace = 0;
            param.Constants.Num32BitValues = (desc.pushConstantSize + 3) / 4;
            param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            m_pushConstantRootIndex = static_cast<uint32>(rootParams.size());
            rootParams.push_back(param);
        }

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc = {};
        rootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rootSigDesc.Desc_1_1.NumParameters = static_cast<UINT>(rootParams.size());
        rootSigDesc.Desc_1_1.pParameters = rootParams.data();
        rootSigDesc.Desc_1_1.NumStaticSamplers = 0;
        rootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
        rootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> error;

        HRESULT hr = D3D12SerializeVersionedRootSignature(&rootSigDesc, &signature, &error);
        if (FAILED(hr))
        {
            if (error)
            {
                RVX_RHI_ERROR("Root signature serialization failed: {}",
                    static_cast<const char*>(error->GetBufferPointer()));
            }
            return;
        }

        DX12_CHECK(d3dDevice->CreateRootSignature(
            0,
            signature->GetBufferPointer(),
            signature->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSignature)));
    }

    // =============================================================================
    // DX12 Pipeline
    // =============================================================================
    DX12Pipeline::DX12Pipeline(DX12Device* device, const RHIGraphicsPipelineDesc& desc)
        : m_device(device)
        , m_isCompute(false)
        , m_primitiveTopology(ToD3DPrimitiveTopology(desc.primitiveTopology))
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        CreateGraphicsPipeline(desc);
    }

    DX12Pipeline::DX12Pipeline(DX12Device* device, const RHIComputePipelineDesc& desc)
        : m_device(device)
        , m_isCompute(true)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        CreateComputePipeline(desc);
    }

    DX12Pipeline::DX12Pipeline(DX12Device* device, const RHIRayTracingPipelineDesc& desc)
        : m_device(device)
        , m_isRayTracing(true)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        CreateRayTracingPipeline(desc);
    }

    DX12Pipeline::~DX12Pipeline() = default;

    const void* DX12Pipeline::GetShaderIdentifier(uint32 shaderGroupIndex) const
    {
        if (!m_stateObjectProperties || shaderGroupIndex >= m_shaderGroupExports.size())
        {
            return nullptr;
        }

        return m_stateObjectProperties->GetShaderIdentifier(m_shaderGroupExports[shaderGroupIndex].c_str());
    }

    RHIShaderStage DX12Pipeline::GetRayTracingShaderGroupStage(uint32 shaderGroupIndex) const
    {
        if (shaderGroupIndex >= m_shaderGroupStages.size())
        {
            return RHIShaderStage::None;
        }

        return m_shaderGroupStages[shaderGroupIndex];
    }

    bool DX12Pipeline::IsRayTracingHitGroup(uint32 shaderGroupIndex) const
    {
        return shaderGroupIndex < m_shaderGroupIsHitGroup.size() &&
               m_shaderGroupIsHitGroup[shaderGroupIndex] != 0;
    }

    void DX12Pipeline::CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc)
    {
        auto d3dDevice = m_device->GetD3DDevice();

        // Get or create root signature
        if (desc.pipelineLayout)
        {
            m_pipelineLayout = static_cast<DX12PipelineLayout*>(desc.pipelineLayout);
            m_rootSignature = m_pipelineLayout->GetRootSignature();
        }
        else
        {
            // Create a default root signature
            RHIPipelineLayoutDesc layoutDesc;
            layoutDesc.pushConstantSize = 128;
            m_ownedLayout = CreateDX12PipelineLayout(m_device, layoutDesc);
            m_pipelineLayout = static_cast<DX12PipelineLayout*>(m_ownedLayout.Get());
            m_rootSignature = m_pipelineLayout->GetRootSignature();
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = m_rootSignature.Get();

        // Shaders
        if (desc.vertexShader)
        {
            auto* dx12Shader = static_cast<DX12Shader*>(desc.vertexShader);
            psoDesc.VS = dx12Shader->GetD3D12Bytecode();
        }
        if (desc.pixelShader)
        {
            auto* dx12Shader = static_cast<DX12Shader*>(desc.pixelShader);
            psoDesc.PS = dx12Shader->GetD3D12Bytecode();
        }
        if (desc.geometryShader)
        {
            auto* dx12Shader = static_cast<DX12Shader*>(desc.geometryShader);
            psoDesc.GS = dx12Shader->GetD3D12Bytecode();
        }
        if (desc.hullShader)
        {
            auto* dx12Shader = static_cast<DX12Shader*>(desc.hullShader);
            psoDesc.HS = dx12Shader->GetD3D12Bytecode();
        }
        if (desc.domainShader)
        {
            auto* dx12Shader = static_cast<DX12Shader*>(desc.domainShader);
            psoDesc.DS = dx12Shader->GetD3D12Bytecode();
        }

        // Input layout
        const RHIVertexInputTranslation vertexInputTranslation =
            BuildRHIVertexInputTranslation(desc.inputLayout);
        std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
        inputElements.reserve(vertexInputTranslation.attributes.size());
        for (const RHIVertexInputAttributeTranslation& attribute :
             vertexInputTranslation.attributes)
        {
            const RHIInputElement& elem =
                desc.inputLayout.elements[attribute.elementIndex];

            D3D12_INPUT_ELEMENT_DESC d3dElem = {};
            d3dElem.SemanticName = elem.semanticName;
            d3dElem.SemanticIndex = elem.semanticIndex;
            d3dElem.Format = ToDXGIFormat(elem.format);
            d3dElem.InputSlot = attribute.inputSlot;
            d3dElem.AlignedByteOffset =
                attribute.alignedByteOffset;
            d3dElem.InputSlotClass = elem.perInstance
                ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            d3dElem.InstanceDataStepRate = elem.instanceDataStepRate;

            inputElements.push_back(d3dElem);
        }

        psoDesc.InputLayout.pInputElementDescs = inputElements.data();
        psoDesc.InputLayout.NumElements = static_cast<UINT>(inputElements.size());

        // Rasterizer state
        psoDesc.RasterizerState.FillMode = ToD3D12FillMode(desc.rasterizerState.fillMode);
        psoDesc.RasterizerState.CullMode = ToD3D12CullMode(desc.rasterizerState.cullMode);
        psoDesc.RasterizerState.FrontCounterClockwise = (desc.rasterizerState.frontFace == RHIFrontFace::CounterClockwise);
        psoDesc.RasterizerState.DepthBias = static_cast<INT>(desc.rasterizerState.depthBias);
        psoDesc.RasterizerState.DepthBiasClamp = desc.rasterizerState.depthBiasClamp;
        psoDesc.RasterizerState.SlopeScaledDepthBias = desc.rasterizerState.slopeScaledDepthBias;
        psoDesc.RasterizerState.DepthClipEnable = desc.rasterizerState.depthClipEnable;
        psoDesc.RasterizerState.MultisampleEnable = desc.rasterizerState.multisampleEnable;
        psoDesc.RasterizerState.AntialiasedLineEnable = desc.rasterizerState.antialiasedLineEnable;
        psoDesc.RasterizerState.ConservativeRaster = desc.rasterizerState.conservativeRasterEnable
            ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        psoDesc.RasterizerState.ForcedSampleCount = 0;

        // Depth stencil state
        psoDesc.DepthStencilState.DepthEnable = desc.depthStencilState.depthTestEnable;
        psoDesc.DepthStencilState.DepthWriteMask = desc.depthStencilState.depthWriteEnable
            ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        psoDesc.DepthStencilState.DepthFunc = ToD3D12CompareFunc(desc.depthStencilState.depthCompareOp);
        psoDesc.DepthStencilState.StencilEnable = desc.depthStencilState.stencilTestEnable;
        psoDesc.DepthStencilState.StencilReadMask = desc.depthStencilState.stencilReadMask;
        psoDesc.DepthStencilState.StencilWriteMask = desc.depthStencilState.stencilWriteMask;

        psoDesc.DepthStencilState.FrontFace.StencilFailOp = ToD3D12StencilOp(desc.depthStencilState.frontFace.failOp);
        psoDesc.DepthStencilState.FrontFace.StencilDepthFailOp = ToD3D12StencilOp(desc.depthStencilState.frontFace.depthFailOp);
        psoDesc.DepthStencilState.FrontFace.StencilPassOp = ToD3D12StencilOp(desc.depthStencilState.frontFace.passOp);
        psoDesc.DepthStencilState.FrontFace.StencilFunc = ToD3D12CompareFunc(desc.depthStencilState.frontFace.compareOp);

        psoDesc.DepthStencilState.BackFace.StencilFailOp = ToD3D12StencilOp(desc.depthStencilState.backFace.failOp);
        psoDesc.DepthStencilState.BackFace.StencilDepthFailOp = ToD3D12StencilOp(desc.depthStencilState.backFace.depthFailOp);
        psoDesc.DepthStencilState.BackFace.StencilPassOp = ToD3D12StencilOp(desc.depthStencilState.backFace.passOp);
        psoDesc.DepthStencilState.BackFace.StencilFunc = ToD3D12CompareFunc(desc.depthStencilState.backFace.compareOp);

        // Blend state
        psoDesc.BlendState.AlphaToCoverageEnable = desc.blendState.alphaToCoverageEnable;
        psoDesc.BlendState.IndependentBlendEnable = desc.blendState.independentBlendEnable;

        for (uint32 i = 0; i < desc.numRenderTargets; ++i)
        {
            const auto& rtBlend = desc.blendState.renderTargets[i];
            auto& d3dRtBlend = psoDesc.BlendState.RenderTarget[i];

            d3dRtBlend.BlendEnable = rtBlend.blendEnable;
            d3dRtBlend.LogicOpEnable = FALSE;
            d3dRtBlend.SrcBlend = ToD3D12BlendFactor(rtBlend.srcColorBlend);
            d3dRtBlend.DestBlend = ToD3D12BlendFactor(rtBlend.dstColorBlend);
            d3dRtBlend.BlendOp = ToD3D12BlendOp(rtBlend.colorBlendOp);
            d3dRtBlend.SrcBlendAlpha = ToD3D12BlendFactor(rtBlend.srcAlphaBlend);
            d3dRtBlend.DestBlendAlpha = ToD3D12BlendFactor(rtBlend.dstAlphaBlend);
            d3dRtBlend.BlendOpAlpha = ToD3D12BlendOp(rtBlend.alphaBlendOp);
            d3dRtBlend.RenderTargetWriteMask = rtBlend.colorWriteMask;
        }

        // Render target formats
        psoDesc.NumRenderTargets = desc.numRenderTargets;
        for (uint32 i = 0; i < desc.numRenderTargets; ++i)
        {
            psoDesc.RTVFormats[i] = ToDXGIFormat(desc.renderTargetFormats[i]);
        }
        psoDesc.DSVFormat = ToDXGIFormat(desc.depthStencilFormat);

        // Sample desc
        psoDesc.SampleDesc.Count = static_cast<UINT>(desc.sampleCount);
        psoDesc.SampleDesc.Quality = 0;
        psoDesc.SampleMask = UINT_MAX;

        // Primitive topology
        psoDesc.PrimitiveTopologyType = ToD3D12PrimitiveTopologyType(desc.primitiveTopology);

        // Create PSO
        HRESULT hr = d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState));
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("Failed to create graphics pipeline state: 0x{:08X}", static_cast<uint32>(hr));
            if (desc.debugName)
            {
                RVX_RHI_ERROR("  Pipeline: {}", desc.debugName);
            }
            RVX_RHI_ERROR("  InputLayout elements: {}", static_cast<uint32>(inputElements.size()));
            RVX_RHI_ERROR("  NumRenderTargets: {} DSVFormat: {}", desc.numRenderTargets,
                static_cast<int>(desc.depthStencilFormat));
            RVX_RHI_ERROR("  RTVFormat[0]: {}", static_cast<int>(desc.renderTargetFormats[0]));
            RVX_RHI_ERROR("  RootSignature: {}", m_rootSignature ? "valid" : "null");
            DumpD3D12InfoQueue(d3dDevice);
            return;
        }

        if (desc.debugName)
        {
            wchar_t wname[256];
            MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, wname, 256);
            m_pipelineState->SetName(wname);
        }
    }

    void DX12Pipeline::CreateComputePipeline(const RHIComputePipelineDesc& desc)
    {
        auto d3dDevice = m_device->GetD3DDevice();

        // Get or create root signature
        if (desc.pipelineLayout)
        {
            m_pipelineLayout = static_cast<DX12PipelineLayout*>(desc.pipelineLayout);
            m_rootSignature = m_pipelineLayout->GetRootSignature();
        }
        else
        {
            RHIPipelineLayoutDesc layoutDesc;
            m_ownedLayout = CreateDX12PipelineLayout(m_device, layoutDesc);
            m_pipelineLayout = static_cast<DX12PipelineLayout*>(m_ownedLayout.Get());
            m_rootSignature = m_pipelineLayout->GetRootSignature();
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = m_rootSignature.Get();

        if (desc.computeShader)
        {
            auto* dx12Shader = static_cast<DX12Shader*>(desc.computeShader);
            psoDesc.CS = dx12Shader->GetD3D12Bytecode();
        }

        DX12_CHECK(d3dDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState)));

        if (desc.debugName)
        {
            wchar_t wname[256];
            MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, wname, 256);
            m_pipelineState->SetName(wname);
        }
    }

    void DX12Pipeline::CreateRayTracingPipeline(const RHIRayTracingPipelineDesc& desc)
    {
        m_shaderGroupExports.clear();
        m_shaderGroupStages.clear();
        m_shaderGroupIsHitGroup.clear();

        auto validation = ValidateRHIRayTracingPipelineDesc(desc);
        if (!validation)
        {
            RVX_RHI_ERROR("DX12 ray tracing pipeline creation failed: {}", validation.message);
            return;
        }

        const RHICapabilities& caps = m_device->GetCapabilities();
        if (!caps.supportsRaytracingPipeline)
        {
            RVX_RHI_ERROR("DX12 ray tracing pipeline creation failed: device does not support DXR pipelines");
            return;
        }

        if (caps.maxRayRecursionDepth == 0 || desc.maxRecursionDepth > caps.maxRayRecursionDepth)
        {
            RVX_RHI_ERROR(
                "DX12 ray tracing pipeline creation failed: recursion depth {} exceeds device limit {}",
                desc.maxRecursionDepth,
                caps.maxRayRecursionDepth);
            return;
        }

        ComPtr<ID3D12Device5> d3dDevice5;
        if (FAILED(m_device->GetD3DDevice()->QueryInterface(IID_PPV_ARGS(&d3dDevice5))) || !d3dDevice5)
        {
            RVX_RHI_ERROR("DX12 ray tracing pipeline creation failed: ID3D12Device5 is unavailable");
            return;
        }

        if (desc.pipelineLayout)
        {
            m_pipelineLayout = static_cast<DX12PipelineLayout*>(desc.pipelineLayout);
            m_rootSignature = m_pipelineLayout->GetRootSignature();
        }
        else
        {
            RHIPipelineLayoutDesc layoutDesc;
            m_ownedLayout = CreateDX12PipelineLayout(m_device, layoutDesc);
            m_pipelineLayout = static_cast<DX12PipelineLayout*>(m_ownedLayout.Get());
            m_rootSignature = m_pipelineLayout ? m_pipelineLayout->GetRootSignature() : nullptr;
        }

        if (!m_rootSignature)
        {
            RVX_RHI_ERROR("DX12 ray tracing pipeline creation failed: missing global root signature");
            return;
        }

        struct LibraryRecord
        {
            D3D12_DXIL_LIBRARY_DESC libraryDesc = {};
            D3D12_EXPORT_DESC exportDesc = {};
            D3D12_SHADER_BYTECODE bytecode = {};
            std::wstring exportName;
            std::wstring sourceName;
        };

        struct HitGroupRecord
        {
            D3D12_HIT_GROUP_DESC desc = {};
            std::wstring exportName;
            std::wstring closestHitImport;
            std::wstring anyHitImport;
            std::wstring intersectionImport;
        };

        std::vector<LibraryRecord> libraries;
        libraries.reserve(desc.shaderGroups.size() * 3);
        std::vector<HitGroupRecord> hitGroups;
        hitGroups.reserve(desc.shaderGroups.size());
        std::vector<std::wstring> shaderGroupExports;
        shaderGroupExports.reserve(desc.shaderGroups.size());
        std::vector<RHIShaderStage> shaderGroupStages;
        shaderGroupStages.reserve(desc.shaderGroups.size());
        std::vector<uint8> shaderGroupIsHitGroup;
        shaderGroupIsHitGroup.reserve(desc.shaderGroups.size());

        auto addShaderLibrary = [&libraries](RHIShader* shader, const std::wstring& exportName) -> bool
        {
            if (!shader || shader->GetBytecode().empty() || exportName.empty())
            {
                return false;
            }

            auto* dx12Shader = static_cast<DX12Shader*>(shader);
            if (!dx12Shader)
            {
                return false;
            }

            LibraryRecord record;
            record.bytecode = {
                dx12Shader->GetBytecode().data(),
                dx12Shader->GetBytecode().size()
            };
            record.exportName = exportName;
            record.sourceName = ToWideString(dx12Shader->GetEntryPoint());
            libraries.push_back(std::move(record));
            return true;
        };

        for (uint32 groupIndex = 0; groupIndex < static_cast<uint32>(desc.shaderGroups.size()); ++groupIndex)
        {
            const RHIRayTracingShaderGroupDesc& group = desc.shaderGroups[groupIndex];

            if (group.type == RHIRayTracingShaderGroupType::General)
            {
                const std::string defaultName =
                    MakeRHIRayTracingDefaultGeneralExportName(group.generalShader->GetStage(), groupIndex);
                const std::wstring exportName = group.exportName
                    ? ToWideString(group.exportName)
                    : ToWideString(defaultName.c_str());

                if (!addShaderLibrary(group.generalShader, exportName))
                {
                    RVX_RHI_ERROR("DX12 ray tracing pipeline creation failed: invalid general shader group {}", groupIndex);
                    return;
                }

                shaderGroupExports.push_back(exportName);
                shaderGroupStages.push_back(group.generalShader->GetStage());
                shaderGroupIsHitGroup.push_back(0);
                continue;
            }

            const std::string defaultGroupName = MakeRHIRayTracingDefaultHitGroupExportName(groupIndex);
            HitGroupRecord hitGroup;
            hitGroup.exportName = group.exportName
                ? ToWideString(group.exportName)
                : ToWideString(defaultGroupName.c_str());

            if (group.closestHitShader)
            {
                hitGroup.closestHitImport = hitGroup.exportName + L"_ClosestHit";
                if (!addShaderLibrary(group.closestHitShader, hitGroup.closestHitImport))
                {
                    RVX_RHI_ERROR("DX12 ray tracing pipeline creation failed: invalid closest-hit shader group {}", groupIndex);
                    return;
                }
            }

            if (group.anyHitShader)
            {
                hitGroup.anyHitImport = hitGroup.exportName + L"_AnyHit";
                if (!addShaderLibrary(group.anyHitShader, hitGroup.anyHitImport))
                {
                    RVX_RHI_ERROR("DX12 ray tracing pipeline creation failed: invalid any-hit shader group {}", groupIndex);
                    return;
                }
            }

            if (group.intersectionShader)
            {
                hitGroup.intersectionImport = hitGroup.exportName + L"_Intersection";
                if (!addShaderLibrary(group.intersectionShader, hitGroup.intersectionImport))
                {
                    RVX_RHI_ERROR("DX12 ray tracing pipeline creation failed: invalid intersection shader group {}", groupIndex);
                    return;
                }
            }

            hitGroup.desc.Type = (group.type == RHIRayTracingShaderGroupType::ProceduralHitGroup)
                ? D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE
                : D3D12_HIT_GROUP_TYPE_TRIANGLES;

            shaderGroupExports.push_back(hitGroup.exportName);
            shaderGroupStages.push_back(RHIShaderStage::None);
            shaderGroupIsHitGroup.push_back(1);
            hitGroups.push_back(std::move(hitGroup));
        }

        for (HitGroupRecord& hitGroup : hitGroups)
        {
            hitGroup.desc.HitGroupExport = hitGroup.exportName.c_str();
            hitGroup.desc.ClosestHitShaderImport =
                hitGroup.closestHitImport.empty() ? nullptr : hitGroup.closestHitImport.c_str();
            hitGroup.desc.AnyHitShaderImport =
                hitGroup.anyHitImport.empty() ? nullptr : hitGroup.anyHitImport.c_str();
            hitGroup.desc.IntersectionShaderImport =
                hitGroup.intersectionImport.empty() ? nullptr : hitGroup.intersectionImport.c_str();
        }

        for (LibraryRecord& library : libraries)
        {
            library.exportDesc.Name = library.exportName.c_str();
            library.exportDesc.ExportToRename = library.sourceName.empty() ? nullptr : library.sourceName.c_str();
            library.exportDesc.Flags = D3D12_EXPORT_FLAG_NONE;
            library.libraryDesc.DXILLibrary = library.bytecode;
            library.libraryDesc.NumExports = 1;
            library.libraryDesc.pExports = &library.exportDesc;
        }

        D3D12_GLOBAL_ROOT_SIGNATURE globalRootSignature = {};
        globalRootSignature.pGlobalRootSignature = m_rootSignature.Get();

        D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
        shaderConfig.MaxPayloadSizeInBytes = desc.maxPayloadSize;
        shaderConfig.MaxAttributeSizeInBytes = desc.maxAttributeSize;

        D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
        pipelineConfig.MaxTraceRecursionDepth = desc.maxRecursionDepth;

        std::vector<D3D12_STATE_SUBOBJECT> subobjects;
        subobjects.reserve(libraries.size() + hitGroups.size() + 3);

        for (LibraryRecord& library : libraries)
        {
            D3D12_STATE_SUBOBJECT subobject = {};
            subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
            subobject.pDesc = &library.libraryDesc;
            subobjects.push_back(subobject);
        }

        for (HitGroupRecord& hitGroup : hitGroups)
        {
            D3D12_STATE_SUBOBJECT subobject = {};
            subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
            subobject.pDesc = &hitGroup.desc;
            subobjects.push_back(subobject);
        }

        D3D12_STATE_SUBOBJECT rootSignatureSubobject = {};
        rootSignatureSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
        rootSignatureSubobject.pDesc = &globalRootSignature;
        subobjects.push_back(rootSignatureSubobject);

        D3D12_STATE_SUBOBJECT shaderConfigSubobject = {};
        shaderConfigSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
        shaderConfigSubobject.pDesc = &shaderConfig;
        subobjects.push_back(shaderConfigSubobject);

        D3D12_STATE_SUBOBJECT pipelineConfigSubobject = {};
        pipelineConfigSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
        pipelineConfigSubobject.pDesc = &pipelineConfig;
        subobjects.push_back(pipelineConfigSubobject);

        D3D12_STATE_OBJECT_DESC stateObjectDesc = {};
        stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
        stateObjectDesc.NumSubobjects = static_cast<UINT>(subobjects.size());
        stateObjectDesc.pSubobjects = subobjects.data();

        HRESULT hr = d3dDevice5->CreateStateObject(&stateObjectDesc, IID_PPV_ARGS(&m_stateObject));
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("Failed to create DX12 ray tracing state object: 0x{:08X}", static_cast<uint32>(hr));
            DumpD3D12InfoQueue(m_device->GetD3DDevice());
            return;
        }

        if (FAILED(m_stateObject.As(&m_stateObjectProperties)) || !m_stateObjectProperties)
        {
            RVX_RHI_ERROR("DX12 ray tracing pipeline creation failed: state object properties unavailable");
            m_stateObject.Reset();
            return;
        }

        m_shaderGroupExports = std::move(shaderGroupExports);
        m_shaderGroupStages = std::move(shaderGroupStages);
        m_shaderGroupIsHitGroup = std::move(shaderGroupIsHitGroup);

        if (desc.debugName)
        {
            wchar_t wname[256];
            MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, wname, 256);
            m_stateObject->SetName(wname);
        }
    }

    // =============================================================================
    // DX12 Shader Table
    // =============================================================================
    DX12ShaderTable::DX12ShaderTable(DX12Device* device, const RHIShaderTableDesc& desc)
        : m_device(device)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        Create(desc);
    }

    bool DX12ShaderTable::IsValid() const
    {
        auto* buffer = static_cast<DX12Buffer*>(m_buffer.Get());
        if (!m_device || !m_pipeline || !buffer || !buffer->GetResource())
        {
            return false;
        }

        const D3D12_GPU_VIRTUAL_ADDRESS baseAddress = buffer->GetGPUVirtualAddress();
        if (baseAddress == 0)
        {
            return false;
        }

        const RHICapabilities& caps = m_device->GetCapabilities();
        const uint64 recordAlignment = caps.shaderGroupHandleAlignment;
        const uint64 tableAlignment = caps.shaderTableBaseAlignment;
        if (recordAlignment == 0 || tableAlignment == 0)
        {
            return false;
        }

        const uint64 bufferSize = buffer->GetSize();
        auto sectionRangeValid = [&](const Section& section, bool required) -> bool
        {
            if (section.count == 0)
            {
                return !required && section.offset == 0 && section.size == 0 && section.stride == 0;
            }

            if (section.size == 0 || section.stride == 0 || section.stride > RVX_DX12_MAX_SHADER_RECORD_STRIDE)
            {
                return false;
            }

            if ((section.offset % tableAlignment) != 0 || (section.stride % recordAlignment) != 0)
            {
                return false;
            }

            uint64 expectedSectionSize = 0;
            if (!TryMultiplyUint64(section.stride, static_cast<uint64>(section.count), expectedSectionSize) ||
                expectedSectionSize != section.size)
            {
                return false;
            }

            uint64 sectionEnd = 0;
            if (!TryAddUint64(section.offset, section.size, sectionEnd) || sectionEnd > bufferSize)
            {
                return false;
            }

            uint64 sectionAddress = 0;
            if (!TryAddUint64(static_cast<uint64>(baseAddress), section.offset, sectionAddress) ||
                (sectionAddress % tableAlignment) != 0)
            {
                return false;
            }

            return true;
        };

        return sectionRangeValid(m_rayGenerationSection, true) &&
               m_rayGenerationSection.count == 1 &&
               sectionRangeValid(m_missSection, false) &&
               sectionRangeValid(m_hitGroupSection, false) &&
               sectionRangeValid(m_callableSection, false);
    }

    bool DX12ShaderTable::Create(const RHIShaderTableDesc& desc)
    {
        m_pipeline = nullptr;
        m_pipelineOwner.Reset();
        m_buffer.Reset();

        auto validation = ValidateRHIShaderTableDesc(desc);
        if (!validation)
        {
            RVX_RHI_ERROR("DX12 shader table creation failed: {}", validation.message);
            return false;
        }

        auto* pipeline = static_cast<DX12Pipeline*>(desc.GetRayTracingPipeline());
        if (!pipeline || !pipeline->IsRayTracing())
        {
            RVX_RHI_ERROR("DX12 shader table creation failed: invalid ray tracing pipeline");
            m_pipeline = nullptr;
            return false;
        }

        const RHICapabilities& caps = m_device->GetCapabilities();
        const uint64 handleSize = caps.shaderGroupHandleSize;
        const uint64 recordAlignment = caps.shaderGroupHandleAlignment;
        const uint64 tableAlignment = caps.shaderTableBaseAlignment;

        if (handleSize == 0 || recordAlignment == 0 || tableAlignment == 0)
        {
            RVX_RHI_ERROR("DX12 shader table creation failed: shader table alignment capabilities are missing");
            return false;
        }

        auto computeSection = [&](std::span<const RHIShaderTableRecord> records,
                                  uint64& cursor,
                                  const char* sectionName,
                                  Section& section) -> bool
        {
            section = {};
            if (records.size() > std::numeric_limits<uint32>::max())
            {
                RVX_RHI_ERROR(
                    "DX12 shader table creation failed: {} shader table record count exceeds the RHI limit",
                    sectionName);
                return false;
            }

            section.count = static_cast<uint32>(records.size());
            if (records.empty())
            {
                return true;
            }

            uint64 maxRecordSize = handleSize;
            for (const RHIShaderTableRecord& record : records)
            {
                uint64 recordSize = 0;
                if (!TryAddUint64(handleSize, static_cast<uint64>(record.localRootDataSize), recordSize))
                {
                    RVX_RHI_ERROR(
                        "DX12 shader table creation failed: {} shader record size overflowed",
                        sectionName);
                    return false;
                }
                maxRecordSize = std::max(maxRecordSize, recordSize);
            }

            if (!TryAlignUpUint64(cursor, tableAlignment, section.offset))
            {
                RVX_RHI_ERROR(
                    "DX12 shader table creation failed: {} shader table offset alignment overflowed",
                    sectionName);
                return false;
            }

            if (!TryAlignUpUint64(maxRecordSize, recordAlignment, section.stride))
            {
                RVX_RHI_ERROR(
                    "DX12 shader table creation failed: {} shader record stride alignment overflowed",
                    sectionName);
                return false;
            }

            if (section.stride > RVX_DX12_MAX_SHADER_RECORD_STRIDE)
            {
                RVX_RHI_ERROR(
                    "DX12 shader table creation failed: {} shader record stride {} exceeds DXR maximum {}",
                    sectionName,
                    section.stride,
                    static_cast<uint64>(RVX_DX12_MAX_SHADER_RECORD_STRIDE));
                return false;
            }

            if (!TryMultiplyUint64(section.stride, static_cast<uint64>(section.count), section.size))
            {
                RVX_RHI_ERROR(
                    "DX12 shader table creation failed: {} shader table section size overflowed",
                    sectionName);
                return false;
            }

            uint64 alignedSectionSize = 0;
            if (!TryAlignUpUint64(section.size, tableAlignment, alignedSectionSize) ||
                !TryAddUint64(section.offset, alignedSectionSize, cursor))
            {
                RVX_RHI_ERROR(
                    "DX12 shader table creation failed: {} shader table cursor overflowed",
                    sectionName);
                return false;
            }

            return true;
        };

        uint64 cursor = 0;
        if (!computeSection(desc.rayGenerationRecords, cursor, "ray generation", m_rayGenerationSection) ||
            !computeSection(desc.missRecords, cursor, "miss", m_missSection) ||
            !computeSection(desc.hitGroupRecords, cursor, "hit group", m_hitGroupSection) ||
            !computeSection(desc.callableRecords, cursor, "callable", m_callableSection))
        {
            return false;
        }

        uint64 tableSize = 0;
        if (!TryAlignUpUint64(cursor, tableAlignment, tableSize))
        {
            RVX_RHI_ERROR("DX12 shader table creation failed: shader table size alignment overflowed");
            return false;
        }
        if (tableSize == 0)
        {
            RVX_RHI_ERROR("DX12 shader table creation failed: shader table is empty");
            return false;
        }

        RHIBufferDesc bufferDesc;
        bufferDesc.size = tableSize;
        bufferDesc.usage = RHIBufferUsage::ShaderBindingTable | RHIBufferUsage::DeviceAddress;
        bufferDesc.memoryType = RHIMemoryType::Upload;
        bufferDesc.stride = 1;
        bufferDesc.debugName = desc.debugName ? desc.debugName : "DX12ShaderTable";
        m_buffer = m_device->CreateBuffer(bufferDesc);
        if (!m_buffer)
        {
            RVX_RHI_ERROR("DX12 shader table creation failed: buffer allocation failed");
            return false;
        }

        auto* dx12Buffer = static_cast<DX12Buffer*>(m_buffer.Get());
        if (!dx12Buffer || !dx12Buffer->GetResource() || dx12Buffer->GetGPUVirtualAddress() == 0)
        {
            RVX_RHI_ERROR("DX12 shader table creation failed: shader table buffer must be GPU-addressable");
            m_buffer.Reset();
            return false;
        }

        auto* mapped = static_cast<uint8*>(m_buffer->Map());
        if (!mapped)
        {
            RVX_RHI_ERROR("DX12 shader table creation failed: buffer mapping failed");
            m_buffer.Reset();
            return false;
        }

        std::memset(mapped, 0, static_cast<size_t>(tableSize));

        auto writeSection = [&](const Section& section, std::span<const RHIShaderTableRecord> records) -> bool
        {
            for (uint32 recordIndex = 0; recordIndex < section.count; ++recordIndex)
            {
                const RHIShaderTableRecord& record = records[recordIndex];
                if (record.shaderGroupIndex >= pipeline->GetShaderGroupCount())
                {
                    RVX_RHI_ERROR("DX12 shader table creation failed: shader group index {} is out of range",
                                  record.shaderGroupIndex);
                    return false;
                }

                const void* identifier = pipeline->GetShaderIdentifier(record.shaderGroupIndex);
                if (!identifier)
                {
                    RVX_RHI_ERROR("DX12 shader table creation failed: shader identifier {} is unavailable",
                                  record.shaderGroupIndex);
                    return false;
                }

                uint8* dst = mapped + section.offset + static_cast<uint64>(recordIndex) * section.stride;
                std::memcpy(dst, identifier, static_cast<size_t>(handleSize));
                if (record.localRootData && record.localRootDataSize > 0)
                {
                    std::memcpy(dst + handleSize, record.localRootData, record.localRootDataSize);
                }
            }

            return true;
        };

        const bool wroteAll =
            writeSection(m_rayGenerationSection, desc.rayGenerationRecords) &&
            writeSection(m_missSection, desc.missRecords) &&
            writeSection(m_hitGroupSection, desc.hitGroupRecords) &&
            writeSection(m_callableSection, desc.callableRecords);

        if (!wroteAll)
        {
            m_pipeline = nullptr;
            m_pipelineOwner.Reset();
            m_buffer.Reset();
            return false;
        }

        m_pipelineOwner = desc.rayTracingPipelineOwner;
        m_pipeline = pipeline;
        if (!IsValid())
        {
            RVX_RHI_ERROR("DX12 shader table creation failed: internal shader table validation failed");
            m_pipeline = nullptr;
            m_pipelineOwner.Reset();
            m_buffer.Reset();
            return false;
        }
        return true;
    }

    D3D12_DISPATCH_RAYS_DESC DX12ShaderTable::BuildDispatchRaysDesc(uint32 width, uint32 height, uint32 depth) const
    {
        D3D12_DISPATCH_RAYS_DESC desc = {};
        auto* buffer = static_cast<DX12Buffer*>(m_buffer.Get());
        if (!buffer)
        {
            return desc;
        }

        const D3D12_GPU_VIRTUAL_ADDRESS baseAddress = buffer->GetGPUVirtualAddress();
        if (baseAddress == 0)
        {
            return desc;
        }

        desc.RayGenerationShaderRecord.StartAddress = baseAddress + m_rayGenerationSection.offset;
        desc.RayGenerationShaderRecord.SizeInBytes = m_rayGenerationSection.size;
        desc.MissShaderTable.StartAddress = m_missSection.count > 0 ? baseAddress + m_missSection.offset : 0;
        desc.MissShaderTable.SizeInBytes = m_missSection.size;
        desc.MissShaderTable.StrideInBytes = m_missSection.stride;
        desc.HitGroupTable.StartAddress = m_hitGroupSection.count > 0 ? baseAddress + m_hitGroupSection.offset : 0;
        desc.HitGroupTable.SizeInBytes = m_hitGroupSection.size;
        desc.HitGroupTable.StrideInBytes = m_hitGroupSection.stride;
        desc.CallableShaderTable.StartAddress = m_callableSection.count > 0 ? baseAddress + m_callableSection.offset : 0;
        desc.CallableShaderTable.SizeInBytes = m_callableSection.size;
        desc.CallableShaderTable.StrideInBytes = m_callableSection.stride;
        desc.Width = width;
        desc.Height = height;
        desc.Depth = depth;
        return desc;
    }

    // =============================================================================
    // DX12 Descriptor Set
    // =============================================================================
    DX12DescriptorSet::DX12DescriptorSet(DX12Device* device, const RHIDescriptorSetDesc& desc)
        : m_device(device)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        m_layout = static_cast<DX12DescriptorSetLayout*>(desc.layout);

        if (m_layout)
        {
            m_cbvSrvUavCount = m_layout->GetCbvSrvUavCount();
            m_samplerCount = m_layout->GetSamplerCount();

            auto& heapManager = m_device->GetDescriptorHeapManager();
            if (m_cbvSrvUavCount > 0)
            {
                m_cbvSrvUavHandle = heapManager.AllocateGpuCbvSrvUavRange(m_cbvSrvUavCount);
            }
            if (m_samplerCount > 0)
            {
                m_samplerHandle = heapManager.AllocateGpuSamplerRange(m_samplerCount);
            }
        }

        if (!desc.bindings.empty())
        {
            m_isValid = Update(desc.bindings);
        }
    }

    DX12DescriptorSet::~DX12DescriptorSet()
    {
        auto& heapManager = m_device->GetDescriptorHeapManager();

        if (m_cbvSrvUavHandle.IsValid() && m_cbvSrvUavCount > 0)
        {
            heapManager.FreeGpuCbvSrvUavRange(m_cbvSrvUavHandle, m_cbvSrvUavCount);
        }

        if (m_samplerHandle.IsValid() && m_samplerCount > 0)
        {
            heapManager.FreeGpuSamplerRange(m_samplerHandle, m_samplerCount);
        }
    }

    bool DX12DescriptorSet::Update(const std::vector<RHIDescriptorBinding>& bindings)
    {
        if (!m_layout)
        {
            RVX_RHI_ERROR("DX12DescriptorSet::Update failed: descriptor set has no layout");
            return false;
        }

        auto validation = ValidateRHIDescriptorBindings(*m_layout, bindings);
        if (!validation)
        {
            RVX_RHI_ERROR("DX12DescriptorSet::Update failed: {} (binding {})",
                          validation.message,
                          validation.binding);
            return false;
        }

        m_bindings = bindings;

        // Update all bindings immediately
        for (const auto& binding : m_bindings)
        {
            if (!UpdateBindingInternal(binding))
                return false;
        }
        m_dirtyBindings.reset();
        m_hasPendingUpdates = false;
        return true;
    }

    bool DX12DescriptorSet::UpdateSingle(uint32 bindingIndex, const RHIDescriptorBinding& binding)
    {
        if (!m_layout)
        {
            RVX_RHI_ERROR("DX12DescriptorSet::UpdateSingle failed: descriptor set has no layout");
            return false;
        }

        std::vector<RHIDescriptorBinding> candidateBindings = m_bindings;

        // Update the binding in our cached list
        bool found = false;
        for (auto& existing : candidateBindings)
        {
            if (existing.binding == binding.binding &&
                existing.arrayElement == binding.arrayElement)
            {
                existing = binding;
                found = true;
                break;
            }
        }
        if (!found)
        {
            candidateBindings.push_back(binding);
        }

        auto validation = ValidateRHIDescriptorBindings(*m_layout, candidateBindings);
        if (!validation)
        {
            RVX_RHI_ERROR("DX12DescriptorSet::UpdateSingle failed: {} (binding {})",
                          validation.message,
                          validation.binding);
            return false;
        }

        m_bindings = std::move(candidateBindings);

        // Mark as dirty for deferred update, or update immediately
        if (bindingIndex < 64)
        {
            m_dirtyBindings.set(bindingIndex);
            m_hasPendingUpdates = true;
        }
        // For now, update immediately (can be deferred later)
        return UpdateBindingInternal(binding);
    }

    void DX12DescriptorSet::FlushUpdates()
    {
        if (!m_hasPendingUpdates || !m_layout)
            return;

        // Update only dirty bindings
        for (size_t i = 0; i < m_bindings.size() && i < 64; ++i)
        {
            if (m_dirtyBindings.test(i))
            {
                UpdateBindingInternal(m_bindings[i]);
            }
        }

        m_dirtyBindings.reset();
        m_hasPendingUpdates = false;
    }

    bool DX12DescriptorSet::UpdateBindingInternal(const RHIDescriptorBinding& binding)
    {
        auto d3dDevice = m_device->GetD3DDevice();
        auto& heapManager = m_device->GetDescriptorHeapManager();

        const uint32 cbvSrvUavSize = heapManager.GetCbvSrvUavDescriptorSize();
        const uint32 samplerSize = heapManager.GetSamplerDescriptorSize();

        // Handle texture views. Sampled texture ranges are SRV descriptors;
        // storage texture ranges are UAV descriptors. This matters now that a
        // RHI texture view owns only the requested native view role.
        if (binding.textureView && m_cbvSrvUavHandle.IsValid())
        {
            auto* dx12View = static_cast<DX12TextureView*>(binding.textureView);
            const RHIBindingLayoutEntry* entry = m_layout ? m_layout->FindEntry(binding.binding) : nullptr;
            const DX12DescriptorHandle* srcHandle = nullptr;
            if (entry && (entry->type == RHIBindingType::SampledTexture ||
                          entry->type == RHIBindingType::CombinedTextureSampler))
            {
                srcHandle = &dx12View->GetSRVHandle();
            }
            else if (entry && entry->type == RHIBindingType::StorageTexture)
            {
                srcHandle = &dx12View->GetUAVHandle();
            }

            if (srcHandle && srcHandle->IsValid())
            {
                uint32 dstIndex = m_layout->GetCbvSrvUavIndex(binding.binding);
                if (dstIndex != UINT32_MAX)
                {
                    D3D12_CPU_DESCRIPTOR_HANDLE dst = m_cbvSrvUavHandle.cpuHandle;
                    dst.ptr += static_cast<SIZE_T>(dstIndex + binding.arrayElement) * cbvSrvUavSize;
                    d3dDevice->CopyDescriptorsSimple(1, dst, srcHandle->cpuHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                }
            }
            else if (entry)
            {
                RVX_RHI_ERROR("DX12DescriptorSet: missing native texture descriptor for binding {} type {}",
                              binding.binding,
                              static_cast<uint32>(entry->type));
                return false;
            }
            else
            {
                RVX_RHI_ERROR("DX12DescriptorSet: texture binding {} is not declared in layout", binding.binding);
                return false;
            }
        }

        // Handle sampler - copy to Sampler heap
        if (binding.sampler && m_samplerHandle.IsValid())
        {
            auto* dx12Sampler = static_cast<DX12Sampler*>(binding.sampler);
            const DX12DescriptorHandle& srcHandle = dx12Sampler->GetHandle();
            if (srcHandle.IsValid())
            {
                uint32 dstIndex = m_layout->GetSamplerIndex(binding.binding);
                if (dstIndex != UINT32_MAX)
                {
                    D3D12_CPU_DESCRIPTOR_HANDLE dst = m_samplerHandle.cpuHandle;
                    dst.ptr += static_cast<SIZE_T>(dstIndex + binding.arrayElement) * samplerSize;
                    d3dDevice->CopyDescriptorsSimple(1, dst, srcHandle.cpuHandle, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
                }
            }
        }

        // Handle buffer (storage/uniform buffer)
        if (binding.buffer && m_cbvSrvUavHandle.IsValid())
        {
            auto* dx12Buffer = static_cast<DX12Buffer*>(binding.buffer);
            const RHIBindingLayoutEntry* entry = m_layout ? m_layout->FindEntry(binding.binding) : nullptr;
            const DX12DescriptorHandle* srcHandle = nullptr;
            if (entry && entry->type == RHIBindingType::ShaderResourceBuffer)
            {
                srcHandle = &dx12Buffer->GetSRVHandle();
            }
            else if (entry && (entry->type == RHIBindingType::StorageBuffer ||
                               entry->type == RHIBindingType::DynamicStorageBuffer))
            {
                srcHandle = &dx12Buffer->GetUAVHandle();
            }
            else
            {
                return true;
            }

            if (srcHandle && srcHandle->IsValid())
            {
                uint32 dstIndex = m_layout->GetCbvSrvUavIndex(binding.binding);
                if (dstIndex != UINT32_MAX)
                {
                    D3D12_CPU_DESCRIPTOR_HANDLE dst = m_cbvSrvUavHandle.cpuHandle;
                    dst.ptr += static_cast<SIZE_T>(dstIndex + binding.arrayElement) * cbvSrvUavSize;
                    d3dDevice->CopyDescriptorsSimple(1, dst, srcHandle->cpuHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                }
            }
            else if (entry)
            {
                RVX_RHI_ERROR("DX12DescriptorSet: missing native buffer descriptor for binding {} type {}",
                              binding.binding,
                              static_cast<uint32>(entry->type));
                return false;
            }
        }

        if (binding.accelerationStructure)
        {
            const RHIBindingLayoutEntry* entry = m_layout ? m_layout->FindEntry(binding.binding) : nullptr;
            if (!entry || entry->type != RHIBindingType::AccelerationStructure)
            {
                RVX_RHI_ERROR("DX12DescriptorSet: acceleration structure binding {} is not declared as an AS binding",
                              binding.binding);
                return false;
            }

            if (!m_cbvSrvUavHandle.IsValid())
            {
                RVX_RHI_ERROR("DX12DescriptorSet: acceleration structure binding {} has no CBV/SRV/UAV heap allocation",
                              binding.binding);
                return false;
            }

            auto* dx12AS = static_cast<DX12AccelerationStructure*>(binding.accelerationStructure);
            if (!dx12AS || dx12AS->GetGPUVirtualAddress() == 0)
            {
                RVX_RHI_ERROR("DX12DescriptorSet: invalid acceleration structure for binding {}", binding.binding);
                return false;
            }

            uint32 dstIndex = m_layout->GetCbvSrvUavIndex(binding.binding);
            if (dstIndex == UINT32_MAX)
            {
                RVX_RHI_ERROR("DX12DescriptorSet: acceleration structure binding {} has no descriptor table slot",
                              binding.binding);
                return false;
            }

            D3D12_CPU_DESCRIPTOR_HANDLE dst = m_cbvSrvUavHandle.cpuHandle;
            dst.ptr += static_cast<SIZE_T>(dstIndex + binding.arrayElement) * cbvSrvUavSize;

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.RaytracingAccelerationStructure.Location = dx12AS->GetGPUVirtualAddress();
            d3dDevice->CreateShaderResourceView(nullptr, &srvDesc, dst);
        }

        return true;
    }

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIDescriptorSetLayoutRef CreateDX12DescriptorSetLayout(DX12Device* device, const RHIDescriptorSetLayoutDesc& desc)
    {
        auto validation = ValidateRHIDescriptorSetLayoutDesc(desc);
        if (!validation)
        {
            RVX_RHI_ERROR("DX12 descriptor set layout creation failed: {} (binding {})",
                          validation.message,
                          validation.binding);
            return nullptr;
        }
        return Ref<DX12DescriptorSetLayout>(new DX12DescriptorSetLayout(device, desc));
    }

    RHIPipelineLayoutRef CreateDX12PipelineLayout(DX12Device* device, const RHIPipelineLayoutDesc& desc)
    {
        auto validation = ValidateRHIPipelineLayoutDesc(desc);
        if (!validation)
        {
            RVX_RHI_ERROR("DX12 pipeline layout creation failed: {}", validation.message);
            return nullptr;
        }
        return Ref<DX12PipelineLayout>(new DX12PipelineLayout(device, desc));
    }

    RHIPipelineRef CreateDX12GraphicsPipeline(DX12Device* device, const RHIGraphicsPipelineDesc& desc)
    {
        auto pipeline = Ref<DX12Pipeline>(new DX12Pipeline(device, desc));
        if (!pipeline->IsValid())
        {
            RVX_RHI_ERROR("DX12 graphics pipeline creation failed (invalid pipeline state)");
            return nullptr;
        }
        return pipeline;
    }

    RHIPipelineRef CreateDX12ComputePipeline(DX12Device* device, const RHIComputePipelineDesc& desc)
    {
        auto pipeline = Ref<DX12Pipeline>(new DX12Pipeline(device, desc));
        if (!pipeline->IsValid())
        {
            RVX_RHI_ERROR("DX12 compute pipeline creation failed (invalid pipeline state)");
            return nullptr;
        }
        return pipeline;
    }

    RHIPipelineRef CreateDX12RayTracingPipeline(DX12Device* device, const RHIRayTracingPipelineDesc& desc)
    {
        auto pipeline = Ref<DX12Pipeline>(new DX12Pipeline(device, desc));
        if (!pipeline->IsValid())
        {
            RVX_RHI_ERROR("DX12 ray tracing pipeline creation failed (invalid state object)");
            return nullptr;
        }
        return pipeline;
    }

    RHIShaderTableRef CreateDX12ShaderTable(DX12Device* device, const RHIShaderTableDesc& desc)
    {
        auto shaderTable = Ref<DX12ShaderTable>(new DX12ShaderTable(device, desc));
        if (!shaderTable->IsValid())
        {
            RVX_RHI_ERROR("DX12 shader table creation failed (invalid shader table)");
            return nullptr;
        }
        return shaderTable;
    }

    RHIDescriptorSetRef CreateDX12DescriptorSet(DX12Device* device, const RHIDescriptorSetDesc& desc)
    {
        auto validation = ValidateRHIDescriptorSetDesc(desc);
        if (!validation)
        {
            RVX_RHI_ERROR("DX12 descriptor set creation failed: {} (binding {})",
                          validation.message,
                          validation.binding);
            return nullptr;
        }
        auto descriptorSet = Ref<DX12DescriptorSet>(new DX12DescriptorSet(device, desc));
        if (!descriptorSet->IsValid())
        {
            RVX_RHI_ERROR("DX12 descriptor set creation failed: initial bindings could not be applied");
            return nullptr;
        }
        return descriptorSet;
    }

} // namespace RVX
