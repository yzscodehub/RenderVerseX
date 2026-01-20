#include "DX11StateCache.h"
#include "DX11Device.h"
#include "DX11Conversions.h"

namespace RVX
{
    DX11StateCache::DX11StateCache(DX11Device* device)
        : m_device(device)
    {
    }

    DX11StateCache::~DX11StateCache()
    {
        Clear();
    }

    void DX11StateCache::Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_rasterizerStates.clear();
        m_depthStencilStates.clear();
        m_blendStates.clear();
        m_samplerStates.clear();
        m_inputLayouts.clear();
        m_stats = {};
    }

    // =============================================================================
    // Rasterizer State
    // =============================================================================
    ID3D11RasterizerState* DX11StateCache::GetRasterizerState(const RHIRasterizerState& desc)
    {
        size_t hash = HashRasterState(desc);

        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_rasterizerStates.find(hash);
        if (it != m_rasterizerStates.end())
        {
            m_stats.cacheHits++;
            return it->second.Get();
        }

        m_stats.cacheMisses++;

        D3D11_RASTERIZER_DESC d3dDesc = {};
        d3dDesc.FillMode = ToD3D11FillMode(desc.fillMode);
        d3dDesc.CullMode = ToD3D11CullMode(desc.cullMode);
        d3dDesc.FrontCounterClockwise = (desc.frontFace == RHIFrontFace::CounterClockwise) ? TRUE : FALSE;
        d3dDesc.DepthBias = static_cast<INT>(desc.depthBias);
        d3dDesc.DepthBiasClamp = desc.depthBiasClamp;
        d3dDesc.SlopeScaledDepthBias = desc.slopeScaledDepthBias;
        d3dDesc.DepthClipEnable = desc.depthClipEnable ? TRUE : FALSE;
        d3dDesc.ScissorEnable = TRUE;  // Always enable scissor
        d3dDesc.MultisampleEnable = desc.multisampleEnable ? TRUE : FALSE;
        d3dDesc.AntialiasedLineEnable = desc.antialiasedLineEnable ? TRUE : FALSE;

        ComPtr<ID3D11RasterizerState> state;
        HRESULT hr = m_device->GetD3DDevice()->CreateRasterizerState(&d3dDesc, &state);

        if (FAILED(hr))
        {
            RVX_RHI_ERROR("Failed to create rasterizer state: {}", HRESULTToString(hr));
            return nullptr;
        }

        m_rasterizerStates[hash] = state;
        m_stats.rasterizerStateCount++;

        return state.Get();
    }

    // =============================================================================
    // Depth Stencil State
    // =============================================================================
    ID3D11DepthStencilState* DX11StateCache::GetDepthStencilState(const RHIDepthStencilState& desc)
    {
        size_t hash = HashDepthStencilState(desc);

        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_depthStencilStates.find(hash);
        if (it != m_depthStencilStates.end())
        {
            m_stats.cacheHits++;
            return it->second.Get();
        }

        m_stats.cacheMisses++;

        D3D11_DEPTH_STENCIL_DESC d3dDesc = {};
        d3dDesc.DepthEnable = desc.depthTestEnable ? TRUE : FALSE;
        d3dDesc.DepthWriteMask = desc.depthWriteEnable ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        d3dDesc.DepthFunc = ToD3D11ComparisonFunc(desc.depthCompareOp);
        d3dDesc.StencilEnable = desc.stencilTestEnable ? TRUE : FALSE;
        d3dDesc.StencilReadMask = desc.stencilReadMask;
        d3dDesc.StencilWriteMask = desc.stencilWriteMask;

        // Front face
        d3dDesc.FrontFace.StencilFailOp = ToD3D11StencilOp(desc.frontFace.failOp);
        d3dDesc.FrontFace.StencilDepthFailOp = ToD3D11StencilOp(desc.frontFace.depthFailOp);
        d3dDesc.FrontFace.StencilPassOp = ToD3D11StencilOp(desc.frontFace.passOp);
        d3dDesc.FrontFace.StencilFunc = ToD3D11ComparisonFunc(desc.frontFace.compareOp);

        // Back face
        d3dDesc.BackFace.StencilFailOp = ToD3D11StencilOp(desc.backFace.failOp);
        d3dDesc.BackFace.StencilDepthFailOp = ToD3D11StencilOp(desc.backFace.depthFailOp);
        d3dDesc.BackFace.StencilPassOp = ToD3D11StencilOp(desc.backFace.passOp);
        d3dDesc.BackFace.StencilFunc = ToD3D11ComparisonFunc(desc.backFace.compareOp);

        ComPtr<ID3D11DepthStencilState> state;
        HRESULT hr = m_device->GetD3DDevice()->CreateDepthStencilState(&d3dDesc, &state);

        if (FAILED(hr))
        {
            RVX_RHI_ERROR("Failed to create depth-stencil state: {}", HRESULTToString(hr));
            return nullptr;
        }

        m_depthStencilStates[hash] = state;
        m_stats.depthStencilStateCount++;

        return state.Get();
    }

    // =============================================================================
    // Blend State
    // =============================================================================
    ID3D11BlendState* DX11StateCache::GetBlendState(const RHIBlendState& desc)
    {
        size_t hash = HashBlendState(desc);

        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_blendStates.find(hash);
        if (it != m_blendStates.end())
        {
            m_stats.cacheHits++;
            return it->second.Get();
        }

        m_stats.cacheMisses++;

        D3D11_BLEND_DESC d3dDesc = {};
        d3dDesc.AlphaToCoverageEnable = desc.alphaToCoverageEnable ? TRUE : FALSE;
        d3dDesc.IndependentBlendEnable = desc.independentBlendEnable ? TRUE : FALSE;

        for (uint32 i = 0; i < RVX_MAX_RENDER_TARGETS; ++i)
        {
            const auto& rt = desc.renderTargets[i];
            auto& d3dRT = d3dDesc.RenderTarget[i];

            d3dRT.BlendEnable = rt.blendEnable ? TRUE : FALSE;
            d3dRT.SrcBlend = ToD3D11Blend(rt.srcColorBlend);
            d3dRT.DestBlend = ToD3D11Blend(rt.dstColorBlend);
            d3dRT.BlendOp = ToD3D11BlendOp(rt.colorBlendOp);
            d3dRT.SrcBlendAlpha = ToD3D11Blend(rt.srcAlphaBlend);
            d3dRT.DestBlendAlpha = ToD3D11Blend(rt.dstAlphaBlend);
            d3dRT.BlendOpAlpha = ToD3D11BlendOp(rt.alphaBlendOp);
            d3dRT.RenderTargetWriteMask = static_cast<UINT8>(rt.colorWriteMask);
        }

        ComPtr<ID3D11BlendState> state;
        HRESULT hr = m_device->GetD3DDevice()->CreateBlendState(&d3dDesc, &state);

        if (FAILED(hr))
        {
            RVX_RHI_ERROR("Failed to create blend state: {}", HRESULTToString(hr));
            return nullptr;
        }

        m_blendStates[hash] = state;
        m_stats.blendStateCount++;

        return state.Get();
    }

    // =============================================================================
    // Sampler State
    // =============================================================================
    ID3D11SamplerState* DX11StateCache::GetSamplerState(const RHISamplerDesc& desc)
    {
        size_t hash = HashSamplerState(desc);

        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_samplerStates.find(hash);
        if (it != m_samplerStates.end())
        {
            m_stats.cacheHits++;
            return it->second.Get();
        }

        m_stats.cacheMisses++;

        D3D11_SAMPLER_DESC d3dDesc = {};
        d3dDesc.Filter = ToD3D11Filter(desc.minFilter, desc.magFilter, desc.mipFilter, desc.anisotropyEnable);
        d3dDesc.AddressU = ToD3D11AddressMode(desc.addressU);
        d3dDesc.AddressV = ToD3D11AddressMode(desc.addressV);
        d3dDesc.AddressW = ToD3D11AddressMode(desc.addressW);
        d3dDesc.MipLODBias = desc.mipLodBias;
        d3dDesc.MaxAnisotropy = static_cast<UINT>(desc.maxAnisotropy);
        d3dDesc.ComparisonFunc = desc.compareEnable ? ToD3D11ComparisonFunc(desc.compareOp) : D3D11_COMPARISON_NEVER;
        d3dDesc.BorderColor[0] = desc.borderColor[0];
        d3dDesc.BorderColor[1] = desc.borderColor[1];
        d3dDesc.BorderColor[2] = desc.borderColor[2];
        d3dDesc.BorderColor[3] = desc.borderColor[3];
        d3dDesc.MinLOD = desc.minLod;
        d3dDesc.MaxLOD = desc.maxLod;

        ComPtr<ID3D11SamplerState> state;
        HRESULT hr = m_device->GetD3DDevice()->CreateSamplerState(&d3dDesc, &state);

        if (FAILED(hr))
        {
            RVX_RHI_ERROR("Failed to create sampler state: {}", HRESULTToString(hr));
            return nullptr;
        }

        m_samplerStates[hash] = state;
        m_stats.samplerStateCount++;

        return state.Get();
    }

    // =============================================================================
    // Input Layout
    // =============================================================================
    ID3D11InputLayout* DX11StateCache::GetInputLayout(
        const std::vector<RHIInputElement>& elements,
        const void* vsBytecode,
        size_t bytecodeSize)
    {
        size_t hash = HashInputLayout(elements);

        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_inputLayouts.find(hash);
        if (it != m_inputLayouts.end())
        {
            m_stats.cacheHits++;
            return it->second.Get();
        }

        m_stats.cacheMisses++;

        std::vector<D3D11_INPUT_ELEMENT_DESC> inputElements;
        inputElements.reserve(elements.size());

        for (const auto& elem : elements)
        {
            D3D11_INPUT_ELEMENT_DESC d3dElem = {};
            d3dElem.SemanticName = elem.semanticName;
            d3dElem.SemanticIndex = elem.semanticIndex;
            d3dElem.Format = ToDXGIFormat(elem.format);
            d3dElem.InputSlot = elem.inputSlot;
            d3dElem.AlignedByteOffset = elem.alignedByteOffset;
            d3dElem.InputSlotClass = elem.perInstance ?
                D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA;
            d3dElem.InstanceDataStepRate = elem.instanceDataStepRate;
            inputElements.push_back(d3dElem);
        }

        ComPtr<ID3D11InputLayout> layout;
        HRESULT hr = m_device->GetD3DDevice()->CreateInputLayout(
            inputElements.data(),
            static_cast<UINT>(inputElements.size()),
            vsBytecode,
            bytecodeSize,
            &layout
        );

        if (FAILED(hr))
        {
            RVX_RHI_ERROR("Failed to create input layout: {}", HRESULTToString(hr));
            return nullptr;
        }

        m_inputLayouts[hash] = layout;
        m_stats.inputLayoutCount++;

        return layout.Get();
    }

    // =============================================================================
    // Hash Functions
    // =============================================================================
    size_t DX11StateCache::HashRasterState(const RHIRasterizerState& desc) const
    {
        size_t hash = 0;
        hash = HashCombine(hash, static_cast<size_t>(desc.fillMode));
        hash = HashCombine(hash, static_cast<size_t>(desc.cullMode));
        hash = HashCombine(hash, static_cast<size_t>(desc.frontFace));
        hash = HashCombine(hash, std::hash<float>{}(desc.depthBias));
        hash = HashCombine(hash, std::hash<float>{}(desc.depthBiasClamp));
        hash = HashCombine(hash, std::hash<float>{}(desc.slopeScaledDepthBias));
        hash = HashCombine(hash, desc.depthClipEnable ? 1ULL : 0ULL);
        return hash;
    }

    size_t DX11StateCache::HashDepthStencilState(const RHIDepthStencilState& desc) const
    {
        size_t hash = 0;
        hash = HashCombine(hash, desc.depthTestEnable ? 1ULL : 0ULL);
        hash = HashCombine(hash, desc.depthWriteEnable ? 1ULL : 0ULL);
        hash = HashCombine(hash, static_cast<size_t>(desc.depthCompareOp));
        hash = HashCombine(hash, desc.stencilTestEnable ? 1ULL : 0ULL);
        hash = HashCombine(hash, static_cast<size_t>(desc.stencilReadMask));
        hash = HashCombine(hash, static_cast<size_t>(desc.stencilWriteMask));
        return hash;
    }

    size_t DX11StateCache::HashBlendState(const RHIBlendState& desc) const
    {
        size_t hash = 0;
        hash = HashCombine(hash, desc.alphaToCoverageEnable ? 1ULL : 0ULL);
        hash = HashCombine(hash, desc.independentBlendEnable ? 1ULL : 0ULL);

        for (uint32 i = 0; i < RVX_MAX_RENDER_TARGETS; ++i)
        {
            const auto& rt = desc.renderTargets[i];
            hash = HashCombine(hash, rt.blendEnable ? 1ULL : 0ULL);
            hash = HashCombine(hash, static_cast<size_t>(rt.srcColorBlend));
            hash = HashCombine(hash, static_cast<size_t>(rt.dstColorBlend));
            hash = HashCombine(hash, static_cast<size_t>(rt.colorBlendOp));
        }
        return hash;
    }

    size_t DX11StateCache::HashSamplerState(const RHISamplerDesc& desc) const
    {
        size_t hash = 0;
        hash = HashCombine(hash, static_cast<size_t>(desc.minFilter));
        hash = HashCombine(hash, static_cast<size_t>(desc.magFilter));
        hash = HashCombine(hash, static_cast<size_t>(desc.mipFilter));
        hash = HashCombine(hash, static_cast<size_t>(desc.addressU));
        hash = HashCombine(hash, static_cast<size_t>(desc.addressV));
        hash = HashCombine(hash, static_cast<size_t>(desc.addressW));
        hash = HashCombine(hash, static_cast<size_t>(desc.maxAnisotropy));
        return hash;
    }

    size_t DX11StateCache::HashInputLayout(const std::vector<RHIInputElement>& elements) const
    {
        size_t hash = elements.size();
        for (const auto& elem : elements)
        {
            hash = HashCombine(hash, std::hash<std::string>{}(elem.semanticName));
            hash = HashCombine(hash, static_cast<size_t>(elem.semanticIndex));
            hash = HashCombine(hash, static_cast<size_t>(elem.format));
            hash = HashCombine(hash, static_cast<size_t>(elem.alignedByteOffset));
            hash = HashCombine(hash, static_cast<size_t>(elem.inputSlot));
        }
        return hash;
    }

} // namespace RVX
