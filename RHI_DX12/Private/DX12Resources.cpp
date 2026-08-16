#include "DX12Resources.h"
#include "DX12Device.h"
#include "RHI/RHITexture.h"

namespace RVX
{
    namespace
    {
        constexpr uint64 RVX_DX12_MAX_CBV_SIZE = D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16ull;

        uint64 AlignCBVSize(uint64 size)
        {
            return (size + 255ull) & ~255ull;
        }

        bool IsTexture2DArrayViewRequired(const RHITexture& texture)
        {
            return texture.GetDimension() == RHITextureDimension::Texture2D &&
                   GetTexturePhysicalLayerCount(texture) > 1;
        }

        const D3D12_CLEAR_VALUE* BuildDX12OptimizedClearValue(
            const RHITextureDesc& desc,
            DXGI_FORMAT format,
            D3D12_CLEAR_VALUE& clearValue)
        {
            clearValue = {};
            switch (desc.optimizedClearValue.type)
            {
                case RHIOptimizedClearValueType::None:
                    return nullptr;
                case RHIOptimizedClearValueType::Color:
                    clearValue.Format = format;
                    clearValue.Color[0] = desc.optimizedClearValue.color.r;
                    clearValue.Color[1] = desc.optimizedClearValue.color.g;
                    clearValue.Color[2] = desc.optimizedClearValue.color.b;
                    clearValue.Color[3] = desc.optimizedClearValue.color.a;
                    return &clearValue;
                case RHIOptimizedClearValueType::DepthStencil:
                    clearValue.Format = format;
                    clearValue.DepthStencil.Depth =
                        desc.optimizedClearValue.depthStencil.depth;
                    clearValue.DepthStencil.Stencil =
                        desc.optimizedClearValue.depthStencil.stencil;
                    return &clearValue;
            }
            return nullptr;
        }
    } // namespace

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

        if (HasFlag(desc.usage, RHIBufferUsage::UnorderedAccess) ||
            HasFlag(desc.usage, RHIBufferUsage::AccelerationStructureStorage))
        {
            resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }

        D3D12_HEAP_TYPE heapType = ToD3D12HeapType(desc.memoryType);

