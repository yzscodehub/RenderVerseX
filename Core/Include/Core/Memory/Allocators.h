/**
 * @file Allocators.h
 * @brief Custom memory allocators for performance-critical code
 * 
 * Provides various allocator types:
 * - LinearAllocator: Fast bump allocator for temporary data
 * - PoolAllocator: Fixed-size object pools
 * - FrameAllocator: Per-frame temporary allocations
 */

#pragma once

#include "Core/Types.h"
#include <cstdlib>
#include <memory>
#include <vector>
#include <mutex>
#include <unordered_map>

namespace RVX
{
    /**
     * @brief Base interface for allocators
     */
    class IAllocator
    {
    public:
        virtual ~IAllocator() = default;

        virtual void* Allocate(size_t size, size_t alignment = 16) = 0;
        virtual void Free(void* ptr) = 0;
        virtual void Reset() = 0;

        virtual size_t GetUsedMemory() const = 0;
        virtual size_t GetTotalMemory() const = 0;
    };

    /**
     * @brief Linear allocator (bump allocator)
     * 
     * Extremely fast allocations but no individual frees.
     * All memory is released at once via Reset().
     * 
     * Perfect for:
     * - Per-frame temporary data
     * - Command buffer building
     * - Scratch memory
     */
    class LinearAllocator : public IAllocator
    {
    public:
        explicit LinearAllocator(size_t capacity);
        ~LinearAllocator() override;

        // Non-copyable
        LinearAllocator(const LinearAllocator&) = delete;
        LinearAllocator& operator=(const LinearAllocator&) = delete;

        // Movable
        LinearAllocator(LinearAllocator&& other) noexcept;
        LinearAllocator& operator=(LinearAllocator&& other) noexcept;

        void* Allocate(size_t size, size_t alignment = 16) override;
        void Free(void* ptr) override { (void)ptr; /* No-op */ }
        void Reset() override;

        size_t GetUsedMemory() const override { return m_offset; }
        size_t GetTotalMemory() const override { return m_capacity; }

        /// Check if there's room for an allocation
        bool CanAllocate(size_t size, size_t alignment = 16) const;

        /// Get remaining capacity
        size_t GetRemainingCapacity() const { return m_capacity - m_offset; }

    private:
        uint8* m_memory = nullptr;
        size_t m_capacity = 0;
        size_t m_offset = 0;
    };

    /**
     * @brief Pool allocator for fixed-size objects
     * 
     * Fast allocation and deallocation of same-sized objects.
     * Uses a free list for O(1) operations.
     * 
     * Perfect for:
     * - Entity/Component pools
     * - Frequently created/destroyed objects
     * - Cache-friendly object storage
     */
    class PoolAllocator : public IAllocator
    {
    public:
        PoolAllocator(size_t objectSize, size_t objectCount, size_t alignment = 16);
        ~PoolAllocator() override;

        // Non-copyable
        PoolAllocator(const PoolAllocator&) = delete;
        PoolAllocator& operator=(const PoolAllocator&) = delete;

        void* Allocate(size_t size, size_t alignment = 16) override;
        void Free(void* ptr) override;
        void Reset() override;

        size_t GetUsedMemory() const override { return m_allocatedCount * m_objectSize; }
        size_t GetTotalMemory() const override { return m_objectCount * m_objectSize; }

        /// Get object size
        size_t GetObjectSize() const { return m_objectSize; }

        /// Get number of allocated objects
        size_t GetAllocatedCount() const { return m_allocatedCount; }

        /// Get total object capacity
        size_t GetObjectCount() const { return m_objectCount; }

        /// Check if pool is full
        bool IsFull() const { return m_allocatedCount >= m_objectCount; }

    private:
        struct FreeNode
        {
            FreeNode* next;
        };

        void BuildFreeList();

        uint8* m_memory = nullptr;
        FreeNode* m_freeList = nullptr;
        size_t m_objectSize = 0;
        size_t m_objectCount = 0;
        size_t m_alignment = 16;
        size_t m_allocatedCount = 0;
    };

    /**
     * @brief Frame allocator for per-frame temporary memory
     * 
     * Double-buffered linear allocator that automatically resets
     * each frame. Thread-safe for allocations.
     * 
     * Usage:
     * @code
     * // Initialize once
     * FrameAllocator::Get().Initialize(1024 * 1024);  // 1MB per frame
     * 
     * // Each frame
     * void* data = FrameAllocator::Get().Allocate(sizeof(MyData));
     * 
     * // At frame end (called by engine)
     * FrameAllocator::Get().NextFrame();
     * @endcode
     */
    class FrameAllocator
    {
    public:
        /// Get global instance
        static FrameAllocator& Get();

