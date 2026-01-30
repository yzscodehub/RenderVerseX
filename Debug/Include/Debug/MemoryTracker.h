#pragma once

/**
 * @file MemoryTracker.h
 * @brief Enhanced memory allocation tracking and leak detection
 * 
 * Features:
 * - Allocation/deallocation tracking with source location
 * - Memory leak detection at shutdown
 * - Per-category memory statistics
 * - Allocation history for debugging
 * - Thread-safe operation
 */

#include "Core/Types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <functional>

namespace RVX
{
    /**
     * @brief Memory allocation category
     */
    enum class MemoryCategory : uint8
    {
        General = 0,
        Render,
        Scene,
        Physics,
        Audio,
        Resource,
        Scripting,
        UI,
        Debug,
        Temp,
        Count
    };

    /**
     * @brief Get string name for a memory category
     */
    const char* GetMemoryCategoryName(MemoryCategory category);

    /**
     * @brief Information about a single allocation
     */
    struct AllocationInfo
    {
        void* address = nullptr;
        size_t size = 0;
        const char* file = nullptr;
        int32 line = 0;
        const char* function = nullptr;
        MemoryCategory category = MemoryCategory::General;
        uint64 frameIndex = 0;
        uint64 allocationIndex = 0;
        std::thread::id threadId;
    };

    /**
     * @brief Statistics for a memory category
     */
    struct MemoryCategoryStats
    {
        size_t currentBytes = 0;
        size_t peakBytes = 0;
        size_t totalAllocated = 0;
        size_t totalFreed = 0;
        uint64 allocationCount = 0;
        uint64 freeCount = 0;
    };

    /**
     * @brief Summary of all memory usage
     */
    struct MemorySummary
    {
        size_t totalCurrentBytes = 0;
        size_t totalPeakBytes = 0;
        uint64 totalAllocations = 0;
        uint64 activeAllocations = 0;
        MemoryCategoryStats categories[static_cast<size_t>(MemoryCategory::Count)];
    };

    /**
     * @brief Callback for memory events
     */
    using MemoryEventCallback = std::function<void(const AllocationInfo&, bool isAllocation)>;

    /**
     * @brief Enhanced memory tracker with categorization and leak detection
     * 
     * Usage:
     * @code
     * // Track allocations
     * void* ptr = malloc(1024);
     * MemoryTracker::Get().TrackAllocation(ptr, 1024, __FILE__, __LINE__, MemoryCategory::Render);
     * 
     * // Track deallocations
     * MemoryTracker::Get().TrackDeallocation(ptr);
     * free(ptr);
     * 
     * // Check for leaks
     * auto leaks = MemoryTracker::Get().GetLeaks();
     * @endcode
     */
    class MemoryTracker
    {
    public:
        // =========================================================================
        // Singleton Access
        // =========================================================================

        /**
         * @brief Get the global tracker instance
         */
        static MemoryTracker& Get();

        // =========================================================================
        // Lifecycle
        // =========================================================================

        /**
         * @brief Initialize the tracker
         * @param trackCallStack Whether to capture call stacks (slower)
         */
        void Initialize(bool trackCallStack = false);

        /**
         * @brief Shutdown and report any leaks
         */
        void Shutdown();

        /**
         * @brief Check if tracker is initialized
         */
        bool IsInitialized() const { return m_initialized; }

        // =========================================================================
        // Allocation Tracking
        // =========================================================================

        /**
         * @brief Track a memory allocation
         * @param ptr Allocated pointer
         * @param size Allocation size in bytes
         * @param file Source file (use __FILE__)
         * @param line Source line (use __LINE__)
         * @param category Memory category
         * @param function Function name (use __FUNCTION__)
         */
        void TrackAllocation(
            void* ptr,
            size_t size,
            const char* file = nullptr,
            int32 line = 0,
            MemoryCategory category = MemoryCategory::General,
            const char* function = nullptr
        );

        /**
         * @brief Track a memory deallocation
         * @param ptr Pointer being freed
         * @return true if the allocation was found, false if unknown
         */
        bool TrackDeallocation(void* ptr);

        /**
         * @brief Track a reallocation
         * @param oldPtr Old pointer
         * @param newPtr New pointer
         * @param newSize New size
         * @param file Source file
         * @param line Source line
         * @param category Memory category
         */
        void TrackReallocation(
            void* oldPtr,
            void* newPtr,
            size_t newSize,
            const char* file = nullptr,
            int32 line = 0,
            MemoryCategory category = MemoryCategory::General
        );

        // =========================================================================
        // Queries
        // =========================================================================

        /**
         * @brief Get information about a specific allocation
         * @param ptr Pointer to query
         * @return Allocation info, or nullptr if not found
         */
        const AllocationInfo* GetAllocationInfo(void* ptr) const;

        /**
         * @brief Get all active allocations
         */
        std::vector<AllocationInfo> GetActiveAllocations() const;

        /**
         * @brief Get all detected memory leaks (same as active allocations at shutdown)
         */
        std::vector<AllocationInfo> GetLeaks() const { return GetActiveAllocations(); }

        /**
         * @brief Get memory summary
         */
        MemorySummary GetSummary() const;

