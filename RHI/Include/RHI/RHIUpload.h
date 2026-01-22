#pragma once

/**
 * @file RHIUpload.h
 * @brief High-efficiency resource upload mechanisms (StagingBuffer, RingBuffer)
 */

#include "RHI/RHIResources.h"
#include "RHI/RHIBuffer.h"

namespace RVX
{
    // Forward declarations
    class RHIStagingBuffer;
    class RHIRingBuffer;

    using RHIStagingBufferRef = Ref<RHIStagingBuffer>;
    using RHIRingBufferRef = Ref<RHIRingBuffer>;

    // =============================================================================
    // Staging Buffer - Used for CPU->GPU data transfer
    // =============================================================================
    struct RHIStagingBufferDesc
    {
        uint64 size = 0;
        const char* debugName = nullptr;

        RHIStagingBufferDesc& SetSize(uint64 s) { size = s; return *this; }
        RHIStagingBufferDesc& SetDebugName(const char* n) { debugName = n; return *this; }
    };

    /**
     * @brief Staging buffer for efficient CPU to GPU data transfers
     * 
     * A staging buffer is a CPU-visible buffer used to stage data before
     * copying it to GPU-only resources. This is more efficient than 
     * using Map/Unmap on individual resources.
     * 
     * Usage:
     * @code
     * auto staging = device->CreateStagingBuffer({dataSize, "TextureUpload"});
     * void* mapped = staging->Map();
     * memcpy(mapped, textureData, dataSize);
     * staging->Unmap();
     * cmdContext->CopyBufferToTexture(staging->GetBuffer(), texture, copyDesc);
     * @endcode
     */
    class RHIStagingBuffer : public RHIResource
    {
    public:
        virtual ~RHIStagingBuffer() = default;

        /**
         * @brief Map the buffer for writing data
         * @param offset Starting offset (default 0)
         * @param size Size to map (RVX_WHOLE_SIZE = entire buffer)
         * @return Pointer to mapped memory, nullptr on failure
         */
        virtual void* Map(uint64 offset = 0, uint64 size = RVX_WHOLE_SIZE) = 0;

        /**
         * @brief Unmap the buffer and flush to GPU-visible memory
         */
        virtual void Unmap() = 0;

        /**
         * @brief Get the buffer size
         */
        virtual uint64 GetSize() const = 0;

        /**
         * @brief Get the underlying RHI buffer for copy commands
         */
        virtual RHIBuffer* GetBuffer() const = 0;
    };

    // =============================================================================
    // Ring Buffer - Used for per-frame temporary data (Constant Buffer, etc.)
    // =============================================================================
    struct RHIRingBufferDesc
    {
        uint64 size = 4 * 1024 * 1024;  // Default 4MB
        uint32 alignment = 256;          // Alignment requirement (Constant Buffer typically 256)
        const char* debugName = nullptr;

        RHIRingBufferDesc& SetSize(uint64 s) { size = s; return *this; }
        RHIRingBufferDesc& SetAlignment(uint32 a) { alignment = a; return *this; }
        RHIRingBufferDesc& SetDebugName(const char* n) { debugName = n; return *this; }
    };

    /**
     * @brief Result of a ring buffer allocation
     */
    struct RHIRingAllocation
    {
        void* cpuAddress = nullptr;      // CPU-writable address
        uint64 gpuOffset = 0;            // Offset within the GPU buffer
        uint64 size = 0;                 // Allocated size
        RHIBuffer* buffer = nullptr;     // Underlying buffer (for binding)

        bool IsValid() const { return cpuAddress != nullptr; }
    };

    /**
     * @brief Ring buffer for per-frame temporary data
     * 
     * A ring buffer allocates temporary memory from a pre-allocated pool,
     * cycling through the buffer each frame. This is efficient for data
     * that changes every frame (per-frame constants, dynamic vertex data).
     * 
     * Usage:
     * @code
     * auto ring = device->CreateRingBuffer({4*1024*1024, 256, "FrameConstants"});
     * 
     * // Each frame:
     * auto alloc = ring->Allocate(sizeof(PerFrameConstants));
     * memcpy(alloc.cpuAddress, &frameConstants, sizeof(PerFrameConstants));
     * // Use alloc.buffer and alloc.gpuOffset for binding
     * 
     * // End of frame:
     * ring->Reset(frameIndex);
     * @endcode
     */
    class RHIRingBuffer : public RHIResource
    {
    public:
        virtual ~RHIRingBuffer() = default;

        /**
         * @brief Allocate temporary memory from the ring buffer
         * @param size Requested size in bytes
         * @return Allocation result, check IsValid() for success
         */
        virtual RHIRingAllocation Allocate(uint64 size) = 0;

        /**
         * @brief Reset the allocator (call at end of frame)
         * @param frameIndex Current frame index for synchronization
         */
        virtual void Reset(uint32 frameIndex) = 0;

        /**
         * @brief Get the underlying buffer (for binding)
         */
        virtual RHIBuffer* GetBuffer() const = 0;

        /**
         * @brief Get the total buffer size
         */
        virtual uint64 GetSize() const = 0;

        /**
         * @brief Get the alignment requirement
         */
        virtual uint32 GetAlignment() const = 0;
    };

} // namespace RVX
