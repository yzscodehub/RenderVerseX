/**
 * @file DX12Upload.cpp
 * @brief DX12 upload buffer implementations (StagingBuffer, RingBuffer)
 */

#include "DX12Upload.h"
#include "DX12Device.h"
#include "Core/Log.h"

#include <algorithm>

namespace RVX
{
    // =============================================================================
    // DX12StagingBuffer Implementation
    // =============================================================================

    DX12StagingBuffer::DX12StagingBuffer(DX12Device* device, const RHIStagingBufferDesc& desc)
        : m_device(device)
        , m_size(desc.size)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        auto d3dDevice = device->GetD3DDevice();

        // Configure heap properties for upload (CPU-writable, GPU-readable)
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask = 0;
        heapProps.VisibleNodeMask = 0;

        // Configure buffer resource
        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Alignment = 0;
        resourceDesc.Width = m_size;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.SampleDesc.Quality = 0;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        // Create the committed resource
        HRESULT hr = d3dDevice->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_resource));

        if (FAILED(hr))
        {
            RVX_RHI_ERROR("DX12: Failed to create staging buffer (size: {}), HRESULT: 0x{:08X}",
                         m_size, static_cast<uint32>(hr));
            return;
        }

        // Set debug name
        if (desc.debugName)
        {
            std::wstring wideName(desc.debugName, desc.debugName + strlen(desc.debugName));
            m_resource->SetName(wideName.c_str());
        }
        else
        {
            m_resource->SetName(L"DX12StagingBuffer");
        }

        RVX_RHI_DEBUG("DX12: Created staging buffer '{}' of size {} bytes",
                      desc.debugName ? desc.debugName : "", m_size);
    }

    DX12StagingBuffer::~DX12StagingBuffer()
    {
        // Unmap if still mapped
        if (m_isMapped)
        {
            Unmap();
        }

        m_wrapperBuffer = nullptr;
        m_resource = nullptr;
    }

    void* DX12StagingBuffer::Map(uint64 offset, uint64 size)
    {
        if (!m_resource)
        {
            RVX_RHI_ERROR("DX12: Cannot map invalid staging buffer");
            return nullptr;
        }

        // If already mapped, return existing pointer with offset
        if (m_isMapped)
        {
            return static_cast<uint8*>(m_mappedData) + offset;
        }

        // Calculate the range to map
        uint64 endOffset = (size == RVX_WHOLE_SIZE) ? m_size : (offset + size);
        D3D12_RANGE range = { offset, endOffset };

        HRESULT hr = m_resource->Map(0, &range, &m_mappedData);
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("DX12: Failed to map staging buffer, HRESULT: 0x{:08X}",
                         static_cast<uint32>(hr));
            return nullptr;
        }

        m_isMapped = true;
        return static_cast<uint8*>(m_mappedData) + offset;
    }

    void DX12StagingBuffer::Unmap()
    {
        if (!m_isMapped || !m_resource)
        {
            return;
        }

        // Unmap with empty range (no read back needed)
        D3D12_RANGE emptyRange = { 0, 0 };
        m_resource->Unmap(0, &emptyRange);

        m_mappedData = nullptr;
        m_isMapped = false;
    }

    RHIBuffer* DX12StagingBuffer::GetBuffer() const
    {
        // Lazily create a wrapper RHIBuffer that wraps our existing resource
        if (!m_wrapperBuffer)
        {
            RHIBufferDesc bufferDesc = {};
            bufferDesc.size = m_size;
            bufferDesc.usage = RHIBufferUsage::TransferSrc;
            bufferDesc.memoryType = RHIMemoryType::Upload;
            bufferDesc.debugName = GetDebugName();

            // Wrap our existing staging buffer resource - don't let buffer own it
            m_wrapperBuffer = new DX12Buffer(m_device, m_resource, bufferDesc, false);
        }
        return m_wrapperBuffer.Get();
    }

    // =============================================================================
    // DX12RingBuffer Implementation
    // =============================================================================

    DX12RingBuffer::DX12RingBuffer(DX12Device* device, const RHIRingBufferDesc& desc)
        : m_device(device)
        , m_totalSize(desc.size)
        , m_alignment(desc.alignment)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        // Ensure minimum alignment (16 bytes for most operations)
        if (m_alignment < 16)
        {
            m_alignment = 16;
        }

        auto d3dDevice = device->GetD3DDevice();

        // Configure heap properties for upload
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask = 0;
        heapProps.VisibleNodeMask = 0;

        // Configure buffer resource
        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Alignment = 0;
        resourceDesc.Width = m_totalSize;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.SampleDesc.Quality = 0;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        // Create the committed resource
        HRESULT hr = d3dDevice->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_resource));

        if (FAILED(hr))
        {
            RVX_RHI_ERROR("DX12: Failed to create ring buffer (size: {}), HRESULT: 0x{:08X}",
                         m_totalSize, static_cast<uint32>(hr));
            return;
        }

        // Set debug name
        if (desc.debugName)
        {
            std::wstring wideName(desc.debugName, desc.debugName + strlen(desc.debugName));
            m_resource->SetName(wideName.c_str());
        }
        else
        {
            m_resource->SetName(L"DX12RingBuffer");
        }

        // Map the buffer persistently for write-combined access
        D3D12_RANGE readRange = { 0, 0 };  // We don't need to read from it
        hr = m_resource->Map(0, &readRange, &m_mappedData);
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("DX12: Failed to map ring buffer, HRESULT: 0x{:08X}",
                         static_cast<uint32>(hr));
            return;
        }

        // Initialize frame fence values
        m_frameFenceValues.fill(0);

        RVX_RHI_DEBUG("DX12: Created ring buffer '{}' of size {} bytes, alignment {}",
                      desc.debugName ? desc.debugName : "", m_totalSize, m_alignment);
    }

    DX12RingBuffer::~DX12RingBuffer()
    {
        // Unmap the buffer
        if (m_mappedData)
        {
            D3D12_RANGE emptyRange = { 0, 0 };
            m_resource->Unmap(0, &emptyRange);
            m_mappedData = nullptr;
        }

        m_wrapperBuffer = nullptr;
        m_resource = nullptr;
    }

    RHIRingAllocation DX12RingBuffer::Allocate(uint64 size)
    {
        RHIRingAllocation result;

        if (!m_resource || size == 0)
        {
            RVX_RHI_WARN("DX12: Invalid ring buffer allocation request");
            return result;
        }

        // Align the allocation size
        uint64 alignedSize = (size + m_alignment - 1) & ~(static_cast<uint64>(m_alignment) - 1);

        // Use atomic operations for lock-free allocation
        uint64 currentHead = m_head.load(std::memory_order_acquire);
        uint64 currentTail = m_tail.load(std::memory_order_acquire);

        while (true)
        {
            // Calculate the space used
            uint64 spaceUsed = (currentHead >= currentTail) ?
                (currentHead - currentTail) :
                (m_totalSize - currentTail + currentHead);

            // Check if we have enough space
            if (spaceUsed + alignedSize > m_totalSize)
            {
                // Not enough space - need to wait for previous frames to complete
                RVX_RHI_WARN("DX12: Ring buffer full, space used: {}, requested: {}, total: {}",
                            spaceUsed, alignedSize, m_totalSize);
                return result;
            }

            // Try to claim this allocation using CAS
            uint64 newHead = currentHead + alignedSize;

            // Wrap around if needed
            if (newHead >= m_totalSize)
            {
                newHead = 0;
            }

            // Attempt atomic update
            if (m_head.compare_exchange_weak(currentHead, newHead,
                                               std::memory_order_release,
                                               std::memory_order_acquire))
            {
                // Allocation successful
                result.cpuAddress = static_cast<uint8*>(m_mappedData) + currentHead;
                result.gpuOffset = currentHead;
                result.size = alignedSize;
                result.buffer = GetBuffer();
                break;
            }
            // CAS failed, retry with updated head value
        }

        return result;
    }

    void DX12RingBuffer::Reset(uint32 frameIndex)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Validate frame index
        uint32 frameIdx = frameIndex % MAX_FRAMES_IN_FLIGHT;

        // Store the current head position for this frame
        // This marks where the current frame's allocations end
        m_frameFenceValues[frameIdx] = m_head.load(std::memory_order_acquire);

        // Calculate the new tail based on oldest in-flight frame
        uint64 minFenceValue = m_frameFenceValues[0];
        for (uint32 i = 1; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            if (m_frameFenceValues[i] < minFenceValue)
            {
                minFenceValue = m_frameFenceValues[i];
            }
        }

        // Update the tail to allow reuse of old allocations
        m_tail.store(minFenceValue, std::memory_order_release);

        RVX_RHI_DEBUG("DX12: Ring buffer reset for frame {}, tail: {}, head: {}",
                      frameIndex, minFenceValue, m_head.load(std::memory_order_acquire));
    }

    RHIBuffer* DX12RingBuffer::GetBuffer() const
    {
        if (!m_wrapperBuffer)
        {
            RHIBufferDesc bufferDesc = {};
            bufferDesc.size = m_totalSize;
            bufferDesc.usage = RHIBufferUsage::Constant | RHIBufferUsage::TransferSrc;
            bufferDesc.memoryType = RHIMemoryType::Upload;
            bufferDesc.debugName = GetDebugName();

            // Wrap our existing ring buffer resource - don't let buffer own it
            m_wrapperBuffer = new DX12Buffer(m_device, m_resource, bufferDesc, false);
        }
        return m_wrapperBuffer.Get();
    }

    // =============================================================================
    // Factory Functions
    // =============================================================================

    RHIStagingBufferRef CreateDX12StagingBuffer(DX12Device* device, const RHIStagingBufferDesc& desc)
    {
        return new DX12StagingBuffer(device, desc);
    }

    RHIRingBufferRef CreateDX12RingBuffer(DX12Device* device, const RHIRingBufferDesc& desc)
    {
        return new DX12RingBuffer(device, desc);
    }

} // namespace RVX
