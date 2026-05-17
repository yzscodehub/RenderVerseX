#include "DX11Upload.h"
#include "DX11Resources.h"

namespace RVX
{
    // =============================================================================
    // DX11 Staging Buffer
    // =============================================================================
    DX11StagingBuffer::DX11StagingBuffer(DX11Device* device, const RHIStagingBufferDesc& desc)
        : m_device(device), m_size(desc.size)
    {
        D3D11_BUFFER_DESC bufferDesc = {};
        bufferDesc.ByteWidth = static_cast<UINT>(m_size);
        bufferDesc.Usage = D3D11_USAGE_STAGING;
        bufferDesc.BindFlags = 0;
        bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
        bufferDesc.MiscFlags = 0;
        bufferDesc.StructureByteStride = 0;

        HRESULT hr = device->GetD3DDevice()->CreateBuffer(&bufferDesc, nullptr, &m_buffer);
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("DX11StagingBuffer: Failed to create buffer: 0x{:08X}", static_cast<uint32>(hr));
            return;
        }

        if (desc.debugName)
        {
            m_buffer->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(strlen(desc.debugName)), desc.debugName);
        }
    }

    DX11StagingBuffer::~DX11StagingBuffer()
    {
        if (m_isMapped)
        {
            Unmap();
        }
    }

    void* DX11StagingBuffer::Map(uint64 offset, uint64 size)
    {
        (void)size;

        if (m_isMapped)
        {
            return static_cast<uint8*>(m_mappedData) + offset;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        UINT subresource = 0;
        D3D11_MAP mapType = D3D11_MAP_WRITE;

        HRESULT hr = m_device->GetImmediateContext()->Map(m_buffer.Get(), subresource, mapType, 0, &mapped);
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("DX11StagingBuffer: Failed to map buffer: 0x{:08X}", static_cast<uint32>(hr));
            return nullptr;
        }

        m_mappedData = mapped.pData;
        m_isMapped = true;
        return static_cast<uint8*>(m_mappedData) + offset;
    }

    void DX11StagingBuffer::Unmap()
    {
        if (!m_isMapped)
        {
            return;
        }

        m_device->GetImmediateContext()->Unmap(m_buffer.Get(), 0);
        m_mappedData = nullptr;
        m_isMapped = false;
    }

    RHIBuffer* DX11StagingBuffer::GetBuffer() const
    {
        if (!m_wrapperBuffer)
        {
            RHIBufferDesc desc = {};
            desc.size = m_size;
            desc.usage = RHIBufferUsage::CopySrc;
            desc.memoryType = RHIMemoryType::Upload;
            m_wrapperBuffer = m_device->CreateBuffer(desc);
        }
        return m_wrapperBuffer.Get();
    }

    // =============================================================================
    // DX11 Ring Buffer
    // =============================================================================
    DX11RingBuffer::DX11RingBuffer(DX11Device* device, const RHIRingBufferDesc& desc)
        : m_device(device), m_totalSize(desc.size), m_alignment(desc.alignment)
    {
        D3D11_BUFFER_DESC bufferDesc = {};
        bufferDesc.ByteWidth = static_cast<UINT>(m_totalSize);
        bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
        bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_CONSTANT_BUFFER;
        bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bufferDesc.MiscFlags = 0;
        bufferDesc.StructureByteStride = 0;

        // Create double buffers
        for (uint32 i = 0; i < 2; ++i)
        {
            HRESULT hr = device->GetD3DDevice()->CreateBuffer(&bufferDesc, nullptr, &m_buffers[i]);
            if (FAILED(hr))
            {
                RVX_RHI_ERROR("DX11RingBuffer: Failed to create buffer {}: 0x{:08X}", i, static_cast<uint32>(hr));
            }
        }

        // Initial mapping
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = m_device->GetImmediateContext()->Map(m_buffers[0].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            m_mappedData = mapped.pData;
            m_device->GetImmediateContext()->Unmap(m_buffers[0].Get(), 0);
        }

        if (desc.debugName)
        {
            for (uint32 i = 0; i < 2; ++i)
            {
                std::string name = std::string(desc.debugName) + "_" + std::to_string(i);
                m_buffers[i]->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.size()), name.c_str());
            }
        }
    }

    DX11RingBuffer::~DX11RingBuffer()
    {
    }

    RHIRingAllocation DX11RingBuffer::Allocate(uint64 size)
    {
        // Align the allocation size
        const uint64 alignment = static_cast<uint64>(m_alignment);
        uint64 alignedSize = (size + alignment - 1) & ~(alignment - 1);

        std::lock_guard<std::mutex> lock(m_mutex);

        // Check if we need to wrap
        if (m_currentOffset + alignedSize > m_totalSize)
        {
            // Switch buffer and remap
            m_currentBuffer = (m_currentBuffer + 1) % 2;
            m_currentOffset = 0;

            D3D11_MAPPED_SUBRESOURCE mapped = {};
            HRESULT hr = m_device->GetImmediateContext()->Map(m_buffers[m_currentBuffer].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (SUCCEEDED(hr))
            {
                m_mappedData = mapped.pData;
                m_device->GetImmediateContext()->Unmap(m_buffers[m_currentBuffer].Get(), 0);
            }
        }

        RHIRingAllocation alloc;
        alloc.cpuAddress = static_cast<uint8*>(m_mappedData) + m_currentOffset;
        alloc.gpuOffset = m_currentOffset;
        alloc.size = alignedSize;
        alloc.buffer = GetBuffer();

        m_currentOffset += alignedSize;
        return alloc;
    }

    void DX11RingBuffer::Reset(uint32 frameIndex)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Switch to next buffer for new frame
        uint32 newBuffer = (frameIndex % 2);
        if (newBuffer != m_currentBuffer)
        {
            m_currentBuffer = newBuffer;
            m_currentOffset = 0;

            D3D11_MAPPED_SUBRESOURCE mapped = {};
            HRESULT hr = m_device->GetImmediateContext()->Map(m_buffers[m_currentBuffer].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (SUCCEEDED(hr))
            {
                m_mappedData = mapped.pData;
                m_device->GetImmediateContext()->Unmap(m_buffers[m_currentBuffer].Get(), 0);
            }
        }
        else
        {
            // Same buffer, just reset offset
            m_currentOffset = 0;
        }
    }

    RHIBuffer* DX11RingBuffer::GetBuffer() const
    {
        if (!m_wrapperBuffer)
        {
            RHIBufferDesc desc = {};
            desc.size = m_totalSize;
            desc.usage = RHIBufferUsage::Vertex | RHIBufferUsage::Constant;
            desc.memoryType = RHIMemoryType::Upload;
            m_wrapperBuffer = m_device->CreateBuffer(desc);
        }
        return m_wrapperBuffer.Get();
    }

    // =============================================================================
    // Factory Functions
    // =============================================================================
    RHIStagingBufferRef CreateDX11StagingBuffer(DX11Device* device, const RHIStagingBufferDesc& desc)
    {
        return RHIStagingBufferRef(new DX11StagingBuffer(device, desc));
    }

    RHIRingBufferRef CreateDX11RingBuffer(DX11Device* device, const RHIRingBufferDesc& desc)
    {
        return RHIRingBufferRef(new DX11RingBuffer(device, desc));
    }

} // namespace RVX
