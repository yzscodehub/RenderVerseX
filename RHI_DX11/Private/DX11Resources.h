#pragma once

#include "DX11Common.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHITexture.h"
#include "RHI/RHISampler.h"
#include "RHI/RHIShader.h"
#include "RHI/RHIQuery.h"
#include "RHI/RHISynchronization.h"

#include <vector>

namespace RVX
{
    class DX11Device;

    // =============================================================================
    // DX11 Buffer (Phase 2)
    // =============================================================================
    class DX11Buffer : public RHIBuffer
    {
    public:
        DX11Buffer(DX11Device* device, const RHIBufferDesc& desc);
        ~DX11Buffer() override;

        // RHIBuffer interface
        uint64 GetSize() const override { return m_desc.size; }
        RHIBufferUsage GetUsage() const override { return m_desc.usage; }
        RHIMemoryType GetMemoryType() const override { return m_desc.memoryType; }
        uint32 GetStride() const override { return m_desc.stride; }
        void* Map() override;
        void Unmap() override;

        // DX11 Specific
        ID3D11Buffer* GetBuffer() const { return m_buffer.Get(); }
        ID3D11ShaderResourceView* GetSRV() const { return m_srv.Get(); }
        ID3D11UnorderedAccessView* GetUAV() const { return m_uav.Get(); }

    private:
        void CreateViews();

        DX11Device* m_device = nullptr;
        RHIBufferDesc m_desc;

        ComPtr<ID3D11Buffer> m_buffer;
        ComPtr<ID3D11ShaderResourceView> m_srv;
        ComPtr<ID3D11UnorderedAccessView> m_uav;
    };

    // =============================================================================
    // DX11 Texture (Phase 2)
    // =============================================================================
    class DX11Texture : public RHITexture
    {
    public:
        DX11Texture(DX11Device* device, const RHITextureDesc& desc);
        DX11Texture(DX11Device* device, ComPtr<ID3D11Texture2D> texture, const RHITextureDesc& desc);
        ~DX11Texture() override;

        // RHITexture interface
        uint32 GetWidth() const override { return m_desc.width; }
        uint32 GetHeight() const override { return m_desc.height; }
        uint32 GetDepth() const override { return m_desc.depth; }
        uint32 GetMipLevels() const override { return m_desc.mipLevels; }
        uint32 GetArraySize() const override { return m_desc.arraySize; }
        RHIFormat GetFormat() const override { return m_desc.format; }
        RHITextureUsage GetUsage() const override { return m_desc.usage; }
        RHITextureDimension GetDimension() const override { return m_desc.dimension; }
        RHISampleCount GetSampleCount() const override { return m_desc.sampleCount; }

        // DX11 Specific
        ID3D11Resource* GetResource() const { return m_resource.Get(); }
        ID3D11Texture2D* GetTexture2D() const;
        DXGI_FORMAT GetDXGIFormat() const { return m_dxgiFormat; }

        ID3D11ShaderResourceView* GetSRV() const { return m_srv.Get(); }
        ID3D11UnorderedAccessView* GetUAV() const { return m_uav.Get(); }
        ID3D11RenderTargetView* GetRTV(uint32 index = 0) const;
        ID3D11DepthStencilView* GetDSV() const { return m_dsv.Get(); }

    private:
        void CreateViews();

        DX11Device* m_device = nullptr;
        RHITextureDesc m_desc;
        DXGI_FORMAT m_dxgiFormat = DXGI_FORMAT_UNKNOWN;
        bool m_ownsResource = true;

        ComPtr<ID3D11Resource> m_resource;
        ComPtr<ID3D11ShaderResourceView> m_srv;
        ComPtr<ID3D11UnorderedAccessView> m_uav;
        std::vector<ComPtr<ID3D11RenderTargetView>> m_rtvs;
        ComPtr<ID3D11DepthStencilView> m_dsv;
    };

    // =============================================================================
    // DX11 Texture View (Phase 2)
    // =============================================================================
    class DX11TextureView : public RHITextureView
    {
    public:
        DX11TextureView(DX11Device* device, RHITexture* texture, const RHITextureViewDesc& desc);
        ~DX11TextureView() override;

        // RHITextureView interface
        RHITexture* GetTexture() const override { return m_texture; }
        RHIFormat GetFormat() const override { return m_format; }
        const RHISubresourceRange& GetSubresourceRange() const override { return m_subresourceRange; }

        // DX11 Specific
        ID3D11ShaderResourceView* GetSRV() const { return m_srv.Get(); }
        ID3D11UnorderedAccessView* GetUAV() const { return m_uav.Get(); }
        ID3D11RenderTargetView* GetRTV() const { return m_rtv.Get(); }
        ID3D11DepthStencilView* GetDSV() const { return m_dsv.Get(); }

