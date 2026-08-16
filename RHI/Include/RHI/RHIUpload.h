#pragma once

/**
 * @file RHIUpload.h
 * @brief High-efficiency resource upload mechanisms (StagingBuffer, RingBuffer)
 */

#include "RHI/RHIResources.h"
#include "RHI/RHIBuffer.h"

#include <limits>

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
    class RHIStagingBuffer : public RHIResource,
                             private RHIMappedWriteTransactionOwner
    {
    public:
        RHIStagingBuffer()
            : m_mappedWriteControl(
                  std::make_shared<RHIMappedWriteTransactionControl>())
        {
            m_mappedWriteControl->owner = this;
        }

        ~RHIStagingBuffer() override
        {
            InvalidateMappedWriteControl();
        }

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
         * @brief Publish writes made through the current Map() access.
         *
         * The default keeps existing staging implementations source-compatible:
         * their legacy Unmap() operation remains the publication boundary and is
         * assumed to succeed. Backends with an observable host-memory failure
         * override this method and return false so callers can avoid recording
         * copies from data that was not safely made GPU-visible.
         */
        virtual bool CommitMappedWrite()
        {
            Unmap();
            return true;
        }

        /**
         * @brief Begin one range-bound staging write transaction.
         *
         * This mirrors RHIBuffer's mapped-write contract while preserving the
         * legacy Map()/Unmap()/CommitMappedWrite() API for existing upload code.
         */
        [[nodiscard]] RHIMappedWriteAccess MapWriteRange(uint64 offset, uint64 size)
        {
            const std::shared_ptr<RHIMappedWriteTransactionControl> control =
                m_mappedWriteControl;
            if (!control || !control->ownerAlive || control->transactionActive ||
                !IsValidMappedWriteRange(GetSize(), offset, size) ||
                offset > std::numeric_limits<size_t>::max() ||
                size > std::numeric_limits<size_t>::max() ||
                offset > std::numeric_limits<size_t>::max() - size)
            {
                return {};
            }

            void* mappedData = MapWriteRangeImpl(offset, size);
            if (!mappedData)
            {
                return {};
            }

            control->transactionActive = true;
            ++control->nextTransactionId;
            if (control->nextTransactionId == 0)
            {
                ++control->nextTransactionId;
            }
            control->activeTransactionId = control->nextTransactionId;
            return RHIMappedWriteAccess(mappedData,
                                        offset,
                                        size,
                                        control,
                                        control->activeTransactionId);
        }

        /** @brief Publish a staging write and report its host-visibility operation. */
        [[nodiscard]] RHIHostWriteReceipt CommitMappedWriteRange(
            RHIMappedWriteAccess&& access)
        {
            RHIHostWriteReceipt receipt;
            receipt.cpuWriteOffset = access.m_offset;
            receipt.cpuWriteSize = access.m_size;
            if (!IsCurrentMappedWrite(access))
            {
                return receipt;
            }

            receipt = CommitMappedWriteRangeImpl(access.m_offset, access.m_size);
            receipt.cpuWriteOffset = access.m_offset;
            receipt.cpuWriteSize = access.m_size;
            if (receipt.committed)
            {
                CompleteMappedWriteTransaction(access.m_control);
            }
            else
            {
                AbortMappedWriteTransaction(
                    access.m_control,
                    access.m_transactionId,
                    access.m_offset,
                    access.m_size);
            }
            access.Consume();
            return receipt;
        }

        /**
         * @brief Abort staging publication without advancing residency.
         *
         * Coherent mapped bytes cannot be rolled back; cancellation only
         * prevents this transaction from becoming a publication boundary.
         */
        bool CancelMappedWriteRange(RHIMappedWriteAccess&& access)
        {
            if (!IsCurrentMappedWrite(access))
            {
                return false;
            }

            const bool cancelled = AbortMappedWriteTransaction(
                access.m_control,
                access.m_transactionId,
                access.m_offset,
                access.m_size);
            access.Consume();
            return cancelled;
        }

        /**
         * @brief Get the buffer size
         */
        virtual uint64 GetSize() const = 0;

        /**
         * @brief Get the underlying RHI buffer for copy commands
         */
        virtual RHIBuffer* GetBuffer() const = 0;

    protected:
        [[nodiscard]] bool HasActiveMappedWriteRange() const
        {
            return m_mappedWriteControl != nullptr &&
                   m_mappedWriteControl->ownerAlive &&
                   m_mappedWriteControl->transactionActive;
        }

        /**
         * @brief Compatibility hook for range mapping.
         *
         * Backends using this default cannot observe an already-open legacy
         * mapping, so callers must not overlap legacy and range accesses.
         * Production backends override it and enforce mutual exclusion in
         * both directions.
         */
        virtual void* MapWriteRangeImpl(uint64 offset, uint64 size)
        {
            return Map(offset, size);
        }

        virtual RHIHostWriteReceipt CommitMappedWriteRangeImpl(uint64, uint64)
        {
            RHIHostWriteReceipt receipt;
            if (CommitMappedWrite())
            {
                receipt.committed = true;
            }
            return receipt;
        }

        virtual bool CancelMappedWriteRangeImpl(uint64, uint64)
        {
            Unmap();
            return true;
        }

    private:
        void AbortMappedWriteAccess(
            const std::shared_ptr<RHIMappedWriteTransactionControl>& control,
            uint64 transactionId,
            uint64 offset,
            uint64 size) override
        {
            if (!IsCurrentMappedWrite(control, transactionId))
            {
                return;
            }
            AbortMappedWriteTransaction(control, transactionId, offset, size);
        }

        [[nodiscard]] bool IsCurrentMappedWrite(
            const RHIMappedWriteAccess& access) const
        {
            return IsCurrentMappedWrite(access.m_control, access.m_transactionId) &&
                   access.IsValid();
        }

        [[nodiscard]] bool IsCurrentMappedWrite(
            const std::shared_ptr<RHIMappedWriteTransactionControl>& control,
            uint64 transactionId) const
        {
            return control != nullptr &&
                   control == m_mappedWriteControl &&
                   control->ownerAlive &&
                   control->transactionActive &&
                   control->activeTransactionId == transactionId;
        }

        bool AbortMappedWriteTransaction(
            const std::shared_ptr<RHIMappedWriteTransactionControl>& control,
            uint64 transactionId,
            uint64 offset,
            uint64 size)
        {
            if (!IsCurrentMappedWrite(control, transactionId))
            {
                return false;
            }

            const bool cancelled = CancelMappedWriteRangeImpl(offset, size);
            if (cancelled)
            {
                CompleteMappedWriteTransaction(control);
            }
            // Failed cleanup leaves the owner poisoned and blocks every later
            // range transaction rather than pretending the backend mapping
            // was released.
            return cancelled;
        }

        void CompleteMappedWriteTransaction(
            const std::shared_ptr<RHIMappedWriteTransactionControl>& control)
        {
            if (control == m_mappedWriteControl)
            {
                control->transactionActive = false;
                control->activeTransactionId = 0;
            }
        }

        void InvalidateMappedWriteControl()
        {
            if (m_mappedWriteControl)
            {
                m_mappedWriteControl->ownerAlive = false;
                m_mappedWriteControl->transactionActive = false;
                m_mappedWriteControl->activeTransactionId = 0;
                m_mappedWriteControl->owner = nullptr;
            }
        }

        std::shared_ptr<RHIMappedWriteTransactionControl> m_mappedWriteControl;
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
