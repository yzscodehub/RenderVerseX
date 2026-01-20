#include "DX11Resources.h"
#include "DX11Device.h"
#include "DX11Conversions.h"

namespace RVX
{
    // =============================================================================
    // DX11 Buffer Implementation
    // =============================================================================
    DX11Buffer::DX11Buffer(DX11Device* device, const RHIBufferDesc& desc)
        : m_device(device)
        , m_desc(desc)
    {
        D3D11_BUFFER_DESC bufferDesc = {};
        bufferDesc.ByteWidth = static_cast<UINT>(desc.size);
        bufferDesc.Usage = ToD3D11Usage(desc.memoryType);
        bufferDesc.BindFlags = ToD3D11BindFlags(desc.usage);
        bufferDesc.CPUAccessFlags = ToD3D11CPUAccessFlags(desc.memoryType);
        bufferDesc.MiscFlags = 0;
        bufferDesc.StructureByteStride = desc.stride;

        // Structured buffer needs StructureByteStride and MiscFlags
        if (HasFlag(desc.usage, RHIBufferUsage::Structured) && desc.stride > 0)
        {
            bufferDesc.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        }

        // IndirectArgs buffer
        if (HasFlag(desc.usage, RHIBufferUsage::IndirectArgs))
        {
            bufferDesc.MiscFlags |= D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        }

        // ByteAddressBuffer (raw buffer)
        if (HasFlag(desc.usage, RHIBufferUsage::ShaderResource) && desc.stride == 0 &&
            !HasFlag(desc.usage, RHIBufferUsage::Constant))
        {
            bufferDesc.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        }

        // Staging buffer for readback (can't have bind flags)
        if (desc.memoryType == RHIMemoryType::Readback)
        {
            bufferDesc.BindFlags = 0;
            bufferDesc.MiscFlags = 0;
        }

        HRESULT hr = device->GetD3DDevice()->CreateBuffer(&bufferDesc, nullptr, &m_buffer);
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("DX11: Failed to create buffer: {}", HRESULTToString(hr));
            return;
        }

        // Set debug name
        if (desc.debugName && desc.debugName[0])
        {
            DX11_SET_DEBUG_NAME(m_buffer.Get(), desc.debugName);
            SetDebugName(desc.debugName);
        }

        // Create views if needed
        CreateViews();

