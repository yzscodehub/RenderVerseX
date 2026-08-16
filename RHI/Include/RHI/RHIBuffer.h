#pragma once

#include "RHI/RHIResources.h"

#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace RVX
{
    // =============================================================================
    // Host Mapped Write Contract
    // =============================================================================
    /**
     * @brief The host-visibility operation that published a mapped CPU write.
     *
     * `cpuWriteOffset`/`cpuWriteSize` on the accompanying receipt always
     * describe the bytes the caller wrote. A synchronized range, when one is
     * available, describes the cache-management range in the backend memory
     * allocation and may be wider than the CPU write because of atom alignment.
     */
    enum class RHIHostWriteSynchronization : uint8
    {
        Unavailable = 0,
        CoherentNoExplicitSync,
        ExactRange,
        AtomAlignedRange,
        WholeResource,
        WholeAllocation,
    };

    /** @brief Outcome of a range-aware host mapped-write transaction. */
    struct RHIHostWriteReceipt
    {
        bool committed = false;
        RHIHostWriteSynchronization synchronization =
            RHIHostWriteSynchronization::Unavailable;
        uint64 cpuWriteOffset = 0;
        uint64 cpuWriteSize = 0;
        uint64 synchronizedOffset = 0;
        uint64 synchronizedSize = 0;
        bool synchronizedRangeAvailable = false;

        [[nodiscard]] bool IsPublished() const
        {
            return committed;
        }

        [[nodiscard]] bool HasSynchronizedRange() const
        {
            return synchronizedRangeAvailable;
        }
    };

    class RHIBuffer;
    class RHIStagingBuffer;
    class RHIMappedWriteTransactionOwner;

    /** @brief Shared identity and liveness state for one mapping owner. */
    struct RHIMappedWriteTransactionControl
    {
        RHIMappedWriteTransactionOwner* owner = nullptr;
        uint64 nextTransactionId = 0;
        uint64 activeTransactionId = 0;
        bool ownerAlive = true;
        bool transactionActive = false;
    };

    /** @brief Internal non-owning abort callback used by RHIMappedWriteAccess. */
    class RHIMappedWriteTransactionOwner
    {
    public:
        virtual void AbortMappedWriteAccess(
            const std::shared_ptr<RHIMappedWriteTransactionControl>& control,
            uint64 transactionId,
            uint64 offset,
            uint64 size) = 0;

    protected:
        virtual ~RHIMappedWriteTransactionOwner() = default;
    };

    /**
     * @brief Move-only capability returned by MapWriteRange().
     *
     * The capability is intentionally consumed by CommitMappedWriteRange() or
     * CancelMappedWriteRange(). Moving it makes the source stale; stale,
     * cancelled, and already-committed capabilities cannot publish data. If
     * the live capability leaves scope, its owner aborts the transaction.
     */
    class RHIMappedWriteAccess
    {
    public:
        RHIMappedWriteAccess() = default;
        ~RHIMappedWriteAccess()
        {
            AbortIfLive();
        }
        RHIMappedWriteAccess(const RHIMappedWriteAccess&) = delete;
        RHIMappedWriteAccess& operator=(const RHIMappedWriteAccess&) = delete;

        RHIMappedWriteAccess(RHIMappedWriteAccess&& other) noexcept
            : m_data(other.m_data)
            , m_offset(other.m_offset)
            , m_size(other.m_size)
            , m_control(std::move(other.m_control))
            , m_transactionId(other.m_transactionId)
        {
            other.Consume();
        }

        RHIMappedWriteAccess& operator=(RHIMappedWriteAccess&&) = delete;

        [[nodiscard]] void* GetData() const { return IsValid() ? m_data : nullptr; }
        [[nodiscard]] uint64 GetOffset() const { return m_offset; }
        [[nodiscard]] uint64 GetSize() const { return m_size; }
        [[nodiscard]] bool IsValid() const
        {
            return m_data != nullptr &&
                   m_control != nullptr &&
                   m_control->ownerAlive &&
                   m_control->transactionActive &&
                   m_control->activeTransactionId == m_transactionId;
        }

    private:
        RHIMappedWriteAccess(void* data,
                             uint64 offset,
                             uint64 size,
                             std::shared_ptr<RHIMappedWriteTransactionControl> control,
                             uint64 transactionId)
            : m_data(data)
            , m_offset(offset)
            , m_size(size)
            , m_control(std::move(control))
            , m_transactionId(transactionId)
        {
        }

        void AbortIfLive()
        {
            const std::shared_ptr<RHIMappedWriteTransactionControl> control =
                m_control;
            if (IsValid() && control->owner != nullptr)
            {
                control->owner->AbortMappedWriteAccess(
                    control, m_transactionId, m_offset, m_size);
            }
            Consume();
        }

        void Consume()
        {
            m_data = nullptr;
            m_offset = 0;
            m_size = 0;
            m_control.reset();
            m_transactionId = 0;
        }

        void* m_data = nullptr;
        uint64 m_offset = 0;
        uint64 m_size = 0;
        std::shared_ptr<RHIMappedWriteTransactionControl> m_control;
        uint64 m_transactionId = 0;

        friend class RHIBuffer;
        friend class RHIStagingBuffer;
    };

    [[nodiscard]] inline bool IsValidMappedWriteRange(
        uint64 resourceSize,
        uint64 offset,
        uint64 size)
    {
        return size != 0 && offset <= resourceSize && size <= resourceSize - offset;
    }

    // =============================================================================
    // Buffer Description
    // =============================================================================
    struct RHIBufferDesc
    {
        uint64 size = 0;
        RHIBufferUsage usage = RHIBufferUsage::None;
        RHIMemoryType memoryType = RHIMemoryType::Default;
        uint32 stride = 0;  // For structured buffers
        const char* debugName = nullptr;

        // Builder pattern helpers
        RHIBufferDesc& SetSize(uint64 s) { size = s; return *this; }
        RHIBufferDesc& SetUsage(RHIBufferUsage u) { usage = u; return *this; }
        RHIBufferDesc& SetMemoryType(RHIMemoryType m) { memoryType = m; return *this; }
        RHIBufferDesc& SetStride(uint32 s) { stride = s; return *this; }
        RHIBufferDesc& SetDebugName(const char* n) { debugName = n; return *this; }
    };

    // =============================================================================
    // Buffer Interface
    // =============================================================================
    class RHIBuffer : public RHIResource, private RHIMappedWriteTransactionOwner
    {
    public:
        RHIBuffer()
            : m_mappedWriteControl(
                  std::make_shared<RHIMappedWriteTransactionControl>())
        {
            m_mappedWriteControl->owner = this;
        }

        ~RHIBuffer() override
        {
            InvalidateMappedWriteControl();
        }

        // Getters
        virtual uint64 GetSize() const = 0;
        virtual RHIBufferUsage GetUsage() const = 0;
        virtual RHIMemoryType GetMemoryType() const = 0;
        virtual uint32 GetStride() const = 0;

        // Mapping (for Upload/Readback buffers)
        virtual void* Map() = 0;
        virtual void Unmap() = 0;

        /**
         * @brief Publish writes made through the current Map() access.
         *
         * The default keeps legacy RHI backends source-compatible: their
         * existing Unmap() implementation remains the completion operation and
         * is assumed to succeed. Backends with an observable host-cache or
         * device-health failure override this method and return false instead
         * of allowing a caller to publish an unsafe CPU-to-GPU write.
         */
        virtual bool CommitMappedWrite()
        {
            Unmap();
            return true;
        }

        /**
         * @brief Begin one range-bound CPU write transaction.
         *
         * Only one range transaction can be active at a time. Invalid,
         * zero-sized, overflowed, or nested ranges return an invalid access
         * capability and do not call the backend mapping path.
         */
        [[nodiscard]] RHIMappedWriteAccess MapWriteRange(uint64 offset, uint64 size)
        {
            const std::shared_ptr<RHIMappedWriteTransactionControl> control =
                m_mappedWriteControl;
            if (!control || !control->ownerAlive || control->transactionActive ||
                GetMemoryType() != RHIMemoryType::Upload ||
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

        /**
         * @brief Publish a previously mapped range and return its visibility receipt.
         */
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
                // A failed publication must relinquish the backend mapping
                // before its capability is consumed, so the next transaction
                // starts from a known nonpublishing state.
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
         * @brief Abort publication/residency advancement for a mapped range.
         *
         * Cancellation cannot roll back bytes already written through a
         * coherent mapping; it only prevents this transaction from being used
         * as the publication boundary.
         * @return True only when this access was the active transaction.
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

        /** @brief Map, bounds-check, write, and commit CPU data to this buffer. */
        template<typename T>
        bool Upload(const T* data, uint64 count, uint64 offset = 0)
        {
            if (count > std::numeric_limits<uint64>::max() / sizeof(T))
            {
                return false;
            }

            const uint64 byteCount = count * sizeof(T);
            if (offset > GetSize() || byteCount > GetSize() - offset)
                return false;

            if (offset > std::numeric_limits<size_t>::max() ||
                byteCount > std::numeric_limits<size_t>::max() ||
                offset > std::numeric_limits<size_t>::max() - byteCount)
            {
                return false;
            }

            if (byteCount == 0)
                return true;

            if (!data)
                return false;

            RHIMappedWriteAccess access = MapWriteRange(offset, byteCount);
            if (!access.IsValid())
                return false;

            std::memcpy(access.GetData(),
                        data,
                        static_cast<size_t>(byteCount));
            return CommitMappedWriteRange(std::move(access)).IsPublished();
        }

    protected:
        [[nodiscard]] bool HasActiveMappedWriteRange() const
        {
            return m_mappedWriteControl != nullptr &&
                   m_mappedWriteControl->ownerAlive &&
                   m_mappedWriteControl->transactionActive;
        }

        /**
         * @brief Backend hook for range mapping; legacy implementations map whole resources.
         *
         * Compatibility backends that use this default hook do not expose a
         * logical legacy-map state. Their callers must not overlap a legacy
         * Map()/Unmap() access with a range transaction. Production backends
         * override this hook and reject both interleaving directions.
         */
        virtual void* MapWriteRangeImpl(uint64 offset, uint64)
        {
            void* mappedData = Map();
            if (!mappedData)
            {
                return nullptr;
            }
            return static_cast<uint8*>(mappedData) + static_cast<size_t>(offset);
        }

        /** @brief Backend hook for range publication; legacy implementations have no sync evidence. */
        virtual RHIHostWriteReceipt CommitMappedWriteRangeImpl(uint64, uint64)
        {
            RHIHostWriteReceipt receipt;
            if (CommitMappedWrite())
            {
                // Map()/Unmap() legacy implementations do not provide enough
                // evidence to distinguish a host memcpy from an explicit
                // host-visibility operation. Preserve successful publication
                // without manufacturing a whole-resource synchronization.
                receipt.committed = true;
            }
            return receipt;
        }

        /** @brief Backend hook for range cancellation; legacy implementations unmap the resource. */
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
            // A backend that cannot end its mapping poisons the owner by
            // deliberately retaining transactionActive. There is no longer a
            // capability that could publish those bytes, and every later
            // MapWriteRange() fails closed until owner destruction.
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

} // namespace RVX
