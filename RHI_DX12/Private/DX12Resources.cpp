#include "DX12Resources.h"
#include "DX12Device.h"

namespace RVX
{
    // =============================================================================
    // DX12 Buffer Implementation
    // =============================================================================
    DX12Buffer::DX12Buffer(DX12Device* device, const RHIBufferDesc& desc)
        : m_device(device)
        , m_desc(desc)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        auto d3dDevice = device->GetD3DDevice();

        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Alignment = 0;
        resourceDesc.Width = desc.size;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.SampleDesc.Quality = 0;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        if (HasFlag(desc.usage, RHIBufferUsage::UnorderedAccess))
        {
            resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }

        D3D12_HEAP_TYPE heapType = ToD3D12HeapType(desc.memoryType);

        D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
        if (desc.memoryType == RHIMemoryType::Upload)
        {
            initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
        }
        else if (desc.memoryType == RHIMemoryType::Readback)
        {
            initialState = D3D12_RESOURCE_STATE_COPY_DEST;
        }

        #ifdef RVX_USE_D3D12MA
        if (device->GetMemoryAllocator())
        {
            D3D12MA::ALLOCATION_DESC allocDesc = {};
            allocDesc.HeapType = heapType;

            DX12_CHECK(device->GetMemoryAllocator()->CreateResource(
                &allocDesc,
                &resourceDesc,
                initialState,
                nullptr,
                &m_allocation,
                IID_PPV_ARGS(&m_resource)));
        }
        else
        #endif
        {
            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = heapType;
            heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

            DX12_CHECK(d3dDevice->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &resourceDesc,
                initialState,
                nullptr,
                IID_PPV_ARGS(&m_resource)));
        }

        if (desc.debugName)
        {
            wchar_t wname[256];
            MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, wname, 256);
            m_resource->SetName(wname);
        }

        CreateViews();

        // For upload buffers, use persistent mapping for efficient per-frame updates
        if (desc.memoryType == RHIMemoryType::Upload)
        {
            D3D12_RANGE readRange = {0, 0};  // CPU doesn't read from upload buffers
            HRESULT hr = m_resource->Map(0, &readRange, &m_mappedData);
            if (FAILED(hr))
            {
                RVX_RHI_ERROR("Failed to persistently map upload buffer: 0x{:08X}", static_cast<uint32>(hr));
                m_mappedData = nullptr;
            }
        }
    }

    DX12Buffer::~DX12Buffer()
    {
        // Unmap persistently mapped buffers
        if (m_mappedData && m_desc.memoryType == RHIMemoryType::Upload)
        {
            D3D12_RANGE writtenRange = {0, m_desc.size};
            m_resource->Unmap(0, &writtenRange);
            m_mappedData = nullptr;
        }
        else if (m_mappedData)
        {
            Unmap();
        }

        auto& heapManager = m_device->GetDescriptorHeapManager();

        if (m_cbvHandle.IsValid())
            heapManager.FreeCbvSrvUav(m_cbvHandle);
        if (m_srvHandle.IsValid())
            heapManager.FreeCbvSrvUav(m_srvHandle);
        if (m_uavHandle.IsValid())
            heapManager.FreeCbvSrvUav(m_uavHandle);
    }

    void DX12Buffer::CreateViews()
    {
        auto d3dDevice = m_device->GetD3DDevice();
        auto& heapManager = m_device->GetDescriptorHeapManager();

        // CBV
        if (HasFlag(m_desc.usage, RHIBufferUsage::Constant))
        {
            m_cbvHandle = heapManager.AllocateCbvSrvUav();

            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
            cbvDesc.BufferLocation = m_resource->GetGPUVirtualAddress();
            cbvDesc.SizeInBytes = static_cast<UINT>((m_desc.size + 255) & ~255);  // Align to 256 bytes

            d3dDevice->CreateConstantBufferView(&cbvDesc, m_cbvHandle.cpuHandle);
        }

        // SRV (for structured/typed buffers)
        if (HasFlag(m_desc.usage, RHIBufferUsage::ShaderResource) ||
            HasFlag(m_desc.usage, RHIBufferUsage::Structured))
        {
            m_srvHandle = heapManager.AllocateCbvSrvUav();

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

            if (HasFlag(m_desc.usage, RHIBufferUsage::Structured) && m_desc.stride > 0)
            {
                srvDesc.Format = DXGI_FORMAT_UNKNOWN;
                srvDesc.Buffer.FirstElement = 0;
                srvDesc.Buffer.NumElements = static_cast<UINT>(m_desc.size / m_desc.stride);
                srvDesc.Buffer.StructureByteStride = m_desc.stride;
                srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
            }
            else
            {
                srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
                srvDesc.Buffer.FirstElement = 0;
                srvDesc.Buffer.NumElements = static_cast<UINT>(m_desc.size / 4);
                srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            }

            d3dDevice->CreateShaderResourceView(m_resource.Get(), &srvDesc, m_srvHandle.cpuHandle);
        }

        // UAV
        if (HasFlag(m_desc.usage, RHIBufferUsage::UnorderedAccess))
        {
            m_uavHandle = heapManager.AllocateCbvSrvUav();

            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;

            if (HasFlag(m_desc.usage, RHIBufferUsage::Structured) && m_desc.stride > 0)
            {
                uavDesc.Format = DXGI_FORMAT_UNKNOWN;
                uavDesc.Buffer.FirstElement = 0;
                uavDesc.Buffer.NumElements = static_cast<UINT>(m_desc.size / m_desc.stride);
                uavDesc.Buffer.StructureByteStride = m_desc.stride;
                uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
            }
            else
            {
                uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
                uavDesc.Buffer.FirstElement = 0;
                uavDesc.Buffer.NumElements = static_cast<UINT>(m_desc.size / 4);
                uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            }

            d3dDevice->CreateUnorderedAccessView(m_resource.Get(), nullptr, &uavDesc, m_uavHandle.cpuHandle);
        }
    }

    void* DX12Buffer::Map()
    {
        // For upload buffers, return persistent mapping
        if (m_mappedData)
            return m_mappedData;

        if (m_desc.memoryType == RHIMemoryType::Default)
        {
            RVX_RHI_ERROR("Cannot map GPU-only buffer");
            return nullptr;
        }

        // For readback buffers, map on demand
        if (m_desc.memoryType == RHIMemoryType::Readback)
        {
            D3D12_RANGE readRange = {0, m_desc.size};
            HRESULT hr = m_resource->Map(0, &readRange, &m_mappedData);
            if (FAILED(hr))
            {
                RVX_RHI_ERROR("Failed to map readback buffer: 0x{:08X}", static_cast<uint32>(hr));
                return nullptr;
            }
        }

        return m_mappedData;
    }

    void DX12Buffer::Unmap()
    {
        if (!m_mappedData)
            return;

        // Upload buffers stay persistently mapped, only unmap readback buffers
        if (m_desc.memoryType == RHIMemoryType::Readback)
        {
            D3D12_RANGE writtenRange = {0, 0};  // CPU doesn't write to readback
            m_resource->Unmap(0, &writtenRange);
            m_mappedData = nullptr;
        }
        // Upload buffers: no-op, stay mapped
    }

    // =============================================================================
    // DX12 Texture Implementation
    // =============================================================================
    DX12Texture::DX12Texture(DX12Device* device, const RHITextureDesc& desc)
        : m_device(device)
        , m_desc(desc)
        , m_dxgiFormat(ToDXGIFormat(desc.format))
        , m_ownsResource(true)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        auto d3dDevice = device->GetD3DDevice();

        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Alignment = 0;
        resourceDesc.Width = desc.width;
        resourceDesc.Height = desc.height;
        resourceDesc.MipLevels = static_cast<UINT16>(desc.mipLevels);
        resourceDesc.Format = m_dxgiFormat;
        resourceDesc.SampleDesc.Count = static_cast<UINT>(desc.sampleCount);
        resourceDesc.SampleDesc.Quality = 0;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        // Use typeless format for depth if SRV is needed
        if (IsDepthFormat(desc.format) && HasFlag(desc.usage, RHITextureUsage::ShaderResource))
        {
            resourceDesc.Format = GetTypelessFormat(m_dxgiFormat);
        }

        switch (desc.dimension)
        {
            case RHITextureDimension::Texture1D:
                resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
                resourceDesc.DepthOrArraySize = static_cast<UINT16>(desc.arraySize);
                break;
            case RHITextureDimension::Texture2D:
            case RHITextureDimension::TextureCube:
                resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                resourceDesc.DepthOrArraySize = static_cast<UINT16>(desc.arraySize);
                break;
            case RHITextureDimension::Texture3D:
                resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
                resourceDesc.DepthOrArraySize = static_cast<UINT16>(desc.depth);
                break;
        }

        if (HasFlag(desc.usage, RHITextureUsage::RenderTarget))
        {
            resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        }
        if (HasFlag(desc.usage, RHITextureUsage::DepthStencil))
        {
            resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        }
        if (HasFlag(desc.usage, RHITextureUsage::UnorderedAccess))
        {
            resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }

        D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;

        D3D12_CLEAR_VALUE clearValue = {};
        D3D12_CLEAR_VALUE* pClearValue = nullptr;

        if (HasFlag(desc.usage, RHITextureUsage::RenderTarget))
        {
            clearValue.Format = m_dxgiFormat;
            clearValue.Color[0] = 0.0f;
            clearValue.Color[1] = 0.0f;
            clearValue.Color[2] = 0.0f;
            clearValue.Color[3] = 1.0f;
            pClearValue = &clearValue;
        }
        else if (HasFlag(desc.usage, RHITextureUsage::DepthStencil))
        {
            clearValue.Format = m_dxgiFormat;
            clearValue.DepthStencil.Depth = 1.0f;
            clearValue.DepthStencil.Stencil = 0;
            pClearValue = &clearValue;
            initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        }

        #ifdef RVX_USE_D3D12MA
        if (device->GetMemoryAllocator())
        {
            D3D12MA::ALLOCATION_DESC allocDesc = {};
            allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

            DX12_CHECK(device->GetMemoryAllocator()->CreateResource(
                &allocDesc,
                &resourceDesc,
                initialState,
                pClearValue,
                &m_allocation,
                IID_PPV_ARGS(&m_resource)));
        }
        else
        #endif
        {
            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

            DX12_CHECK(d3dDevice->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &resourceDesc,
                initialState,
                pClearValue,
                IID_PPV_ARGS(&m_resource)));
        }

        if (desc.debugName)
        {
            wchar_t wname[256];
            MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, wname, 256);
            m_resource->SetName(wname);
        }

        CreateViews();
    }

    DX12Texture::DX12Texture(DX12Device* device, ComPtr<ID3D12Resource> resource, const RHITextureDesc& desc)
        : m_device(device)
        , m_desc(desc)
        , m_dxgiFormat(ToDXGIFormat(desc.format))
        , m_resource(resource)
        , m_ownsResource(false)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        CreateViews();
    }

    DX12Texture::~DX12Texture()
    {
        auto& heapManager = m_device->GetDescriptorHeapManager();

        if (m_srvHandle.IsValid())
            heapManager.FreeCbvSrvUav(m_srvHandle);
        if (m_uavHandle.IsValid())
            heapManager.FreeCbvSrvUav(m_uavHandle);
        for (auto& rtvHandle : m_rtvHandles)
        {
            if (rtvHandle.IsValid())
                heapManager.FreeRTV(rtvHandle);
        }
        if (m_dsvHandle.IsValid())
            heapManager.FreeDSV(m_dsvHandle);
    }

    void DX12Texture::CreateViews()
    {
        auto d3dDevice = m_device->GetD3DDevice();
        auto& heapManager = m_device->GetDescriptorHeapManager();

        // SRV
        if (HasFlag(m_desc.usage, RHITextureUsage::ShaderResource) || !m_ownsResource)
        {
            m_srvHandle = heapManager.AllocateCbvSrvUav();

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

            // Use depth SRV format for depth textures
            if (IsDepthFormat(m_desc.format))
            {
                srvDesc.Format = GetDepthSRVFormat(m_dxgiFormat);
            }
            else
            {
                srvDesc.Format = m_dxgiFormat;
            }

            switch (m_desc.dimension)
            {
                case RHITextureDimension::Texture1D:
                    if (m_desc.arraySize > 1)
                    {
                        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
                        srvDesc.Texture1DArray.MipLevels = m_desc.mipLevels;
                        srvDesc.Texture1DArray.ArraySize = m_desc.arraySize;
                    }
                    else
                    {
                        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                        srvDesc.Texture1D.MipLevels = m_desc.mipLevels;
                    }
                    break;

                case RHITextureDimension::Texture2D:
                    if (m_desc.arraySize > 1)
                    {
                        if (static_cast<uint32>(m_desc.sampleCount) > 1)
                        {
                            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
                            srvDesc.Texture2DMSArray.ArraySize = m_desc.arraySize;
                        }
                        else
                        {
                            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                            srvDesc.Texture2DArray.MipLevels = m_desc.mipLevels;
                            srvDesc.Texture2DArray.ArraySize = m_desc.arraySize;
                        }
                    }
                    else
                    {
                        if (static_cast<uint32>(m_desc.sampleCount) > 1)
                        {
                            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
                        }
                        else
                        {
                            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                            srvDesc.Texture2D.MipLevels = m_desc.mipLevels;
                        }
                    }
                    break;

                case RHITextureDimension::TextureCube:
                    if (m_desc.arraySize > 6)
                    {
                        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                        srvDesc.TextureCubeArray.MipLevels = m_desc.mipLevels;
                        srvDesc.TextureCubeArray.NumCubes = m_desc.arraySize / 6;
                    }
                    else
                    {
                        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                        srvDesc.TextureCube.MipLevels = m_desc.mipLevels;
                    }
                    break;

                case RHITextureDimension::Texture3D:
                    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                    srvDesc.Texture3D.MipLevels = m_desc.mipLevels;
                    break;
            }

            d3dDevice->CreateShaderResourceView(m_resource.Get(), &srvDesc, m_srvHandle.cpuHandle);
        }

        // UAV
        if (HasFlag(m_desc.usage, RHITextureUsage::UnorderedAccess))
        {
            m_uavHandle = heapManager.AllocateCbvSrvUav();

            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = m_dxgiFormat;

            switch (m_desc.dimension)
            {
                case RHITextureDimension::Texture1D:
                    if (m_desc.arraySize > 1)
                    {
                        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
                        uavDesc.Texture1DArray.ArraySize = m_desc.arraySize;
                    }
                    else
                    {
                        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
                    }
                    break;

                case RHITextureDimension::Texture2D:
                case RHITextureDimension::TextureCube:
                    if (m_desc.arraySize > 1)
                    {
                        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                        uavDesc.Texture2DArray.ArraySize = m_desc.arraySize;
                    }
                    else
                    {
                        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    }
                    break;

                case RHITextureDimension::Texture3D:
                    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                    uavDesc.Texture3D.WSize = m_desc.depth;
                    break;
            }

            d3dDevice->CreateUnorderedAccessView(m_resource.Get(), nullptr, &uavDesc, m_uavHandle.cpuHandle);
        }

        // RTV
        if (HasFlag(m_desc.usage, RHITextureUsage::RenderTarget))
        {
            uint32 rtvCount = m_desc.arraySize;
            m_rtvHandles.resize(rtvCount);

            for (uint32 i = 0; i < rtvCount; ++i)
            {
                m_rtvHandles[i] = heapManager.AllocateRTV();

                D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
                rtvDesc.Format = m_dxgiFormat;

                if (m_desc.arraySize > 1)
                {
                    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                    rtvDesc.Texture2DArray.FirstArraySlice = i;
                    rtvDesc.Texture2DArray.ArraySize = 1;
                }
                else
                {
                    if (static_cast<uint32>(m_desc.sampleCount) > 1)
                    {
                        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
                    }
                    else
                    {
                        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                    }
                }

                d3dDevice->CreateRenderTargetView(m_resource.Get(), &rtvDesc, m_rtvHandles[i].cpuHandle);
            }
        }

        // DSV
        if (HasFlag(m_desc.usage, RHITextureUsage::DepthStencil))
        {
            m_dsvHandle = heapManager.AllocateDSV();

            D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
            dsvDesc.Format = m_dxgiFormat;
            dsvDesc.Flags = D3D12_DSV_FLAG_NONE;

            if (static_cast<uint32>(m_desc.sampleCount) > 1)
            {
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
            }
            else
            {
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            }

            d3dDevice->CreateDepthStencilView(m_resource.Get(), &dsvDesc, m_dsvHandle.cpuHandle);
        }
    }

    // =============================================================================
    // DX12 Texture View Implementation
    // =============================================================================
    DX12TextureView::DX12TextureView(DX12Device* device, RHITexture* texture, const RHITextureViewDesc& desc)
        : m_device(device)
        , m_texture(texture)
        , m_format(desc.format == RHIFormat::Unknown ? texture->GetFormat() : desc.format)
        , m_subresourceRange(desc.subresourceRange)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        auto d3dDevice = device->GetD3DDevice();
        auto& heapManager = device->GetDescriptorHeapManager();
        auto* dx12Texture = static_cast<DX12Texture*>(texture);

        DXGI_FORMAT dxgiFormat = ToDXGIFormat(m_format);
        RHITextureUsage usage = texture->GetUsage();

        // Create SRV for shader resource textures
        if (HasFlag(usage, RHITextureUsage::ShaderResource) || 
            HasFlag(usage, RHITextureUsage::RenderTarget))  // RT textures may also be sampled
        {
            DXGI_FORMAT srvFormat = dxgiFormat;
            if (IsDepthFormat(m_format))
            {
                srvFormat = GetDepthSRVFormat(dxgiFormat);
            }

            m_srvHandle = heapManager.AllocateCbvSrvUav();

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = srvFormat;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MostDetailedMip = desc.subresourceRange.baseMipLevel;
            srvDesc.Texture2D.MipLevels = desc.subresourceRange.mipLevelCount == RVX_ALL_MIPS
                ? texture->GetMipLevels() - desc.subresourceRange.baseMipLevel
                : desc.subresourceRange.mipLevelCount;

            d3dDevice->CreateShaderResourceView(dx12Texture->GetResource(), &srvDesc, m_srvHandle.cpuHandle);
        }

        // Create RTV for render target textures
        if (HasFlag(usage, RHITextureUsage::RenderTarget))
        {
            m_rtvHandle = heapManager.AllocateRTV();

            D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
            rtvDesc.Format = dxgiFormat;
            
            if (texture->GetArraySize() > 1)
            {
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rtvDesc.Texture2DArray.MipSlice = desc.subresourceRange.baseMipLevel;
                rtvDesc.Texture2DArray.FirstArraySlice = desc.subresourceRange.baseArrayLayer;
                rtvDesc.Texture2DArray.ArraySize = desc.subresourceRange.arrayLayerCount == RVX_ALL_LAYERS
                    ? texture->GetArraySize() - desc.subresourceRange.baseArrayLayer
                    : desc.subresourceRange.arrayLayerCount;
            }
            else
            {
                if (static_cast<uint32>(texture->GetSampleCount()) > 1)
                {
                    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
                }
                else
                {
                    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                    rtvDesc.Texture2D.MipSlice = desc.subresourceRange.baseMipLevel;
                }
            }

            d3dDevice->CreateRenderTargetView(dx12Texture->GetResource(), &rtvDesc, m_rtvHandle.cpuHandle);
        }

        // Create DSV for depth stencil textures
        if (HasFlag(usage, RHITextureUsage::DepthStencil))
        {
            m_dsvHandle = heapManager.AllocateDSV();

            D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
            dsvDesc.Format = dxgiFormat;
            dsvDesc.Flags = D3D12_DSV_FLAG_NONE;

            if (static_cast<uint32>(texture->GetSampleCount()) > 1)
            {
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
            }
            else
            {
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                dsvDesc.Texture2D.MipSlice = desc.subresourceRange.baseMipLevel;
            }

            d3dDevice->CreateDepthStencilView(dx12Texture->GetResource(), &dsvDesc, m_dsvHandle.cpuHandle);
        }

        // Create UAV for unordered access textures
        if (HasFlag(usage, RHITextureUsage::UnorderedAccess))
        {
            m_uavHandle = heapManager.AllocateCbvSrvUav();

            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = dxgiFormat;

            if (texture->GetArraySize() > 1)
            {
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                uavDesc.Texture2DArray.MipSlice = desc.subresourceRange.baseMipLevel;
                uavDesc.Texture2DArray.FirstArraySlice = desc.subresourceRange.baseArrayLayer;
                uavDesc.Texture2DArray.ArraySize = desc.subresourceRange.arrayLayerCount == RVX_ALL_LAYERS
                    ? texture->GetArraySize() - desc.subresourceRange.baseArrayLayer
                    : desc.subresourceRange.arrayLayerCount;
            }
            else
            {
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uavDesc.Texture2D.MipSlice = desc.subresourceRange.baseMipLevel;
            }

            d3dDevice->CreateUnorderedAccessView(dx12Texture->GetResource(), nullptr, &uavDesc, m_uavHandle.cpuHandle);
        }
    }

    DX12TextureView::~DX12TextureView()
    {
        auto& heapManager = m_device->GetDescriptorHeapManager();

        if (m_srvHandle.IsValid())
            heapManager.FreeCbvSrvUav(m_srvHandle);
        if (m_uavHandle.IsValid())
            heapManager.FreeCbvSrvUav(m_uavHandle);
        if (m_rtvHandle.IsValid())
            heapManager.FreeRTV(m_rtvHandle);
        if (m_dsvHandle.IsValid())
            heapManager.FreeDSV(m_dsvHandle);
    }

    // =============================================================================
    // DX12 Sampler Implementation
    // =============================================================================
    DX12Sampler::DX12Sampler(DX12Device* device, const RHISamplerDesc& desc)
        : m_device(device)
    {
        auto d3dDevice = device->GetD3DDevice();
        auto& heapManager = device->GetDescriptorHeapManager();

        m_handle = heapManager.AllocateSampler();

        auto toD3D12Filter = [](RHIFilterMode min, RHIFilterMode mag, RHIFilterMode mip, bool anisotropic) -> D3D12_FILTER {
            if (anisotropic) return D3D12_FILTER_ANISOTROPIC;
            
            int filter = 0;
            if (min == RHIFilterMode::Linear) filter |= 0x10;
            if (mag == RHIFilterMode::Linear) filter |= 0x04;
            if (mip == RHIFilterMode::Linear) filter |= 0x01;
            return static_cast<D3D12_FILTER>(filter);
        };

        auto toD3D12AddressMode = [](RHIAddressMode mode) -> D3D12_TEXTURE_ADDRESS_MODE {
            switch (mode)
            {
                case RHIAddressMode::Repeat:       return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                case RHIAddressMode::MirrorRepeat: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
                case RHIAddressMode::ClampToEdge:  return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                case RHIAddressMode::ClampToBorder: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
                default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            }
        };

        D3D12_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = toD3D12Filter(desc.minFilter, desc.magFilter, desc.mipFilter, desc.anisotropyEnable);
        samplerDesc.AddressU = toD3D12AddressMode(desc.addressU);
        samplerDesc.AddressV = toD3D12AddressMode(desc.addressV);
        samplerDesc.AddressW = toD3D12AddressMode(desc.addressW);
        samplerDesc.MipLODBias = desc.mipLodBias;
        samplerDesc.MaxAnisotropy = static_cast<UINT>(desc.maxAnisotropy);
        samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samplerDesc.MinLOD = desc.minLod;
        samplerDesc.MaxLOD = desc.maxLod;

        d3dDevice->CreateSampler(&samplerDesc, m_handle.cpuHandle);
    }

    DX12Sampler::~DX12Sampler()
    {
        if (m_handle.IsValid())
        {
            m_device->GetDescriptorHeapManager().FreeSampler(m_handle);
        }
    }

    // =============================================================================
    // DX12 Shader Implementation
    // =============================================================================
    DX12Shader::DX12Shader(DX12Device* device, const RHIShaderDesc& desc)
        : m_stage(desc.stage)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        m_bytecode.resize(desc.bytecodeSize);
        std::memcpy(m_bytecode.data(), desc.bytecode, desc.bytecodeSize);
    }

    DX12Shader::~DX12Shader() = default;

    // =============================================================================
    // DX12 Fence Implementation
    // =============================================================================
    DX12Fence::DX12Fence(DX12Device* device, uint64 initialValue)
        : m_device(device)
    {
        DX12_CHECK(device->GetD3DDevice()->CreateFence(initialValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
        m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    DX12Fence::~DX12Fence()
    {
        if (m_event)
        {
            CloseHandle(m_event);
        }
    }

    uint64 DX12Fence::GetCompletedValue() const
    {
        return m_fence->GetCompletedValue();
    }

    void DX12Fence::Signal(uint64 value)
    {
        m_device->GetGraphicsQueue()->Signal(m_fence.Get(), value);
    }

    void DX12Fence::Wait(uint64 value, uint64 timeoutNs)
    {
        if (m_fence->GetCompletedValue() < value)
        {
            m_fence->SetEventOnCompletion(value, m_event);
            DWORD timeoutMs = (timeoutNs == UINT64_MAX) ? INFINITE : static_cast<DWORD>(timeoutNs / 1000000);
            WaitForSingleObjectEx(m_event, timeoutMs, FALSE);
        }
    }

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIBufferRef CreateDX12Buffer(DX12Device* device, const RHIBufferDesc& desc)
    {
        return Ref<DX12Buffer>(new DX12Buffer(device, desc));
    }

    RHITextureRef CreateDX12Texture(DX12Device* device, const RHITextureDesc& desc)
    {
        return Ref<DX12Texture>(new DX12Texture(device, desc));
    }

    RHITextureRef CreateDX12TextureFromResource(DX12Device* device, ComPtr<ID3D12Resource> resource, const RHITextureDesc& desc)
    {
        return Ref<DX12Texture>(new DX12Texture(device, resource, desc));
    }

    RHITextureViewRef CreateDX12TextureView(DX12Device* device, RHITexture* texture, const RHITextureViewDesc& desc)
    {
        return Ref<DX12TextureView>(new DX12TextureView(device, texture, desc));
    }

    RHISamplerRef CreateDX12Sampler(DX12Device* device, const RHISamplerDesc& desc)
    {
        return Ref<DX12Sampler>(new DX12Sampler(device, desc));
    }

    RHIShaderRef CreateDX12Shader(DX12Device* device, const RHIShaderDesc& desc)
    {
        return Ref<DX12Shader>(new DX12Shader(device, desc));
    }

    RHIFenceRef CreateDX12Fence(DX12Device* device, uint64 initialValue)
    {
        return Ref<DX12Fence>(new DX12Fence(device, initialValue));
    }

    void WaitForDX12Fence(DX12Device* device, RHIFence* fence, uint64 value)
    {
        auto* dx12Fence = static_cast<DX12Fence*>(fence);
        dx12Fence->Wait(value, UINT64_MAX);
    }

    // =============================================================================
    // DX12 Heap Implementation (for Memory Aliasing)
    // =============================================================================
    DX12Heap::DX12Heap(DX12Device* device, const RHIHeapDesc& desc)
        : m_device(device)
        , m_size(desc.size)
        , m_type(desc.type)
        , m_flags(desc.flags)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        D3D12_HEAP_DESC heapDesc = {};
        heapDesc.SizeInBytes = desc.size;
        heapDesc.Alignment = desc.alignment > 0 ? desc.alignment : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

        // Set heap type
        switch (desc.type)
        {
            case RHIHeapType::Default:
                heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
                break;
            case RHIHeapType::Upload:
                heapDesc.Properties.Type = D3D12_HEAP_TYPE_UPLOAD;
                break;
            case RHIHeapType::Readback:
                heapDesc.Properties.Type = D3D12_HEAP_TYPE_READBACK;
                break;
        }

        // Set heap flags based on allowed resource types
        heapDesc.Flags = D3D12_HEAP_FLAG_NONE;
        
        bool allowTextures = HasFlag(desc.flags, RHIHeapFlags::AllowTextures);
        bool allowBuffers = HasFlag(desc.flags, RHIHeapFlags::AllowBuffers);
        bool allowRT = HasFlag(desc.flags, RHIHeapFlags::AllowRenderTargets);
        bool allowDS = HasFlag(desc.flags, RHIHeapFlags::AllowDepthStencil);

        // D3D12 heap tier 1 requires separate heaps for buffers and textures
        // For simplicity, we use ALLOW_ALL if both are requested
        if (allowTextures && allowBuffers)
        {
            heapDesc.Flags |= D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;
        }
        else if (allowBuffers)
        {
            heapDesc.Flags |= D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        }
        else if (allowTextures)
        {
            if (allowRT || allowDS)
            {
                heapDesc.Flags |= D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
            }
            else
            {
                heapDesc.Flags |= D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
            }
        }

        HRESULT hr = device->GetD3DDevice()->CreateHeap(&heapDesc, IID_PPV_ARGS(&m_heap));
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("Failed to create DX12 Heap: 0x{:08X}", static_cast<uint32>(hr));
            return;
        }

        if (desc.debugName)
        {
            wchar_t wname[256];
            MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, wname, 256);
            m_heap->SetName(wname);
        }

        RVX_RHI_DEBUG("Created DX12 Heap: {} bytes", desc.size);
    }

    DX12Heap::~DX12Heap()
    {
        // D3D12 heap is released automatically via ComPtr
    }

    RHIHeapRef CreateDX12Heap(DX12Device* device, const RHIHeapDesc& desc)
    {
        auto heap = Ref<DX12Heap>(new DX12Heap(device, desc));
        if (!heap->GetHeap())
        {
            return nullptr;
        }
        return heap;
    }

    // =============================================================================
    // DX12 Placed Texture Implementation
    // =============================================================================
    RHITextureRef CreateDX12PlacedTexture(DX12Device* device, RHIHeap* heap, uint64 offset, const RHITextureDesc& desc)
    {
        auto* dx12Heap = static_cast<DX12Heap*>(heap);
        if (!dx12Heap || !dx12Heap->GetHeap())
        {
            RVX_RHI_ERROR("Invalid heap for placed texture");
            return nullptr;
        }

        DXGI_FORMAT dxgiFormat = ToDXGIFormat(desc.format);

        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Alignment = 0;
        resourceDesc.Width = desc.width;
        resourceDesc.Height = desc.height;
        resourceDesc.MipLevels = static_cast<UINT16>(desc.mipLevels);
        resourceDesc.Format = dxgiFormat;
        resourceDesc.SampleDesc.Count = static_cast<UINT>(desc.sampleCount);
        resourceDesc.SampleDesc.Quality = 0;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        // Use typeless format for depth if SRV is needed
        if (IsDepthFormat(desc.format) && HasFlag(desc.usage, RHITextureUsage::ShaderResource))
        {
            resourceDesc.Format = GetTypelessFormat(dxgiFormat);
        }

        switch (desc.dimension)
        {
            case RHITextureDimension::Texture1D:
                resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
                resourceDesc.DepthOrArraySize = static_cast<UINT16>(desc.arraySize);
                break;
            case RHITextureDimension::Texture2D:
            case RHITextureDimension::TextureCube:
                resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                resourceDesc.DepthOrArraySize = static_cast<UINT16>(desc.arraySize);
                break;
            case RHITextureDimension::Texture3D:
                resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
                resourceDesc.DepthOrArraySize = static_cast<UINT16>(desc.depth);
                break;
        }

        if (HasFlag(desc.usage, RHITextureUsage::RenderTarget))
        {
            resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        }
        if (HasFlag(desc.usage, RHITextureUsage::DepthStencil))
        {
            resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        }
        if (HasFlag(desc.usage, RHITextureUsage::UnorderedAccess))
        {
            resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }

        D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;

        D3D12_CLEAR_VALUE clearValue = {};
        D3D12_CLEAR_VALUE* pClearValue = nullptr;

        if (HasFlag(desc.usage, RHITextureUsage::RenderTarget))
        {
            clearValue.Format = dxgiFormat;
            clearValue.Color[0] = 0.0f;
            clearValue.Color[1] = 0.0f;
            clearValue.Color[2] = 0.0f;
            clearValue.Color[3] = 1.0f;
            pClearValue = &clearValue;
        }
        else if (HasFlag(desc.usage, RHITextureUsage::DepthStencil))
        {
            clearValue.Format = dxgiFormat;
            clearValue.DepthStencil.Depth = 1.0f;
            clearValue.DepthStencil.Stencil = 0;
            pClearValue = &clearValue;
            initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        }

        ComPtr<ID3D12Resource> resource;
        HRESULT hr = device->GetD3DDevice()->CreatePlacedResource(
            dx12Heap->GetHeap(),
            offset,
            &resourceDesc,
            initialState,
            pClearValue,
            IID_PPV_ARGS(&resource));

        if (FAILED(hr))
        {
            RVX_RHI_ERROR("Failed to create placed texture: 0x{:08X}", static_cast<uint32>(hr));
            return nullptr;
        }

        if (desc.debugName)
        {
            wchar_t wname[256];
            MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, wname, 256);
            resource->SetName(wname);
        }

        // Use the existing constructor that accepts a pre-created resource
        return Ref<DX12Texture>(new DX12Texture(device, resource, desc));
    }

    // =============================================================================
    // DX12 Placed Buffer Implementation
    // =============================================================================
    RHIBufferRef CreateDX12PlacedBuffer(DX12Device* device, RHIHeap* heap, uint64 offset, const RHIBufferDesc& desc)
    {
        auto* dx12Heap = static_cast<DX12Heap*>(heap);
        if (!dx12Heap || !dx12Heap->GetHeap())
        {
            RVX_RHI_ERROR("Invalid heap for placed buffer");
            return nullptr;
        }

        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Alignment = 0;
        resourceDesc.Width = desc.size;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.SampleDesc.Quality = 0;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        if (HasFlag(desc.usage, RHIBufferUsage::UnorderedAccess))
        {
            resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }

        D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
        if (desc.memoryType == RHIMemoryType::Upload)
        {
            initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
        }
        else if (desc.memoryType == RHIMemoryType::Readback)
        {
            initialState = D3D12_RESOURCE_STATE_COPY_DEST;
        }

        ComPtr<ID3D12Resource> resource;
        HRESULT hr = device->GetD3DDevice()->CreatePlacedResource(
            dx12Heap->GetHeap(),
            offset,
            &resourceDesc,
            initialState,
            nullptr,
            IID_PPV_ARGS(&resource));

        if (FAILED(hr))
        {
            RVX_RHI_ERROR("Failed to create placed buffer: 0x{:08X}", static_cast<uint32>(hr));
            return nullptr;
        }

        if (desc.debugName)
        {
            wchar_t wname[256];
            MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, wname, 256);
            resource->SetName(wname);
        }

        // Note: For placed buffers, we need a special constructor or factory
        // For now, we create a wrapper that uses the pre-created resource
        // This requires extending DX12Buffer to accept external resources
        // TODO: Add DX12Buffer constructor that accepts ComPtr<ID3D12Resource>
        
        // Temporary: Fall back to committed resource (no aliasing)
        RVX_RHI_WARN("Placed buffer creation not fully implemented, using committed resource");
        return CreateDX12Buffer(device, desc);
    }

} // namespace RVX