        RVX_RHI_DEBUG("DX11: Created buffer '{}' size={} bytes", 
            desc.debugName ? desc.debugName : "", desc.size);
    }

    DX11Buffer::~DX11Buffer()
    {
    }

    void* DX11Buffer::Map()
    {
        if (!m_buffer) return nullptr;

        D3D11_MAP mapType;
        switch (m_desc.memoryType)
        {
            case RHIMemoryType::Upload:
                mapType = D3D11_MAP_WRITE_DISCARD;
                break;
            case RHIMemoryType::Readback:
                mapType = D3D11_MAP_READ;
                break;
            default:
                RVX_RHI_ERROR("DX11: Cannot map buffer with Default memory type");
                return nullptr;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = m_device->GetImmediateContext()->Map(
            m_buffer.Get(), 0, mapType, 0, &mapped);

        if (FAILED(hr))
        {
            RVX_RHI_ERROR("DX11: Failed to map buffer: {}", HRESULTToString(hr));
            return nullptr;
        }

        return mapped.pData;
    }

    void DX11Buffer::Unmap()
    {
        if (m_buffer)
        {
            m_device->GetImmediateContext()->Unmap(m_buffer.Get(), 0);
        }
    }

    void DX11Buffer::CreateViews()
    {
        if (!m_buffer) return;

        auto* d3dDevice = m_device->GetD3DDevice();

        // Create SRV for ShaderResource or Structured buffers
        if (HasFlag(m_desc.usage, RHIBufferUsage::ShaderResource) ||
            HasFlag(m_desc.usage, RHIBufferUsage::Structured))
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            
            if (HasFlag(m_desc.usage, RHIBufferUsage::Structured) && m_desc.stride > 0)
            {
                // Structured buffer
                srvDesc.Format = DXGI_FORMAT_UNKNOWN;
                srvDesc.Buffer.FirstElement = 0;
                srvDesc.Buffer.NumElements = static_cast<UINT>(m_desc.size / m_desc.stride);
            }
            else
            {
                // Raw buffer (ByteAddressBuffer)
                srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
                srvDesc.BufferEx.FirstElement = 0;
                srvDesc.BufferEx.NumElements = static_cast<UINT>(m_desc.size / 4);
                srvDesc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            }

            HRESULT hr = d3dDevice->CreateShaderResourceView(m_buffer.Get(), &srvDesc, &m_srv);
            if (FAILED(hr))
            {
                RVX_RHI_WARN("DX11: Failed to create buffer SRV: {}", HRESULTToString(hr));
            }
        }

        // Create UAV for UnorderedAccess buffers
        if (HasFlag(m_desc.usage, RHIBufferUsage::UnorderedAccess))
        {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;

            if (HasFlag(m_desc.usage, RHIBufferUsage::Structured) && m_desc.stride > 0)
            {
                // Structured buffer UAV
                uavDesc.Format = DXGI_FORMAT_UNKNOWN;
                uavDesc.Buffer.FirstElement = 0;
                uavDesc.Buffer.NumElements = static_cast<UINT>(m_desc.size / m_desc.stride);
            }
            else
            {
                // Raw buffer UAV
                uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
                uavDesc.Buffer.FirstElement = 0;
                uavDesc.Buffer.NumElements = static_cast<UINT>(m_desc.size / 4);
                uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            }

            HRESULT hr = d3dDevice->CreateUnorderedAccessView(m_buffer.Get(), &uavDesc, &m_uav);
            if (FAILED(hr))
            {
                RVX_RHI_WARN("DX11: Failed to create buffer UAV: {}", HRESULTToString(hr));
            }
        }
    }

    // =============================================================================
    // DX11 Texture Implementation
    // =============================================================================
    DX11Texture::DX11Texture(DX11Device* device, const RHITextureDesc& desc)
        : m_device(device)
        , m_desc(desc)
    {
        m_dxgiFormat = ToDXGIFormat(desc.format);

        // Use typeless format for depth textures that need SRV
        bool isDepthFormat = (desc.format == RHIFormat::D16_UNORM ||
                              desc.format == RHIFormat::D24_UNORM_S8_UINT ||
                              desc.format == RHIFormat::D32_FLOAT ||
                              desc.format == RHIFormat::D32_FLOAT_S8_UINT);

        DXGI_FORMAT resourceFormat = m_dxgiFormat;
        if (isDepthFormat && HasFlag(desc.usage, RHITextureUsage::ShaderResource))
        {
            resourceFormat = GetTypelessFormat(m_dxgiFormat);
        }

        UINT bindFlags = ToD3D11BindFlags(desc.usage);
        UINT miscFlags = 0;

        // Generate mipmaps
        if (desc.mipLevels > 1 && HasFlag(desc.usage, RHITextureUsage::RenderTarget))
        {
            miscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
        }

        // Cube texture
        if (desc.dimension == RHITextureDimension::TextureCube)
        {
            miscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
        }

        auto* d3dDevice = device->GetD3DDevice();
        HRESULT hr = E_FAIL;

        switch (desc.dimension)
        {
            case RHITextureDimension::Texture1D:
            {
                D3D11_TEXTURE1D_DESC tex1dDesc = {};
                tex1dDesc.Width = desc.width;
                tex1dDesc.MipLevels = desc.mipLevels;
                tex1dDesc.ArraySize = desc.arraySize;
                tex1dDesc.Format = resourceFormat;
                tex1dDesc.Usage = D3D11_USAGE_DEFAULT;
                tex1dDesc.BindFlags = bindFlags;
                tex1dDesc.CPUAccessFlags = 0;
                tex1dDesc.MiscFlags = miscFlags;

                ComPtr<ID3D11Texture1D> tex1d;
                hr = d3dDevice->CreateTexture1D(&tex1dDesc, nullptr, &tex1d);
                if (SUCCEEDED(hr))
                {
                    m_resource = tex1d;
                }
                break;
            }

            case RHITextureDimension::Texture2D:
            case RHITextureDimension::TextureCube:
            {
                D3D11_TEXTURE2D_DESC tex2dDesc = {};
                tex2dDesc.Width = desc.width;
                tex2dDesc.Height = desc.height;
                tex2dDesc.MipLevels = desc.mipLevels;
                tex2dDesc.ArraySize = (desc.dimension == RHITextureDimension::TextureCube) 
                    ? (desc.arraySize * 6) : desc.arraySize;
                tex2dDesc.Format = resourceFormat;
                tex2dDesc.SampleDesc.Count = static_cast<UINT>(desc.sampleCount);
                tex2dDesc.SampleDesc.Quality = 0;
                tex2dDesc.Usage = D3D11_USAGE_DEFAULT;
                tex2dDesc.BindFlags = bindFlags;
                tex2dDesc.CPUAccessFlags = 0;
                tex2dDesc.MiscFlags = miscFlags;

                ComPtr<ID3D11Texture2D> tex2d;
                hr = d3dDevice->CreateTexture2D(&tex2dDesc, nullptr, &tex2d);
                if (SUCCEEDED(hr))
                {
                    m_resource = tex2d;
                }
                break;
            }

            case RHITextureDimension::Texture3D:
            {
                D3D11_TEXTURE3D_DESC tex3dDesc = {};
                tex3dDesc.Width = desc.width;
                tex3dDesc.Height = desc.height;
                tex3dDesc.Depth = desc.depth;
                tex3dDesc.MipLevels = desc.mipLevels;
                tex3dDesc.Format = resourceFormat;
                tex3dDesc.Usage = D3D11_USAGE_DEFAULT;
                tex3dDesc.BindFlags = bindFlags;
                tex3dDesc.CPUAccessFlags = 0;
                tex3dDesc.MiscFlags = miscFlags;

                ComPtr<ID3D11Texture3D> tex3d;
                hr = d3dDevice->CreateTexture3D(&tex3dDesc, nullptr, &tex3d);
                if (SUCCEEDED(hr))
                {
                    m_resource = tex3d;
                }
                break;
            }
        }

        if (FAILED(hr) || !m_resource)
        {
            RVX_RHI_ERROR("DX11: Failed to create texture: {}", HRESULTToString(hr));
            return;
        }

        // Set debug name
        if (desc.debugName && desc.debugName[0])
        {
            DX11_SET_DEBUG_NAME(m_resource.Get(), desc.debugName);
            SetDebugName(desc.debugName);
        }

        CreateViews();

        RVX_RHI_DEBUG("DX11: Created texture '{}' {}x{}x{} mips={}", 
            desc.debugName ? desc.debugName : "", desc.width, desc.height, desc.depth, desc.mipLevels);
    }

    DX11Texture::DX11Texture(DX11Device* device, ComPtr<ID3D11Texture2D> texture, const RHITextureDesc& desc)
        : m_device(device)
        , m_desc(desc)
        , m_ownsResource(false)
    {
        m_resource = texture;
        m_dxgiFormat = ToDXGIFormat(desc.format);
        CreateViews();
    }

    DX11Texture::~DX11Texture()
    {
    }

    ID3D11Texture2D* DX11Texture::GetTexture2D() const
    {
        ComPtr<ID3D11Texture2D> tex2d;
        if (m_resource && SUCCEEDED(m_resource.As(&tex2d)))
        {
            return tex2d.Get();
        }
        return nullptr;
    }

    ID3D11RenderTargetView* DX11Texture::GetRTV(uint32 index) const
    {
        if (index < m_rtvs.size())
        {
            return m_rtvs[index].Get();
        }
        return nullptr;
    }

    void DX11Texture::CreateViews()
    {
        if (!m_resource) return;

        auto* d3dDevice = m_device->GetD3DDevice();

        bool isDepthFormat = (m_desc.format == RHIFormat::D16_UNORM ||
                              m_desc.format == RHIFormat::D24_UNORM_S8_UINT ||
                              m_desc.format == RHIFormat::D32_FLOAT ||
                              m_desc.format == RHIFormat::D32_FLOAT_S8_UINT);

        // Create SRV
        if (HasFlag(m_desc.usage, RHITextureUsage::ShaderResource))
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = isDepthFormat ? GetDepthSRVFormat(m_dxgiFormat) : m_dxgiFormat;

            switch (m_desc.dimension)
            {
                case RHITextureDimension::Texture1D:
                    if (m_desc.arraySize > 1)
                    {
                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
                        srvDesc.Texture1DArray.MipLevels = m_desc.mipLevels;
                        srvDesc.Texture1DArray.ArraySize = m_desc.arraySize;
                    }
                    else
                    {
                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
                        srvDesc.Texture1D.MipLevels = m_desc.mipLevels;
                    }
                    break;

                case RHITextureDimension::Texture2D:
                    if (m_desc.sampleCount != RHISampleCount::Count1)
                    {
                        if (m_desc.arraySize > 1)
                        {
                            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
                            srvDesc.Texture2DMSArray.ArraySize = m_desc.arraySize;
                        }
                        else
                        {
                            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
                        }
                    }
                    else if (m_desc.arraySize > 1)
                    {
                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                        srvDesc.Texture2DArray.MipLevels = m_desc.mipLevels;
                        srvDesc.Texture2DArray.ArraySize = m_desc.arraySize;
                    }
                    else
                    {
                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                        srvDesc.Texture2D.MipLevels = m_desc.mipLevels;
                    }
                    break;

                case RHITextureDimension::Texture3D:
                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
                    srvDesc.Texture3D.MipLevels = m_desc.mipLevels;
                    break;

                case RHITextureDimension::TextureCube:
                    if (m_desc.arraySize > 1)
                    {
                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
                        srvDesc.TextureCubeArray.MipLevels = m_desc.mipLevels;
                        srvDesc.TextureCubeArray.NumCubes = m_desc.arraySize;
                    }
                    else
                    {
                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
                        srvDesc.TextureCube.MipLevels = m_desc.mipLevels;
                    }
                    break;
            }

            HRESULT hr = d3dDevice->CreateShaderResourceView(m_resource.Get(), &srvDesc, &m_srv);
            if (FAILED(hr))
            {
                RVX_RHI_WARN("DX11: Failed to create texture SRV: {}", HRESULTToString(hr));
            }
        }

        // Create RTV(s) for render targets
        if (HasFlag(m_desc.usage, RHITextureUsage::RenderTarget))
        {
            uint32 rtvCount = m_desc.arraySize * m_desc.mipLevels;
            if (m_desc.dimension == RHITextureDimension::TextureCube)
            {
                rtvCount = m_desc.arraySize * 6 * m_desc.mipLevels;
            }

            m_rtvs.resize(rtvCount);

            for (uint32 arraySlice = 0; arraySlice < m_desc.arraySize; ++arraySlice)
            {
                for (uint32 mipLevel = 0; mipLevel < m_desc.mipLevels; ++mipLevel)
                {
                    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
                    rtvDesc.Format = m_dxgiFormat;

                    uint32 rtvIndex = arraySlice * m_desc.mipLevels + mipLevel;

                    switch (m_desc.dimension)
                    {
                        case RHITextureDimension::Texture1D:
                            if (m_desc.arraySize > 1)
                            {
                                rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1DARRAY;
                                rtvDesc.Texture1DArray.MipSlice = mipLevel;
                                rtvDesc.Texture1DArray.FirstArraySlice = arraySlice;
                                rtvDesc.Texture1DArray.ArraySize = 1;
                            }
                            else
                            {
                                rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1D;
                                rtvDesc.Texture1D.MipSlice = mipLevel;
                            }
                            break;

                        case RHITextureDimension::Texture2D:
                        case RHITextureDimension::TextureCube:
                            if (m_desc.sampleCount != RHISampleCount::Count1)
                            {
                                if (m_desc.arraySize > 1)
                                {
                                    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
                                    rtvDesc.Texture2DMSArray.FirstArraySlice = arraySlice;
                                    rtvDesc.Texture2DMSArray.ArraySize = 1;
                                }
                                else
                                {
                                    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
                                }
                            }
                            else if (m_desc.arraySize > 1 || m_desc.dimension == RHITextureDimension::TextureCube)
                            {
                                rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                                rtvDesc.Texture2DArray.MipSlice = mipLevel;
                                rtvDesc.Texture2DArray.FirstArraySlice = arraySlice;
                                rtvDesc.Texture2DArray.ArraySize = 1;
                            }
                            else
                            {
                                rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                                rtvDesc.Texture2D.MipSlice = mipLevel;
                            }
                            break;

                        case RHITextureDimension::Texture3D:
                            rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
                            rtvDesc.Texture3D.MipSlice = mipLevel;
                            rtvDesc.Texture3D.FirstWSlice = 0;
                            rtvDesc.Texture3D.WSize = m_desc.depth >> mipLevel;
                            break;
                    }

                    HRESULT hr = d3dDevice->CreateRenderTargetView(m_resource.Get(), &rtvDesc, &m_rtvs[rtvIndex]);
                    if (FAILED(hr))
                    {
                        RVX_RHI_WARN("DX11: Failed to create texture RTV: {}", HRESULTToString(hr));
                    }
                }
            }
        }

        // Create DSV for depth stencil
        if (HasFlag(m_desc.usage, RHITextureUsage::DepthStencil))
        {
            D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
            dsvDesc.Format = m_dxgiFormat;
            dsvDesc.Flags = 0;

            switch (m_desc.dimension)
            {
                case RHITextureDimension::Texture1D:
                    if (m_desc.arraySize > 1)
                    {
                        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1DARRAY;
                        dsvDesc.Texture1DArray.ArraySize = m_desc.arraySize;
                    }
                    else
                    {
                        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1D;
                    }
                    break;

                case RHITextureDimension::Texture2D:
                case RHITextureDimension::TextureCube:
                    if (m_desc.sampleCount != RHISampleCount::Count1)
                    {
                        if (m_desc.arraySize > 1)
                        {
                            dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
                            dsvDesc.Texture2DMSArray.ArraySize = m_desc.arraySize;
                        }
                        else
                        {
                            dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
                        }
                    }
                    else if (m_desc.arraySize > 1 || m_desc.dimension == RHITextureDimension::TextureCube)
                    {
                        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
                        dsvDesc.Texture2DArray.ArraySize = m_desc.arraySize;
                    }
                    else
                    {
                        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                    }
                    break;

                default:
                    break;
            }

            HRESULT hr = d3dDevice->CreateDepthStencilView(m_resource.Get(), &dsvDesc, &m_dsv);
            if (FAILED(hr))
            {
                RVX_RHI_WARN("DX11: Failed to create texture DSV: {}", HRESULTToString(hr));
            }
        }

        // Create UAV for unordered access
        if (HasFlag(m_desc.usage, RHITextureUsage::UnorderedAccess))
        {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = m_dxgiFormat;

            switch (m_desc.dimension)
            {
                case RHITextureDimension::Texture1D:
                    if (m_desc.arraySize > 1)
                    {
                        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1DARRAY;
                        uavDesc.Texture1DArray.ArraySize = m_desc.arraySize;
                    }
                    else
                    {
                        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1D;
                    }
                    break;

                case RHITextureDimension::Texture2D:
                case RHITextureDimension::TextureCube:
                    if (m_desc.arraySize > 1 || m_desc.dimension == RHITextureDimension::TextureCube)
                    {
                        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
                        uavDesc.Texture2DArray.ArraySize = m_desc.arraySize;
                    }
                    else
                    {
                        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                    }
                    break;

                case RHITextureDimension::Texture3D:
                    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
                    uavDesc.Texture3D.WSize = m_desc.depth;
                    break;
            }

            HRESULT hr = d3dDevice->CreateUnorderedAccessView(m_resource.Get(), &uavDesc, &m_uav);
            if (FAILED(hr))
            {
                RVX_RHI_WARN("DX11: Failed to create texture UAV: {}", HRESULTToString(hr));
            }
        }
    }

    // =============================================================================
    // DX11 Texture View Implementation
    // =============================================================================
    DX11TextureView::DX11TextureView(DX11Device* device, RHITexture* texture, const RHITextureViewDesc& desc)
        : m_device(device)
        , m_texture(texture)
        , m_format(desc.format == RHIFormat::Unknown ? texture->GetFormat() : desc.format)
        , m_subresourceRange(desc.subresourceRange)
    {
        auto* dx11Texture = static_cast<DX11Texture*>(texture);
        if (!dx11Texture || !dx11Texture->GetResource())
        {
            RVX_RHI_ERROR("DX11: Invalid texture for view creation");
            return;
        }

        auto* d3dDevice = device->GetD3DDevice();
        DXGI_FORMAT viewFormat = ToDXGIFormat(m_format);

        bool isDepthFormat = (m_format == RHIFormat::D16_UNORM ||
                              m_format == RHIFormat::D24_UNORM_S8_UINT ||
                              m_format == RHIFormat::D32_FLOAT ||
                              m_format == RHIFormat::D32_FLOAT_S8_UINT);

        RHITextureUsage usage = texture->GetUsage();
        RHITextureDimension dimension = texture->GetDimension();

        // Create SRV
        if (HasFlag(usage, RHITextureUsage::ShaderResource))
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = isDepthFormat ? GetDepthSRVFormat(viewFormat) : viewFormat;

            uint32 baseMip = m_subresourceRange.baseMipLevel;
            uint32 mipCount = m_subresourceRange.mipLevelCount;
            uint32 baseArray = m_subresourceRange.baseArrayLayer;
            uint32 arraySize = m_subresourceRange.arrayLayerCount;

            switch (dimension)
            {
                case RHITextureDimension::Texture1D:
                    if (arraySize > 1)
                    {
                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
                        srvDesc.Texture1DArray.MostDetailedMip = baseMip;
                        srvDesc.Texture1DArray.MipLevels = mipCount;
                        srvDesc.Texture1DArray.FirstArraySlice = baseArray;
                        srvDesc.Texture1DArray.ArraySize = arraySize;
                    }
                    else
                    {
                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
                        srvDesc.Texture1D.MostDetailedMip = baseMip;
                        srvDesc.Texture1D.MipLevels = mipCount;
                    }
                    break;

                case RHITextureDimension::Texture2D:
                    if (texture->GetSampleCount() != RHISampleCount::Count1)
                    {
                        if (arraySize > 1)
                        {
                            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
                            srvDesc.Texture2DMSArray.FirstArraySlice = baseArray;
                            srvDesc.Texture2DMSArray.ArraySize = arraySize;
                        }
                        else
                        {
                            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
                        }
                    }
                    else if (arraySize > 1)
                    {
                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                        srvDesc.Texture2DArray.MostDetailedMip = baseMip;
                        srvDesc.Texture2DArray.MipLevels = mipCount;
                        srvDesc.Texture2DArray.FirstArraySlice = baseArray;
                        srvDesc.Texture2DArray.ArraySize = arraySize;
                    }
                    else
                    {
                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                        srvDesc.Texture2D.MostDetailedMip = baseMip;
                        srvDesc.Texture2D.MipLevels = mipCount;
                    }
                    break;

                case RHITextureDimension::Texture3D:
                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
                    srvDesc.Texture3D.MostDetailedMip = baseMip;
                    srvDesc.Texture3D.MipLevels = mipCount;
                    break;

                case RHITextureDimension::TextureCube:
                    if (texture->GetArraySize() > 1)
                    {
                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
                        srvDesc.TextureCubeArray.MostDetailedMip = baseMip;
                        srvDesc.TextureCubeArray.MipLevels = mipCount;
                        srvDesc.TextureCubeArray.First2DArrayFace = baseArray;
                        srvDesc.TextureCubeArray.NumCubes = arraySize / 6;
                    }
                    else
                    {
                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
                        srvDesc.TextureCube.MostDetailedMip = baseMip;
                        srvDesc.TextureCube.MipLevels = mipCount;
                    }
                    break;
            }

            HRESULT hr = d3dDevice->CreateShaderResourceView(
                dx11Texture->GetResource(), &srvDesc, &m_srv);
            if (FAILED(hr))
            {
                RVX_RHI_WARN("DX11: Failed to create texture view SRV: {}", HRESULTToString(hr));
            }
        }

        // Create RTV if render target
        if (HasFlag(usage, RHITextureUsage::RenderTarget))
        {
            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
            rtvDesc.Format = viewFormat;

            switch (dimension)
            {
                case RHITextureDimension::Texture2D:
                case RHITextureDimension::TextureCube:
                    if (m_subresourceRange.arrayLayerCount > 1 || dimension == RHITextureDimension::TextureCube)
                    {
                        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                        rtvDesc.Texture2DArray.MipSlice = m_subresourceRange.baseMipLevel;
                        rtvDesc.Texture2DArray.FirstArraySlice = m_subresourceRange.baseArrayLayer;
                        rtvDesc.Texture2DArray.ArraySize = m_subresourceRange.arrayLayerCount;
                    }
                    else
                    {
                        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                        rtvDesc.Texture2D.MipSlice = m_subresourceRange.baseMipLevel;
                    }
                    break;
                default:
                    break;
            }

            HRESULT hr = d3dDevice->CreateRenderTargetView(
                dx11Texture->GetResource(), &rtvDesc, &m_rtv);
            if (FAILED(hr))
            {
                RVX_RHI_WARN("DX11: Failed to create texture view RTV: {}", HRESULTToString(hr));
            }
            else
            {
                RVX_RHI_DEBUG("DX11: Created texture view RTV for format {}", static_cast<int>(m_format));
            }
        }
        else
        {
            RVX_RHI_DEBUG("DX11: Skipping RTV creation - texture usage does not include RenderTarget (usage={})", 
                static_cast<uint32>(usage));
        }

        // Create DSV if depth stencil
        if (HasFlag(usage, RHITextureUsage::DepthStencil))
        {
            D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
            dsvDesc.Format = viewFormat;
            dsvDesc.Flags = 0;

            switch (dimension)
            {
                case RHITextureDimension::Texture2D:
                    if (m_subresourceRange.arrayLayerCount > 1)
                    {
                        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
                        dsvDesc.Texture2DArray.MipSlice = m_subresourceRange.baseMipLevel;
                        dsvDesc.Texture2DArray.FirstArraySlice = m_subresourceRange.baseArrayLayer;
                        dsvDesc.Texture2DArray.ArraySize = m_subresourceRange.arrayLayerCount;
                    }
                    else
                    {
                        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                        dsvDesc.Texture2D.MipSlice = m_subresourceRange.baseMipLevel;
                    }
                    break;
                default:
                    break;
            }

            HRESULT hr = d3dDevice->CreateDepthStencilView(
                dx11Texture->GetResource(), &dsvDesc, &m_dsv);
            if (FAILED(hr))
            {
                RVX_RHI_WARN("DX11: Failed to create texture view DSV: {}", HRESULTToString(hr));
            }
        }

        // Create UAV if unordered access
        if (HasFlag(usage, RHITextureUsage::UnorderedAccess))
        {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = viewFormat;

            switch (dimension)
            {
                case RHITextureDimension::Texture2D:
                case RHITextureDimension::TextureCube:
                    if (m_subresourceRange.arrayLayerCount > 1 || dimension == RHITextureDimension::TextureCube)
                    {
                        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
                        uavDesc.Texture2DArray.MipSlice = m_subresourceRange.baseMipLevel;
                        uavDesc.Texture2DArray.FirstArraySlice = m_subresourceRange.baseArrayLayer;
                        uavDesc.Texture2DArray.ArraySize = m_subresourceRange.arrayLayerCount;
                    }
                    else
                    {
                        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                        uavDesc.Texture2D.MipSlice = m_subresourceRange.baseMipLevel;
                    }
                    break;

                case RHITextureDimension::Texture3D:
                    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
                    uavDesc.Texture3D.MipSlice = m_subresourceRange.baseMipLevel;
                    uavDesc.Texture3D.FirstWSlice = 0;
                    uavDesc.Texture3D.WSize = texture->GetDepth() >> m_subresourceRange.baseMipLevel;
                    break;

                default:
                    break;
            }

            HRESULT hr = d3dDevice->CreateUnorderedAccessView(
                dx11Texture->GetResource(), &uavDesc, &m_uav);
            if (FAILED(hr))
            {
                RVX_RHI_WARN("DX11: Failed to create texture view UAV: {}", HRESULTToString(hr));
            }
        }
    }

    DX11TextureView::~DX11TextureView()
    {
    }

    // =============================================================================
    // DX11 Sampler Implementation
    // =============================================================================
    DX11Sampler::DX11Sampler(DX11Device* device, const RHISamplerDesc& desc)
    {
        D3D11_SAMPLER_DESC samplerDesc = {};
        
        bool anisotropic = desc.maxAnisotropy > 1;
        samplerDesc.Filter = ToD3D11Filter(desc.minFilter, desc.magFilter, desc.mipFilter, anisotropic);
        samplerDesc.AddressU = ToD3D11AddressMode(desc.addressU);
        samplerDesc.AddressV = ToD3D11AddressMode(desc.addressV);
        samplerDesc.AddressW = ToD3D11AddressMode(desc.addressW);
        samplerDesc.MipLODBias = desc.mipLodBias;
        samplerDesc.MaxAnisotropy = static_cast<UINT>(desc.maxAnisotropy);
        samplerDesc.ComparisonFunc = desc.compareEnable ? ToD3D11ComparisonFunc(desc.compareOp) : D3D11_COMPARISON_NEVER;
        samplerDesc.MinLOD = desc.minLod;
        samplerDesc.MaxLOD = desc.maxLod;

        // Border color
        samplerDesc.BorderColor[0] = desc.borderColor[0];
        samplerDesc.BorderColor[1] = desc.borderColor[1];
        samplerDesc.BorderColor[2] = desc.borderColor[2];
        samplerDesc.BorderColor[3] = desc.borderColor[3];

        HRESULT hr = device->GetD3DDevice()->CreateSamplerState(&samplerDesc, &m_sampler);
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("DX11: Failed to create sampler: {}", HRESULTToString(hr));
        }
    }

    DX11Sampler::~DX11Sampler()
    {
    }

    // =============================================================================
    // DX11 Shader Implementation
    // =============================================================================
    DX11Shader::DX11Shader(DX11Device* device, const RHIShaderDesc& desc)
        : m_stage(desc.stage)
    {
        if (desc.bytecode && desc.bytecodeSize > 0)
        {
            m_bytecode.resize(desc.bytecodeSize);
            memcpy(m_bytecode.data(), desc.bytecode, desc.bytecodeSize);
        }
        else
        {
            RVX_RHI_ERROR("DX11: Shader bytecode is required");
            return;
        }

        auto* d3dDevice = device->GetD3DDevice();
        HRESULT hr = E_FAIL;

        switch (desc.stage)
        {
            case RHIShaderStage::Vertex:
                hr = d3dDevice->CreateVertexShader(
                    m_bytecode.data(), m_bytecode.size(), nullptr, &m_vertexShader);
                break;

            case RHIShaderStage::Pixel:
                hr = d3dDevice->CreatePixelShader(
                    m_bytecode.data(), m_bytecode.size(), nullptr, &m_pixelShader);
                break;

            case RHIShaderStage::Geometry:
                hr = d3dDevice->CreateGeometryShader(
                    m_bytecode.data(), m_bytecode.size(), nullptr, &m_geometryShader);
                break;

            case RHIShaderStage::Hull:
                hr = d3dDevice->CreateHullShader(
                    m_bytecode.data(), m_bytecode.size(), nullptr, &m_hullShader);
                break;

            case RHIShaderStage::Domain:
                hr = d3dDevice->CreateDomainShader(
                    m_bytecode.data(), m_bytecode.size(), nullptr, &m_domainShader);
                break;

            case RHIShaderStage::Compute:
                hr = d3dDevice->CreateComputeShader(
                    m_bytecode.data(), m_bytecode.size(), nullptr, &m_computeShader);
                break;

            default:
                RVX_RHI_ERROR("DX11: Unknown shader stage: {}", static_cast<int>(desc.stage));
                return;
        }

        if (FAILED(hr))
        {
            RVX_RHI_ERROR("DX11: Failed to create shader: {}", HRESULTToString(hr));
            return;
        }

        RVX_RHI_DEBUG("DX11: Created {} shader '{}'",
            desc.stage == RHIShaderStage::Vertex ? "vertex" :
            desc.stage == RHIShaderStage::Pixel ? "pixel" :
            desc.stage == RHIShaderStage::Compute ? "compute" : "other",
            desc.entryPoint);
    }

    DX11Shader::~DX11Shader()
    {
    }

    // =============================================================================
    // DX11 Fence Implementation
    // =============================================================================
    DX11Fence::DX11Fence(DX11Device* device, uint64 initialValue)
        : m_device(device)
        , m_value(initialValue)
    {
        // Try to create ID3D11Fence (Win10+)
        if (auto device5 = device->GetD3DDevice5())
        {
            HRESULT hr = device5->CreateFence(
                initialValue,
                D3D11_FENCE_FLAG_NONE,
                IID_PPV_ARGS(&m_fence)
            );

            if (SUCCEEDED(hr))
            {
                m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                RVX_RHI_DEBUG("DX11: Using native ID3D11Fence");
                return;
            }
        }

        // Fallback to query-based fence
        RVX_RHI_DEBUG("DX11: Using query-based fence (legacy)");
        D3D11_QUERY_DESC queryDesc = {};
        queryDesc.Query = D3D11_QUERY_EVENT;
        device->GetD3DDevice()->CreateQuery(&queryDesc, &m_query);
    }

    DX11Fence::~DX11Fence()
    {
        if (m_event)
        {
            CloseHandle(m_event);
        }
    }

    uint64 DX11Fence::GetCompletedValue() const
    {
        if (m_fence)
        {
            return m_fence->GetCompletedValue();
        }
        return m_value;
    }

    void DX11Fence::Signal(uint64 value)
    {
        m_value = value;
        if (m_fence)
        {
            // Need ID3D11DeviceContext4 to signal fence
            ComPtr<ID3D11DeviceContext4> context4;
            if (SUCCEEDED(m_device->GetImmediateContext()->QueryInterface(IID_PPV_ARGS(&context4))))
            {
                context4->Signal(m_fence.Get(), value);
            }
        }
        else if (m_query)
        {
            m_device->GetImmediateContext()->End(m_query.Get());
        }
    }

    void DX11Fence::Wait(uint64 value, uint64 timeoutNs)
    {
        if (m_fence)
        {
            if (m_fence->GetCompletedValue() < value)
            {
                m_fence->SetEventOnCompletion(value, m_event);
                DWORD timeoutMs = (timeoutNs == UINT64_MAX) ? INFINITE :
                                  static_cast<DWORD>(timeoutNs / 1000000);
                WaitForSingleObject(m_event, timeoutMs);
            }
        }
        else if (m_query)
        {
            BOOL result = FALSE;
            auto* context = m_device->GetImmediateContext();
            while (context->GetData(m_query.Get(), &result, sizeof(result), 0) == S_FALSE)
            {
                std::this_thread::yield();
            }
        }
    }

} // namespace RVX