        D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
        if (HasFlag(desc.usage, RHIBufferUsage::AccelerationStructureStorage))
        {
            initialState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
        }
        else if (desc.memoryType == RHIMemoryType::Upload)
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
                m_device->QueryRuntimeStatus();
                m_mappedData = nullptr;
            }
        }
    }

    DX12Buffer::DX12Buffer(DX12Device* device, ComPtr<ID3D12Resource> resource, const RHIBufferDesc& desc, bool ownsResource)
        : m_device(device)
        , m_desc(desc)
        , m_resource(resource)
        , m_ownsResource(ownsResource)
    {
        // NOTE: For Placed Resources, the Heap type must match the buffer memory type:
        // - Upload buffer requires D3D12_HEAP_TYPE_UPLOAD heap
        // - Readback buffer requires D3D12_HEAP_TYPE_READBACK heap
        // - Default buffer requires D3D12_HEAP_TYPE_DEFAULT heap
        // CreatePlacedResource will fail if there's a mismatch, so validation is implicit.

        if (desc.debugName)
        {
            SetDebugName(desc.debugName);

            wchar_t wname[256];
            MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, wname, 256);
            m_resource->SetName(wname);
        }

        CreateViews();

        // Upload buffers: persistent mapping for efficient per-frame updates
        // Placed resources support mapping if bound to host-visible heap (Upload/Readback)
        if (desc.memoryType == RHIMemoryType::Upload)
        {
            D3D12_RANGE readRange = {0, 0};  // CPU doesn't read from upload buffers
            HRESULT hr = m_resource->Map(0, &readRange, &m_mappedData);
            if (FAILED(hr))
            {
                RVX_RHI_ERROR("Failed to persistently map placed upload buffer: 0x{:08X}", static_cast<uint32>(hr));
                m_device->QueryRuntimeStatus();
                m_mappedData = nullptr;
            }
        }
        // Note: Readback buffers are mapped on-demand in Map() method
    }

    DX12Buffer::~DX12Buffer()
    {
        // Unmap persistently mapped buffers
        if (m_mappedData && m_desc.memoryType == RHIMemoryType::Upload &&
            IsHostAccessReady())
        {
            D3D12_RANGE writtenRange = {0, m_desc.size};
            m_resource->Unmap(0, &writtenRange);
            m_mappedData = nullptr;
        }
        else if (m_mappedData)
        {
            if (m_isMappedForAccess && IsHostAccessReady())
                Unmap();
            m_mappedData = nullptr;
            m_isMappedForAccess = false;
        }

        auto& heapManager = m_device->GetDescriptorHeapManager();

        if (m_cbvHandle.IsValid())
            heapManager.FreeCpuCbvSrvUav(m_cbvHandle);
        if (m_srvHandle.IsValid())
            heapManager.FreeCpuCbvSrvUav(m_srvHandle);
        if (m_uavHandle.IsValid())
            heapManager.FreeCpuCbvSrvUav(m_uavHandle);

        // IMPORTANT: Resource lifecycle for Placed Resources:
        // - m_resource (ID3D12Resource) is destroyed here via ComPtr::Release()
        // - The underlying memory remains valid as it's owned by the ID3D12Heap
        // - RenderGraph/caller MUST ensure correct destruction order:
        //   1. First: Release all Placed Buffers/Textures (this destructor)
        //   2. Then: Destroy the Heap (ID3D12Heap)
        // Destroying the Heap before the Placed Resources causes undefined behavior.
    }

    void DX12Buffer::CreateViews()
    {
        auto d3dDevice = m_device->GetD3DDevice();
        auto& heapManager = m_device->GetDescriptorHeapManager();

        // CBV
        if (HasFlag(m_desc.usage, RHIBufferUsage::Constant))
        {
            const uint64 cbvSize = AlignCBVSize(m_desc.size);
            if (m_desc.size <= RVX_DX12_MAX_CBV_SIZE && cbvSize <= RVX_DX12_MAX_CBV_SIZE)
            {
                m_cbvHandle = heapManager.AllocateCpuCbvSrvUav();
                if (!m_cbvHandle.IsValid())
                {
                    m_requiredViewsValid = false;
                    RVX_RHI_ERROR("Failed to allocate DX12 CBV descriptor for buffer '{}'", GetDebugName());
                    return;
                }

                D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
                cbvDesc.BufferLocation = m_resource->GetGPUVirtualAddress();
                cbvDesc.SizeInBytes = static_cast<UINT>(cbvSize);

                if (!TryCreateDX12CpuDescriptor(m_cbvHandle, [&](D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle) {
                        d3dDevice->CreateConstantBufferView(&cbvDesc, cpuHandle);
                    }))
                {
                    m_requiredViewsValid = false;
                    return;
                }
            }
            else
            {
                // Large dynamic constant buffers are bound through root CBVs with
                // GPU virtual address offsets. Creating a descriptor CBV for the
                // entire backing buffer would violate D3D12's 64KB CBV limit.
                RVX_RHI_DEBUG("Skipping CBV descriptor for oversized constant buffer '{}' ({} bytes)",
                              GetDebugName(), m_desc.size);
            }
        }

        // SRV (for structured/typed buffers)
        if (HasFlag(m_desc.usage, RHIBufferUsage::ShaderResource) ||
            HasFlag(m_desc.usage, RHIBufferUsage::Structured))
        {
            m_srvHandle = heapManager.AllocateCpuCbvSrvUav();
            if (!m_srvHandle.IsValid())
            {
                m_requiredViewsValid = false;
                RVX_RHI_ERROR("Failed to allocate DX12 SRV descriptor for buffer '{}'", GetDebugName());
                return;
            }

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

            if (!TryCreateDX12CpuDescriptor(m_srvHandle, [&](D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle) {
                    d3dDevice->CreateShaderResourceView(m_resource.Get(), &srvDesc, cpuHandle);
                }))
            {
                m_requiredViewsValid = false;
                return;
            }
        }

        // UAV
        if (HasFlag(m_desc.usage, RHIBufferUsage::UnorderedAccess))
        {
            m_uavHandle = heapManager.AllocateCpuCbvSrvUav();
            if (!m_uavHandle.IsValid())
            {
                m_requiredViewsValid = false;
                RVX_RHI_ERROR("Failed to allocate DX12 UAV descriptor for buffer '{}'", GetDebugName());
                return;
            }

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

            if (!TryCreateDX12CpuDescriptor(m_uavHandle, [&](D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle) {
                    d3dDevice->CreateUnorderedAccessView(m_resource.Get(), nullptr, &uavDesc, cpuHandle);
                }))
            {
                m_requiredViewsValid = false;
                return;
            }
        }
    }

    bool DX12Buffer::IsHostAccessReady() const noexcept
    {
        return m_device != nullptr && m_resource != nullptr &&
               m_device->QueryRuntimeStatus() == RHIDeviceRuntimeStatus::Ready;
    }

    void* DX12Buffer::BeginMappedAccess()
    {
        if (!IsHostAccessReady())
        {
            RVX_RHI_ERROR("Cannot map an unavailable DX12 buffer");
            return nullptr;
        }

        // Upload memory owns a persistent native mapping. Track the logical
        // access independently so legacy and range transactions cannot overlap.
        if (m_desc.memoryType == RHIMemoryType::Upload)
        {
            m_isMappedForAccess = m_mappedData != nullptr;
            return m_mappedData;
        }

        if (m_desc.memoryType == RHIMemoryType::Default)
        {
            RVX_RHI_ERROR("Cannot map GPU-only buffer");
            return nullptr;
        }

        D3D12_RANGE readRange = {0, m_desc.size};
        const HRESULT hr = m_resource->Map(0, &readRange, &m_mappedData);
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("Failed to map readback buffer: 0x{:08X}",
                          static_cast<uint32>(hr));
            m_device->QueryRuntimeStatus();
            m_mappedData = nullptr;
            return nullptr;
        }

        m_isMappedForAccess = true;
        return m_mappedData;
    }

    void* DX12Buffer::Map()
    {
        if (HasActiveMappedWriteRange())
        {
            RVX_RHI_ERROR("Cannot use legacy Map() during an active DX12 mapped-write transaction");
            return nullptr;
        }
        if (m_isMappedForAccess)
        {
            RVX_RHI_ERROR("Cannot overlap DX12 legacy mapped accesses");
            return nullptr;
        }

        return BeginMappedAccess();
    }

    void* DX12Buffer::MapWriteRangeImpl(uint64 offset, uint64)
    {
        // A mapped write is only valid for upload memory. Legacy Map() keeps
        // readback support for CPU reads, but a readback mapping must not be
        // presented as publishable CPU-to-GPU data.
        if (m_desc.memoryType != RHIMemoryType::Upload)
        {
            RVX_RHI_ERROR("Cannot begin a mapped write on a non-upload DX12 buffer");
            return nullptr;
        }

        if (m_isMappedForAccess)
        {
            RVX_RHI_ERROR("Cannot begin a DX12 range write during a legacy mapped access");
            return nullptr;
        }

        void* mappedData = BeginMappedAccess();
        if (!mappedData)
        {
            return nullptr;
        }
        return static_cast<uint8*>(mappedData) + static_cast<size_t>(offset);
    }

    RHIHostWriteReceipt DX12Buffer::CommitMappedWriteRangeImpl(uint64, uint64)
    {
        RHIHostWriteReceipt receipt;
        if (m_desc.memoryType != RHIMemoryType::Upload ||
            !m_isMappedForAccess || !m_mappedData || !IsHostAccessReady())
        {
            return receipt;
        }

        // D3D12 upload heaps are CPU/GPU coherent and stay persistently
        // mapped. There is no per-write cache flush or Unmap range to report.
        receipt.committed = true;
        receipt.synchronization =
            RHIHostWriteSynchronization::CoherentNoExplicitSync;
        m_isMappedForAccess = false;
        return receipt;
    }

    bool DX12Buffer::CancelMappedWriteRangeImpl(uint64, uint64)
    {
        // Persistent upload mappings do not need an API operation to discard
        // unpublished bytes. The transaction is invalidated by RHIBuffer.
        if (m_desc.memoryType != RHIMemoryType::Upload ||
            !m_isMappedForAccess || m_mappedData == nullptr)
        {
            return false;
        }

        // Persistent upload mappings need no native abort operation. Clearing
        // the logical access is safe even after device loss.
        m_isMappedForAccess = false;
        return true;
    }

    void DX12Buffer::Unmap()
    {
        if (HasActiveMappedWriteRange())
        {
            RVX_RHI_ERROR("Cannot use legacy Unmap() during an active DX12 mapped-write transaction");
            return;
        }

        if (!m_isMappedForAccess)
            return;

        // Upload buffers stay persistently mapped, only unmap readback buffers
        if (m_desc.memoryType == RHIMemoryType::Readback && IsHostAccessReady())
        {
            D3D12_RANGE writtenRange = {0, 0};  // CPU doesn't write to readback
            m_resource->Unmap(0, &writtenRange);
            m_mappedData = nullptr;
        }
        // Upload buffers stay persistently mapped. Device-lost readback state
        // is discarded without issuing another native host operation.
        m_isMappedForAccess = false;
    }

    bool DX12Buffer::CommitMappedWrite()
    {
        if (HasActiveMappedWriteRange())
        {
            RVX_RHI_ERROR("Cannot use legacy CommitMappedWrite() during an active DX12 mapped-write transaction");
            return false;
        }

        if (!m_isMappedForAccess)
            return true;
        if (!IsHostAccessReady())
        {
            m_isMappedForAccess = false;
            return false;
        }

        Unmap();
        return true;
    }

    // =============================================================================
    // DX12 Acceleration Structure Implementation
    // =============================================================================
    DX12AccelerationStructure::DX12AccelerationStructure(DX12Device* device, const RHIAccelerationStructureDesc& desc)
        : m_device(device)
        , m_desc(desc)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        if (!m_device || desc.size == 0)
        {
            RVX_RHI_ERROR("DX12AccelerationStructure: invalid device or zero-sized acceleration structure");
            return;
        }

        RHIBufferDesc bufferDesc;
        bufferDesc.size = desc.size;
        bufferDesc.usage = RHIBufferUsage::AccelerationStructureStorage |
                           RHIBufferUsage::UnorderedAccess |
                           RHIBufferUsage::ShaderResource |
                           RHIBufferUsage::DeviceAddress;
        bufferDesc.memoryType = RHIMemoryType::Default;
        bufferDesc.debugName = desc.debugName ? desc.debugName : "DX12AccelerationStructure";

        m_storageBuffer = m_device->CreateBuffer(bufferDesc);
        if (!m_storageBuffer)
        {
            RVX_RHI_ERROR("DX12AccelerationStructure: failed to create storage buffer");
        }
    }

    uint64 DX12AccelerationStructure::GetGPUVirtualAddress() const
    {
        auto* buffer = static_cast<DX12Buffer*>(m_storageBuffer.Get());
        return buffer ? buffer->GetGPUVirtualAddress() : 0;
    }

    ID3D12Resource* DX12AccelerationStructure::GetResource() const
    {
        auto* buffer = static_cast<DX12Buffer*>(m_storageBuffer.Get());
        return buffer ? buffer->GetResource() : nullptr;
    }

    // =============================================================================
    // DX12 Texture Implementation
    // =============================================================================
    DX12Texture::DX12Texture(DX12Device* device, const RHITextureDesc& desc)
        : m_desc(desc)
        , m_dxgiFormat(ToDXGIFormat(desc.format))
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
                resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                resourceDesc.DepthOrArraySize = static_cast<UINT16>(desc.arraySize);
                break;
            case RHITextureDimension::TextureCube:
                resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                resourceDesc.DepthOrArraySize = static_cast<UINT16>(GetTexturePhysicalLayerCount(desc));
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
        const D3D12_CLEAR_VALUE* pClearValue =
            BuildDX12OptimizedClearValue(desc, m_dxgiFormat, clearValue);

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

    }

    DX12Texture::DX12Texture(DX12Device*, ComPtr<ID3D12Resource> resource, const RHITextureDesc& desc)
        : m_desc(desc)
        , m_dxgiFormat(ToDXGIFormat(desc.format))
        , m_resource(resource)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }
    }

    // =============================================================================
    // DX12 Texture View Implementation
    // =============================================================================
    DX12TextureView::DX12TextureView(DX12Device* device, RHITexture* texture, const RHITextureViewDesc& desc)
        : RHITextureView(RHITextureRef(texture))
        , m_device(device)
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
        const uint32 mipCount = (desc.subresourceRange.mipLevelCount == 0 || desc.subresourceRange.mipLevelCount == RVX_ALL_MIPS)
            ? texture->GetMipLevels() - desc.subresourceRange.baseMipLevel
            : desc.subresourceRange.mipLevelCount;
        const uint32 arrayLayerCount = ResolveTextureArrayLayerCount(*texture, desc.subresourceRange);

        // Create only the requested native view role. The RHI view type is part
        // of cache identity, so SRV/RTV/DSV/UAV wrappers must not all carry
        // unrelated descriptor handles.
        if (desc.type == RHITextureViewType::ShaderResource)
        {
            DXGI_FORMAT srvFormat = dxgiFormat;
            if (IsDepthFormat(m_format))
            {
                srvFormat = GetDepthSRVFormat(dxgiFormat);
            }

            m_srvHandle = heapManager.AllocateCpuCbvSrvUav();
            if (!m_srvHandle.IsValid())
            {
                RVX_RHI_ERROR("Failed to allocate DX12 texture SRV descriptor");
                return;
            }

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = srvFormat;
            switch (texture->GetDimension())
            {
                case RHITextureDimension::TextureCube:
                    if (arrayLayerCount > 6)
                    {
                        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                        srvDesc.TextureCubeArray.MostDetailedMip = desc.subresourceRange.baseMipLevel;
                        srvDesc.TextureCubeArray.MipLevels = mipCount;
                        srvDesc.TextureCubeArray.First2DArrayFace = desc.subresourceRange.baseArrayLayer;
                        srvDesc.TextureCubeArray.NumCubes = arrayLayerCount / 6;
                    }
                    else
                    {
                        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                        srvDesc.TextureCube.MostDetailedMip = desc.subresourceRange.baseMipLevel;
                        srvDesc.TextureCube.MipLevels = mipCount;
                    }
                    break;
                case RHITextureDimension::Texture2D:
                    if (static_cast<uint32>(texture->GetSampleCount()) > 1)
                    {
                        if (IsTexture2DArrayViewRequired(*texture))
                        {
                            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
                            srvDesc.Texture2DMSArray.FirstArraySlice = desc.subresourceRange.baseArrayLayer;
                            srvDesc.Texture2DMSArray.ArraySize = arrayLayerCount;
                        }
                        else
                        {
                            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
                        }
                    }
                    else if (IsTexture2DArrayViewRequired(*texture))
                    {
                        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                        srvDesc.Texture2DArray.MostDetailedMip = desc.subresourceRange.baseMipLevel;
                        srvDesc.Texture2DArray.MipLevels = mipCount;
                        srvDesc.Texture2DArray.FirstArraySlice = desc.subresourceRange.baseArrayLayer;
                        srvDesc.Texture2DArray.ArraySize = arrayLayerCount;
                    }
                    else
                    {
                        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                        srvDesc.Texture2D.MostDetailedMip = desc.subresourceRange.baseMipLevel;
                        srvDesc.Texture2D.MipLevels = mipCount;
                    }
                    break;
                default:
                    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Texture2D.MostDetailedMip = desc.subresourceRange.baseMipLevel;
                    srvDesc.Texture2D.MipLevels = mipCount;
                    break;
            }

            if (!TryCreateDX12CpuDescriptor(m_srvHandle, [&](D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle) {
                    d3dDevice->CreateShaderResourceView(dx12Texture->GetResource(), &srvDesc, cpuHandle);
                }))
            {
                return;
            }
        }

        if (desc.type == RHITextureViewType::RenderTarget)
        {
            m_rtvHandle = heapManager.AllocateRTV();
            if (!m_rtvHandle.IsValid())
            {
                RVX_RHI_ERROR("Failed to allocate DX12 texture RTV descriptor");
                return;
            }

            D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
            rtvDesc.Format = dxgiFormat;

            const bool useTextureArrayView = texture->GetDimension() == RHITextureDimension::TextureCube ||
                                             IsTexture2DArrayViewRequired(*texture);
            if (static_cast<uint32>(texture->GetSampleCount()) > 1)
            {
                if (useTextureArrayView)
                {
                    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
                    rtvDesc.Texture2DMSArray.FirstArraySlice = desc.subresourceRange.baseArrayLayer;
                    rtvDesc.Texture2DMSArray.ArraySize = arrayLayerCount;
                }
                else
                {
                    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
                }
            }
            else if (useTextureArrayView)
            {
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rtvDesc.Texture2DArray.MipSlice = desc.subresourceRange.baseMipLevel;
                rtvDesc.Texture2DArray.FirstArraySlice = desc.subresourceRange.baseArrayLayer;
                rtvDesc.Texture2DArray.ArraySize = arrayLayerCount;
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

            if (!TryCreateDX12CpuDescriptor(m_rtvHandle, [&](D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle) {
                    d3dDevice->CreateRenderTargetView(dx12Texture->GetResource(), &rtvDesc, cpuHandle);
                }))
            {
                return;
            }
        }

        if (desc.type == RHITextureViewType::DepthStencil)
        {
            m_dsvHandle = heapManager.AllocateDSV();
            if (!m_dsvHandle.IsValid())
            {
                RVX_RHI_ERROR("Failed to allocate DX12 texture DSV descriptor");
                return;
            }

            D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
            dsvDesc.Format = dxgiFormat;
            dsvDesc.Flags = D3D12_DSV_FLAG_NONE;

            const bool useTextureArrayView = IsTexture2DArrayViewRequired(*texture);
            if (static_cast<uint32>(texture->GetSampleCount()) > 1)
            {
                if (useTextureArrayView)
                {
                    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
                    dsvDesc.Texture2DMSArray.FirstArraySlice = desc.subresourceRange.baseArrayLayer;
                    dsvDesc.Texture2DMSArray.ArraySize = arrayLayerCount;
                }
                else
                {
                    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
                }
            }
            else if (useTextureArrayView)
            {
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                dsvDesc.Texture2DArray.MipSlice = desc.subresourceRange.baseMipLevel;
                dsvDesc.Texture2DArray.FirstArraySlice = desc.subresourceRange.baseArrayLayer;
                dsvDesc.Texture2DArray.ArraySize = arrayLayerCount;
            }
            else
            {
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                dsvDesc.Texture2D.MipSlice = desc.subresourceRange.baseMipLevel;
            }

            if (!TryCreateDX12CpuDescriptor(m_dsvHandle, [&](D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle) {
                    d3dDevice->CreateDepthStencilView(dx12Texture->GetResource(), &dsvDesc, cpuHandle);
                }))
            {
                return;
            }
        }

        if (desc.type == RHITextureViewType::UnorderedAccess)
        {
            m_uavHandle = heapManager.AllocateCpuCbvSrvUav();
            if (!m_uavHandle.IsValid())
            {
                RVX_RHI_ERROR("Failed to allocate DX12 texture UAV descriptor");
                return;
            }

            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = dxgiFormat;

            if (texture->GetDimension() == RHITextureDimension::TextureCube || IsTexture2DArrayViewRequired(*texture))
            {
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                uavDesc.Texture2DArray.MipSlice = desc.subresourceRange.baseMipLevel;
                uavDesc.Texture2DArray.FirstArraySlice = desc.subresourceRange.baseArrayLayer;
                uavDesc.Texture2DArray.ArraySize = arrayLayerCount;
            }
            else
            {
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uavDesc.Texture2D.MipSlice = desc.subresourceRange.baseMipLevel;
            }

            if (!TryCreateDX12CpuDescriptor(m_uavHandle, [&](D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle) {
                    d3dDevice->CreateUnorderedAccessView(dx12Texture->GetResource(), nullptr, &uavDesc, cpuHandle);
                }))
            {
                return;
            }
        }
    }

    DX12TextureView::~DX12TextureView()
    {
        auto& heapManager = m_device->GetDescriptorHeapManager();

        if (m_srvHandle.IsValid())
            heapManager.FreeCpuCbvSrvUav(m_srvHandle);
        if (m_uavHandle.IsValid())
            heapManager.FreeCpuCbvSrvUav(m_uavHandle);
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

        m_handle = heapManager.AllocateCpuSampler();
        if (!m_handle.IsValid())
        {
            RVX_RHI_ERROR("Failed to allocate DX12 sampler descriptor");
            return;
        }

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

        if (!TryCreateDX12CpuDescriptor(m_handle, [&](D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle) {
                d3dDevice->CreateSampler(&samplerDesc, cpuHandle);
            }))
        {
            return;
        }
    }

    DX12Sampler::~DX12Sampler()
    {
        if (m_handle.IsValid())
        {
            m_device->GetDescriptorHeapManager().FreeCpuSampler(m_handle);
        }
    }

    // =============================================================================
    // DX12 Shader Implementation
    // =============================================================================
    DX12Shader::DX12Shader(DX12Device* device, const RHIShaderDesc& desc)
        : RHIShader(desc)
        , m_stage(desc.stage)
        , m_entryPoint(desc.entryPoint ? desc.entryPoint : "main")
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
        , m_nextSignalValue(initialValue + 1)
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
        TrackSubmittedValue(value);
        m_device->GetGraphicsQueue()->Signal(m_fence.Get(), value);
    }

    void DX12Fence::SignalOnQueue(uint64 value, RHICommandQueueType queueType)
    {
        ID3D12CommandQueue* queue = m_device->GetQueue(queueType);
        if (queue)
        {
            TrackSubmittedValue(value);
            queue->Signal(m_fence.Get(), value);
        }
        else
        {
            RVX_RHI_WARN("SignalOnQueue: Invalid queue type {}", static_cast<int>(queueType));
        }
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

    uint64 DX12Fence::AllocateSignalValue()
    {
        return m_nextSignalValue.fetch_add(1, std::memory_order_relaxed);
    }

    void DX12Fence::TrackSubmittedValue(uint64 value)
    {
        uint64 expected = m_nextSignalValue.load(std::memory_order_relaxed);
        while (expected <= value &&
               !m_nextSignalValue.compare_exchange_weak(expected, value + 1,
                                                        std::memory_order_relaxed,
                                                        std::memory_order_relaxed))
        {
        }
    }

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIBufferRef CreateDX12Buffer(DX12Device* device, const RHIBufferDesc& desc)
    {
        auto buffer = Ref<DX12Buffer>(new DX12Buffer(device, desc));
        if (!buffer->AreRequiredViewsValid())
        {
            RVX_RHI_ERROR("DX12: Failed to create required buffer descriptors");
            return nullptr;
        }
        return buffer;
    }

    RHITextureRef CreateDX12Texture(DX12Device* device, const RHITextureDesc& desc)
    {
        if (!IsRHIOptimizedClearValueCompatible(desc))
        {
            RVX_RHI_ERROR("DX12: texture '{}' has an incompatible optimized clear value",
                          desc.debugName ? desc.debugName : "<unnamed>");
            return nullptr;
        }
        return Ref<DX12Texture>(new DX12Texture(device, desc));
    }

    RHITextureRef CreateDX12TextureFromResource(DX12Device* device, ComPtr<ID3D12Resource> resource, const RHITextureDesc& desc)
    {
        return Ref<DX12Texture>(new DX12Texture(device, resource, desc));
    }

    RHITextureViewRef CreateDX12TextureView(DX12Device* device, RHITexture* texture, const RHITextureViewDesc& desc)
    {
        if (!texture)
        {
            RVX_RHI_ERROR("DX12: Cannot create texture view from null texture");
            return nullptr;
        }
        if (!IsTextureViewTypeCompatible(texture->GetUsage(), texture->GetFormat(), desc))
        {
            RVX_RHI_ERROR("DX12: Cannot create {} texture view for texture usage {} format {}",
                          GetTextureViewTypeName(desc.type),
                          static_cast<uint32>(texture->GetUsage()),
                          static_cast<uint32>(desc.format == RHIFormat::Unknown ? texture->GetFormat() : desc.format));
            return nullptr;
        }

        auto view = Ref<DX12TextureView>(new DX12TextureView(device, texture, desc));
        const bool nativeViewCreated =
            (desc.type == RHITextureViewType::ShaderResource && view->GetSRVHandle().IsValid()) ||
            (desc.type == RHITextureViewType::RenderTarget && view->GetRTVHandle().IsValid()) ||
            (desc.type == RHITextureViewType::DepthStencil && view->GetDSVHandle().IsValid()) ||
            (desc.type == RHITextureViewType::UnorderedAccess && view->GetUAVHandle().IsValid());
        if (!nativeViewCreated)
        {
            RVX_RHI_ERROR("DX12: Failed to create native {} texture view",
                          GetTextureViewTypeName(desc.type));
            return nullptr;
        }

        return view;
    }

    RHISamplerRef CreateDX12Sampler(DX12Device* device, const RHISamplerDesc& desc)
    {
        auto sampler = Ref<DX12Sampler>(new DX12Sampler(device, desc));
        if (!sampler->IsValid())
        {
            RVX_RHI_ERROR("DX12: Failed to create sampler descriptor");
            return nullptr;
        }
        return sampler;
    }

    RHIShaderRef CreateDX12Shader(DX12Device* device, const RHIShaderDesc& desc)
    {
        return Ref<DX12Shader>(new DX12Shader(device, desc));
    }

    RHIAccelerationStructureRef CreateDX12AccelerationStructure(DX12Device* device, const RHIAccelerationStructureDesc& desc)
    {
        if (!device || !device->GetCapabilities().supportsRaytracing)
        {
            RVX_RHI_ERROR("DX12: Cannot create acceleration structure without ray tracing support");
            return nullptr;
        }

        auto validation = ValidateRHIAccelerationStructureDesc(desc);
        if (!validation)
        {
            RVX_RHI_ERROR("DX12: Cannot create acceleration structure: {}", validation.message);
            return nullptr;
        }

        return Ref<DX12AccelerationStructure>(new DX12AccelerationStructure(device, desc));
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
        if (!IsRHIOptimizedClearValueCompatible(desc))
        {
            RVX_RHI_ERROR("DX12: placed texture '{}' has an incompatible optimized clear value",
                          desc.debugName ? desc.debugName : "<unnamed>");
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
                resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                resourceDesc.DepthOrArraySize = static_cast<UINT16>(desc.arraySize);
                break;
            case RHITextureDimension::TextureCube:
                resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                resourceDesc.DepthOrArraySize = static_cast<UINT16>(GetTexturePhysicalLayerCount(desc));
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
        const D3D12_CLEAR_VALUE* pClearValue =
            BuildDX12OptimizedClearValue(desc, dxgiFormat, clearValue);

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

        if (HasFlag(desc.usage, RHIBufferUsage::UnorderedAccess) ||
            HasFlag(desc.usage, RHIBufferUsage::AccelerationStructureStorage))
        {
            resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }

        D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
        if (HasFlag(desc.usage, RHIBufferUsage::AccelerationStructureStorage))
        {
            initialState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
        }
        else if (desc.memoryType == RHIMemoryType::Upload)
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

        // Use the external resource constructor (memory owned by heap).
        auto buffer = Ref<DX12Buffer>(new DX12Buffer(device, resource, desc, false));
        if (!buffer->AreRequiredViewsValid())
        {
            RVX_RHI_ERROR("DX12: Failed to create required placed-buffer descriptors");
            return nullptr;
        }
        return buffer;
    }

} // namespace RVX