        /**
         * @brief Get statistics for a specific category
         */
        MemoryCategoryStats GetCategoryStats(MemoryCategory category) const;

        // =========================================================================
        // Analysis
        // =========================================================================

        /**
         * @brief Check if there are any memory leaks
         */
        bool HasLeaks() const;

        /**
         * @brief Get the number of active allocations
         */
        uint64 GetActiveAllocationCount() const;

        /**
         * @brief Get the total currently allocated bytes
         */
        size_t GetTotalAllocatedBytes() const;

        /**
         * @brief Get peak memory usage
         */
        size_t GetPeakAllocatedBytes() const { return m_peakBytes; }

        /**
         * @brief Print a memory summary to the log
         */
        void PrintSummary() const;

        /**
         * @brief Print all active allocations to the log
         */
        void PrintAllocations() const;

        /**
         * @brief Print detected leaks to the log
         */
        void PrintLeaks() const;

        // =========================================================================
        // Configuration
        // =========================================================================

        /**
         * @brief Enable/disable tracking
         */
        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        /**
         * @brief Set the current frame index (for tracking allocation age)
         */
        void SetFrameIndex(uint64 frame) { m_currentFrame = frame; }

        /**
         * @brief Register a callback for memory events
         */
        void RegisterCallback(MemoryEventCallback callback);

        /**
         * @brief Clear all registered callbacks
         */
        void ClearCallbacks();

        // =========================================================================
        // Snapshot and Diff
        // =========================================================================

        /**
         * @brief Take a snapshot of current allocations
         * @return Snapshot ID
         */
        uint32 TakeSnapshot();

        /**
         * @brief Get allocations made since a snapshot
         * @param snapshotId Snapshot ID from TakeSnapshot
         */
        std::vector<AllocationInfo> GetAllocationsSinceSnapshot(uint32 snapshotId) const;

        /**
         * @brief Clear a snapshot
         */
        void ClearSnapshot(uint32 snapshotId);

    private:
        MemoryTracker() = default;
        ~MemoryTracker() { Shutdown(); }

        // Non-copyable
        MemoryTracker(const MemoryTracker&) = delete;
        MemoryTracker& operator=(const MemoryTracker&) = delete;

        void NotifyCallbacks(const AllocationInfo& info, bool isAllocation);

        // =========================================================================
        // Internal State
        // =========================================================================

        bool m_initialized = false;
        bool m_enabled = true;
        bool m_trackCallStack = false;

        // Allocation tracking
        std::unordered_map<void*, AllocationInfo> m_allocations;
        uint64 m_nextAllocationIndex = 0;
        uint64 m_currentFrame = 0;

        // Statistics
        size_t m_currentBytes = 0;
        size_t m_peakBytes = 0;
        MemoryCategoryStats m_categoryStats[static_cast<size_t>(MemoryCategory::Count)];

        // Snapshots
        struct Snapshot
        {
            uint64 allocationIndex;
            std::vector<void*> activePointers;
        };
        std::unordered_map<uint32, Snapshot> m_snapshots;
        uint32 m_nextSnapshotId = 0;

        // Callbacks
        std::vector<MemoryEventCallback> m_callbacks;

        // Thread safety
        mutable std::mutex m_mutex;
    };

} // namespace RVX

// =============================================================================
// Memory Tracking Macros
// =============================================================================

#ifdef RVX_TRACK_MEMORY
    /**
     * @brief Track a malloc-style allocation
     */
    #define RVX_TRACK_ALLOC(ptr, size) \
        ::RVX::MemoryTracker::Get().TrackAllocation(ptr, size, __FILE__, __LINE__, \
            ::RVX::MemoryCategory::General, __FUNCTION__)

    /**
     * @brief Track an allocation with category
     */
    #define RVX_TRACK_ALLOC_CAT(ptr, size, category) \
        ::RVX::MemoryTracker::Get().TrackAllocation(ptr, size, __FILE__, __LINE__, \
            category, __FUNCTION__)

    /**
     * @brief Track a deallocation
     */
    #define RVX_TRACK_FREE(ptr) \
        ::RVX::MemoryTracker::Get().TrackDeallocation(ptr)

    /**
     * @brief Tracked new operator
     */
    #define RVX_TRACKED_NEW(Type, ...) \
        ([&]() -> Type* { \
            Type* _ptr = new Type(__VA_ARGS__); \
            ::RVX::MemoryTracker::Get().TrackAllocation(_ptr, sizeof(Type), \
                __FILE__, __LINE__, ::RVX::MemoryCategory::General, __FUNCTION__); \
            return _ptr; \
        })()

    /**
     * @brief Tracked delete operator
     */
    #define RVX_TRACKED_DELETE(ptr) \
        do { \
            ::RVX::MemoryTracker::Get().TrackDeallocation(ptr); \
            delete ptr; \
        } while(false)

#else
    #define RVX_TRACK_ALLOC(ptr, size) ((void)0)
    #define RVX_TRACK_ALLOC_CAT(ptr, size, category) ((void)0)
    #define RVX_TRACK_FREE(ptr) ((void)0)
    #define RVX_TRACKED_NEW(Type, ...) new Type(__VA_ARGS__)
    #define RVX_TRACKED_DELETE(ptr) delete ptr
#endif
