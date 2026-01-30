/**
 * @file MemoryTracker.cpp
 * @brief Memory tracking implementation
 */

#include "Debug/MemoryTracker.h"
#include "Core/Log.h"
#include <algorithm>

namespace RVX
{
    // =========================================================================
    // Category Names
    // =========================================================================

    const char* GetMemoryCategoryName(MemoryCategory category)
    {
        static const char* names[] = {
            "General",
            "Render",
            "Scene",
            "Physics",
            "Audio",
            "Resource",
            "Scripting",
            "UI",
            "Debug",
            "Temp"
        };

        static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(MemoryCategory::Count),
            "Memory category names array size mismatch");

        return names[static_cast<size_t>(category)];
    }

    // =========================================================================
    // Singleton
    // =========================================================================

    MemoryTracker& MemoryTracker::Get()
    {
        static MemoryTracker instance;
        return instance;
    }

    // =========================================================================
    // Lifecycle
    // =========================================================================

    void MemoryTracker::Initialize(bool trackCallStack)
    {
        if (m_initialized)
        {
            return;
        }

        m_trackCallStack = trackCallStack;
        m_allocations.reserve(1024);

        m_initialized = true;
        RVX_CORE_INFO("MemoryTracker initialized (callstack tracking: {})", trackCallStack);
    }

    void MemoryTracker::Shutdown()
    {
        if (!m_initialized)
        {
            return;
        }

        // Report any remaining allocations as leaks
        if (HasLeaks())
        {
            RVX_CORE_WARN("MemoryTracker: {} memory leaks detected!", m_allocations.size());
            PrintLeaks();
        }

        m_allocations.clear();
        m_snapshots.clear();
        m_callbacks.clear();

        m_initialized = false;
        RVX_CORE_INFO("MemoryTracker shutdown");
    }

    // =========================================================================
    // Allocation Tracking
    // =========================================================================

    void MemoryTracker::TrackAllocation(
        void* ptr,
        size_t size,
        const char* file,
        int32 line,
        MemoryCategory category,
        const char* function)
    {
        if (!m_initialized || !m_enabled || ptr == nullptr)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        // Check for duplicate tracking
        if (m_allocations.find(ptr) != m_allocations.end())
        {
            RVX_CORE_WARN("MemoryTracker: Duplicate allocation tracking for ptr {}", ptr);
            return;
        }

        AllocationInfo info;
        info.address = ptr;
        info.size = size;
        info.file = file;
        info.line = line;
        info.function = function;
        info.category = category;
        info.frameIndex = m_currentFrame;
        info.allocationIndex = m_nextAllocationIndex++;
        info.threadId = std::this_thread::get_id();

        m_allocations[ptr] = info;

        // Update statistics
        m_currentBytes += size;
        m_peakBytes = std::max(m_peakBytes, m_currentBytes);

        auto catIndex = static_cast<size_t>(category);
        m_categoryStats[catIndex].currentBytes += size;
        m_categoryStats[catIndex].peakBytes = std::max(
            m_categoryStats[catIndex].peakBytes,
            m_categoryStats[catIndex].currentBytes
        );
        m_categoryStats[catIndex].totalAllocated += size;
        m_categoryStats[catIndex].allocationCount++;

        // Notify callbacks
        NotifyCallbacks(info, true);
    }

    bool MemoryTracker::TrackDeallocation(void* ptr)
    {
        if (!m_initialized || !m_enabled || ptr == nullptr)
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_allocations.find(ptr);
        if (it == m_allocations.end())
        {
            // Unknown allocation - might be from before tracking started
            return false;
        }

        const AllocationInfo& info = it->second;

        // Update statistics
        m_currentBytes -= info.size;

        auto catIndex = static_cast<size_t>(info.category);
        m_categoryStats[catIndex].currentBytes -= info.size;
        m_categoryStats[catIndex].totalFreed += info.size;
        m_categoryStats[catIndex].freeCount++;

        // Notify callbacks
        NotifyCallbacks(info, false);

        m_allocations.erase(it);
        return true;
    }

    void MemoryTracker::TrackReallocation(
        void* oldPtr,
        void* newPtr,
        size_t newSize,
        const char* file,
        int32 line,
        MemoryCategory category)
    {
        if (oldPtr != nullptr)
        {
            TrackDeallocation(oldPtr);
        }

        if (newPtr != nullptr)
        {
            TrackAllocation(newPtr, newSize, file, line, category, nullptr);
        }
    }

    // =========================================================================
    // Queries
    // =========================================================================

    const AllocationInfo* MemoryTracker::GetAllocationInfo(void* ptr) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_allocations.find(ptr);
        if (it != m_allocations.end())
        {
            return &it->second;
        }

        return nullptr;
    }

    std::vector<AllocationInfo> MemoryTracker::GetActiveAllocations() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::vector<AllocationInfo> result;
        result.reserve(m_allocations.size());

        for (const auto& [ptr, info] : m_allocations)
        {
            result.push_back(info);
        }

        // Sort by allocation index for consistent ordering
        std::sort(result.begin(), result.end(),
            [](const AllocationInfo& a, const AllocationInfo& b) {
                return a.allocationIndex < b.allocationIndex;
            });

        return result;
    }

    MemorySummary MemoryTracker::GetSummary() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        MemorySummary summary;
        summary.totalCurrentBytes = m_currentBytes;
        summary.totalPeakBytes = m_peakBytes;
        summary.totalAllocations = m_nextAllocationIndex;
        summary.activeAllocations = m_allocations.size();

        for (size_t i = 0; i < static_cast<size_t>(MemoryCategory::Count); ++i)
        {
            summary.categories[i] = m_categoryStats[i];
        }

        return summary;
    }

    MemoryCategoryStats MemoryTracker::GetCategoryStats(MemoryCategory category) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_categoryStats[static_cast<size_t>(category)];
    }

    // =========================================================================
    // Analysis
    // =========================================================================

    bool MemoryTracker::HasLeaks() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_allocations.empty();
    }

    uint64 MemoryTracker::GetActiveAllocationCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_allocations.size();
    }

    size_t MemoryTracker::GetTotalAllocatedBytes() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_currentBytes;
    }

    void MemoryTracker::PrintSummary() const
    {
        auto summary = GetSummary();

        RVX_CORE_INFO("=== Memory Summary ===");
        RVX_CORE_INFO("  Current: {} bytes ({:.2f} MB)",
            summary.totalCurrentBytes,
            summary.totalCurrentBytes / (1024.0 * 1024.0));
        RVX_CORE_INFO("  Peak: {} bytes ({:.2f} MB)",
            summary.totalPeakBytes,
            summary.totalPeakBytes / (1024.0 * 1024.0));
        RVX_CORE_INFO("  Total Allocations: {}", summary.totalAllocations);
        RVX_CORE_INFO("  Active Allocations: {}", summary.activeAllocations);

        RVX_CORE_INFO("  By Category:");
        for (size_t i = 0; i < static_cast<size_t>(MemoryCategory::Count); ++i)
        {
            const auto& cat = summary.categories[i];
            if (cat.allocationCount > 0)
            {
                RVX_CORE_INFO("    {}: {} bytes ({} allocs)",
                    GetMemoryCategoryName(static_cast<MemoryCategory>(i)),
                    cat.currentBytes,
                    cat.allocationCount - cat.freeCount);
            }
        }
    }

    void MemoryTracker::PrintAllocations() const
    {
        auto allocations = GetActiveAllocations();

        RVX_CORE_INFO("=== Active Allocations ({}) ===", allocations.size());
        for (const auto& info : allocations)
        {
            RVX_CORE_INFO("  {} bytes at {} [{}] {}:{}",
                info.size,
                info.address,
                GetMemoryCategoryName(info.category),
                info.file ? info.file : "unknown",
                info.line);
        }
    }

    void MemoryTracker::PrintLeaks() const
    {
        auto leaks = GetLeaks();

        RVX_CORE_WARN("=== Memory Leaks ({}) ===", leaks.size());
        for (const auto& info : leaks)
        {
            RVX_CORE_WARN("  LEAK: {} bytes at {} [{}]",
                info.size,
                info.address,
                GetMemoryCategoryName(info.category));
            if (info.file)
            {
                RVX_CORE_WARN("    Location: {}:{}", info.file, info.line);
            }
            if (info.function)
            {
                RVX_CORE_WARN("    Function: {}", info.function);
            }
        }
    }

    // =========================================================================
    // Callbacks
    // =========================================================================

    void MemoryTracker::RegisterCallback(MemoryEventCallback callback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_callbacks.push_back(std::move(callback));
    }

    void MemoryTracker::ClearCallbacks()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_callbacks.clear();
    }

    void MemoryTracker::NotifyCallbacks(const AllocationInfo& info, bool isAllocation)
    {
        for (const auto& callback : m_callbacks)
        {
            callback(info, isAllocation);
        }
    }

    // =========================================================================
    // Snapshots
    // =========================================================================

    uint32 MemoryTracker::TakeSnapshot()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        uint32 snapshotId = m_nextSnapshotId++;

        Snapshot snapshot;
        snapshot.allocationIndex = m_nextAllocationIndex;

        for (const auto& [ptr, info] : m_allocations)
        {
            snapshot.activePointers.push_back(ptr);
        }

        m_snapshots[snapshotId] = std::move(snapshot);

        return snapshotId;
    }

    std::vector<AllocationInfo> MemoryTracker::GetAllocationsSinceSnapshot(uint32 snapshotId) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::vector<AllocationInfo> result;

        auto it = m_snapshots.find(snapshotId);
        if (it == m_snapshots.end())
        {
            return result;
        }

        const Snapshot& snapshot = it->second;

        for (const auto& [ptr, info] : m_allocations)
        {
            if (info.allocationIndex >= snapshot.allocationIndex)
            {
                result.push_back(info);
            }
        }

        return result;
    }

    void MemoryTracker::ClearSnapshot(uint32 snapshotId)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshots.erase(snapshotId);
    }

} // namespace RVX
