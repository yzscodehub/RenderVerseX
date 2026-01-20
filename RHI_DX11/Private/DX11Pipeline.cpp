#include "DX11Pipeline.h"
#include "DX11Device.h"
#include "DX11Resources.h"
#include "DX11Conversions.h"
#include "DX11StateCache.h"
#include "DX11BindingRemapper.h"

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

    void DX11DescriptorSet::Apply(ID3D11DeviceContext* context, RHIShaderStage stages) const
    {
        if (!context || !m_layout) return;

        const auto& entries = m_layout->GetEntries();

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
                        UINT slot = binding.binding;

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
                    }
                    break;
                }

                case RHIBindingType::StorageBuffer:
                case RHIBindingType::DynamicStorageBuffer:
                {
                    if (binding.buffer)
                    {
                        auto* dx11Buffer = static_cast<DX11Buffer*>(binding.buffer);
                        UINT slot = binding.binding;

                        // Storage buffers use SRV or UAV depending on usage
                        if (dx11Buffer->GetUAV())
                        {
                            ID3D11UnorderedAccessView* uav = dx11Buffer->GetUAV();
                            if (HasFlag(stages, RHIShaderStage::Compute))
                            {
                                UINT initialCount = (UINT)-1;
                                context->CSSetUnorderedAccessViews(slot, 1, &uav, &initialCount);
                            }
                        }
                        else if (dx11Buffer->GetSRV())
                        {
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
                        UINT slot = binding.binding;

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
                        UINT slot = binding.binding;

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
                        UINT slot = binding.binding;

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
                    UINT slot = binding.binding;
                    
                    if (binding.textureView)
                    {
                        auto* dx11View = static_cast<DX11TextureView*>(binding.textureView);
                        ID3D11ShaderResourceView* srv = dx11View->GetSRV();

                        if (HasFlag(stages, RHIShaderStage::Vertex))
                            context->VSSetShaderResources(slot, 1, &srv);
                        if (HasFlag(stages, RHIShaderStage::Pixel))
                            context->PSSetShaderResources(slot, 1, &srv);
                        if (HasFlag(stages, RHIShaderStage::Compute))
                            context->CSSetShaderResources(slot, 1, &srv);
                    }

                    if (binding.sampler)
                    {
                        auto* dx11Sampler = static_cast<DX11Sampler*>(binding.sampler);
                        ID3D11SamplerState* sampler = dx11Sampler->GetSampler();

                        if (HasFlag(stages, RHIShaderStage::Vertex))
                            context->VSSetSamplers(slot, 1, &sampler);
                        if (HasFlag(stages, RHIShaderStage::Pixel))
                            context->PSSetSamplers(slot, 1, &sampler);
                        if (HasFlag(stages, RHIShaderStage::Compute))
                            context->CSSetSamplers(slot, 1, &sampler);
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

        // Create rasterizer state
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

        device->GetD3DDevice()->CreateRasterizerState(&rasterDesc, &m_rasterizerState);

        // Create depth stencil state
        D3D11_DEPTH_STENCIL_DESC dsDesc = {};
        dsDesc.DepthEnable = desc.depthStencilState.depthTestEnable;
        dsDesc.DepthWriteMask = desc.depthStencilState.depthWriteEnable ? 
            D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        dsDesc.DepthFunc = ToD3D11ComparisonFunc(desc.depthStencilState.depthCompareOp);
        dsDesc.StencilEnable = desc.depthStencilState.stencilTestEnable;
        dsDesc.StencilReadMask = desc.depthStencilState.stencilReadMask;
        dsDesc.StencilWriteMask = desc.depthStencilState.stencilWriteMask;

        dsDesc.FrontFace.StencilFailOp = ToD3D11StencilOp(desc.depthStencilState.frontFace.failOp);
        dsDesc.FrontFace.StencilDepthFailOp = ToD3D11StencilOp(desc.depthStencilState.frontFace.depthFailOp);
        dsDesc.FrontFace.StencilPassOp = ToD3D11StencilOp(desc.depthStencilState.frontFace.passOp);
        dsDesc.FrontFace.StencilFunc = ToD3D11ComparisonFunc(desc.depthStencilState.frontFace.compareOp);

        dsDesc.BackFace.StencilFailOp = ToD3D11StencilOp(desc.depthStencilState.backFace.failOp);
        dsDesc.BackFace.StencilDepthFailOp = ToD3D11StencilOp(desc.depthStencilState.backFace.depthFailOp);
        dsDesc.BackFace.StencilPassOp = ToD3D11StencilOp(desc.depthStencilState.backFace.passOp);
        dsDesc.BackFace.StencilFunc = ToD3D11ComparisonFunc(desc.depthStencilState.backFace.compareOp);

        device->GetD3DDevice()->CreateDepthStencilState(&dsDesc, &m_depthStencilState);

        // Create blend state
        D3D11_BLEND_DESC blendDesc = {};
        blendDesc.AlphaToCoverageEnable = desc.blendState.alphaToCoverageEnable;
        blendDesc.IndependentBlendEnable = desc.blendState.independentBlendEnable;

        for (uint32 i = 0; i < RVX_MAX_RENDER_TARGETS; ++i)
        {
            const auto& rt = desc.blendState.renderTargets[i];
            blendDesc.RenderTarget[i].BlendEnable = rt.blendEnable;
            blendDesc.RenderTarget[i].SrcBlend = ToD3D11Blend(rt.srcColorBlend);
            blendDesc.RenderTarget[i].DestBlend = ToD3D11Blend(rt.dstColorBlend);
            blendDesc.RenderTarget[i].BlendOp = ToD3D11BlendOp(rt.colorBlendOp);
            blendDesc.RenderTarget[i].SrcBlendAlpha = ToD3D11Blend(rt.srcAlphaBlend);
            blendDesc.RenderTarget[i].DestBlendAlpha = ToD3D11Blend(rt.dstAlphaBlend);
            blendDesc.RenderTarget[i].BlendOpAlpha = ToD3D11BlendOp(rt.alphaBlendOp);
            blendDesc.RenderTarget[i].RenderTargetWriteMask = rt.colorWriteMask;
        }

        device->GetD3DDevice()->CreateBlendState(&blendDesc, &m_blendState);

        // Create input layout
        if (desc.vertexShader && !desc.inputLayout.elements.empty())
        {
            auto* dx11Shader = static_cast<DX11Shader*>(desc.vertexShader);
            const auto& bytecode = dx11Shader->GetBytecode();

            std::vector<D3D11_INPUT_ELEMENT_DESC> inputElements;
            uint32 currentOffset[16] = {};  // Track offset per slot

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

            HRESULT hr = device->GetD3DDevice()->CreateInputLayout(
                inputElements.data(),
                static_cast<UINT>(inputElements.size()),
                bytecode.data(),
                bytecode.size(),
                &m_inputLayout
            );

            if (FAILED(hr))
            {
                RVX_RHI_ERROR("DX11: Failed to create input layout: {}", HRESULTToString(hr));
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

        // Set shaders
        context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
        context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
        context->GSSetShader(m_geometryShader.Get(), nullptr, 0);
        context->HSSetShader(m_hullShader.Get(), nullptr, 0);
        context->DSSetShader(m_domainShader.Get(), nullptr, 0);

        // Set state objects
        context->RSSetState(m_rasterizerState.Get());
        context->OMSetDepthStencilState(m_depthStencilState.Get(), 0);
        
        float blendFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        context->OMSetBlendState(m_blendState.Get(), blendFactor, 0xFFFFFFFF);

        // Set input layout and topology
        context->IASetInputLayout(m_inputLayout.Get());
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