    private:
        DX11Device* m_device = nullptr;
        RHITexture* m_texture = nullptr;
        RHIFormat m_format = RHIFormat::Unknown;
        RHISubresourceRange m_subresourceRange;

        ComPtr<ID3D11ShaderResourceView> m_srv;
        ComPtr<ID3D11UnorderedAccessView> m_uav;
        ComPtr<ID3D11RenderTargetView> m_rtv;
        ComPtr<ID3D11DepthStencilView> m_dsv;
    };

    // =============================================================================
    // DX11 Sampler (Phase 2)
    // =============================================================================
    class DX11Sampler : public RHISampler
    {
    public:
        DX11Sampler(DX11Device* device, const RHISamplerDesc& desc);
        ~DX11Sampler() override;

        ID3D11SamplerState* GetSampler() const { return m_sampler.Get(); }

    private:
        ComPtr<ID3D11SamplerState> m_sampler;
    };

    // =============================================================================
    // DX11 Shader (Phase 2)
    // =============================================================================
    class DX11Shader : public RHIShader
    {
    public:
        DX11Shader(DX11Device* device, const RHIShaderDesc& desc);
        ~DX11Shader() override;

        // RHIShader interface
        RHIShaderStage GetStage() const override { return m_stage; }
        const std::vector<uint8>& GetBytecode() const override { return m_bytecode; }

        // DX11 Specific
        ID3D11VertexShader* GetVertexShader() const { return m_vertexShader.Get(); }
        ID3D11PixelShader* GetPixelShader() const { return m_pixelShader.Get(); }
        ID3D11GeometryShader* GetGeometryShader() const { return m_geometryShader.Get(); }
        ID3D11HullShader* GetHullShader() const { return m_hullShader.Get(); }
        ID3D11DomainShader* GetDomainShader() const { return m_domainShader.Get(); }
        ID3D11ComputeShader* GetComputeShader() const { return m_computeShader.Get(); }

    private:
        RHIShaderStage m_stage = RHIShaderStage::None;
        std::vector<uint8> m_bytecode;

        ComPtr<ID3D11VertexShader> m_vertexShader;
        ComPtr<ID3D11PixelShader> m_pixelShader;
        ComPtr<ID3D11GeometryShader> m_geometryShader;
        ComPtr<ID3D11HullShader> m_hullShader;
        ComPtr<ID3D11DomainShader> m_domainShader;
        ComPtr<ID3D11ComputeShader> m_computeShader;
    };

    // =============================================================================
    // DX11 Fence (Phase 2)
    // =============================================================================
    // DX11 Query Pool
    // =============================================================================
    class DX11QueryPool : public RHIQueryPool
    {
    public:
        DX11QueryPool(DX11Device* device, const RHIQueryPoolDesc& desc);
        ~DX11QueryPool() override;

        // RHIQueryPool interface
        RHIQueryType GetType() const override { return m_type; }
        uint32 GetCount() const override { return m_count; }
        uint64 GetTimestampFrequency() const override { return m_timestampFrequency; }

        // DX11 Specific
        ID3D11Query* GetQuery(uint32 index) const;
        ID3D11Predicate* GetPredicate(uint32 index) const;

    private:
        DX11Device* m_device = nullptr;
        RHIQueryType m_type = RHIQueryType::Occlusion;
        uint32 m_count = 0;
        uint64 m_timestampFrequency = 0;
        std::vector<ComPtr<ID3D11Query>> m_queries;
    };

    // =============================================================================
    // DX11 Fence
    // =============================================================================
    class DX11Fence : public RHIFence
    {
    public:
        DX11Fence(DX11Device* device, uint64 initialValue);
        ~DX11Fence() override;

        // RHIFence interface
        uint64 GetCompletedValue() const override;
        void Signal(uint64 value) override;
        void SignalOnQueue(uint64 value, RHICommandQueueType queueType) override;
        void Wait(uint64 value, uint64 timeoutNs) override;

        // DX11 Specific
        ID3D11Fence* GetFence() const { return m_fence.Get(); }
        bool HasNativeFence() const { return m_fence != nullptr; }

    private:
        DX11Device* m_device = nullptr;
        uint64 m_value = 0;

        ComPtr<ID3D11Fence> m_fence;
        HANDLE m_event = nullptr;

        // Fallback for systems without ID3D11Fence
        ComPtr<ID3D11Query> m_query;
    };

} // namespace RVX
