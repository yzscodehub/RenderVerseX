#include "DX12Pipeline.h"
#include "DX12Device.h"
#include "DX12Resources.h"
#include <algorithm>
#include <vector>
#include <d3d12sdklayers.h>

namespace RVX
{
    namespace
    {
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
                case RHIBindingType::StorageTexture:
                case RHIBindingType::StorageBuffer:
                case RHIBindingType::DynamicStorageBuffer:
                case RHIBindingType::CombinedTextureSampler:
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
        : m_device(device)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        CreateRootSignature(desc);
    }

    DX12PipelineLayout::~DX12PipelineLayout() = default;

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

        // Process each descriptor set layout
        // Use root CBVs for uniform buffers for efficiency
        uint32 cbvRegister = 0;

        for (uint32 setIndex = 0; setIndex < desc.setLayouts.size(); ++setIndex)
        {
            auto* setLayout = static_cast<DX12DescriptorSetLayout*>(desc.setLayouts[setIndex]);
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
                         entry.type == RHIBindingType::CombinedTextureSampler)
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

    DX12Pipeline::~DX12Pipeline() = default;

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
        std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
        uint32 offset = 0;
        for (const auto& elem : desc.inputLayout.elements)
        {
            D3D12_INPUT_ELEMENT_DESC d3dElem = {};
            d3dElem.SemanticName = elem.semanticName;
            d3dElem.SemanticIndex = elem.semanticIndex;
            d3dElem.Format = ToDXGIFormat(elem.format);
            d3dElem.InputSlot = elem.inputSlot;
            d3dElem.AlignedByteOffset = (elem.alignedByteOffset == 0xFFFFFFFF) ? offset : elem.alignedByteOffset;
            d3dElem.InputSlotClass = elem.perInstance ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            d3dElem.InstanceDataStepRate = elem.instanceDataStepRate;

            inputElements.push_back(d3dElem);
            offset = d3dElem.AlignedByteOffset + GetFormatBytesPerPixel(elem.format);
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
        m_bindings = desc.bindings;

        if (m_layout)
        {
            m_cbvSrvUavCount = m_layout->GetCbvSrvUavCount();
            m_samplerCount = m_layout->GetSamplerCount();

            auto& heapManager = m_device->GetDescriptorHeapManager();
            if (m_cbvSrvUavCount > 0)
            {
                m_cbvSrvUavHandle = heapManager.AllocateCbvSrvUavRange(m_cbvSrvUavCount);
            }
            if (m_samplerCount > 0)
            {
                m_samplerHandle = heapManager.AllocateSamplerRange(m_samplerCount);
            }
        }

        if (!m_bindings.empty())
        {
            Update(m_bindings);
        }
    }

    DX12DescriptorSet::~DX12DescriptorSet()
    {
        auto& heapManager = m_device->GetDescriptorHeapManager();

        if (m_cbvSrvUavHandle.IsValid() && m_cbvSrvUavCount > 0)
        {
            heapManager.FreeCbvSrvUavRange(m_cbvSrvUavHandle, m_cbvSrvUavCount);
        }

        if (m_samplerHandle.IsValid() && m_samplerCount > 0)
        {
            heapManager.FreeSamplerRange(m_samplerHandle, m_samplerCount);
        }
    }

    void DX12DescriptorSet::Update(const std::vector<RHIDescriptorBinding>& bindings)
    {
        m_bindings = bindings;
        if (!m_layout)
        {
            return;
        }

        auto d3dDevice = m_device->GetD3DDevice();
        auto& heapManager = m_device->GetDescriptorHeapManager();

        const uint32 cbvSrvUavSize = heapManager.GetCbvSrvUavDescriptorSize();
        const uint32 samplerSize = heapManager.GetSamplerDescriptorSize();

        // Process bindings based on what resources are actually set,
        // rather than relying on layout entry type (which may conflict when
        // texture and sampler share the same binding number in different register spaces)
        for (const auto& binding : m_bindings)
        {
            // Handle texture view (SRV) - copy to CBV_SRV_UAV heap
            if (binding.textureView && m_cbvSrvUavHandle.IsValid())
            {
                auto* dx12View = static_cast<DX12TextureView*>(binding.textureView);
                const DX12DescriptorHandle& srcHandle = dx12View->GetSRVHandle();
                if (srcHandle.IsValid())
                {
                    uint32 dstIndex = m_layout->GetCbvSrvUavIndex(binding.binding);
                    if (dstIndex != UINT32_MAX)
                    {
                        D3D12_CPU_DESCRIPTOR_HANDLE dst = m_cbvSrvUavHandle.cpuHandle;
                        dst.ptr += static_cast<SIZE_T>(dstIndex) * cbvSrvUavSize;
                        d3dDevice->CopyDescriptorsSimple(1, dst, srcHandle.cpuHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    }
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
                        dst.ptr += static_cast<SIZE_T>(dstIndex) * samplerSize;
                        d3dDevice->CopyDescriptorsSimple(1, dst, srcHandle.cpuHandle, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
                    }
                }
            }

            // Handle buffer (storage/uniform buffer)
            if (binding.buffer && m_cbvSrvUavHandle.IsValid())
            {
                auto* dx12Buffer = static_cast<DX12Buffer*>(binding.buffer);
                // Prefer UAV if available, otherwise use SRV
                const DX12DescriptorHandle& srcHandle =
                    dx12Buffer->GetUAVHandle().IsValid() ? dx12Buffer->GetUAVHandle() : dx12Buffer->GetSRVHandle();

                if (srcHandle.IsValid())
                {
                    uint32 dstIndex = m_layout->GetCbvSrvUavIndex(binding.binding);
                    if (dstIndex != UINT32_MAX)
                    {
                        D3D12_CPU_DESCRIPTOR_HANDLE dst = m_cbvSrvUavHandle.cpuHandle;
                        dst.ptr += static_cast<SIZE_T>(dstIndex) * cbvSrvUavSize;
                        d3dDevice->CopyDescriptorsSimple(1, dst, srcHandle.cpuHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    }
                }
            }
        }
    }

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIDescriptorSetLayoutRef CreateDX12DescriptorSetLayout(DX12Device* device, const RHIDescriptorSetLayoutDesc& desc)
    {
        return Ref<DX12DescriptorSetLayout>(new DX12DescriptorSetLayout(device, desc));
    }

    RHIPipelineLayoutRef CreateDX12PipelineLayout(DX12Device* device, const RHIPipelineLayoutDesc& desc)
    {
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

    RHIDescriptorSetRef CreateDX12DescriptorSet(DX12Device* device, const RHIDescriptorSetDesc& desc)
    {
        return Ref<DX12DescriptorSet>(new DX12DescriptorSet(device, desc));
    }

} // namespace RVX
