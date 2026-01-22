#include "DX11Pipeline.h"
#include "DX11Device.h"
#include "DX11Resources.h"
#include "DX11Conversions.h"
#include "DX11StateCache.h"
#include "DX11BindingRemapper.h"

#include <span>

namespace RVX
{
    // =============================================================================
    // DX11 Descriptor Set Layout
    // =============================================================================
    DX11DescriptorSetLayout::DX11DescriptorSetLayout(DX11Device* device, const RHIDescriptorSetLayoutDesc& desc)
    {
        (void)device;
        m_entries = desc.entries;

        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        RVX_RHI_DEBUG("DX11: Created DescriptorSetLayout with {} entries", m_entries.size());
    }

    DX11DescriptorSetLayout::~DX11DescriptorSetLayout()
    {
    }

    // =============================================================================
    // DX11 Pipeline Layout
    // =============================================================================
    DX11PipelineLayout::DX11PipelineLayout(DX11Device* device, const RHIPipelineLayoutDesc& desc)
        : m_setLayouts(desc.setLayouts)
        , m_pushConstantSize(desc.pushConstantSize)
    {
        // Create push constant buffer if needed
        if (desc.pushConstantSize > 0)
        {
            D3D11_BUFFER_DESC bufferDesc = {};
            // Round up to 16 bytes alignment (constant buffer requirement)
            bufferDesc.ByteWidth = (desc.pushConstantSize + 15) & ~15;
            bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
            bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

            HRESULT hr = device->GetD3DDevice()->CreateBuffer(&bufferDesc, nullptr, &m_pushConstantBuffer);
            if (FAILED(hr))
            {
                RVX_RHI_ERROR("DX11: Failed to create push constant buffer: {}", HRESULTToString(hr));
            }
        }

        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        RVX_RHI_DEBUG("DX11: Created PipelineLayout with {} set layouts, {} bytes push constants",
            m_setLayouts.size(), desc.pushConstantSize);
    }

    DX11PipelineLayout::~DX11PipelineLayout()
    {
    }

