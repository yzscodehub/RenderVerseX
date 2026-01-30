#include "MetalUpload.h"
#include "MetalResources.h"
#include "Core/Log.h"

namespace RVX
{
    // =============================================================================
    // MetalStagingBuffer Implementation
    // =============================================================================

    MetalStagingBuffer::MetalStagingBuffer(id<MTLDevice> device, const RHIStagingBufferDesc& desc)
        : m_device(device)
        , m_size(desc.size)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        // Create shared storage buffer for CPU->GPU transfers
        // Use write-combined mode for optimal CPU write performance
        MTLResourceOptions options = MTLResourceStorageModeShared | 
                                     MTLResourceCPUCacheModeWriteCombined;
        
        m_buffer = [device newBufferWithLength:desc.size options:options];
        
        if (!m_buffer)
        {
            RVX_RHI_ERROR("Metal: Failed to create staging buffer of size {}", desc.size);
            return;
        }

        if (desc.debugName)
        {
            m_buffer.label = [NSString stringWithUTF8String:desc.debugName];
        }

        RVX_RHI_DEBUG("Metal: Created staging buffer '{}' of size {} bytes", 
                     desc.debugName ? desc.debugName : "", desc.size);
    }

    MetalStagingBuffer::~MetalStagingBuffer()
    {
        m_wrapperBuffer = nullptr;
        m_buffer = nil;
    }

    void* MetalStagingBuffer::Map(uint64 offset, uint64 /*size*/)
    {
        if (!m_buffer)
        {
            RVX_RHI_ERROR("Metal: Cannot map invalid staging buffer");
            return nullptr;
        }

        if (m_isMapped)
        {
            RVX_RHI_WARN("Metal: Staging buffer already mapped");
            return static_cast<uint8*>(m_mappedPtr) + offset;
        }

        // Metal shared buffers are always mapped
        m_mappedPtr = static_cast<uint8*>(m_buffer.contents) + offset;
        m_isMapped = true;

        return m_mappedPtr;
    }

    void MetalStagingBuffer::Unmap()
    {
        if (!m_isMapped)
        {
            RVX_RHI_WARN("Metal: Staging buffer was not mapped");
            return;
        }

        // Metal shared buffers don't need explicit unmap
        // But we may need to synchronize on macOS
        #if TARGET_OS_OSX
        if (m_buffer.storageMode == MTLStorageModeManaged)
        {
            [m_buffer didModifyRange:NSMakeRange(0, m_size)];
        }
        #endif

        m_mappedPtr = nullptr;
        m_isMapped = false;
    }

    RHIBuffer* MetalStagingBuffer::GetBuffer() const
    {
        // Lazily create a wrapper RHIBuffer if needed
        if (!m_wrapperBuffer)
        {
            RHIBufferDesc bufferDesc;
            bufferDesc.size = m_size;
            bufferDesc.usage = RHIBufferUsage::TransferSrc;
            bufferDesc.memoryType = RHIMemoryType::Upload;
            bufferDesc.debugName = GetDebugName();

            // Create wrapper that wraps our MTLBuffer
            m_wrapperBuffer = MakeRef<MetalBuffer>(m_buffer, bufferDesc);
        }
        return m_wrapperBuffer.Get();
    }

    // =============================================================================
    // MetalRingBuffer Implementation
    // =============================================================================

    MetalRingBuffer::MetalRingBuffer(id<MTLDevice> device, const RHIRingBufferDesc& desc)
        : m_device(device)
        , m_totalSize(desc.size)
        , m_alignment(desc.alignment)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        // Ensure minimum alignment
        if (m_alignment < 16)
        {
            m_alignment = 16;
        }

        // Create shared storage buffer for per-frame data
        MTLResourceOptions options = MTLResourceStorageModeShared | 
                                     MTLResourceCPUCacheModeWriteCombined;
        
        m_buffer = [device newBufferWithLength:desc.size options:options];
        
        if (!m_buffer)
        {
            RVX_RHI_ERROR("Metal: Failed to create ring buffer of size {}", desc.size);
            return;
        }

        if (desc.debugName)
        {
            m_buffer.label = [NSString stringWithUTF8String:desc.debugName];
        }

        // Keep buffer permanently mapped
        m_mappedPtr = m_buffer.contents;

        // Initialize frame allocations
        for (auto& frame : m_frameAllocations)
        {
            frame.startOffset = 0;
            frame.endOffset = 0;
        }

        RVX_RHI_DEBUG("Metal: Created ring buffer '{}' of size {} bytes, alignment {}", 
                     desc.debugName ? desc.debugName : "", desc.size, m_alignment);
    }

    MetalRingBuffer::~MetalRingBuffer()
    {
        m_wrapperBuffer = nullptr;
        m_buffer = nil;
    }

    RHIRingAllocation MetalRingBuffer::Allocate(uint64 size)
    {
        RHIRingAllocation result;

        if (!m_buffer || size == 0)
        {
            RVX_RHI_WARN("Metal: Invalid ring buffer allocation request");
            return result;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        // Align the allocation
        uint64 alignedOffset = (m_currentOffset + m_alignment - 1) & ~(static_cast<uint64>(m_alignment) - 1);
        uint64 alignedSize = (size + m_alignment - 1) & ~(static_cast<uint64>(m_alignment) - 1);

        // Check if we have enough space
        if (alignedOffset + alignedSize > m_totalSize)
        {
            // Wrap around to start if possible
            // Check if we're not overlapping with previous frames still in flight
            uint64 wrappedOffset = 0;
            
            // Check for overlap with in-flight frames
            for (uint32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
            {
                if (i == m_currentFrameIndex) continue;
                
                const auto& frame = m_frameAllocations[i];
                if (frame.endOffset > 0 && wrappedOffset < frame.endOffset)
                {
                    // Would overlap with in-flight frame
                    RVX_RHI_WARN("Metal: Ring buffer full, cannot wrap (in-flight data)");
                    return result;
                }
            }

            // Safe to wrap
            alignedOffset = 0;
            
            if (alignedOffset + alignedSize > m_totalSize)
            {
                RVX_RHI_ERROR("Metal: Ring buffer allocation too large ({} > {})", 
                             alignedSize, m_totalSize);
                return result;
            }
        }

        // Record allocation
        result.cpuAddress = static_cast<uint8*>(m_mappedPtr) + alignedOffset;
        result.gpuOffset = alignedOffset;
        result.size = alignedSize;
        result.buffer = GetBuffer();

        // Update current offset
        m_currentOffset = alignedOffset + alignedSize;

        // Track end of frame allocations
        m_frameAllocations[m_currentFrameIndex].endOffset = m_currentOffset;

        return result;
    }

    void MetalRingBuffer::Reset(uint32 frameIndex)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Move to next frame
        m_currentFrameIndex = frameIndex % MAX_FRAMES_IN_FLIGHT;

        // Mark the current frame's start
        m_frameAllocations[m_currentFrameIndex].startOffset = m_currentOffset;
        m_frameAllocations[m_currentFrameIndex].endOffset = m_currentOffset;
    }

    RHIBuffer* MetalRingBuffer::GetBuffer() const
    {
        if (!m_wrapperBuffer)
        {
            RHIBufferDesc bufferDesc;
            bufferDesc.size = m_totalSize;
            bufferDesc.usage = RHIBufferUsage::Constant | RHIBufferUsage::TransferSrc;
            bufferDesc.memoryType = RHIMemoryType::Upload;
            bufferDesc.debugName = GetDebugName();

            m_wrapperBuffer = MakeRef<MetalBuffer>(m_buffer, bufferDesc);
        }
        return m_wrapperBuffer.Get();
    }

} // namespace RVX