        /**
         * @brief Initialize the frame allocator
         * @param capacityPerFrame Memory capacity per frame
         */
        void Initialize(size_t capacityPerFrame);

        /**
         * @brief Shutdown and release memory
         */
        void Shutdown();

        /**
         * @brief Allocate memory from current frame
         */
        void* Allocate(size_t size, size_t alignment = 16);

        /**
         * @brief Typed allocation helper
         */
        template<typename T, typename... Args>
        T* New(Args&&... args)
        {
            void* mem = Allocate(sizeof(T), alignof(T));
            return new (mem) T(std::forward<Args>(args)...);
        }

        /**
         * @brief Allocate array
         */
        template<typename T>
        T* AllocateArray(size_t count)
        {
            return static_cast<T*>(Allocate(sizeof(T) * count, alignof(T)));
        }

        /**
         * @brief Advance to next frame (resets previous frame's allocator)
         */
        void NextFrame();

        /**
         * @brief Get current frame index
         */
        uint64 GetFrameIndex() const { return m_frameIndex; }

        /**
         * @brief Get memory used this frame
         */
        size_t GetUsedMemory() const;

        /**
         * @brief Get total memory capacity per frame
         */
        size_t GetCapacityPerFrame() const { return m_capacityPerFrame; }

    private:
        FrameAllocator() = default;
        ~FrameAllocator() { Shutdown(); }

        static constexpr int kBufferCount = 2;

        std::unique_ptr<LinearAllocator> m_allocators[kBufferCount];
        size_t m_capacityPerFrame = 0;
        uint64 m_frameIndex = 0;
        int m_currentBuffer = 0;

        std::mutex m_mutex;
    };

    /**
     * @brief Stack allocator with markers for nested allocations
     */
    class StackAllocator : public IAllocator
    {
    public:
        using Marker = size_t;

        explicit StackAllocator(size_t capacity);
        ~StackAllocator() override;

        void* Allocate(size_t size, size_t alignment = 16) override;
        void Free(void* ptr) override { (void)ptr; /* Use FreeToMarker */ }
        void Reset() override;

        size_t GetUsedMemory() const override { return m_offset; }
        size_t GetTotalMemory() const override { return m_capacity; }

        /// Get a marker for the current position
        Marker GetMarker() const { return m_offset; }

        /// Free all memory allocated after the marker
        void FreeToMarker(Marker marker);

    private:
        uint8* m_memory = nullptr;
        size_t m_capacity = 0;
        size_t m_offset = 0;
    };

    /**
     * @brief RAII scope for stack allocator
     */
    class StackScope
    {
    public:
        explicit StackScope(StackAllocator& allocator)
            : m_allocator(allocator)
            , m_marker(allocator.GetMarker())
        {
        }

        ~StackScope()
        {
            m_allocator.FreeToMarker(m_marker);
        }

        // Non-copyable
        StackScope(const StackScope&) = delete;
        StackScope& operator=(const StackScope&) = delete;

    private:
        StackAllocator& m_allocator;
        StackAllocator::Marker m_marker;
    };

    /**
     * @brief Memory tracker for debugging allocations
     */
    class MemoryTracker
    {
    public:
        static MemoryTracker& Get();

        /// Track an allocation
        void TrackAllocation(void* ptr, size_t size, const char* file, int line);

        /// Track a deallocation
        void TrackDeallocation(void* ptr);

        /// Get total allocated bytes
        size_t GetTotalAllocated() const { return m_totalAllocated; }

        /// Get allocation count
        size_t GetAllocationCount() const { return m_allocationCount; }

        /// Get peak allocated bytes
        size_t GetPeakAllocated() const { return m_peakAllocated; }

        /// Print allocation summary
        void PrintSummary() const;

        /// Check for leaks (call at shutdown)
        bool CheckForLeaks() const;

    private:
        struct AllocationInfo
        {
            size_t size;
            const char* file;
            int line;
        };

        std::unordered_map<void*, AllocationInfo> m_allocations;
        size_t m_totalAllocated = 0;
        size_t m_peakAllocated = 0;
        size_t m_allocationCount = 0;
        mutable std::mutex m_mutex;
    };

} // namespace RVX

// Tracked allocation macros
#ifdef RVX_TRACK_MEMORY
    #define RVX_NEW(Type, ...) \
        (::RVX::MemoryTracker::Get().TrackAllocation( \
            new Type(__VA_ARGS__), sizeof(Type), __FILE__, __LINE__), \
         new Type(__VA_ARGS__))
    #define RVX_DELETE(ptr) \
        (::RVX::MemoryTracker::Get().TrackDeallocation(ptr), delete ptr)
#else
    #define RVX_NEW(Type, ...) new Type(__VA_ARGS__)
    #define RVX_DELETE(ptr) delete ptr
#endif