    // =============================================================================
    // DX11 Descriptor Set
    // =============================================================================
    DX11DescriptorSet::DX11DescriptorSet(DX11Device* device, const RHIDescriptorSetDesc& desc)
        : m_device(device)
        , m_layout(static_cast<DX11DescriptorSetLayout*>(desc.layout))
    {
        m_bindings = desc.bindings;

        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }
    }

    DX11DescriptorSet::~DX11DescriptorSet()
    {
    }

    void DX11DescriptorSet::Update(const std::vector<RHIDescriptorBinding>& bindings)
    {
        m_bindings = bindings;
    }

    void DX11DescriptorSet::Apply(ID3D11DeviceContext* context, RHIShaderStage stages, uint32 setIndex,
                                   std::span<const uint32> dynamicOffsets) const
    {
        if (!context || !m_layout) return;

        const auto& entries = m_layout->GetEntries();
        auto& remapper = DX11BindingRemapper::Get();

        // Try to get ID3D11DeviceContext1 for dynamic offset support
        ComPtr<ID3D11DeviceContext1> context1;
        context->QueryInterface(IID_PPV_ARGS(&context1));

        uint32 dynamicOffsetIndex = 0;

        for (const auto& binding : m_bindings)
        {
            // Find the layout entry for this binding
            const RHIBindingLayoutEntry* entry = nullptr;
            for (const auto& e : entries)
            {
                if (e.binding == binding.binding)
                {
                    entry = &e;
                    break;
                }
            }
            if (!entry) continue;

            // Apply based on binding type
            switch (entry->type)
            {
                case RHIBindingType::UniformBuffer:
                case RHIBindingType::DynamicUniformBuffer:
                {
                    if (binding.buffer)
                    {
                        auto* dx11Buffer = static_cast<DX11Buffer*>(binding.buffer);
                        ID3D11Buffer* cb = dx11Buffer->GetBuffer();
                        UINT slot = remapper.GetCBSlot(setIndex, binding.binding);
                        if (slot == UINT32_MAX) slot = binding.binding; // Fallback

                        // Check if this is a dynamic buffer with offset
                        bool useDynamicOffset = (entry->type == RHIBindingType::DynamicUniformBuffer) &&
                                               (dynamicOffsetIndex < dynamicOffsets.size()) &&
                                               (context1 != nullptr);

                        if (useDynamicOffset)
                        {
                            // Use *SetConstantBuffers1 with offset
                            // Offset is in bytes, but DX11 requires 16-byte aligned offset in units of 16 bytes
                            UINT offsetIn16Bytes = dynamicOffsets[dynamicOffsetIndex] / 16;
                            UINT bufferSizeIn16Bytes = static_cast<UINT>(dx11Buffer->GetSize() / 16);
                            
                            // Ensure the remaining size doesn't exceed buffer
                            UINT numConstants = bufferSizeIn16Bytes - offsetIn16Bytes;

                            if (HasFlag(stages, RHIShaderStage::Vertex))
                                context1->VSSetConstantBuffers1(slot, 1, &cb, &offsetIn16Bytes, &numConstants);
                            if (HasFlag(stages, RHIShaderStage::Pixel))
                                context1->PSSetConstantBuffers1(slot, 1, &cb, &offsetIn16Bytes, &numConstants);
                            if (HasFlag(stages, RHIShaderStage::Geometry))
                                context1->GSSetConstantBuffers1(slot, 1, &cb, &offsetIn16Bytes, &numConstants);
                            if (HasFlag(stages, RHIShaderStage::Hull))
                                context1->HSSetConstantBuffers1(slot, 1, &cb, &offsetIn16Bytes, &numConstants);
                            if (HasFlag(stages, RHIShaderStage::Domain))
                                context1->DSSetConstantBuffers1(slot, 1, &cb, &offsetIn16Bytes, &numConstants);
                            if (HasFlag(stages, RHIShaderStage::Compute))
                                context1->CSSetConstantBuffers1(slot, 1, &cb, &offsetIn16Bytes, &numConstants);

                            dynamicOffsetIndex++;
                        }
                        else
                        {
                            // Standard binding without offset
                            if (HasFlag(stages, RHIShaderStage::Vertex))
                                context->VSSetConstantBuffers(slot, 1, &cb);
                            if (HasFlag(stages, RHIShaderStage::Pixel))
                                context->PSSetConstantBuffers(slot, 1, &cb);
                            if (HasFlag(stages, RHIShaderStage::Geometry))
                                context->GSSetConstantBuffers(slot, 1, &cb);
                            if (HasFlag(stages, RHIShaderStage::Hull))
                                context->HSSetConstantBuffers(slot, 1, &cb);
                            if (HasFlag(stages, RHIShaderStage::Domain))
                                context->DSSetConstantBuffers(slot, 1, &cb);
                            if (HasFlag(stages, RHIShaderStage::Compute))
                                context->CSSetConstantBuffers(slot, 1, &cb);
                            
                            // Consume dynamic offset even if we couldn't use it
                            if (entry->type == RHIBindingType::DynamicUniformBuffer)
                            {
                                dynamicOffsetIndex++;
                            }
                        }
                    }
                    break;
                }

                case RHIBindingType::StorageBuffer:
                case RHIBindingType::DynamicStorageBuffer:
                {
                    if (binding.buffer)
                    {
                        auto* dx11Buffer = static_cast<DX11Buffer*>(binding.buffer);

                        // Storage buffers use SRV or UAV depending on usage
                        if (dx11Buffer->GetUAV())
                        {
                            UINT slot = remapper.GetUAVSlot(setIndex, binding.binding);
                            if (slot == UINT32_MAX) slot = binding.binding;

                            ID3D11UnorderedAccessView* uav = dx11Buffer->GetUAV();
                            if (HasFlag(stages, RHIShaderStage::Compute))
                            {
                                UINT initialCount = (UINT)-1;
                                context->CSSetUnorderedAccessViews(slot, 1, &uav, &initialCount);
                            }
                        }
                        else if (dx11Buffer->GetSRV())
                        {
                            UINT slot = remapper.GetSRVSlot(setIndex, binding.binding);
                            if (slot == UINT32_MAX) slot = binding.binding;

                            ID3D11ShaderResourceView* srv = dx11Buffer->GetSRV();
                            if (HasFlag(stages, RHIShaderStage::Vertex))
                                context->VSSetShaderResources(slot, 1, &srv);
                            if (HasFlag(stages, RHIShaderStage::Pixel))
                                context->PSSetShaderResources(slot, 1, &srv);
                            if (HasFlag(stages, RHIShaderStage::Compute))
                                context->CSSetShaderResources(slot, 1, &srv);
                        }
                    }
                    break;
                }

                case RHIBindingType::SampledTexture:
                {
                    if (binding.textureView)
                    {
                        auto* dx11View = static_cast<DX11TextureView*>(binding.textureView);
                        ID3D11ShaderResourceView* srv = dx11View->GetSRV();
                        UINT slot = remapper.GetSRVSlot(setIndex, binding.binding);
                        if (slot == UINT32_MAX) slot = binding.binding;

                        if (HasFlag(stages, RHIShaderStage::Vertex))
                            context->VSSetShaderResources(slot, 1, &srv);
                        if (HasFlag(stages, RHIShaderStage::Pixel))
                            context->PSSetShaderResources(slot, 1, &srv);
                        if (HasFlag(stages, RHIShaderStage::Compute))
                            context->CSSetShaderResources(slot, 1, &srv);
                    }
                    break;
                }

                case RHIBindingType::StorageTexture:
                {
                    if (binding.textureView)
                    {
                        auto* dx11View = static_cast<DX11TextureView*>(binding.textureView);
                        ID3D11UnorderedAccessView* uav = dx11View->GetUAV();
                        UINT slot = remapper.GetUAVSlot(setIndex, binding.binding);
                        if (slot == UINT32_MAX) slot = binding.binding;

                        if (HasFlag(stages, RHIShaderStage::Compute))
                        {
                            UINT initialCount = (UINT)-1;
                            context->CSSetUnorderedAccessViews(slot, 1, &uav, &initialCount);
                        }
                    }
                    break;
                }

                case RHIBindingType::Sampler:
                {
                    if (binding.sampler)
                    {
                        auto* dx11Sampler = static_cast<DX11Sampler*>(binding.sampler);
                        ID3D11SamplerState* sampler = dx11Sampler->GetSampler();
                        UINT slot = remapper.GetSamplerSlot(setIndex, binding.binding);
                        if (slot == UINT32_MAX) slot = binding.binding;

                        if (HasFlag(stages, RHIShaderStage::Vertex))
                            context->VSSetSamplers(slot, 1, &sampler);
                        if (HasFlag(stages, RHIShaderStage::Pixel))
                            context->PSSetSamplers(slot, 1, &sampler);
                        if (HasFlag(stages, RHIShaderStage::Compute))
                            context->CSSetSamplers(slot, 1, &sampler);
                    }
                    break;
                }

                case RHIBindingType::CombinedTextureSampler:
                {
                    UINT srvSlot = remapper.GetSRVSlot(setIndex, binding.binding);
                    UINT samplerSlot = remapper.GetSamplerSlot(setIndex, binding.binding);
                    if (srvSlot == UINT32_MAX) srvSlot = binding.binding;
                    if (samplerSlot == UINT32_MAX) samplerSlot = binding.binding;
                    
                    if (binding.textureView)
                    {
                        auto* dx11View = static_cast<DX11TextureView*>(binding.textureView);
                        ID3D11ShaderResourceView* srv = dx11View->GetSRV();

                        if (HasFlag(stages, RHIShaderStage::Vertex))
                            context->VSSetShaderResources(srvSlot, 1, &srv);
                        if (HasFlag(stages, RHIShaderStage::Pixel))
                            context->PSSetShaderResources(srvSlot, 1, &srv);
                        if (HasFlag(stages, RHIShaderStage::Compute))
                            context->CSSetShaderResources(srvSlot, 1, &srv);
                    }

                    if (binding.sampler)
                    {
                        auto* dx11Sampler = static_cast<DX11Sampler*>(binding.sampler);
                        ID3D11SamplerState* sampler = dx11Sampler->GetSampler();

                        if (HasFlag(stages, RHIShaderStage::Vertex))
                            context->VSSetSamplers(samplerSlot, 1, &sampler);
                        if (HasFlag(stages, RHIShaderStage::Pixel))
                            context->PSSetSamplers(samplerSlot, 1, &sampler);
                        if (HasFlag(stages, RHIShaderStage::Compute))
                            context->CSSetSamplers(samplerSlot, 1, &sampler);
                    }
                    break;
                }
            }
        }
    }

    // =============================================================================
    // DX11 Graphics Pipeline
    // =============================================================================
    DX11GraphicsPipeline::DX11GraphicsPipeline(DX11Device* device, const RHIGraphicsPipelineDesc& desc)
        : m_device(device)
        , m_layout(static_cast<DX11PipelineLayout*>(desc.pipelineLayout))
    {
        // Get shaders
        if (desc.vertexShader)
        {
            auto* dx11Shader = static_cast<DX11Shader*>(desc.vertexShader);
            m_vertexShader = dx11Shader->GetVertexShader();
        }
        if (desc.pixelShader)
        {
            auto* dx11Shader = static_cast<DX11Shader*>(desc.pixelShader);
            m_pixelShader = dx11Shader->GetPixelShader();
        }
        if (desc.geometryShader)
        {
            auto* dx11Shader = static_cast<DX11Shader*>(desc.geometryShader);
            m_geometryShader = dx11Shader->GetGeometryShader();
        }
        if (desc.hullShader)
        {
            auto* dx11Shader = static_cast<DX11Shader*>(desc.hullShader);
            m_hullShader = dx11Shader->GetHullShader();
        }
        if (desc.domainShader)
        {
            auto* dx11Shader = static_cast<DX11Shader*>(desc.domainShader);
            m_domainShader = dx11Shader->GetDomainShader();
        }

        m_topology = ToD3D11PrimitiveTopology(desc.primitiveTopology);

        // Use StateCache for state objects
        DX11StateCache* stateCache = device->GetStateCache();
        if (stateCache)
        {
            // Get cached rasterizer state
            m_rasterizerState = stateCache->GetRasterizerState(desc.rasterizerState);

            // Get cached depth stencil state
            m_depthStencilState = stateCache->GetDepthStencilState(desc.depthStencilState);

            // Get cached blend state
            m_blendState = stateCache->GetBlendState(desc.blendState);
        }
        else
        {
            RVX_RHI_WARN("DX11: StateCache not available, creating state objects directly");
            
            // Fallback: create directly (not recommended)
            D3D11_RASTERIZER_DESC rasterDesc = {};
            rasterDesc.FillMode = ToD3D11FillMode(desc.rasterizerState.fillMode);
            rasterDesc.CullMode = ToD3D11CullMode(desc.rasterizerState.cullMode);
            rasterDesc.FrontCounterClockwise = (desc.rasterizerState.frontFace == RHIFrontFace::CounterClockwise);
            rasterDesc.DepthBias = static_cast<INT>(desc.rasterizerState.depthBias);
            rasterDesc.DepthBiasClamp = desc.rasterizerState.depthBiasClamp;
            rasterDesc.SlopeScaledDepthBias = desc.rasterizerState.slopeScaledDepthBias;
            rasterDesc.DepthClipEnable = desc.rasterizerState.depthClipEnable;
            rasterDesc.ScissorEnable = TRUE;
            rasterDesc.MultisampleEnable = desc.rasterizerState.multisampleEnable;
            rasterDesc.AntialiasedLineEnable = desc.rasterizerState.antialiasedLineEnable;
            
            ComPtr<ID3D11RasterizerState> rasterizerState;
            device->GetD3DDevice()->CreateRasterizerState(&rasterDesc, &rasterizerState);
            m_rasterizerState = rasterizerState.Get();
        }

        // Create input layout using StateCache
        if (desc.vertexShader && !desc.inputLayout.elements.empty())
        {
            auto* dx11Shader = static_cast<DX11Shader*>(desc.vertexShader);
            const auto& bytecode = dx11Shader->GetBytecode();

            if (stateCache)
            {
                m_inputLayout = stateCache->GetInputLayout(
                    desc.inputLayout.elements,
                    bytecode.data(),
                    bytecode.size()
                );
            }
            else
            {
                // Fallback: create directly
                std::vector<D3D11_INPUT_ELEMENT_DESC> inputElements;

                for (const auto& elem : desc.inputLayout.elements)
                {
                    D3D11_INPUT_ELEMENT_DESC d3dElem = {};
                    d3dElem.SemanticName = elem.semanticName;
                    d3dElem.SemanticIndex = elem.semanticIndex;
                    d3dElem.Format = ToDXGIFormat(elem.format);
                    d3dElem.InputSlot = elem.inputSlot;
                    d3dElem.AlignedByteOffset = (elem.alignedByteOffset == 0xFFFFFFFF) 
                        ? D3D11_APPEND_ALIGNED_ELEMENT 
                        : elem.alignedByteOffset;
                    d3dElem.InputSlotClass = elem.perInstance 
                        ? D3D11_INPUT_PER_INSTANCE_DATA 
                        : D3D11_INPUT_PER_VERTEX_DATA;
                    d3dElem.InstanceDataStepRate = elem.instanceDataStepRate;

                    inputElements.push_back(d3dElem);
                }

                ComPtr<ID3D11InputLayout> inputLayout;
                HRESULT hr = device->GetD3DDevice()->CreateInputLayout(
                    inputElements.data(),
                    static_cast<UINT>(inputElements.size()),
                    bytecode.data(),
                    bytecode.size(),
                    &inputLayout
                );

                if (FAILED(hr))
                {
                    RVX_RHI_ERROR("DX11: Failed to create input layout: {}", HRESULTToString(hr));
                }
                m_inputLayout = inputLayout.Get();
            }
        }

        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        RVX_RHI_DEBUG("DX11: Created GraphicsPipeline '{}'", desc.debugName ? desc.debugName : "");
    }

    DX11GraphicsPipeline::~DX11GraphicsPipeline()
    {
    }

    void DX11GraphicsPipeline::Apply(ID3D11DeviceContext* context) const
    {
        if (!context) return;

        // Set required shaders (VS/PS are typically always needed)
        context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
        context->PSSetShader(m_pixelShader.Get(), nullptr, 0);

        // Only set optional shaders if they exist (avoid unnecessary API calls)
        // Note: We still need to unbind if previously bound, so we set nullptr explicitly
        if (m_geometryShader)
            context->GSSetShader(m_geometryShader.Get(), nullptr, 0);
        else
            context->GSSetShader(nullptr, nullptr, 0);

        if (m_hullShader)
            context->HSSetShader(m_hullShader.Get(), nullptr, 0);
        else
            context->HSSetShader(nullptr, nullptr, 0);

        if (m_domainShader)
            context->DSSetShader(m_domainShader.Get(), nullptr, 0);
        else
            context->DSSetShader(nullptr, nullptr, 0);

        // Set state objects (raw pointers from StateCache)
        if (m_rasterizerState)
            context->RSSetState(m_rasterizerState);
        
        if (m_depthStencilState)
            context->OMSetDepthStencilState(m_depthStencilState, 0);
        
        if (m_blendState)
        {
            float blendFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            context->OMSetBlendState(m_blendState, blendFactor, 0xFFFFFFFF);
        }

        // Set input layout and topology
        if (m_inputLayout)
            context->IASetInputLayout(m_inputLayout);
        
        context->IASetPrimitiveTopology(m_topology);
    }

    // =============================================================================
    // DX11 Compute Pipeline
    // =============================================================================
    DX11ComputePipeline::DX11ComputePipeline(DX11Device* device, const RHIComputePipelineDesc& desc)
        : m_device(device)
        , m_layout(static_cast<DX11PipelineLayout*>(desc.pipelineLayout))
    {
        if (desc.computeShader)
        {
            auto* dx11Shader = static_cast<DX11Shader*>(desc.computeShader);
            m_computeShader = dx11Shader->GetComputeShader();
        }

        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        RVX_RHI_DEBUG("DX11: Created ComputePipeline '{}'", desc.debugName ? desc.debugName : "");
    }

    DX11ComputePipeline::~DX11ComputePipeline()
    {
    }

    void DX11ComputePipeline::Apply(ID3D11DeviceContext* context) const
    {
        if (!context) return;

        context->CSSetShader(m_computeShader.Get(), nullptr, 0);
    }

} // namespace RVX
